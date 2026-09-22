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
			(records[i].hud || records[i].flood || records[i].translucent))
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
			std::stable_sort(opaqueEnd, surfaceLast,
				[](const FGLESSceneOrderRecord *left, const FGLESSceneOrderRecord *right)
				{
					return left->sortDepth > right->sortDepth;
				});
		};
		orderSurfaceGroup(first, wallEnd);
		orderSurfaceGroup(wallEnd, last);
	};
	const auto nonFloodEnd = std::stable_partition(included.begin(), nonHudEnd,
		[](const FGLESSceneOrderRecord *record) { return !record->flood; });
	orderFloodGroup(included.begin(), nonFloodEnd);
	orderFloodGroup(nonFloodEnd, nonHudEnd);
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

void gl_GLESInternalSceneGetLightUpload(const FGLESSceneLightSelection &selection,
	unsigned int firstLight, unsigned int maxLightCount, FGLESSceneLightUpload *upload)
{
	if (upload == nullptr) return;
	*upload = {};
	const size_t storedLightCount = std::min(SceneLightPositionStream.size(),
		SceneLightColorStream.size()) / 4;
	if (selection.streamOffset > storedLightCount || firstLight > selection.lightCount)
	{
		ReportSceneLightFailure("native selected-light batch range is outside the frame stream");
		return;
	}
	const size_t batchOffset = selection.streamOffset + firstLight;
	const size_t available = storedLightCount - batchOffset;
	const size_t remaining = selection.lightCount - firstLight;
	const size_t requested = std::min<size_t>(remaining, maxLightCount);
	upload->lightCount = static_cast<unsigned int>(std::min<size_t>(
		std::min<size_t>(requested, GLES_MAX_LIGHTS), available));
	if (available < remaining)
		ReportSceneLightFailure("native selected-light stream is shorter than a batch range; missing lights were omitted");
	upload->normalCount = std::min(selection.normalCount, firstLight + upload->lightCount);
	upload->normalCount = upload->normalCount > firstLight ? upload->normalCount - firstLight : 0;
	upload->subtractiveCount = std::min(selection.subtractiveCount,
		firstLight + upload->lightCount);
	upload->subtractiveCount = upload->subtractiveCount > firstLight ?
		upload->subtractiveCount - firstLight : 0;
	if (upload->lightCount > 0)
	{
		const size_t floatOffset = batchOffset * 4;
		upload->positions = &SceneLightPositionStream[floatOffset];
		upload->colors = &SceneLightColorStream[floatOffset];
	}
}

void gl_GLESInternalSceneDrawLightOverflow(const FGLESSceneLightPass &pass)
{
	if (pass.selection.lightCount <= GLES_MAX_LIGHTS) return;
	const GLint lightOnlyMode = pass.lightOnlyUniform;
	if (lightOnlyMode < 0)
	{
		ReportSceneLightFailure("native light-overflow shader is missing its light-only control");
		return;
	}
	if (pass.positionUniform < 0 || pass.colorUniform < 0 || pass.countsUniform < 0)
	{
		ReportSceneLightFailure("native light-overflow shader is missing a required light uniform");
		return;
	}

	GLint depthFunction = pass.depthFunction;
	GLboolean depthWrite = pass.depthWrite ? GL_TRUE : GL_FALSE;
	if (!pass.depthStateKnown)
	{
		glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
		glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
	}
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glDepthMask(GL_FALSE);
	glDepthFunc(depthWrite ? GL_EQUAL : depthFunction);

	const auto drawRange = [&](unsigned int first, unsigned int end, GLint mode, GLenum equation)
	{
		while (first < end)
		{
			FGLESSceneLightUpload upload = {};
			gl_GLESInternalSceneGetLightUpload(pass.selection, first, end - first, &upload);
			if (upload.lightCount == 0) break;
			glUniform4fv(pass.positionUniform, upload.lightCount, upload.positions);
			glUniform4fv(pass.colorUniform, upload.lightCount, upload.colors);
			glUniform3i(pass.countsUniform, static_cast<GLint>(upload.normalCount),
				static_cast<GLint>(upload.subtractiveCount),
				static_cast<GLint>(upload.lightCount));
			glUniform1i(lightOnlyMode, mode);
			const bool projected = pass.dynamicLightTexture != 0 &&
				(pass.planeNormal[0] != 0.0f || pass.planeNormal[1] != 0.0f ||
				pass.planeNormal[2] != 0.0f);
			if (pass.projectedUniform >= 0)
				glUniform1i(pass.projectedUniform, projected ? 1 : 0);
			glActiveTexture(GL_TEXTURE2);
			glBindTexture(GL_TEXTURE_2D,
				projected ? pass.dynamicLightTexture : pass.checkerTexture);
			glActiveTexture(GL_TEXTURE0);
			glBlendEquation(equation);
			gl_GLES_RecordProfileOverflow();
			gl_GLES_RecordProfileDraw(false, pass.indexCount);
			const GLenum indexType = pass.indexType != 0 ? pass.indexType : GL_UNSIGNED_INT;
			const size_t indexStride = pass.indexStride != 0 ? pass.indexStride : sizeof(GLuint);
			glDrawElements(GL_TRIANGLES, pass.indexCount, indexType,
				reinterpret_cast<const void *>(pass.firstIndex * indexStride));
			first += upload.lightCount;
		}
	};

	const unsigned int normalEnd = std::min(pass.selection.normalCount,
		pass.selection.lightCount);
	const unsigned int subtractiveEnd = std::min(pass.selection.subtractiveCount,
		pass.selection.lightCount);
	const unsigned int firstOverflow = GLES_MAX_LIGHTS;
	drawRange(firstOverflow, normalEnd, 1, GL_FUNC_ADD);
	drawRange(std::max(firstOverflow, normalEnd), subtractiveEnd, 2,
		GL_FUNC_REVERSE_SUBTRACT);
	drawRange(std::max(firstOverflow, subtractiveEnd), pass.selection.lightCount, 3,
		GL_FUNC_ADD);

	glUniform1i(lightOnlyMode, 0);
	glDepthFunc(depthFunction);
	glDepthMask(depthWrite);
	if (pass.translucent)
	{
		glEnable(GL_BLEND);
		GLenum equation = GL_FUNC_ADD;
		if (pass.blendMode == GLES_BLEND_SUBTRACT) equation = GL_FUNC_SUBTRACT;
		else if (pass.blendMode == GLES_BLEND_REVERSE_SUBTRACT)
			equation = GL_FUNC_REVERSE_SUBTRACT;
		glBlendEquation(equation);
		const bool additive = pass.blendMode == GLES_BLEND_ADD ||
			pass.blendMode == GLES_BLEND_SUBTRACT ||
			pass.blendMode == GLES_BLEND_REVERSE_SUBTRACT;
		if (pass.blendMode == GLES_BLEND_FUZZ)
			glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
		else if (pass.blendMode == GLES_BLEND_MULTIPLY)
			glBlendFunc(GL_DST_COLOR, GL_ZERO);
		else glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		glDisable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
	}
}

#endif
