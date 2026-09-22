#ifndef ZANDRONUM_GL_GLES_SCENE_H
#define ZANDRONUM_GL_GLES_SCENE_H

#include <stddef.h>

#include "gl/system/gl_gles_api.h"

enum { GLES_MAX_LIGHTS = 32 };

struct FGLESSceneOrderRecord
{
	size_t batchIndex;
	bool included;
	bool hud;
	bool flood;
	bool flat;
	bool translucent;
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

struct FGLESSceneLightUpload
{
	const GLfloat *positions;
	const GLfloat *colors;
	unsigned int lightCount;
	unsigned int normalCount;
	unsigned int subtractiveCount;
};

struct FGLESSceneLightPass
{
	FGLESSceneLightSelection selection;
	float planeNormal[3];
	GLuint dynamicLightTexture;
	GLuint checkerTexture;
	GLuint program;
	GLint positionUniform;
	GLint colorUniform;
	GLint countsUniform;
	GLint lightOnlyUniform;
	GLint projectedUniform;
	GLint depthFunction;
	bool depthStateKnown;
	bool depthWrite;
	GLsizei indexCount;
	size_t firstIndex;
	GLenum indexType;
	size_t indexStride;
	bool translucent;
	int blendMode;
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
void gl_GLESInternalSceneGetLightUpload(const FGLESSceneLightSelection &selection,
	unsigned int firstLight, unsigned int maxLightCount, FGLESSceneLightUpload *upload);
void gl_GLESInternalSceneDrawLightOverflow(const FGLESSceneLightPass &pass);

#endif
