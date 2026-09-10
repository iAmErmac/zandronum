#ifndef ZANDRONUM_GL_ANDROID_H
#define ZANDRONUM_GL_ANDROID_H

#include "v_palette.h"

class FMaterial;

struct FAndroidGLESInfo
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

enum EAndroidNativeBlendMode
{
	ANDROID_BLEND_OPAQUE,
	ANDROID_BLEND_ALPHA,
	ANDROID_BLEND_MULTIPLY,
	ANDROID_BLEND_ADD,
	ANDROID_BLEND_SUBTRACT,
	ANDROID_BLEND_REVERSE_SUBTRACT,
	ANDROID_BLEND_FUZZ
};

enum EAndroidNativeMaterialFlags
{
	ANDROID_MATERIAL_RED_IS_ALPHA = 1,
	ANDROID_MATERIAL_INVERT = 2,
	ANDROID_MATERIAL_FADE_TO_BLACK = 4,
	ANDROID_MATERIAL_FUZZ = 8,
	ANDROID_MATERIAL_COLOR_OVERLAY = 16,
	ANDROID_MATERIAL_INVERT_SOURCE = 32,
	ANDROID_MATERIAL_COLOR_FIXED = 64
};

#ifdef __ANDROID__
bool gl_AndroidNativeGLES_CollectCapabilities();
bool gl_AndroidNativeGLES_InitializeBootstrap(int width, int height);
void gl_AndroidNativeGLES_RenderBootstrap(int width, int height);
void gl_AndroidNativeGLES_BeginScene(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, float fieldOfView, float aspect, float fovRatio);
void gl_AndroidNativeGLES_ClearScene();
void gl_AndroidNativeGLES_SetFlatCollectionDeferred(bool deferred);
bool gl_AndroidNativeGLES_IsFlatCollectionDeferred();
void gl_AndroidNativeGLES_ClearFlatTasks();
void gl_AndroidNativeGLES_EmitFlatTasks();
void gl_AndroidNativeGLES_SetSky(FMaterial *material, float xOffset, float yOffset, bool mirrored,
	bool sky2, PalEntry fadeColor);
void gl_AndroidNativeGLES_SetSkyLayer(FMaterial *material, float xOffset, float yOffset, bool mirrored);
void gl_AndroidNativeGLES_AddSkyMask(const float *positions);
void gl_AndroidNativeGLES_AddWall(const float *positions, const float *texcoords,
	const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode,
	unsigned int materialFlags = 0, const float *lightData = 0, const unsigned int *lightCounts = 0,
	unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_AndroidNativeGLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode,
	unsigned int materialFlags = 0, const float *lightData = 0, const unsigned int *lightCounts = 0,
	unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_AndroidNativeGLES_AddFloodPlane(const float *wallPositions, const float *planePositions,
	const float *texcoords, const float *color, unsigned int texture, bool fog,
	const float *fogColor, float fogDensity);
void gl_AndroidNativeGLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode,
	unsigned int materialFlags = 0, unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_AndroidNativeGLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode,
	unsigned int materialFlags = 0, const float *normals = 0, unsigned int brightmap = 0,
	int brightmapDesaturation = 0, bool cullBackFaces = false);
void gl_AndroidNativeGLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EAndroidNativeBlendMode blendMode, unsigned int materialFlags = 0);
void gl_AndroidNativeGLES_AddHUDPolygon(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EAndroidNativeBlendMode blendMode, unsigned int materialFlags = 0);
void gl_AndroidNativeGLES_AddScreenQuad(const float *color, float alpha,
	EAndroidNativeBlendMode blendMode);
unsigned int gl_AndroidNativeGLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires);
void gl_AndroidNativeGLES_ClearMaterialCache();
void gl_AndroidNativeGLES_EndScene();
unsigned int gl_AndroidNativeGLES_BeginPortalCapture();
void gl_AndroidNativeGLES_AddPortalMask(unsigned int portalId, const float *positions);
void gl_AndroidNativeGLES_SetPortalView(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, bool mirrored, bool planeMirrored);
void gl_AndroidNativeGLES_SetPortalClipPlane(float a, float b, float c, float d);
void gl_AndroidNativeGLES_EndPortalCapture(unsigned int portalId);
bool gl_AndroidNativeGLES_IsActive();
bool gl_AndroidNativeGLES_CanUseResources();
void gl_AndroidNativeGLES_OnContextLost();
bool gl_AndroidNativeGLES_OnContextRestored(int width, int height);
const FAndroidGLESInfo &gl_AndroidNativeGLES_GetCapabilities();
void gl_AndroidNativeGLES_PrintStartupLog();
void gl_AndroidNativeGLES_ResetState(int width, int height);
bool gl_AndroidNativeGLES_ApplyRenderState(int srcBlend, int dstBlend, int alphaFunc,
	float alphaThreshold, bool alphaTest, int blendEquation, bool fogEnabled,
	bool textureEnabled, int textureMode);
void gl_AndroidNativeGLES_RegisterShaderPrograms();
void gl_AndroidNativeGLES_UnregisterShaderPrograms();
unsigned int gl_AndroidNativeGLES_GetShaderProgram(const char *name);
void gl_AndroidNativeGLES_UseProgram(unsigned int handle);
unsigned int gl_AndroidNativeGLES_BindShaderProgram(const char *name, unsigned int fallback);
void gl_AndroidNativeGLES_InvalidateTextures();
void gl_AndroidNativeGLES_InvalidateFlatBuffers();
void gl_AndroidNativeGLES_GenerateMipmap();
bool gl_AndroidNativeGLES_CreateFlatBufferObjects(unsigned int *vbo, unsigned int *vao, unsigned int *ebo);
bool gl_AndroidNativeGLES_UploadFlatBuffer(unsigned int vbo, unsigned int vao, unsigned int ebo,
	const void *vertices, int vertexCount, int vertexStride);
bool gl_AndroidNativeGLES_UpdateFlatBuffer(unsigned int vbo, int offset, int size, const void *vertices);
void gl_AndroidNativeGLES_DestroyFlatBufferObjects(unsigned int vbo, unsigned int vao, unsigned int ebo);
void gl_AndroidNativeGLES_BindFlatBuffer(unsigned int vao, unsigned int vbo);
#else
inline bool gl_AndroidNativeGLES_IsActive() { return false; }
#endif

#endif
