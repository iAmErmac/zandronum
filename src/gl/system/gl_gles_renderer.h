#ifndef ZANDRONUM_GL_GLES_RENDERER_H
#define ZANDRONUM_GL_GLES_RENDERER_H

#include <stdio.h>

#include "v_palette.h"

class FMaterial;

struct FGLESNativeCapabilities
{
	int majorVersion;
	int minorVersion;
	int maxTextureSize;
	int maxTextureUnits;
	int maxVertexUniformVectors;
	int maxFragmentUniformVectors;
	int extensionCount;
	const char *vendor;
	const char *renderer;
	const char *version;
	const char *shadingLanguageVersion;
	bool hasVertexBuffers;
	bool hasVertexArrays;
	bool hasUniformBuffers;
	bool hasFramebuffers;
	bool hasDepthStencil;
	bool hasBufferMapping;
	bool hasAnisotropicFiltering;
	bool hasAstcCompression;
	bool hasEtc2Compression;
	bool hasDebugLabels;
	bool hasMultiview;
};

enum EGLESBlendMode
{
	GLES_BLEND_OPAQUE,
	GLES_BLEND_ALPHA,
	GLES_BLEND_MULTIPLY,
	GLES_BLEND_ADD,
	GLES_BLEND_SUBTRACT,
	GLES_BLEND_REVERSE_SUBTRACT,
	GLES_BLEND_FUZZ
};

enum EGLESMaterialFlags
{
	GLES_MATERIAL_RED_IS_ALPHA = 1,
	GLES_MATERIAL_INVERT = 2,
	GLES_MATERIAL_FADE_TO_BLACK = 4,
	GLES_MATERIAL_FUZZ = 8,
	GLES_MATERIAL_COLOR_OVERLAY = 16,
	GLES_MATERIAL_INVERT_SOURCE = 32,
	GLES_MATERIAL_COLOR_FIXED = 64,
	GLES_MATERIAL_DECAL = 128
};

bool gl_GLES_CollectCapabilities();
bool gl_GLES_InitializeBootstrap(int width, int height);
void gl_GLES_RenderBootstrap(int width, int height);
void gl_GLES_RecordHostPresentation(double waitMilliseconds, double presentMilliseconds,
	bool presented, int configuredLimit, int capFPS, int displayLimit, int effectiveLimit);
bool gl_GLES_WipeStart(int type);
void gl_GLES_WipeEnd();
bool gl_GLES_WipeDo(int ticks);
void gl_GLES_WipeCleanup();
bool gl_GLES_IsWipeInProgress();
void gl_GLES_BeginWipeOverlay();
void gl_GLES_EndWipeOverlay();
void gl_GLES_BeginScene(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, float fieldOfView, float aspect, float fovRatio);
void gl_GLES_ClearScene();
void gl_GLES_SetFlatCollectionDeferred(bool deferred);
bool gl_GLES_IsFlatCollectionDeferred();
void gl_GLES_ClearFlatTasks();
void gl_GLES_EmitFlatTasks();
void gl_GLES_SetSky(FMaterial *material, float xOffset, float yOffset, bool mirrored,
	bool sky2, PalEntry fadeColor);
void gl_GLES_SetSkyLayer(FMaterial *material, float xOffset, float yOffset, bool mirrored);
void gl_GLES_AddSkyMask(const float *positions);
void gl_GLES_AddWall(const float *positions, const float *texcoords,
	const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, const float *lightData = 0, const unsigned int *lightCounts = 0,
	unsigned int brightmap = 0, int brightmapDesaturation = 0, const float *topGlowColor = 0,
	const float *bottomGlowColor = 0, const float *glowDistances = 0);
void gl_GLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, const float *lightData = 0, const unsigned int *lightCounts = 0,
	unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_GLES_AddFloodPlane(const float *wallPositions, const float *planePositions,
	const float *texcoords, const float *color, unsigned int texture, bool fog,
	const float *fogColor, float fogDensity);
void gl_GLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_GLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, const float *normals = 0, unsigned int brightmap = 0,
	int brightmapDesaturation = 0, bool cullBackFaces = false);
void gl_GLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EGLESBlendMode blendMode, unsigned int materialFlags = 0);
void gl_GLES_AddHUDPolygon(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EGLESBlendMode blendMode, unsigned int materialFlags = 0);
void gl_GLES_AddScreenQuad(const float *color, float alpha, EGLESBlendMode blendMode);
unsigned int gl_GLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires);
unsigned int gl_GLES_EnsureMaterialTexture(const void *key, int width, int height,
	bool repeat, int colormap, int translation, bool allowhires);
void gl_GLES_MarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires);
bool gl_GLES_EndSceneToTexture(unsigned int targetTexture, int width, int height);
void gl_GLES_ClearMaterialCache();
void gl_GLES_EndScene();
bool gl_GLES_ReadScreenshot(unsigned char *rgba, int width, int height);
bool gl_GLES_WriteSavePic(FILE *file, int width, int height);
unsigned int gl_GLES_BeginPortalCapture();
bool gl_GLES_ClearPortalCapture();
void gl_GLES_AddPortalMask(unsigned int portalId, const float *positions);
void gl_GLES_SetPortalView(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, bool mirrored, bool planeMirrored);
void gl_GLES_SetPortalClipPlane(float a, float b, float c, float d);
void gl_GLES_EndPortalCapture(unsigned int portalId);
bool gl_GLES_IsActive();
bool gl_GLES_CanUseResources();
void gl_GLES_OnContextLost();
bool gl_GLES_OnContextRestored(int width, int height);
const FGLESNativeCapabilities &gl_GLES_GetCapabilities();
void gl_GLES_PrintStartupLog();
void gl_GLES_ResetState(int width, int height);
bool gl_GLES_ApplyRenderState(int srcBlend, int dstBlend, int alphaFunc,
	float alphaThreshold, bool alphaTest, int blendEquation, bool fogEnabled,
	bool textureEnabled, int textureMode);
void gl_GLES_RegisterShaderPrograms();
void gl_GLES_UnregisterShaderPrograms();
unsigned int gl_GLES_GetShaderProgram(const char *name);
void gl_GLES_UseProgram(unsigned int handle);
unsigned int gl_GLES_BindShaderProgram(const char *name, unsigned int fallback);
void gl_GLES_InvalidateTextures();
void gl_GLES_InvalidateFlatBuffers();
void gl_GLES_GenerateMipmap();
bool gl_GLES_CreateFlatBufferObjects(unsigned int *vbo, unsigned int *vao, unsigned int *ebo);
bool gl_GLES_UploadFlatBuffer(unsigned int vbo, unsigned int vao, unsigned int ebo,
	const void *vertices, int vertexCount, int vertexStride);
bool gl_GLES_UpdateFlatBuffer(unsigned int vbo, int offset, int size, const void *vertices);
void gl_GLES_DestroyFlatBufferObjects(unsigned int vbo, unsigned int vao, unsigned int ebo);
void gl_GLES_BindFlatBuffer(unsigned int vao, unsigned int vbo);

#endif
