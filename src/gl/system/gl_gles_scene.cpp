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
	std::vector<GLfloat> SceneLightPositionStream;
	std::vector<GLfloat> SceneLightColorStream;
	std::vector<size_t> SceneDrawOrders[2];

	void ReportSceneLightFailure(const char *message)
	{
		if (SceneLightFailureLogged) return;
		SceneLightFailureLogged = true;
		gl_GLES_Report("lights", message);
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

	std::vector<const FGLESSceneOrderRecord *> included;
	included.reserve(recordCount);
	for (size_t i = 0; i < recordCount; ++i)
		if (records[i].included) included.push_back(&records[i]);
	std::stable_sort(included.begin(), included.end(),
		[](const FGLESSceneOrderRecord *left, const FGLESSceneOrderRecord *right)
	{
		if (left->hud != right->hud) return !left->hud;
		if (left->hud) return left->batchIndex < right->batchIndex;
		if (left->flood != right->flood) return !left->flood;
		if (left->flat != right->flat) return !left->flat;
		if (left->translucent != right->translucent) return !left->translucent;
		if (left->translucent && left->sortDepth != right->sortDepth)
			return left->sortDepth > right->sortDepth;
		return left->batchIndex < right->batchIndex;
	});
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

void gl_GLESInternalSceneUploadGeometry(GLuint vertexArray, GLuint vertexBuffer,
	GLuint indexBuffer, const void *vertices, size_t vertexBytes,
	const GLuint *indices, size_t indexCount)
{
	if (vertexArray == 0 || vertexBuffer == 0 || indexBuffer == 0 ||
		vertices == nullptr || vertexBytes == 0 || indices == nullptr || indexCount == 0)
		return;
	glBindVertexArray(vertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
	glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertexBytes), vertices, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
		static_cast<GLsizeiptr>(indexCount * sizeof(GLuint)), indices, GL_DYNAMIC_DRAW);
	glBindVertexArray(0);
	gl_GLES_CheckErrors("scene geometry upload");
}

void gl_GLESInternalSceneClearLights()
{
	SceneLightPositionStream.clear();
	SceneLightColorStream.clear();
}

bool gl_GLESInternalSceneAppendLights(const float *lightData,
	const unsigned int *lightCounts, FGLESSceneLightSelection *selection)
{
	if (selection == nullptr) return false;
	*selection = {};
	selection->streamOffset = SceneLightPositionStream.size() / 4;
	if (lightData == nullptr || lightCounts == nullptr) return true;

	const unsigned int sourceNormalCount = lightCounts[0] / 2;
	const unsigned int sourceSubtractiveCount = lightCounts[1] / 2;
	const unsigned int sourceLightCount = lightCounts[2] / 2;
	const size_t maxLightCount = std::min(SceneLightPositionStream.max_size(),
		SceneLightColorStream.max_size()) / 4;
	if (selection->streamOffset > maxLightCount ||
		sourceLightCount > maxLightCount - selection->streamOffset)
	{
		ReportSceneLightFailure(
			"native selected-light stream exceeded addressable storage; this batch was rejected");
		return false;
	}
	selection->normalCount = std::min(sourceNormalCount, sourceLightCount);
	selection->subtractiveCount = std::min(sourceSubtractiveCount, sourceLightCount);
	selection->lightCount = sourceLightCount;
	if (selection->subtractiveCount > selection->lightCount)
		selection->subtractiveCount = selection->lightCount;
	if (selection->normalCount > selection->subtractiveCount)
		selection->normalCount = selection->subtractiveCount;
	for (unsigned int light = 0; light < selection->lightCount; ++light)
	{
		for (unsigned int component = 0; component < 4; ++component)
		{
			SceneLightPositionStream.push_back(lightData[light * 8 + component]);
			SceneLightColorStream.push_back(lightData[light * 8 + 4 + component]);
		}
	}
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
	const GLint lightOnlyMode = glGetUniformLocation(pass.program, "u_light_only_mode");
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

	GLint depthFunction = GL_LESS;
	GLboolean depthWrite = GL_TRUE;
	glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
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
			glDrawElements(GL_TRIANGLES, pass.indexCount, GL_UNSIGNED_INT,
				reinterpret_cast<const void *>(pass.firstIndex * sizeof(GLuint)));
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
