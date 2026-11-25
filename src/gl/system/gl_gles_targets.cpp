#include "gl/system/gl_gles_targets.h"

#include <algorithm>
#include <stdio.h>

#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_renderer.h"

namespace
{
	void ClearErrors(const FGLESProcTable &gles)
	{
		while (gles.GetError() != GL_NO_ERROR) {}
	}

	bool IsComplete(const FGLESProcTable &gles)
	{
		return gles.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	}

	void DeleteTarget(const FGLESProcTable &gles, FGLESTargetDescriptor &target)
	{
		if (target.msaaDepthStencilAttachment != 0)
			gles.DeleteRenderbuffers(1, &target.msaaDepthStencilAttachment);
		if (target.msaaColorAttachment != 0)
			gles.DeleteRenderbuffers(1, &target.msaaColorAttachment);
		if (target.resolveDepthStencilAttachment != 0)
			gles.DeleteRenderbuffers(1, &target.resolveDepthStencilAttachment);
		if (target.resolveFramebuffer != 0)
			gles.DeleteFramebuffers(1, &target.resolveFramebuffer);
		if (target.colorAttachment != 0)
			gles.DeleteTextures(1, &target.colorAttachment);
		if (target.framebuffer != 0 && target.framebuffer != target.resolveFramebuffer)
			gles.DeleteFramebuffers(1, &target.framebuffer);
		target = {};
	}

	bool BuildCandidate(const FGLESProcTable &gles, FGLESTargetDescriptor &target,
		int width, int height, int samples)
	{
		target.renderWidth = width;
		target.renderHeight = height;
		target.sampleCount = 1;

		gles.GenTextures(1, &target.colorAttachment);
		gles.BindTexture(GL_TEXTURE_2D, target.colorAttachment);
		gles.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gles.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		gles.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gles.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		gles.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, nullptr);

		gles.GenFramebuffers(1, &target.resolveFramebuffer);
		gles.BindFramebuffer(GL_FRAMEBUFFER, target.resolveFramebuffer);
		gles.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			target.colorAttachment, 0);
		gles.GenRenderbuffers(1, &target.resolveDepthStencilAttachment);
		gles.BindRenderbuffer(GL_RENDERBUFFER, target.resolveDepthStencilAttachment);
		gles.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
		gles.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, target.resolveDepthStencilAttachment);
		const bool resolveComplete = IsComplete(gles);
		const GLenum resolveError = gles.GetError();
		if (!resolveComplete || resolveError != GL_NO_ERROR)
			return false;

		if (samples <= 1)
		{
			target.framebuffer = target.resolveFramebuffer;
			target.depthStencilAttachment = target.resolveDepthStencilAttachment;
			return true;
		}

		gles.GenFramebuffers(1, &target.framebuffer);
		gles.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
		gles.GenRenderbuffers(1, &target.msaaColorAttachment);
		gles.BindRenderbuffer(GL_RENDERBUFFER, target.msaaColorAttachment);
		gles.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
		gles.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
			target.msaaColorAttachment);
		gles.GenRenderbuffers(1, &target.msaaDepthStencilAttachment);
		gles.BindRenderbuffer(GL_RENDERBUFFER, target.msaaDepthStencilAttachment);
		gles.RenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
			width, height);
		gles.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, target.msaaDepthStencilAttachment);
		const bool msaaComplete = IsComplete(gles);
		const GLenum msaaError = gles.GetError();
		if (!msaaComplete || msaaError != GL_NO_ERROR)
			return false;
		GLint colorSamples = 0;
		GLint depthSamples = 0;
		gles.BindRenderbuffer(GL_RENDERBUFFER, target.msaaColorAttachment);
		gles.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &colorSamples);
		gles.BindRenderbuffer(GL_RENDERBUFFER, target.msaaDepthStencilAttachment);
		gles.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &depthSamples);
		if (gles.GetError() != GL_NO_ERROR || colorSamples <= 1 ||
			colorSamples != depthSamples)
			return false;

		target.depthStencilAttachment = target.msaaDepthStencilAttachment;
		target.sampleCount = colorSamples;
		return true;
	}
}

bool gl_GLES_CreateRenderTarget(FGLESTargetDescriptor *target, int width, int height,
	int requestedSamples)
{
	if (target == nullptr || width <= 0 || height <= 0 || !gl_GLES_HasContext()) return false;
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousRenderbuffer = 0;
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousTexture = 0;
	gles.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	gles.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	gles.GetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
	gles.GetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	gles.GetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	FGLESTargetDescriptor oldTarget = *target;
	const bool drawWasFramebuffer = oldTarget.framebuffer != 0 &&
		previousDrawFramebuffer == static_cast<GLint>(oldTarget.framebuffer);
	const bool drawWasResolve = oldTarget.resolveFramebuffer != 0 &&
		previousDrawFramebuffer == static_cast<GLint>(oldTarget.resolveFramebuffer);
	const bool readWasFramebuffer = oldTarget.framebuffer != 0 &&
		previousReadFramebuffer == static_cast<GLint>(oldTarget.framebuffer);
	const bool readWasResolve = oldTarget.resolveFramebuffer != 0 &&
		previousReadFramebuffer == static_cast<GLint>(oldTarget.resolveFramebuffer);
	const bool renderbufferWasDrawDepth = oldTarget.depthStencilAttachment != 0 &&
		previousRenderbuffer ==
		static_cast<GLint>(oldTarget.depthStencilAttachment);
	const bool renderbufferWasResolveDepth = oldTarget.resolveDepthStencilAttachment != 0 &&
		previousRenderbuffer ==
		static_cast<GLint>(oldTarget.resolveDepthStencilAttachment);
	const bool renderbufferWasMsaaColor = oldTarget.msaaColorAttachment != 0 &&
		previousRenderbuffer ==
		static_cast<GLint>(oldTarget.msaaColorAttachment);
	const bool textureWasTarget = oldTarget.colorAttachment != 0 &&
		previousTexture == static_cast<GLint>(oldTarget.colorAttachment);
	const bool hostOwnsPresentation = oldTarget.hostOwnsPresentation;
	auto restoreState = [&](const FGLESTargetDescriptor &replacement)
	{
		GLuint drawFramebuffer = static_cast<GLuint>(previousDrawFramebuffer);
		GLuint readFramebuffer = static_cast<GLuint>(previousReadFramebuffer);
		GLuint renderbuffer = static_cast<GLuint>(previousRenderbuffer);
		GLuint texture = static_cast<GLuint>(previousTexture);
		if (drawWasFramebuffer) drawFramebuffer = replacement.framebuffer;
		else if (drawWasResolve) drawFramebuffer = replacement.resolveFramebuffer;
		if (readWasFramebuffer) readFramebuffer = replacement.framebuffer;
		else if (readWasResolve) readFramebuffer = replacement.resolveFramebuffer;
		if (renderbufferWasDrawDepth) renderbuffer = replacement.depthStencilAttachment;
		else if (renderbufferWasResolveDepth) renderbuffer = replacement.resolveDepthStencilAttachment;
		else if (renderbufferWasMsaaColor) renderbuffer = replacement.msaaColorAttachment;
		if (textureWasTarget) texture = replacement.colorAttachment;
		gles.BindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFramebuffer);
		gles.BindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
		gles.BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
		gles.ActiveTexture(static_cast<GLenum>(previousActiveTexture));
		gles.BindTexture(GL_TEXTURE_2D, texture);
	};

	int samples = 1;
	if (requestedSamples > 1 && gl_GLES_GetContextInfo().hasMultisample)
	{
		GLint maxSamples = 1;
		gles.GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
		samples = std::max(1, std::min(requestedSamples, static_cast<int>(maxSamples)));
	}

	for (;;)
	{
		ClearErrors(gles);
		FGLESTargetDescriptor candidate = {};
		if (BuildCandidate(gles, candidate, width, height, samples))
		{
			candidate.hostOwnsPresentation = hostOwnsPresentation;
			DeleteTarget(gles, oldTarget);
			*target = candidate;
			restoreState(*target);
			return true;
		}
		DeleteTarget(gles, candidate);
		if (samples == 1) break;
		samples = samples > 2 ? std::max(2, samples / 2) : 1;
	}

	ClearErrors(gles);
	restoreState(oldTarget);
	if (gl_GLES_IsProfileEnabled())
	{
		char message[96];
		snprintf(message, sizeof(message), "render target allocation failed at %dx%d", width, height);
		gl_GLES_Report("target", message);
	}
	return false;
}

void gl_GLES_DestroyRenderTarget(FGLESTargetDescriptor *target)
{
	if (target == nullptr) return;
	if (!gl_GLES_HasContext())
	{
		*target = {};
		return;
	}
	DeleteTarget(gl_GLES_GetProcTable(), *target);
}

void gl_GLES_InvalidateRenderTarget(FGLESTargetDescriptor *target)
{
	if (target != nullptr)
		*target = {};
}

bool gl_GLES_ResolveRenderTarget(const FGLESTargetDescriptor *target)
{
	if (target == nullptr || target->framebuffer == 0 || target->resolveFramebuffer == 0 ||
		!gl_GLES_HasContext()) return false;
	if (target->sampleCount <= 1) return true;
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	if (gles.BlitFramebuffer == nullptr) return false;
	GLint drawFramebuffer = 0;
	GLint readFramebuffer = 0;
	gles.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
	gles.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
	gles.BindFramebuffer(GL_READ_FRAMEBUFFER, target->framebuffer);
	gles.BindFramebuffer(GL_DRAW_FRAMEBUFFER, target->resolveFramebuffer);
	gles.BlitFramebuffer(0, 0, target->renderWidth, target->renderHeight,
		0, 0, target->renderWidth, target->renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	gl_GLES_RecordProfileResolve();
	const GLenum error = gl_GLES_CheckErrors("target resolve");
	gles.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
	gles.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
	return error == GL_NO_ERROR;
}

void gl_GLES_BindRenderTarget(const FGLESTargetDescriptor *target)
{
	if (target == nullptr || !gl_GLES_HasContext()) return;
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	gles.BindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	gles.Viewport(0, 0, target->renderWidth, target->renderHeight);
}
