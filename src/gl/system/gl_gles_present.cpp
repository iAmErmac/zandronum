#include "gl/system/gl_gles_present.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif

#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_dispatch.h"
#include "gl/system/gl_gles_shader.h"
#include "basictypes.h"
#include "f_wipe.h"
#include "m_png.h"

#include <vector>

namespace
{
	struct FGLESPresenter
	{
		GLuint program;
		GLuint fillProgram;
		GLuint vertexArray;
		GLint texture;
		GLint textureTransform;
		GLint depth;
		GLint wipeStart;
		GLint wipeEnd;
		GLint wipeMask;
		GLint wipeProgress;
		GLint wipeType;
		GLint wipeActive;
		GLint gamma;
		GLint brightness;
		GLint contrast;
		GLint fillColor;
	};

	FGLESPresenter Presenter = {};

	static const char *PresentVertexSource =
		"#version 320 es\n"
		"uniform vec4 u_texture_transform;\n"
		"uniform float u_depth;\n"
		"out vec2 v_uv;\n"
		"void main() { vec2 position = gl_VertexID == 0 ? vec2(-1.0, -1.0) : (gl_VertexID == 1 ? vec2(3.0, -1.0) : vec2(-1.0, 3.0)); gl_Position = vec4(position, u_depth, 1.0); v_uv = (position * 0.5 + 0.5) * u_texture_transform.xy + u_texture_transform.zw; }\n";

	static const char *PresentFragmentSource =
		"#version 320 es\n"
		"precision highp float;\n"
		"in vec2 v_uv;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"uniform sampler2D u_texture;\n"
		"uniform sampler2D u_wipe_start;\n"
		"uniform sampler2D u_wipe_end;\n"
		"uniform sampler2D u_wipe_mask;\n"
		"uniform float u_wipe_progress;\n"
		"uniform int u_wipe_type;\n"
		"uniform bool u_wipe_active;\n"
		"uniform float u_gamma;\n"
		"uniform float u_brightness;\n"
		"uniform float u_contrast;\n"
		"vec4 wipe_color() { vec4 wipeCurrent; vec4 wipeStart; vec4 wipeEnd; float wipeProgress; float wipeColumn; vec4 wipePacked; float wipeFall; float wipeFallUv; float wipeBurn; wipeCurrent = texture(u_texture, v_uv); wipeStart = texture(u_wipe_start, v_uv); wipeEnd = texture(u_wipe_end, v_uv); wipeProgress = clamp(u_wipe_progress, 0.0, 1.0); if (!u_wipe_active) return wipeCurrent; if (u_wipe_type == 1) { wipeColumn = floor(clamp(v_uv.x, 0.0, 0.999999) * 320.0); wipePacked = texture(u_wipe_mask, vec2((wipeColumn + 0.5) / 320.0, 0.5)); wipeFall = (floor(wipePacked.r * 255.0 + 0.5) + floor(wipePacked.g * 255.0 + 0.5) * 256.0) / 65535.0 * 200.0; wipeFallUv = wipeFall / 200.0; if (v_uv.y <= 1.0 - wipeFallUv) return texture(u_wipe_start, vec2(v_uv.x, clamp(v_uv.y + wipeFallUv, 0.0, 1.0))); return wipeEnd; } if (u_wipe_type == 2) { wipeBurn = texture(u_wipe_mask, v_uv).r; return mix(wipeStart, wipeEnd, wipeBurn); } return mix(wipeStart, wipeEnd, wipeProgress); }\n"
		"void main() { vec4 color = wipe_color(); color.rgb = max((color.rgb - 0.5) * u_contrast + 0.5 + u_brightness * 0.5, vec3(0.0)); color.rgb = pow(color.rgb, vec3(1.0 / max(u_gamma, 0.1))); frag_color = color; }\n";

	static const char *FillVertexSource =
		"#version 320 es\n"
		"void main() { vec2 position = gl_VertexID == 0 ? vec2(-1.0, -1.0) : (gl_VertexID == 1 ? vec2(3.0, -1.0) : vec2(-1.0, 3.0)); gl_Position = vec4(position, 0.0, 1.0); }\n";

	static const char *FillFragmentSource =
		"#version 320 es\n"
		"precision highp float;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"uniform vec4 u_fill_color;\n"
		"void main() { frag_color = u_fill_color; }\n";

	static void ResetUniforms()
	{
		Presenter.texture = -1;
		Presenter.textureTransform = -1;
		Presenter.depth = -1;
		Presenter.wipeStart = -1;
		Presenter.wipeEnd = -1;
		Presenter.wipeMask = -1;
		Presenter.wipeProgress = -1;
		Presenter.wipeType = -1;
		Presenter.wipeActive = -1;
		Presenter.gamma = -1;
		Presenter.brightness = -1;
		Presenter.contrast = -1;
		Presenter.fillColor = -1;
	}
}

bool gl_GLESInternalPresentInitialize()
{
	if (Presenter.program != 0 && Presenter.fillProgram != 0 && Presenter.vertexArray != 0) return true;
	char log[1024] = {};
	Presenter.program = gl_GLES_LinkProgram(PresentVertexSource, PresentFragmentSource,
		"present", log, sizeof(log));
	if (Presenter.program == 0)
	{
		gl_GLES_Report("present", log);
		return false;
	}
	Presenter.texture = glGetUniformLocation(Presenter.program, "u_texture");
	Presenter.textureTransform = glGetUniformLocation(Presenter.program, "u_texture_transform");
	Presenter.depth = glGetUniformLocation(Presenter.program, "u_depth");
	Presenter.wipeStart = glGetUniformLocation(Presenter.program, "u_wipe_start");
	Presenter.wipeEnd = glGetUniformLocation(Presenter.program, "u_wipe_end");
	Presenter.wipeMask = glGetUniformLocation(Presenter.program, "u_wipe_mask");
	Presenter.wipeProgress = glGetUniformLocation(Presenter.program, "u_wipe_progress");
	Presenter.wipeType = glGetUniformLocation(Presenter.program, "u_wipe_type");
	Presenter.wipeActive = glGetUniformLocation(Presenter.program, "u_wipe_active");
	Presenter.gamma = glGetUniformLocation(Presenter.program, "u_gamma");
	Presenter.brightness = glGetUniformLocation(Presenter.program, "u_brightness");
	Presenter.contrast = glGetUniformLocation(Presenter.program, "u_contrast");
	Presenter.fillProgram = gl_GLES_LinkProgram(FillVertexSource, FillFragmentSource,
		"portal termination fill", log, sizeof(log));
	if (Presenter.fillProgram == 0)
	{
		gl_GLES_Report("portal", log);
		gl_GLESInternalPresentDestroy();
		return false;
	}
	Presenter.fillColor = glGetUniformLocation(Presenter.fillProgram, "u_fill_color");
	glGenVertexArrays(1, &Presenter.vertexArray);
	if (Presenter.vertexArray == 0 || gl_GLES_CheckErrors("present resource setup") != GL_NO_ERROR)
	{
		gl_GLESInternalPresentDestroy();
		return false;
	}
	return true;
}

void gl_GLESInternalPresentDestroy()
{
	if (Presenter.vertexArray != 0) glDeleteVertexArrays(1, &Presenter.vertexArray);
	if (Presenter.program != 0) glDeleteProgram(Presenter.program);
	if (Presenter.fillProgram != 0) glDeleteProgram(Presenter.fillProgram);
	Presenter.vertexArray = 0;
	Presenter.program = 0;
	Presenter.fillProgram = 0;
	ResetUniforms();
}

void gl_GLESInternalPresentContextLost()
{
	Presenter.vertexArray = 0;
	Presenter.program = 0;
	Presenter.fillProgram = 0;
	ResetUniforms();
}

GLuint gl_GLESInternalPresentGetProgram()
{
	return Presenter.program;
}

bool gl_GLESInternalPresent(const FGLESPresentConfig &config)
{
	if (Presenter.program == 0 || Presenter.vertexArray == 0 ||
		config.target.colorAttachment == 0 || config.target.renderWidth <= 0 ||
		config.target.renderHeight <= 0 || !config.presentationTarget.hostOwnsPresentation ||
		config.presentationTarget.renderWidth <= 0 || config.presentationTarget.renderHeight <= 0)
		return false;

	gl_GLESInternalResetState(config.stateWidth, config.stateHeight);
	glBindFramebuffer(GL_FRAMEBUFFER, config.presentationTarget.framebuffer);
	glViewport(0, 0, config.presentationTarget.renderWidth, config.presentationTarget.renderHeight);
	glScissor(0, 0, config.presentationTarget.renderWidth, config.presentationTarget.renderHeight);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(Presenter.program);
	if (Presenter.texture >= 0) glUniform1i(Presenter.texture, 0);
	if (Presenter.textureTransform >= 0)
		glUniform4f(Presenter.textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (Presenter.depth >= 0) glUniform1f(Presenter.depth, 0.0f);
	const FGLESWipeBindings &wipe = config.wipe;
	const bool wipeMaskReady = wipe.type == wipe_Fade || wipe.maskTexture != 0;
	const bool wipeReady = wipe.active && wipe.startReady && wipe.endReady && wipeMaskReady;
	if (Presenter.wipeStart >= 0) glUniform1i(Presenter.wipeStart, 1);
	if (Presenter.wipeEnd >= 0) glUniform1i(Presenter.wipeEnd, 2);
	if (Presenter.wipeMask >= 0) glUniform1i(Presenter.wipeMask, 3);
	if (Presenter.wipeProgress >= 0)
		glUniform1f(Presenter.wipeProgress, wipe.progress);
	if (Presenter.wipeType >= 0) glUniform1i(Presenter.wipeType, wipe.type);
	if (Presenter.wipeActive >= 0) glUniform1i(Presenter.wipeActive, wipeReady ? 1 : 0);
	if (Presenter.gamma >= 0) glUniform1f(Presenter.gamma, config.gamma);
	if (Presenter.brightness >= 0) glUniform1f(Presenter.brightness, config.brightness);
	if (Presenter.contrast >= 0) glUniform1f(Presenter.contrast, config.contrast);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, config.target.colorAttachment);
	glBindSampler(0, config.sceneSampler);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, wipeReady ? wipe.startTexture : config.target.colorAttachment);
	glBindSampler(1, config.sceneSampler);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, wipeReady ? wipe.endTexture : config.target.colorAttachment);
	glBindSampler(2, config.sceneSampler);
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, wipeReady && wipe.type != wipe_Fade ? wipe.maskTexture : 0);
	glBindSampler(3, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(Presenter.vertexArray);
gl_GLES_GetProcTable().DrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE2);
	glBindSampler(2, 0);
	glActiveTexture(GL_TEXTURE3);
	glBindSampler(3, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glUseProgram(0);
	gl_GLESInternalResetState(config.stateWidth, config.stateHeight);
	return gl_GLES_CheckErrors("present") == GL_NO_ERROR;
}

FGLESViewArea gl_GLESInternalComputeViewArea(int fullWidth, int fullHeight,
	int trueHeight, int screenBlocks, int viewX, int viewY, int viewWidth, int viewHeight)
{
	FGLESViewArea area = {};
	const int renderHeight = screenBlocks >= 10 ? fullHeight : (screenBlocks * fullHeight / 10) & ~7;
	const int renderWidth = screenBlocks >= 10 ? fullWidth : screenBlocks * fullWidth / 10;
	const int bars = (trueHeight - fullHeight) / 2;
	area.x = viewX;
	area.y = viewY;
	area.width = viewWidth;
	area.height = viewHeight;
	area.renderX = viewX;
	area.renderY = trueHeight - bars -
		(renderHeight + viewY - ((renderHeight - viewHeight) / 2));
	area.renderWidth = renderWidth;
	area.renderHeight = renderHeight;
	area.scissorX = viewX;
	area.scissorY = trueHeight - bars - (viewHeight + viewY);
	area.scissorWidth = viewWidth;
	area.scissorHeight = viewHeight;
	area.valid = true;
	return area;
}

void gl_GLESInternalSetSceneViewport(const FGLESViewArea &area)
{
	glViewport(area.renderX, area.renderY, area.renderWidth, area.renderHeight);
	glScissor(area.scissorX, area.scissorY, area.scissorWidth, area.scissorHeight);
	glEnable(GL_SCISSOR_TEST);
}

bool gl_GLESInternalFillStencil(int width, int height, GLuint stencilReference,
	GLuint stencilMask, const float color[4])
{
	if (width <= 0 || height <= 0 || color == nullptr ||
		!gl_GLESInternalPresentInitialize() || Presenter.fillColor < 0)
		return false;

	GLint viewport[4] = {};
	GLint program = 0;
	GLint vertexArray = 0;
	GLint depthFunction = GL_LESS;
	GLint stencilFunction = GL_ALWAYS;
	GLint oldStencilReference = 0;
	GLint stencilValueMask = 0xffffffffu;
	GLint stencilWriteMask = 0xffffffffu;
	GLint stencilFail = GL_KEEP;
	GLint stencilDepthFail = GL_KEEP;
	GLint stencilDepthPass = GL_KEEP;
	GLboolean depthWrite = GL_TRUE;
	GLboolean colorWrite[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	glGetIntegerv(GL_VIEWPORT, viewport);
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertexArray);
	glGetIntegerv(GL_DEPTH_FUNC, &depthFunction);
	glGetIntegerv(GL_STENCIL_FUNC, &stencilFunction);
	glGetIntegerv(GL_STENCIL_REF, &oldStencilReference);
	glGetIntegerv(GL_STENCIL_VALUE_MASK, &stencilValueMask);
	glGetIntegerv(GL_STENCIL_WRITEMASK, &stencilWriteMask);
	glGetIntegerv(GL_STENCIL_FAIL, &stencilFail);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &stencilDepthFail);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &stencilDepthPass);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWrite);
	glGetBooleanv(GL_COLOR_WRITEMASK, colorWrite);
	const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean stencilEnabled = glIsEnabled(GL_STENCIL_TEST);
	const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
	const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
	const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);

	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0x00);
	glStencilFunc(GL_EQUAL, static_cast<GLint>(stencilReference), stencilMask);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glUseProgram(Presenter.fillProgram);
	glUniform4fv(Presenter.fillColor, 1, color);
	glBindVertexArray(Presenter.vertexArray);
	gl_GLES_GetProcTable().DrawArrays(GL_TRIANGLES, 0, 3);
	const GLenum drawError = gl_GLES_CheckErrors("portal termination fill");

	glBindVertexArray(static_cast<GLuint>(vertexArray));
	glUseProgram(static_cast<GLuint>(program));
	gl_GLESInternalInvalidateProgramBinding();
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
	glColorMask(colorWrite[0], colorWrite[1], colorWrite[2], colorWrite[3]);
	glStencilMask(static_cast<GLuint>(stencilWriteMask));
	glStencilFunc(static_cast<GLenum>(stencilFunction), oldStencilReference,
		static_cast<GLuint>(stencilValueMask));
	glStencilOp(static_cast<GLenum>(stencilFail), static_cast<GLenum>(stencilDepthFail),
		static_cast<GLenum>(stencilDepthPass));
	glDepthFunc(static_cast<GLenum>(depthFunction));
	glDepthMask(depthWrite);
	if (depthEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (stencilEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
	if (blendEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (scissorEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	if (cullEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	return drawError == GL_NO_ERROR;
}

void gl_GLESInternalPublishFrameContract(uint64_t frameNumber, double timeSeconds,
	const FGLESTargetDescriptor &target, const FGLESViewDescriptor &view, bool viewValid)
{
	FGLESViewDescriptor publishedView = view;
	if (!viewValid)
	{
		publishedView.viewportX = 0;
		publishedView.viewportY = 0;
		publishedView.viewportWidth = target.renderWidth;
		publishedView.viewportHeight = target.renderHeight;
	}
	FGLESFrameDescriptor frame = {};
	frame.frameNumber = frameNumber;
	frame.timeSeconds = timeSeconds;
	frame.targetWidth = target.renderWidth;
	frame.targetHeight = target.renderHeight;
	gl_GLES_SetFrameContract(frame, target, publishedView);
}

bool gl_GLESInternalCopyTargetToTexture(const FGLESTargetDescriptor &target, GLuint texture)
{
	if (target.resolveFramebuffer == 0 || texture == 0 ||
		target.renderWidth <= 0 || target.renderHeight <= 0)
		return false;

	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousTexture = 0;
	GLint previousTexture0 = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, target.resolveFramebuffer);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, texture);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
	target.renderWidth, target.renderHeight);
	glGenerateMipmap(GL_TEXTURE_2D);
	const GLenum copyError = glGetError();
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	return copyError == GL_NO_ERROR;
}

bool gl_GLESInternalReadTarget(const FGLESTargetDescriptor &target, unsigned char *rgba,
	int width, int height)
{
	if (rgba == nullptr || width <= 0 || height <= 0 ||
		target.renderWidth <= 0 || target.renderHeight <= 0)
		return false;
	const GLuint readFramebuffer = target.resolveFramebuffer != 0 ?
		target.resolveFramebuffer : target.framebuffer;
	if (readFramebuffer == 0)
		return false;

	const int sourceWidth = target.renderWidth;
	const int sourceHeight = target.renderHeight;
	std::vector<unsigned char> source(static_cast<size_t>(sourceWidth) * sourceHeight * 4);
	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousPackAlignment = 4;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, readFramebuffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, sourceWidth, sourceHeight, GL_RGBA, GL_UNSIGNED_BYTE, source.data());
	const GLenum readbackError = gl_GLES_CheckErrors("screenshot readback");
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	if (readbackError != GL_NO_ERROR)
		return false;

	for (int y = 0; y < height; ++y)
	{
		const int sourceY = (y * sourceHeight) / height;
		for (int x = 0; x < width; ++x)
		{
			const int sourceX = (x * sourceWidth) / width;
			const size_t sourceOffset = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
			const size_t destinationOffset = (static_cast<size_t>(y) * width + x) * 4;
			rgba[destinationOffset + 0] = source[sourceOffset + 0];
			rgba[destinationOffset + 1] = source[sourceOffset + 1];
			rgba[destinationOffset + 2] = source[sourceOffset + 2];
			rgba[destinationOffset + 3] = source[sourceOffset + 3];
		}
	}
	return true;
}

bool gl_GLESInternalWriteSavePic(FILE *file, const FGLESTargetDescriptor &target,
	int width, int height)
{
	if (file == nullptr || width <= 0 || height <= 0 ||
		target.renderWidth <= 0 || target.renderHeight <= 0)
		return false;
	const GLuint readFramebuffer = target.resolveFramebuffer != 0 ?
		target.resolveFramebuffer : target.framebuffer;
	if (readFramebuffer == 0)
		return false;

	const int sourceWidth = target.renderWidth;
	const int sourceHeight = target.renderHeight;
	std::vector<BYTE> rgba(static_cast<size_t>(sourceWidth) * sourceHeight * 4);
	std::vector<BYTE> rgb(static_cast<size_t>(width) * height * 3);
	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousPackAlignment = 4;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);
	glBindFramebuffer(GL_FRAMEBUFFER, readFramebuffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, sourceWidth, sourceHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	const GLenum readbackError = gl_GLES_CheckErrors("save picture readback");
	glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	if (readbackError != GL_NO_ERROR) return false;

	for (int y = 0; y < height; ++y)
	{
		const int sourceY = (y * sourceHeight) / height;
		for (int x = 0; x < width; ++x)
		{
			const int sourceX = (x * sourceWidth) / width;
			const size_t source = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
			const size_t destination = (static_cast<size_t>(y) * width + x) * 3;
			rgb[destination + 0] = rgba[source + 0];
			rgb[destination + 1] = rgba[source + 1];
			rgb[destination + 2] = rgba[source + 2];
		}
	}

	return M_CreatePNG(file, rgb.data() + static_cast<size_t>(height - 1) * width * 3,
		nullptr, SS_RGB, width, height, -width * 3);
}

#endif
