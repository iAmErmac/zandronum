#ifndef ZANDRONUM_GL_GLES_SCENE_H
#define ZANDRONUM_GL_GLES_SCENE_H

#include <stddef.h>

#include "gl/system/gl_gles_api.h"

void gl_GLESInternalSceneMarkProjectedLight(float *light, unsigned int order, unsigned int kind);
void gl_GLESInternalSceneOrderProjectedLights(float *lights, unsigned int count);

struct FGLESSceneOrderRecord
{
	size_t batchIndex;
	bool included;
	bool sourceOrdered;
	bool hud;
	bool flood;
	bool flat;
	bool opaqueMasked;
	bool translucent;
	bool decal;
	float sortDepth;
};

enum FGLESSceneOrderSlot
{
	GLES_SCENE_ORDER_PORTAL,
	GLES_SCENE_ORDER_VIEW
};

struct FGLESSceneDrawOrderView
{
	const size_t *indices;
	size_t count;
};

struct FGLESSceneLightSelection
{
	size_t streamOffset;
	unsigned int lightCount;
	unsigned int normalCount;
	unsigned int subtractiveCount;
};

FGLESSceneDrawOrderView gl_GLESInternalSceneSortBatches(const FGLESSceneOrderRecord *records,
	size_t recordCount, FGLESSceneOrderSlot slot);
bool gl_GLESInternalSceneCanAppend(size_t currentVertexCount, size_t currentIndexCount,
	size_t vertexCount, size_t indexCount, const char *kind);
unsigned int gl_GLESInternalSceneUploadGeometry(GLuint vertexArray, GLuint vertexBuffer,
	GLuint indexBuffer, const void *vertices, size_t vertexBytes,
	const void *indices, size_t indexBytes);
void gl_GLESInternalSceneInvalidateBuffers();
void gl_GLESInternalSceneClearLights();
bool gl_GLESInternalSceneAppendLights(const float *lightData,
	const unsigned int *lightCounts, FGLESSceneLightSelection *selection);
bool gl_GLESInternalSceneUploadLights();
void gl_GLESInternalSceneDeleteLights();

#endif
