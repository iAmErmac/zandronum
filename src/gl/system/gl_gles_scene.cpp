#include "gl/system/gl_gles_scene.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#include <algorithm>
#include <limits>
#include <stdio.h>
#include <vector>

#include "gl/system/gl_gles_dispatch.h"
#include "gl/system/gl_gles_renderer.h"

namespace
{
	bool SceneCapacityWarningLogged = false;
	bool SceneLightFailureLogged = false;
	size_t SceneVertexBufferCapacity = 0;
	size_t SceneIndexBufferCapacity = 0;
	static const size_t MaxSceneLightSelections = 64;
	struct FSceneLightSelectionCacheEntry
	{
		const float *sourceData;
		FGLESSceneLightSelection selection;
	};
	GLuint SceneLightTexture = 0;
	GLsizei SceneLightTextureWidth = 0;
	GLsizei SceneLightTextureHeight = 0;
	std::vector<GLfloat> SceneLightTexels;
	std::vector<GLfloat> SceneLightPositionStream;
	std::vector<GLfloat> SceneLightColorStream;
	std::vector<FSceneLightSelectionCacheEntry> SceneLightSelections;
	std::vector<size_t> SceneDrawOrders[2];
	std::vector<const FGLESSceneOrderRecord *> SceneIncludedRecords;

	void ReportSceneLightFailure(const char *message)
	{
		if (SceneLightFailureLogged) return;
		SceneLightFailureLogged = true;
		gl_GLES_Report("lights", message);
	}

	size_t GrowBufferCapacity(size_t capacity, size_t required)
	{
		if (capacity == 0) capacity = 64 * 1024;
		while (capacity < required)
		{
			if (capacity > std::numeric_limits<size_t>::max() / 2)
				return required;
			capacity *= 2;
		}
		return capacity;
	}

	bool MatchesLightSelection(const float *lightData,
		const FGLESSceneLightSelection &candidate)
	{
		if (lightData == nullptr)
			return false;
		const size_t streamEnd = candidate.streamOffset + candidate.lightCount;
		if (streamEnd > SceneLightPositionStream.size() / 4 ||
			streamEnd > SceneLightColorStream.size() / 4)
			return false;
		for (unsigned int light = 0; light < candidate.lightCount; ++light)
		{
			const size_t offset = (candidate.streamOffset + light) * 4;
			if (memcmp(&SceneLightPositionStream[offset], lightData + light * 8, 4 * sizeof(GLfloat)) != 0 ||
				memcmp(&SceneLightColorStream[offset], lightData + light * 8 + 4, 4 * sizeof(GLfloat)) != 0)
				return false;
		}
		return true;
	}
}

FGLESSceneDrawOrderView gl_GLESInternalSceneSortBatches(const FGLESSceneOrderRecord *records,
	size_t recordCount, FGLESSceneOrderSlot slot)
{
	std::vector<size_t> *drawOrder = nullptr;
	if (slot == GLES_SCENE_ORDER_PORTAL) drawOrder = &SceneDrawOrders[0];
	else if (slot == GLES_SCENE_ORDER_VIEW) drawOrder = &SceneDrawOrders[1];
	if (drawOrder == nullptr) return { nullptr, 0 };

	drawOrder->clear();
	if (records == nullptr || recordCount == 0) return { nullptr, 0 };
	bool requiresOrdering = false;
	for (size_t i = 0; i < recordCount; ++i)
	{
		if (records[i].included &&
			(records[i].hud || records[i].flood || records[i].translucent || records[i].sourceOrdered))
		{
			requiresOrdering = true;
			break;
		}
	}
	if (!requiresOrdering)
	{
		drawOrder->reserve(recordCount);
		for (size_t i = 0; i < recordCount; ++i)
			if (records[i].included) drawOrder->push_back(records[i].batchIndex);
		return { drawOrder->empty() ? nullptr : drawOrder->data(), drawOrder->size() };
	}

	std::vector<const FGLESSceneOrderRecord *> &included = SceneIncludedRecords;
	included.clear();
	included.reserve(recordCount);
	for (size_t i = 0; i < recordCount; ++i)
		if (records[i].included) included.push_back(&records[i]);
	// The opaque groups only need their collection order preserved. Partition
	// them in linear passes and reserve sorting for the translucent subgroups.
	const auto nonHudEnd = std::stable_partition(included.begin(), included.end(),
		[](const FGLESSceneOrderRecord *record) { return !record->hud; });
	auto orderFloodGroup = [](std::vector<const FGLESSceneOrderRecord *>::iterator first,
		std::vector<const FGLESSceneOrderRecord *>::iterator last)
	{
		const auto wallEnd = std::stable_partition(first, last,
			[](const FGLESSceneOrderRecord *record) { return !record->flat; });
		auto orderSurfaceGroup = [](std::vector<const FGLESSceneOrderRecord *>::iterator surfaceFirst,
			std::vector<const FGLESSceneOrderRecord *>::iterator surfaceLast)
		{
			const auto opaqueEnd = std::stable_partition(surfaceFirst, surfaceLast,
				[](const FGLESSceneOrderRecord *record) { return !record->translucent; });
			const auto decalEnd = std::stable_partition(opaqueEnd, surfaceLast,
				[](const FGLESSceneOrderRecord *record) { return record->decal; });
			std::stable_sort(decalEnd, surfaceLast,
				[](const FGLESSceneOrderRecord *left, const FGLESSceneOrderRecord *right)
				{
					return left->sortDepth > right->sortDepth;
				});
		};
		orderSurfaceGroup(first, wallEnd);
		orderSurfaceGroup(wallEnd, last);
	};
	const auto sourceOrderStart = std::stable_partition(included.begin(), nonHudEnd,
		[](const FGLESSceneOrderRecord *record) { return !record->sourceOrdered; });
	const auto nonFloodEnd = std::stable_partition(included.begin(), sourceOrderStart,
		[](const FGLESSceneOrderRecord *record) { return !record->flood; });
	orderFloodGroup(included.begin(), nonFloodEnd);
	orderFloodGroup(nonFloodEnd, sourceOrderStart);
	drawOrder->reserve(included.size());
	for (size_t i = 0; i < included.size(); ++i)
		drawOrder->push_back(included[i]->batchIndex);
	return { drawOrder->empty() ? nullptr : drawOrder->data(), drawOrder->size() };
}

bool gl_GLESInternalSceneCanAppend(size_t currentVertexCount, size_t currentIndexCount,
	size_t vertexCount, size_t indexCount, const char *kind)
{
	const size_t limit = static_cast<size_t>(std::numeric_limits<GLsizei>::max());
	const bool valid = currentVertexCount <= limit && currentIndexCount <= limit &&
		vertexCount <= limit - currentVertexCount && indexCount <= limit - currentIndexCount;
	if (!valid && !SceneCapacityWarningLogged)
	{
		SceneCapacityWarningLogged = true;
		char message[192];
		snprintf(message, sizeof(message),
			"native scene %s exceeds the GLsizei upload range; the batch was rejected",
			kind != nullptr ? kind : "geometry");
		gl_GLES_Report("scene", message);
	}
	return valid;
}

unsigned int gl_GLESInternalSceneUploadGeometry(GLuint vertexArray, GLuint vertexBuffer,
	GLuint indexBuffer, const void *vertices, size_t vertexBytes,
	const void *indices, size_t indexBytes)
{
	if (vertexArray == 0 || vertexBuffer == 0 || indexBuffer == 0 ||
		vertices == nullptr || vertexBytes == 0 || indices == nullptr || indexBytes == 0)
		return 0;
	unsigned int reallocations = 0;
	glBindVertexArray(vertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
	if (vertexBytes > SceneVertexBufferCapacity)
	{
		++reallocations;
		SceneVertexBufferCapacity = GrowBufferCapacity(SceneVertexBufferCapacity, vertexBytes);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(SceneVertexBufferCapacity), nullptr, GL_DYNAMIC_DRAW);
	}
	glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vertexBytes), vertices);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
	if (indexBytes > SceneIndexBufferCapacity)
	{
		++reallocations;
		SceneIndexBufferCapacity = GrowBufferCapacity(SceneIndexBufferCapacity, indexBytes);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(SceneIndexBufferCapacity), nullptr, GL_DYNAMIC_DRAW);
	}
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(indexBytes), indices);
	glBindVertexArray(0);
	if (gl_GLES_IsProfileEnabled())
		gl_GLES_CheckErrors("scene geometry upload");
	return reallocations;
}

void gl_GLESInternalSceneInvalidateBuffers()
{
	SceneVertexBufferCapacity = 0;
	SceneIndexBufferCapacity = 0;
	SceneLightTexture = 0;
	SceneLightTextureWidth = SceneLightTextureHeight = 0;
}

void gl_GLESInternalSceneClearLights()
{
	SceneLightPositionStream.clear();
	SceneLightColorStream.clear();
	SceneLightSelections.clear();
}

bool gl_GLESInternalSceneAppendLights(const float *lightData,
	const unsigned int *lightCounts, FGLESSceneLightSelection *selection)
{
	if (selection == nullptr) return false;
	*selection = {};
	if (lightData == nullptr || lightCounts == nullptr)
	{
		// All empty selections share one stable offset so draw-state caching can
		// recognize the identical zero-light uniform payload.
		selection->streamOffset = 0;
		return true;
	}
	const unsigned int sourceNormalCount = lightCounts[0] / 2;
	const unsigned int sourceSubtractiveCount = lightCounts[1] / 2;
	const unsigned int sourceLightCount = lightCounts[2] / 2;
	const unsigned int normalCount = std::min(sourceNormalCount, sourceLightCount);
	const unsigned int subtractiveCount = std::min(sourceSubtractiveCount, sourceLightCount);
	const unsigned int normalizedNormalCount = std::min(normalCount, subtractiveCount);
	const size_t maxLightCount = std::min(SceneLightPositionStream.max_size(),
		SceneLightColorStream.max_size()) / 4;
	for (size_t cachedIndex = SceneLightSelections.size(); cachedIndex > 0; --cachedIndex)
	{
		const FSceneLightSelectionCacheEntry &cached = SceneLightSelections[cachedIndex - 1];
		const FGLESSceneLightSelection &candidate = cached.selection;
		if (cached.sourceData != lightData || candidate.normalCount != normalizedNormalCount ||
			candidate.subtractiveCount != subtractiveCount || candidate.lightCount != sourceLightCount ||
			!MatchesLightSelection(lightData, candidate))
			continue;
		gl_GLES_RecordProfileLightSelection(true);
		*selection = candidate;
		return true;
	}

	selection->streamOffset = SceneLightPositionStream.size() / 4;
	if (selection->streamOffset > maxLightCount ||
		sourceLightCount > maxLightCount - selection->streamOffset)
	{
		ReportSceneLightFailure(
			"native selected-light stream exceeded addressable storage; this batch was rejected");
		return false;
	}
	selection->normalCount = normalizedNormalCount;
	selection->subtractiveCount = subtractiveCount;
	selection->lightCount = sourceLightCount;
	if (selection->subtractiveCount > selection->lightCount)
		selection->subtractiveCount = selection->lightCount;
	if (selection->normalCount > selection->subtractiveCount)
		selection->normalCount = selection->subtractiveCount;
	gl_GLES_RecordProfileLightSelection(false);
	for (unsigned int light = 0; light < selection->lightCount; ++light)
	{
		for (unsigned int component = 0; component < 4; ++component)
		{
			SceneLightPositionStream.push_back(lightData[light * 8 + component]);
			SceneLightColorStream.push_back(lightData[light * 8 + 4 + component]);
		}
	}
	if (SceneLightSelections.size() >= MaxSceneLightSelections)
		SceneLightSelections.erase(SceneLightSelections.begin());
	SceneLightSelections.push_back({ lightData, *selection });
	return true;
}

bool gl_GLESInternalSceneUploadLights()
{
	const size_t lightCount = SceneLightPositionStream.size() / 4;
	const size_t texelCount = std::max<size_t>(2, lightCount * 2);
	const int maximumSize = gl_GLES_GetContextInfo().maxTextureSize;
	if (maximumSize <= 0) return false;
	const size_t width = std::min<size_t>(maximumSize, 1024);
	const size_t height = (texelCount + width - 1) / width;
	if (maximumSize <= 0 || height > static_cast<size_t>(maximumSize) ||
		lightCount > static_cast<size_t>(std::numeric_limits<GLint>::max()) / 2 ||
		SceneLightColorStream.size() != SceneLightPositionStream.size())
	{
		ReportSceneLightFailure("native light data exceeds the texture address range");
		return false;
	}
	SceneLightTexels.resize(width * height * 4);
	for (size_t light = 0; light < lightCount; ++light)
	{
		memcpy(&SceneLightTexels[light * 8], &SceneLightPositionStream[light * 4], 4 * sizeof(GLfloat));
		memcpy(&SceneLightTexels[light * 8 + 4], &SceneLightColorStream[light * 4], 4 * sizeof(GLfloat));
	}
	glActiveTexture(GL_TEXTURE3);
	if (SceneLightTexture == 0) glGenTextures(1, &SceneLightTexture);
	glBindTexture(GL_TEXTURE_2D, SceneLightTexture);
	glBindSampler(3, 0);
	if (SceneLightTextureWidth != width || SceneLightTextureHeight < height)
	{
		SceneLightTextureWidth = static_cast<GLsizei>(width);
		SceneLightTextureHeight = static_cast<GLsizei>(height);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, SceneLightTextureWidth,
			SceneLightTextureHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
	}
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width),
		static_cast<GLsizei>(height), GL_RGBA, GL_FLOAT, SceneLightTexels.data());
	glActiveTexture(GL_TEXTURE0);
	return gl_GLES_CheckErrors("scene light upload") == GL_NO_ERROR;
}

void gl_GLESInternalSceneDeleteLights()
{
	if (SceneLightTexture != 0) glDeleteTextures(1, &SceneLightTexture);
	SceneLightTexture = 0;
	SceneLightTextureWidth = SceneLightTextureHeight = 0;
}

#endif
