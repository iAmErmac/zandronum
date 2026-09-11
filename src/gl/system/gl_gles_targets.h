#ifndef ZANDRONUM_GL_GLES_TARGETS_H
#define ZANDRONUM_GL_GLES_TARGETS_H

#include <GLES3/gl32.h>

struct FGLESTargetDescriptor
{
	GLuint framebuffer;
	GLuint colorAttachment;
	GLuint depthStencilAttachment;
	GLuint resolveFramebuffer;
	GLuint resolveDepthStencilAttachment;
	GLuint msaaColorAttachment;
	GLuint msaaDepthStencilAttachment;
	int renderWidth;
	int renderHeight;
	int sampleCount;
};

bool gl_GLES_CreateRenderTarget(FGLESTargetDescriptor *target, int width, int height,
	int requestedSamples);
void gl_GLES_DestroyRenderTarget(FGLESTargetDescriptor *target);
bool gl_GLES_ResolveRenderTarget(const FGLESTargetDescriptor *target);
void gl_GLES_BindRenderTarget(const FGLESTargetDescriptor *target);

#endif
