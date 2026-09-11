#include "gl/system/gl_gles_targets.h"

#include <algorithm>

#include "i_system.h"

namespace
{
	static bool TargetFallbackLogged = false;

	static void ClearErrors()
	{
		while (glGetError() != GL_NO_ERROR) {}
	}

	static bool IsComplete()
	{
		return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
	}

	static void DeleteCandidate(FGLESTargetDescriptor &target)
	{
		if (target.msaaDepthStencilAttachment != 0)
			glDeleteRenderbuffers(1, &target.msaaDepthStencilAttachment);
		if (target.msaaColorAttachment != 0)
			glDeleteRenderbuffers(1, &target.msaaColorAttachment);
		if (target.resolveDepthStencilAttachment != 0)
			glDeleteRenderbuffers(1, &target.resolveDepthStencilAttachment);
		if (target.resolveFramebuffer != 0)
			glDeleteFramebuffers(1, &target.resolveFramebuffer);
		if (target.colorAttachment != 0)
			glDeleteTextures(1, &target.colorAttachment);
		if (target.framebuffer != 0 && target.framebuffer != target.resolveFramebuffer)
			glDeleteFramebuffers(1, &target.framebuffer);
		target = {};
	}

	static bool BuildCandidate(FGLESTargetDescriptor &target, int width, int height, int samples)
	{
	target.renderWidth = width;
	target.renderHeight = height;
	target.sampleCount = samples > 1 ? samples : 1;

		glGenTextures(1, &target.colorAttachment);
		glBindTexture(GL_TEXTURE_2D, target.colorAttachment);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, NULL);

		glGenFramebuffers(1, &target.resolveFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, target.resolveFramebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			target.colorAttachment, 0);
		glGenRenderbuffers(1, &target.resolveDepthStencilAttachment);
		glBindRenderbuffer(GL_RENDERBUFFER, target.resolveDepthStencilAttachment);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, target.resolveDepthStencilAttachment);
		if (!IsComplete() || glGetError() != GL_NO_ERROR)
		{
			DeleteCandidate(target);
			return false;
		}

		if (samples <= 1)
		{
			target.framebuffer = target.resolveFramebuffer;
			target.depthStencilAttachment = target.resolveDepthStencilAttachment;
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			return true;
		}

		glGenFramebuffers(1, &target.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
		glGenRenderbuffers(1, &target.msaaColorAttachment);
		glBindRenderbuffer(GL_RENDERBUFFER, target.msaaColorAttachment);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
			target.msaaColorAttachment);
		glGenRenderbuffers(1, &target.msaaDepthStencilAttachment);
		glBindRenderbuffer(GL_RENDERBUFFER, target.msaaDepthStencilAttachment);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8,
			width, height);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
			GL_RENDERBUFFER, target.msaaDepthStencilAttachment);
		if (!IsComplete() || glGetError() != GL_NO_ERROR)
		{
			DeleteCandidate(target);
			return false;
		}

		target.depthStencilAttachment = target.msaaDepthStencilAttachment;
		glBindRenderbuffer(GL_RENDERBUFFER, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		return true;
	}
}

bool gl_GLES_CreateRenderTarget(FGLESTargetDescriptor *target, int width, int height,
	int requestedSamples)
{
	if (target == NULL || width <= 0 || height <= 0) return false;
	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousRenderbuffer = 0;
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousTexture = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	const GLuint oldFramebuffer = target->framebuffer;
	const GLuint oldResolveFramebuffer = target->resolveFramebuffer;
	const GLuint oldColorAttachment = target->colorAttachment;
	const GLuint oldDepthStencilAttachment = target->depthStencilAttachment;
	const GLuint oldResolveDepthStencilAttachment = target->resolveDepthStencilAttachment;
	const GLuint oldMsaaDepthStencilAttachment = target->msaaDepthStencilAttachment;
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
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
		glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(previousRenderbuffer));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	};

	GLint maxSamples = 1;
	glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
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
		ClearErrors();
		FGLESTargetDescriptor candidate = {};
		if (!BuildCandidate(candidate, width, height, candidates[index])) continue;
		*target = candidate;
		restoreState();
		if (!TargetFallbackLogged)
		{
			TargetFallbackLogged = true;
			if (requested > 1 && target->sampleCount != requested)
				Printf("GLES render target selected %dx%d after requested %dx%d MSAA fallback.\n",
					target->renderWidth, target->renderHeight, requested, requested);
			else if (target->sampleCount > 1)
				Printf("GLES render target selected %dx%d with %dx MSAA and explicit color resolve.\n",
					target->renderWidth, target->renderHeight, target->sampleCount);
			else
				Printf("GLES render target selected %dx%d single-sample RGBA8 with depth/stencil.\n",
					target->renderWidth, target->renderHeight);
		}
		return true;
	}

	restoreState();
	Printf("GLES render target allocation failed at %dx%d; tried requested MSAA, lower samples, and single-sample RGBA8.\n",
		width, height);
	return false;
}

void gl_GLES_DestroyRenderTarget(FGLESTargetDescriptor *target)
{
	if (target == NULL) return;
	if (target->msaaDepthStencilAttachment != 0)
		glDeleteRenderbuffers(1, &target->msaaDepthStencilAttachment);
	if (target->msaaColorAttachment != 0)
		glDeleteRenderbuffers(1, &target->msaaColorAttachment);
	if (target->resolveDepthStencilAttachment != 0)
		glDeleteRenderbuffers(1, &target->resolveDepthStencilAttachment);
	if (target->resolveFramebuffer != 0)
		glDeleteFramebuffers(1, &target->resolveFramebuffer);
	if (target->colorAttachment != 0)
		glDeleteTextures(1, &target->colorAttachment);
	if (target->framebuffer != 0 && target->framebuffer != target->resolveFramebuffer)
		glDeleteFramebuffers(1, &target->framebuffer);
	*target = {};
}

bool gl_GLES_ResolveRenderTarget(const FGLESTargetDescriptor *target)
{
	if (target == NULL || target->framebuffer == 0 || target->resolveFramebuffer == 0)
		return false;
	if (target->sampleCount <= 1) return true;

	GLint drawFramebuffer = 0;
	GLint readFramebuffer = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, target->framebuffer);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->resolveFramebuffer);
	glBlitFramebuffer(0, 0, target->renderWidth, target->renderHeight,
		0, 0, target->renderWidth, target->renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	const GLenum error = glGetError();
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFramebuffer));
	return error == GL_NO_ERROR;
}

void gl_GLES_BindRenderTarget(const FGLESTargetDescriptor *target)
{
	if (target == NULL) return;
	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glViewport(0, 0, target->renderWidth, target->renderHeight);
}
