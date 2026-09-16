#include "gl/system/gl_gles_internal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#include "gl/system/gl_gles_dispatch.h"

#include "doomstat.h"
#include "i_system.h"

namespace
{
	bool StateWarningLogged = false;
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
}

void gl_GLESInternalStateContextLost()
{
	StateWarningLogged = false;
}

bool gl_GLESInternalApplyRenderState(bool resourcesAvailable, int srcBlend, int dstBlend,
	int alphaFunc, float alphaThreshold, bool alphaTest, int blendEquation,
	bool fogEnabled, bool textureEnabled, int textureMode)
{
	(void)alphaFunc;
	(void)alphaThreshold;
	(void)textureMode;
	if (!resourcesAvailable) return false;
	glBlendFunc(static_cast<GLenum>(srcBlend), static_cast<GLenum>(dstBlend));
	glBlendEquation(static_cast<GLenum>(blendEquation));
	if (alphaTest || fogEnabled || !textureEnabled)
	{
		if (developer && !StateWarningLogged)
		{
			StateWarningLogged = true;
			DPrintf("Zandronum GLES state request is consumed by the native shader variants; alpha, fog, and texture flags remain submission semantics.\n");
		}
	}
	return gl_GLES_CheckErrors("FRenderState::Apply") == GL_NO_ERROR;
}

#endif
