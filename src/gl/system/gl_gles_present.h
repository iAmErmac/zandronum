#ifndef ZANDRONUM_GL_GLES_PRESENT_H
#define ZANDRONUM_GL_GLES_PRESENT_H

#include "gl/system/gl_gles_internal.h"

#include <stdio.h>

struct FGLESPresentConfig
{
	FGLESTargetDescriptor target;
	FGLESTargetDescriptor presentationTarget;
	FGLESWipeBindings wipe;
	GLuint sceneSampler;
	int stateWidth;
	int stateHeight;
	float gamma;
	float brightness;
	float contrast;
};

struct FGLESViewArea
{
	int x;
	int y;
	int width;
	int height;
	int renderX;
	int renderY;
	int renderWidth;
	int renderHeight;
	int scissorX;
	int scissorY;
	int scissorWidth;
	int scissorHeight;
	bool valid;
};

bool gl_GLESInternalPresentInitialize();
void gl_GLESInternalPresentDestroy();
void gl_GLESInternalPresentContextLost();
GLuint gl_GLESInternalPresentGetProgram();
bool gl_GLESInternalPresent(const FGLESPresentConfig &config);
FGLESViewArea gl_GLESInternalComputeViewArea(int fullWidth, int fullHeight,
	int trueHeight, int screenBlocks, int viewX, int viewY, int viewWidth, int viewHeight);
void gl_GLESInternalSetSceneViewport(const FGLESViewArea &area);
bool gl_GLESInternalFillStencil(int width, int height, GLuint stencilReference,
	GLuint stencilMask, const float color[4]);
void gl_GLESInternalPublishFrameContract(uint64_t frameNumber, double timeSeconds,
	const FGLESTargetDescriptor &target, const FGLESViewDescriptor &view, bool viewValid);
bool gl_GLESInternalCopyTargetToTexture(const FGLESTargetDescriptor &target, GLuint texture);
bool gl_GLESInternalReadTarget(const FGLESTargetDescriptor &target, unsigned char *rgba,
	int width, int height);
bool gl_GLESInternalWriteSavePic(FILE *file, const FGLESTargetDescriptor &target,
	int width, int height);

#endif
