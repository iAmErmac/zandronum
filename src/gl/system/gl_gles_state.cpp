#include "gl/system/gl_gles_internal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#include "gl/system/gl_gles_dispatch.h"

#include "doomstat.h"
#include "i_system.h"

namespace
{
	bool StateWarningLogged = false;
	bool BlendStateKnown = false;
	GLenum CachedSourceBlend = 0;
	GLenum CachedDestinationBlend = 0;
	GLenum CachedBlendEquation = 0;
}

void gl_GLESInternalResetState(int width, int height)
{
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, width, height);
	glScissor(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xffffffffu);
	glStencilFunc(GL_ALWAYS, 0, 0xffffffffu);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glDisable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBlendEquation(GL_FUNC_ADD);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glActiveTexture(GL_TEXTURE1);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE2);
	glBindSampler(2, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(0);
	glUseProgram(0);
	gl_GLESInternalInvalidateProgramBinding();
	gl_GLESInternalInvalidateStateCache();
}

void gl_GLESInternalInvalidateStateCache()
{
	BlendStateKnown = false;
}

void gl_GLESInternalStateContextLost()
{
	StateWarningLogged = false;
	gl_GLESInternalInvalidateStateCache();
}

bool gl_GLESInternalApplyRenderState(bool resourcesAvailable, int srcBlend, int dstBlend,
	int alphaFunc, float alphaThreshold, bool alphaTest, int blendEquation,
	bool fogEnabled, bool textureEnabled, int textureMode)
{
	(void)alphaFunc;
	(void)alphaThreshold;
	(void)textureMode;
	if (!resourcesAvailable) return false;
	const GLenum requestedSourceBlend = static_cast<GLenum>(srcBlend);
	const GLenum requestedDestinationBlend = static_cast<GLenum>(dstBlend);
	const GLenum requestedBlendEquation = static_cast<GLenum>(blendEquation);
	const bool unchanged = BlendStateKnown &&
		CachedSourceBlend == requestedSourceBlend &&
		CachedDestinationBlend == requestedDestinationBlend &&
		CachedBlendEquation == requestedBlendEquation;
	if (unchanged)
	{
		gl_GLES_RecordProfileState(true);
	}
	else
	{
		glBlendFunc(requestedSourceBlend, requestedDestinationBlend);
		glBlendEquation(requestedBlendEquation);
		CachedSourceBlend = requestedSourceBlend;
		CachedDestinationBlend = requestedDestinationBlend;
		CachedBlendEquation = requestedBlendEquation;
		BlendStateKnown = true;
		gl_GLES_RecordProfileState(false);
	}
	if (alphaTest || fogEnabled || !textureEnabled)
	{
		if (developer && !StateWarningLogged)
		{
			StateWarningLogged = true;
			DPrintf("Zandronum GLES state request is consumed by the native shader variants; alpha, fog, and texture flags remain submission semantics.\n");
		}
	}
	// Keep the error drain out of normal render-state traffic; scene boundaries
	// perform the regular GLES check, while developer mode retains this local
	// diagnostic when tracing a legacy state request.
	return !developer || gl_GLES_CheckErrors("FRenderState::Apply") == GL_NO_ERROR;
}

#endif
