#ifndef ZANDRONUM_GL_GLES_RENDERER_H
#define ZANDRONUM_GL_GLES_RENDERER_H

#include <stdio.h>

#include "v_palette.h"

class FMaterial;
struct FShaderLightParameters;

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
	GLES_MATERIAL_INVERT_TEXTURE = 2,
	GLES_MATERIAL_OPAQUE_TEXTURE = 4,
	GLES_MATERIAL_FUZZ = 8,
	GLES_MATERIAL_COLOR_OVERLAY = 16,
	GLES_MATERIAL_COLOR_FIXED = 64,
	GLES_MATERIAL_DECAL = 128,
	GLES_MATERIAL_FOG_BOUNDARY = 256,
	GLES_MATERIAL_SPHERE_MAP = 512,
	GLES_MATERIAL_FUZZ_SHIFT = 10,
	GLES_MATERIAL_RADIAL_FOG = 8192,
	GLES_MATERIAL_CLAMP_X = 16384,
	GLES_MATERIAL_CLAMP_Y = 32768,
	GLES_MATERIAL_GLOW = 65536,
	GLES_MATERIAL_MIRROR_DECAL = 131072,
	GLES_MATERIAL_SPRITE_FOG_LAYER = 262144
};

enum EGLESPrimitiveMode
{
	GLES_PRIMITIVE_TRIANGLE_FAN,
	GLES_PRIMITIVE_LINES,
	GLES_PRIMITIVE_POINTS
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
	const float *bottomGlowColor = 0, const float *glowDistances = 0,
	const FShaderLightParameters *lighting = 0, float alphaCutoff = 0.5f, bool customBlend = false,
	int sourceBlend = 0, int destinationBlend = 0);
void gl_GLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, const float *lightData = 0, const unsigned int *lightCounts = 0,
	unsigned int brightmap = 0, int brightmapDesaturation = 0,
	const FShaderLightParameters *lighting = 0);
void gl_GLES_AddFloodPlane(const float *wallPositions, const float *planePositions,
	const float *texcoords, const float *color, unsigned int texture, bool fog,
	const float *fogColor, float fogDensity, const FShaderLightParameters *lighting = 0);
void gl_GLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, unsigned int brightmap = 0, int brightmapDesaturation = 0,
	bool customBlend = false, int sourceBlend = 0, int destinationBlend = 0, float alphaCutoff = 0.5f,
	const FShaderLightParameters *lighting = 0);
void gl_GLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode,
	unsigned int materialFlags = 0, const float *normals = 0, unsigned int brightmap = 0,
	int brightmapDesaturation = 0, bool cullBackFaces = false, bool customBlend = false,
	int sourceBlend = 0, int destinationBlend = 0,
	const FShaderLightParameters *lighting = 0, float alphaCutoff = 0.5f);
void gl_GLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EGLESBlendMode blendMode, unsigned int materialFlags = 0, bool customBlend = false,
	int sourceBlend = 0, int destinationBlend = 0, float alphaCutoff = 0.5f,
	unsigned int brightmap = 0, int brightmapDesaturation = 0);
void gl_GLES_AddHUDPrimitive(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EGLESBlendMode blendMode, unsigned int materialFlags = 0,
	EGLESPrimitiveMode primitiveMode = GLES_PRIMITIVE_TRIANGLE_FAN);
void gl_GLES_AddScreenQuad(const float *color, float alpha, EGLESBlendMode blendMode);
unsigned int gl_GLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires, bool noFilter = false, bool noCompression = false);
unsigned int gl_GLES_GetTextureFormat(bool fullPrecision);
unsigned int gl_GLES_EnsureMaterialTexture(const void *key, int width, int height,
	bool repeat, int colormap, int translation, bool allowhires);
void gl_GLES_MarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires);
bool gl_GLES_EndSceneToTexture(unsigned int targetTexture, int width, int height);
void gl_GLES_ClearMaterialCache();
void gl_GLES_EndScene();
bool gl_GLES_ReadScreenshot(unsigned char *rgba, int width, int height, int trueHeight);
bool gl_GLES_WriteSavePic(FILE *file, int width, int height);
unsigned int gl_GLES_BeginPortalCapture();
bool gl_GLES_ClearPortalCapture();
void gl_GLES_AddPortalMask(unsigned int portalId, const float *positions);
void gl_GLES_SetPortalView(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, bool mirrored, bool planeMirrored);
void gl_GLES_SetPortalClipPlane(float a, float b, float c, float d);
void gl_GLES_EndPortalCapture(unsigned int portalId);
bool gl_GLES_IsActive();
bool gl_GLES_IsProfileEnabled();
void gl_GLES_DesktopProfileBegin();
void gl_GLES_DesktopProfileEnd();
bool gl_GLES_CanUseResources();
void gl_GLES_OnContextLost();
bool gl_GLES_OnContextRestored(int width, int height);
const FGLESNativeCapabilities &gl_GLES_GetCapabilities();
void gl_GLES_PrintStartupLog();
void gl_GLES_ResetState(int width, int height);
bool gl_GLES_ApplyBlendState(int srcBlend, int dstBlend, int blendEquation);
void gl_GLES_RegisterShaderPrograms();
void gl_GLES_UnregisterShaderPrograms();
unsigned int gl_GLES_GetShaderProgram(const char *name);
void gl_GLES_UseProgram(unsigned int handle);
unsigned int gl_GLES_BindShaderProgram(const char *name, unsigned int fallback);
void gl_GLES_RecordProfileDraw(bool triangleStrip, int indexCount);
void gl_GLES_RecordProfileState(bool skipped);
void gl_GLES_RecordProfilePortalState(bool skipped);
void gl_GLES_RecordProfilePortalSkyUpload(bool skipped);
void gl_GLES_RecordProfileMaterial(bool hit, bool upload);
void gl_GLES_RecordProfileLightSelection(bool reused);
void gl_GLES_RecordProfileResolve();
void gl_GLES_RecordProfileReadback();
void gl_GLES_RecordProfileWipeCapture();
void gl_GLES_InvalidateTextures();
void gl_GLES_InvalidateFlatBuffers();
void gl_GLES_GenerateMipmap();
bool gl_GLES_CreateFlatBufferObjects(unsigned int *vbo, unsigned int *vao, unsigned int *ebo);
bool gl_GLES_UploadFlatBuffer(unsigned int vbo, unsigned int vao, unsigned int ebo,
	const void *vertices, int vertexCount, int vertexStride);
bool gl_GLES_UpdateFlatBuffer(unsigned int vbo, int offset, int size, const void *vertices);
void gl_GLES_DestroyFlatBufferObjects(unsigned int vbo, unsigned int vao, unsigned int ebo);
void gl_GLES_BindFlatBuffer(unsigned int vao, unsigned int vbo);

void gl_GLES_SetSourceOrder(bool enabled);

#endif
