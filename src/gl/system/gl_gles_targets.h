#ifndef ZANDRONUM_GL_GLES_TARGETS_H
#define ZANDRONUM_GL_GLES_TARGETS_H

#include "gl/system/gl_gles_api.h"
#include "gl/system/gl_gles_contract.h"

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
	bool hostOwnsPresentation;
};

bool gl_GLES_CreateRenderTarget(FGLESTargetDescriptor *target, int width, int height,
	int requestedSamples);
void gl_GLES_DestroyRenderTarget(FGLESTargetDescriptor *target);
bool gl_GLES_ResolveRenderTarget(const FGLESTargetDescriptor *target);
void gl_GLES_BindRenderTarget(const FGLESTargetDescriptor *target);

#endif
