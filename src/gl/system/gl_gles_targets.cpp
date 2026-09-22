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

	void DeleteCandidate(const FGLESProcTable &gles, FGLESTargetDescriptor &target)
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
		target.sampleCount = samples > 1 ? samples : 1;

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
		if (!IsComplete(gles) || gles.GetError() != GL_NO_ERROR)
		{
			DeleteCandidate(gles, target);
			return false;
		}

		if (samples <= 1)
		{
			target.framebuffer = target.resolveFramebuffer;
			target.depthStencilAttachment = target.resolveDepthStencilAttachment;
			gles.BindRenderbuffer(GL_RENDERBUFFER, 0);
			gles.BindFramebuffer(GL_FRAMEBUFFER, 0);
			return true;
		}

		if (!gl_GLES_GetContextInfo().hasMultisample ||
			gles.RenderbufferStorageMultisample == nullptr || gles.BlitFramebuffer == nullptr)
		{
			DeleteCandidate(gles, target);
			return false;
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
		if (!IsComplete(gles) || gles.GetError() != GL_NO_ERROR)
		{
			DeleteCandidate(gles, target);
			return false;
		}

		target.depthStencilAttachment = target.msaaDepthStencilAttachment;
		gles.BindRenderbuffer(GL_RENDERBUFFER, 0);
		gles.BindFramebuffer(GL_FRAMEBUFFER, 0);
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
	const GLuint oldFramebuffer = target->framebuffer;
	const GLuint oldResolveFramebuffer = target->resolveFramebuffer;
	const GLuint oldColorAttachment = target->colorAttachment;
	const GLuint oldDepthStencilAttachment = target->depthStencilAttachment;
	const GLuint oldResolveDepthStencilAttachment = target->resolveDepthStencilAttachment;
	const GLuint oldMsaaDepthStencilAttachment = target->msaaDepthStencilAttachment;
	const bool hostOwnsPresentation = target->hostOwnsPresentation;
	const bool targetWasBound = previousDrawFramebuffer == static_cast<GLint>(oldFramebuffer) ||
		previousReadFramebuffer == static_cast<GLint>(oldFramebuffer) ||
		previousDrawFramebuffer == static_cast<GLint>(oldResolveFramebuffer) ||
		previousReadFramebuffer == static_cast<GLint>(oldResolveFramebuffer);
	if (previousTexture == static_cast<GLint>(oldColorAttachment)) previousTexture = 0;
	if (previousRenderbuffer == static_cast<GLint>(oldDepthStencilAttachment) ||
		previousRenderbuffer == static_cast<GLint>(oldResolveDepthStencilAttachment) ||
		previousRenderbuffer == static_cast<GLint>(oldMsaaDepthStencilAttachment))
		previousRenderbuffer = 0;
	gl_GLES_DestroyRenderTarget(target);
	if (targetWasBound)
	{
		previousDrawFramebuffer = 0;
		previousReadFramebuffer = 0;
	}
	auto restoreState = [&]()
	{
		gles.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
		gles.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
		gles.BindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
		gles.ActiveTexture(static_cast<GLenum>(previousActiveTexture));
		gles.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	};

	GLint maxSamples = 1;
	gles.GetIntegerv(GL_MAX_SAMPLES, &maxSamples);
	if (maxSamples < 1) maxSamples = 1;
	const int requested = std::max(requestedSamples, 0);
	int candidates[8] = {};
	int candidateCount = 0;
	if (requested > 1)
	{
		int sample = std::min(requested, static_cast<int>(maxSamples));
		while (sample > 1 && candidateCount < 8)
		{
			candidates[candidateCount++] = sample;
			const int nextSample = sample > 2 ? std::max(2, sample / 2) : 1;
			if (nextSample == sample) break;
			sample = nextSample;
		}
	}
	candidates[candidateCount++] = 1;

	for (int index = 0; index < candidateCount; ++index)
	{
		ClearErrors(gles);
		FGLESTargetDescriptor candidate = {};
		if (!BuildCandidate(gles, candidate, width, height, candidates[index])) continue;
		candidate.hostOwnsPresentation = hostOwnsPresentation;
		*target = candidate;
		restoreState();
		return true;
	}

	restoreState();
	char message[256];
	snprintf(message, sizeof(message), "render target allocation failed at %dx%d", width, height);
	gl_GLES_Report("target", message);
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
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	if (target->msaaDepthStencilAttachment != 0)
		gles.DeleteRenderbuffers(1, &target->msaaDepthStencilAttachment);
	if (target->msaaColorAttachment != 0)
		gles.DeleteRenderbuffers(1, &target->msaaColorAttachment);
	if (target->resolveDepthStencilAttachment != 0)
		gles.DeleteRenderbuffers(1, &target->resolveDepthStencilAttachment);
	if (target->resolveFramebuffer != 0)
		gles.DeleteFramebuffers(1, &target->resolveFramebuffer);
	if (target->colorAttachment != 0)
		gles.DeleteTextures(1, &target->colorAttachment);
	if (target->framebuffer != 0 && target->framebuffer != target->resolveFramebuffer)
		gles.DeleteFramebuffers(1, &target->framebuffer);
	*target = {};
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
