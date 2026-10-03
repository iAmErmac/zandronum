#ifndef ZANDRONUM_GL_GLES_INTERNAL_H
#define ZANDRONUM_GL_GLES_INTERNAL_H

#include "gl/system/gl_gles_targets.h"

struct FGLESWipeBindings
{
	GLuint startTexture;
	GLuint endTexture;
	GLuint maskTexture;
	int type;
	int simulatedTicks;
	float progress;
	bool active;
	bool startReady;
	bool endReady;
};

bool gl_GLESInternalWipeStart(int type, const FGLESTargetDescriptor &target);
void gl_GLESInternalWipeEnd();
bool gl_GLESInternalWipeDo(int ticks);
void gl_GLESInternalWipeCleanup();
void gl_GLESInternalWipeDestroy();
void gl_GLESInternalWipeContextLost();
bool gl_GLESInternalWipeCaptureEndFrame(const FGLESTargetDescriptor &target);
bool gl_GLESInternalWipeIsActive();
FGLESWipeBindings gl_GLESInternalWipeGetBindings();

void gl_GLESInternalResetState(int width, int height);
void gl_GLESInternalInvalidateProgramBinding();
void gl_GLESInternalInvalidateStateCache();
void gl_GLESInternalStateContextLost();
bool gl_GLESInternalApplyBlendState(bool resourcesAvailable, int srcBlend, int dstBlend, int blendEquation);

GLuint gl_GLESInternalFindMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires, int width, int height);
GLuint gl_GLESInternalFindStaticMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires);
GLuint gl_GLESInternalBindMaterial(bool resourcesAvailable, const void *key,
	const unsigned char *pixels, int width, int height, bool repeat, int colormap,
	int translation, bool allowhires, bool palette, bool noFilter = false);
enum
{
	GLES_TEXTURE_FLAG_PALETTE = 1u << 0,
	GLES_TEXTURE_FLAG_FRAMEBUFFER = 1u << 1,
	GLES_TEXTURE_FLAG_NOFILTER = 1u << 2
};
unsigned int gl_GLESInternalGetMaterialFlags(GLuint texture);
struct FGLESMaterialEffect
{
	int shaderIndex;
	float speed;
	int colormap;
};
void gl_GLESInternalSetMaterialEffect(GLuint texture, int shaderIndex, float speed, int colormap);
FGLESMaterialEffect gl_GLESInternalGetMaterialEffect(GLuint texture);
bool gl_GLESInternalIsPaletteTexture(GLuint texture);
void gl_GLESInternalMarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires);
void gl_GLESInternalDeleteMaterialTextures();
void gl_GLESInternalInvalidateMaterials();
void gl_GLESInternalClearMaterials(bool contextAvailable);
void gl_GLES_RecordProfileMaterial(bool hit, bool upload);
void gl_GLES_RecordProfileState(bool skipped);
void gl_GLES_RecordProfilePortalState(bool skipped);

#endif
