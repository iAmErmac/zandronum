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
void gl_GLESInternalStateContextLost();
bool gl_GLESInternalApplyRenderState(bool resourcesAvailable, int srcBlend, int dstBlend,
	int alphaFunc, float alphaThreshold, bool alphaTest, int blendEquation,
	bool fogEnabled, bool textureEnabled, int textureMode);

GLuint gl_GLESInternalFindMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires, int width, int height);
GLuint gl_GLESInternalBindMaterial(bool resourcesAvailable, const void *key,
	const unsigned char *pixels, int width, int height, bool repeat, int colormap,
	int translation, bool allowhires, bool palette);
bool gl_GLESInternalIsPaletteTexture(GLuint texture);
void gl_GLESInternalMarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires);
void gl_GLESInternalDeleteMaterialTextures();
void gl_GLESInternalInvalidateMaterials();
void gl_GLESInternalClearMaterials(bool contextAvailable);

#endif
