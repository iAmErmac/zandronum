#include "gl/system/gl_gles_context.h"

#include <stdio.h>
#include <string.h>

#include "gl/system/gl_gles_targets.h"

namespace
{
	FGLESProcTable Procedures = {};
	FGLESContextInfo ContextInfo = {};
	FGLESHostCallbacks HostCallbacks = {};
	FGLESTargetDescriptor HostTarget = {};
	FGLESFrameDescriptor LastFrame = {};
	FGLESTargetDescriptor LastTarget = {};
	FGLESViewDescriptor LastView = {};
	bool ContextReady = false;
	bool FrameReady = false;
	bool HostTargetReady = false;
	bool PresentationUnavailableReported = false;

	static void ResetContextState()
	{
		Procedures = {};
		ContextInfo = {};
		HostTarget = {};
		LastFrame = {};
		LastTarget = {};
		LastView = {};
		ContextReady = false;
		FrameReady = false;
		HostTargetReady = false;
		PresentationUnavailableReported = false;
	}

	static void LogMessage(const char *message)
	{
		if (HostCallbacks.log != nullptr)
			HostCallbacks.log(HostCallbacks.userData, message);
	}

	static bool IsInvalidProc(void *proc)
	{
		return proc == nullptr || proc == reinterpret_cast<void *>(1) ||
			proc == reinterpret_cast<void *>(2) || proc == reinterpret_cast<void *>(3) ||
			proc == reinterpret_cast<void *>(-1);
	}

	static bool HasRequiredProcedures()
	{
		return Procedures.GetError != nullptr && Procedures.GetString != nullptr &&
			Procedures.GetIntegerv != nullptr && Procedures.GenFramebuffers != nullptr &&
			Procedures.DeleteFramebuffers != nullptr && Procedures.BindFramebuffer != nullptr &&
			Procedures.FramebufferTexture2D != nullptr && Procedures.CheckFramebufferStatus != nullptr &&
			Procedures.GenRenderbuffers != nullptr && Procedures.DeleteRenderbuffers != nullptr &&
			Procedures.BindRenderbuffer != nullptr && Procedures.RenderbufferStorage != nullptr &&
			Procedures.FramebufferRenderbuffer != nullptr && Procedures.CreateShader != nullptr &&
			Procedures.ShaderSource != nullptr && Procedures.CompileShader != nullptr &&
			Procedures.GetShaderiv != nullptr && Procedures.GetShaderInfoLog != nullptr &&
			Procedures.DeleteShader != nullptr && Procedures.CreateProgram != nullptr &&
			Procedures.AttachShader != nullptr && Procedures.LinkProgram != nullptr &&
			Procedures.GetProgramiv != nullptr && Procedures.GetProgramInfoLog != nullptr &&
			Procedures.DeleteProgram != nullptr && Procedures.ActiveTexture != nullptr &&
			Procedures.BindTexture != nullptr && Procedures.GenTextures != nullptr &&
			Procedures.DeleteTextures != nullptr && Procedures.TexParameteri != nullptr &&
			Procedures.TexImage2D != nullptr && Procedures.UseProgram != nullptr &&
			Procedures.GetUniformLocation != nullptr && Procedures.Uniform1i != nullptr &&
			Procedures.Uniform1f != nullptr && Procedures.Uniform4f != nullptr &&
			Procedures.UniformMatrix4fv != nullptr && Procedures.GenVertexArrays != nullptr &&
			Procedures.DeleteVertexArrays != nullptr && Procedures.BindVertexArray != nullptr &&
			Procedures.GenBuffers != nullptr && Procedures.DeleteBuffers != nullptr &&
			Procedures.BindBuffer != nullptr && Procedures.BufferData != nullptr &&
			Procedures.EnableVertexAttribArray != nullptr && Procedures.VertexAttribPointer != nullptr &&
			Procedures.DrawElements != nullptr && Procedures.Viewport != nullptr &&
			Procedures.Scissor != nullptr && Procedures.ClearColor != nullptr &&
			Procedures.Clear != nullptr && Procedures.Enable != nullptr &&
			Procedures.Disable != nullptr && Procedures.BlendFunc != nullptr &&
			Procedures.DepthFunc != nullptr && Procedures.DepthMask != nullptr &&
			Procedures.BlendEquation != nullptr && Procedures.BindSampler != nullptr &&
			Procedures.GenSamplers != nullptr && Procedures.DeleteSamplers != nullptr &&
			Procedures.SamplerParameteri != nullptr && Procedures.BufferSubData != nullptr &&
			Procedures.ClearDepthf != nullptr && Procedures.ClearStencil != nullptr &&
			Procedures.ColorMask != nullptr && Procedures.CopyTexSubImage2D != nullptr &&
			Procedures.CullFace != nullptr && Procedures.DepthRangef != nullptr &&
			Procedures.FrontFace != nullptr && Procedures.GenerateMipmap != nullptr &&
			Procedures.PixelStorei != nullptr && Procedures.ReadPixels != nullptr &&
			Procedures.StencilFunc != nullptr && Procedures.StencilMask != nullptr &&
			Procedures.StencilOp != nullptr && Procedures.TexSubImage2D != nullptr &&
			Procedures.Uniform3f != nullptr && Procedures.Uniform3fv != nullptr &&
			Procedures.Uniform3i != nullptr && Procedures.Uniform4fv != nullptr;
	}

	static int ParseVersion(const char *version, bool *isGLES)
	{
		if (version == nullptr) return 0;
		int major = 0;
		int minor = 0;
		if (sscanf(version, "OpenGL ES %d.%d", &major, &minor) == 2)
		{
			if (isGLES != nullptr) *isGLES = true;
			return major * 100 + minor;
		}
		if (sscanf(version, "%d.%d", &major, &minor) == 2)
		{
			if (isGLES != nullptr) *isGLES = false;
			return major * 100 + minor;
		}
		return 0;
	}

	static bool ExtensionPresent(const char *name)
	{
		if (name == nullptr || Procedures.GetString == nullptr) return false;
		if (Procedures.GetStringi != nullptr)
		{
			GLint count = 0;
			Procedures.GetIntegerv(GL_NUM_EXTENSIONS, &count);
			for (GLint index = 0; index < count; ++index)
			{
				const GLubyte *extension = Procedures.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index));
				if (extension != nullptr && strcmp(reinterpret_cast<const char *>(extension), name) == 0)
					return true;
			}
			return false;
		}
		const GLubyte *extensions = Procedures.GetString(GL_EXTENSIONS);
		if (extensions == nullptr) return false;
		const char *cursor = reinterpret_cast<const char *>(extensions);
		const size_t nameLength = strlen(name);
		while (*cursor != '\0')
		{
			while (*cursor == ' ') ++cursor;
			const char *end = strchr(cursor, ' ');
			const size_t length = end == nullptr ? strlen(cursor) : static_cast<size_t>(end - cursor);
			if (length == nameLength && strncmp(cursor, name, length) == 0) return true;
			if (end == nullptr) break;
			cursor = end + 1;
		}
		return false;
	}

	static bool FillContextInfo(int minimumMajor, int minimumMinor, bool requireGLES,
		FGLESContextInfo &info)
	{
		const char *version = reinterpret_cast<const char *>(Procedures.GetString(GL_VERSION));
		const char *vendor = reinterpret_cast<const char *>(Procedures.GetString(GL_VENDOR));
		const char *renderer = reinterpret_cast<const char *>(Procedures.GetString(GL_RENDERER));
		const char *shadingLanguage = reinterpret_cast<const char *>(Procedures.GetString(GL_SHADING_LANGUAGE_VERSION));
		bool isGLES = false;
		const int parsedVersion = ParseVersion(version, &isGLES);
		GLint major = 0;
		GLint minor = 0;
		Procedures.GetIntegerv(GL_MAJOR_VERSION, &major);
		Procedures.GetIntegerv(GL_MINOR_VERSION, &minor);
		if (major <= 0 || minor < 0)
		{
			major = parsedVersion / 100;
			minor = parsedVersion % 100;
		}
		info.majorVersion = major;
		info.minorVersion = minor;
		info.vendor = vendor;
		info.renderer = renderer;
		info.version = version;
		info.shadingLanguageVersion = shadingLanguage;
		info.isGLES = isGLES;
		Procedures.GetIntegerv(GL_MAX_TEXTURE_SIZE, &info.maxTextureSize);
		Procedures.GetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &info.maxTextureUnits);
		if (isGLES)
		{
			Procedures.GetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &info.maxVertexUniformVectors);
			Procedures.GetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &info.maxFragmentUniformVectors);
		}
		else
		{
			Procedures.GetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS, &info.maxVertexUniformVectors);
			Procedures.GetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &info.maxFragmentUniformVectors);
		}
		info.extensionCount = 0;
		if (Procedures.GetStringi != nullptr)
			Procedures.GetIntegerv(GL_NUM_EXTENSIONS, &info.extensionCount);
		info.hasDepthStencil = true;
		info.hasMultisample = Procedures.RenderbufferStorageMultisample != nullptr &&
			Procedures.BlitFramebuffer != nullptr;
		info.hasDebugLabels = ExtensionPresent("GL_KHR_debug") || ExtensionPresent("GL_ARB_debug_output");
		info.hasAnisotropicFiltering = ExtensionPresent("GL_EXT_texture_filter_anisotropic");
		info.hasMultiview = ExtensionPresent("GL_OVR_multiview2") ||
			ExtensionPresent("GL_ANDROID_extension_pack_es31a");
		if (requireGLES && !isGLES) return false;
		if (major < minimumMajor || (major == minimumMajor && minor < minimumMinor)) return false;
		return info.maxTextureSize > 0 && info.maxTextureUnits > 0;
	}

	static void LoadProcedure(FGLESProcResolver resolver, const char *name, void **slot)
	{
		void *proc = resolver != nullptr ? resolver(name) : nullptr;
		*slot = IsInvalidProc(proc) ? nullptr : proc;
	}

#define LOAD_GLES_PROCEDURE(name) LoadProcedure(resolver, "gl" #name, reinterpret_cast<void **>(&Procedures.name))

	static void LoadAllProcedures(FGLESProcResolver resolver)
	{
		LOAD_GLES_PROCEDURE(GetError);
		LOAD_GLES_PROCEDURE(GetString);
		LOAD_GLES_PROCEDURE(GetStringi);
		LOAD_GLES_PROCEDURE(GetIntegerv);
		LOAD_GLES_PROCEDURE(ActiveTexture);
		LOAD_GLES_PROCEDURE(BindTexture);
		LOAD_GLES_PROCEDURE(GenTextures);
		LOAD_GLES_PROCEDURE(DeleteTextures);
		LOAD_GLES_PROCEDURE(TexParameteri);
		LOAD_GLES_PROCEDURE(TexImage2D);
		LOAD_GLES_PROCEDURE(GenFramebuffers);
		LOAD_GLES_PROCEDURE(DeleteFramebuffers);
		LOAD_GLES_PROCEDURE(BindFramebuffer);
		LOAD_GLES_PROCEDURE(FramebufferTexture2D);
		LOAD_GLES_PROCEDURE(CheckFramebufferStatus);
		LOAD_GLES_PROCEDURE(GenRenderbuffers);
		LOAD_GLES_PROCEDURE(DeleteRenderbuffers);
		LOAD_GLES_PROCEDURE(BindRenderbuffer);
		LOAD_GLES_PROCEDURE(RenderbufferStorage);
		LOAD_GLES_PROCEDURE(RenderbufferStorageMultisample);
		LOAD_GLES_PROCEDURE(FramebufferRenderbuffer);
		LOAD_GLES_PROCEDURE(BlitFramebuffer);
		LOAD_GLES_PROCEDURE(CreateShader);
		LOAD_GLES_PROCEDURE(ShaderSource);
		LOAD_GLES_PROCEDURE(CompileShader);
		LOAD_GLES_PROCEDURE(GetShaderiv);
		LOAD_GLES_PROCEDURE(GetShaderInfoLog);
		LOAD_GLES_PROCEDURE(DeleteShader);
		LOAD_GLES_PROCEDURE(CreateProgram);
		LOAD_GLES_PROCEDURE(AttachShader);
		LOAD_GLES_PROCEDURE(BindAttribLocation);
		LOAD_GLES_PROCEDURE(LinkProgram);
		LOAD_GLES_PROCEDURE(GetProgramiv);
		LOAD_GLES_PROCEDURE(GetProgramInfoLog);
		LOAD_GLES_PROCEDURE(DeleteProgram);
		LOAD_GLES_PROCEDURE(UseProgram);
		LOAD_GLES_PROCEDURE(GetUniformLocation);
		LOAD_GLES_PROCEDURE(Uniform1i);
		LOAD_GLES_PROCEDURE(Uniform1f);
		LOAD_GLES_PROCEDURE(Uniform4f);
		LOAD_GLES_PROCEDURE(UniformMatrix4fv);
		LOAD_GLES_PROCEDURE(GenVertexArrays);
		LOAD_GLES_PROCEDURE(DeleteVertexArrays);
		LOAD_GLES_PROCEDURE(BindVertexArray);
		LOAD_GLES_PROCEDURE(GenBuffers);
		LOAD_GLES_PROCEDURE(DeleteBuffers);
		LOAD_GLES_PROCEDURE(BindBuffer);
		LOAD_GLES_PROCEDURE(BufferData);
		LOAD_GLES_PROCEDURE(EnableVertexAttribArray);
		LOAD_GLES_PROCEDURE(DisableVertexAttribArray);
		LOAD_GLES_PROCEDURE(VertexAttribPointer);
		LOAD_GLES_PROCEDURE(DrawArrays);
		LOAD_GLES_PROCEDURE(Viewport);
		LOAD_GLES_PROCEDURE(Scissor);
		LOAD_GLES_PROCEDURE(ClearColor);
		LOAD_GLES_PROCEDURE(Clear);
		LOAD_GLES_PROCEDURE(Enable);
		LOAD_GLES_PROCEDURE(Disable);
		LOAD_GLES_PROCEDURE(BlendFunc);
		LOAD_GLES_PROCEDURE(DepthFunc);
		LOAD_GLES_PROCEDURE(DepthMask);
		LOAD_GLES_PROCEDURE(BlendEquation);
		LOAD_GLES_PROCEDURE(BindSampler);
		LOAD_GLES_PROCEDURE(GenSamplers);
		LOAD_GLES_PROCEDURE(DeleteSamplers);
		LOAD_GLES_PROCEDURE(SamplerParameteri);
		LOAD_GLES_PROCEDURE(BufferSubData);
		LOAD_GLES_PROCEDURE(ClearDepthf);
		if (Procedures.ClearDepthf == nullptr)
			LoadProcedure(resolver, "glClearDepth", reinterpret_cast<void **>(&Procedures.ClearDepthf));
		LOAD_GLES_PROCEDURE(ClearStencil);
		LOAD_GLES_PROCEDURE(ColorMask);
		LOAD_GLES_PROCEDURE(CopyTexSubImage2D);
		LOAD_GLES_PROCEDURE(CullFace);
		LOAD_GLES_PROCEDURE(DepthRangef);
		if (Procedures.DepthRangef == nullptr)
			LoadProcedure(resolver, "glDepthRange", reinterpret_cast<void **>(&Procedures.DepthRangef));
		LOAD_GLES_PROCEDURE(DrawElements);
		LOAD_GLES_PROCEDURE(FrontFace);
		LOAD_GLES_PROCEDURE(GenerateMipmap);
		LOAD_GLES_PROCEDURE(PixelStorei);
		LOAD_GLES_PROCEDURE(ReadPixels);
		LOAD_GLES_PROCEDURE(StencilFunc);
		LOAD_GLES_PROCEDURE(StencilMask);
		LOAD_GLES_PROCEDURE(StencilOp);
		LOAD_GLES_PROCEDURE(TexSubImage2D);
		LOAD_GLES_PROCEDURE(Uniform3f);
		LOAD_GLES_PROCEDURE(Uniform3fv);
		LOAD_GLES_PROCEDURE(Uniform3i);
		LOAD_GLES_PROCEDURE(Uniform4fv);
	}

#undef LOAD_GLES_PROCEDURE

#if defined(__ANDROID__)
	static void InstallDirectProcedures()
	{
		Procedures.GetError = &glGetError;
		Procedures.GetString = &glGetString;
		Procedures.GetStringi = &glGetStringi;
		Procedures.GetIntegerv = &glGetIntegerv;
		Procedures.ActiveTexture = &glActiveTexture;
		Procedures.BindTexture = &glBindTexture;
		Procedures.GenTextures = &glGenTextures;
		Procedures.DeleteTextures = &glDeleteTextures;
		Procedures.TexParameteri = &glTexParameteri;
		Procedures.TexImage2D = &glTexImage2D;
		Procedures.GenFramebuffers = &glGenFramebuffers;
		Procedures.DeleteFramebuffers = &glDeleteFramebuffers;
		Procedures.BindFramebuffer = &glBindFramebuffer;
		Procedures.FramebufferTexture2D = &glFramebufferTexture2D;
		Procedures.CheckFramebufferStatus = &glCheckFramebufferStatus;
		Procedures.GenRenderbuffers = &glGenRenderbuffers;
		Procedures.DeleteRenderbuffers = &glDeleteRenderbuffers;
		Procedures.BindRenderbuffer = &glBindRenderbuffer;
		Procedures.RenderbufferStorage = &glRenderbufferStorage;
		Procedures.RenderbufferStorageMultisample = &glRenderbufferStorageMultisample;
		Procedures.FramebufferRenderbuffer = &glFramebufferRenderbuffer;
		Procedures.BlitFramebuffer = &glBlitFramebuffer;
		Procedures.CreateShader = &glCreateShader;
		Procedures.ShaderSource = &glShaderSource;
		Procedures.CompileShader = &glCompileShader;
		Procedures.GetShaderiv = &glGetShaderiv;
		Procedures.GetShaderInfoLog = &glGetShaderInfoLog;
		Procedures.DeleteShader = &glDeleteShader;
		Procedures.CreateProgram = &glCreateProgram;
		Procedures.AttachShader = &glAttachShader;
		Procedures.BindAttribLocation = &glBindAttribLocation;
		Procedures.LinkProgram = &glLinkProgram;
		Procedures.GetProgramiv = &glGetProgramiv;
		Procedures.GetProgramInfoLog = &glGetProgramInfoLog;
		Procedures.DeleteProgram = &glDeleteProgram;
		Procedures.UseProgram = &glUseProgram;
		Procedures.GetUniformLocation = &glGetUniformLocation;
		Procedures.Uniform1i = &glUniform1i;
		Procedures.Uniform1f = &glUniform1f;
		Procedures.Uniform4f = &glUniform4f;
		Procedures.UniformMatrix4fv = &glUniformMatrix4fv;
		Procedures.GenVertexArrays = &glGenVertexArrays;
		Procedures.DeleteVertexArrays = &glDeleteVertexArrays;
		Procedures.BindVertexArray = &glBindVertexArray;
		Procedures.GenBuffers = &glGenBuffers;
		Procedures.DeleteBuffers = &glDeleteBuffers;
		Procedures.BindBuffer = &glBindBuffer;
		Procedures.BufferData = &glBufferData;
		Procedures.EnableVertexAttribArray = &glEnableVertexAttribArray;
		Procedures.DisableVertexAttribArray = &glDisableVertexAttribArray;
		Procedures.VertexAttribPointer = &glVertexAttribPointer;
		Procedures.DrawArrays = &glDrawArrays;
		Procedures.Viewport = &glViewport;
		Procedures.Scissor = &glScissor;
		Procedures.ClearColor = &glClearColor;
		Procedures.Clear = &glClear;
		Procedures.Enable = &glEnable;
		Procedures.Disable = &glDisable;
		Procedures.BlendFunc = &glBlendFunc;
		Procedures.DepthFunc = &glDepthFunc;
		Procedures.DepthMask = &glDepthMask;
		Procedures.BlendEquation = &glBlendEquation;
		Procedures.BindSampler = &glBindSampler;
		Procedures.GenSamplers = &glGenSamplers;
		Procedures.DeleteSamplers = &glDeleteSamplers;
		Procedures.SamplerParameteri = &glSamplerParameteri;
		Procedures.BufferSubData = &glBufferSubData;
		Procedures.ClearDepthf = &glClearDepthf;
		Procedures.ClearStencil = &glClearStencil;
		Procedures.ColorMask = &glColorMask;
		Procedures.CopyTexSubImage2D = &glCopyTexSubImage2D;
		Procedures.CullFace = &glCullFace;
		Procedures.DepthRangef = &glDepthRangef;
		Procedures.DrawElements = &glDrawElements;
		Procedures.FrontFace = &glFrontFace;
		Procedures.GenerateMipmap = &glGenerateMipmap;
		Procedures.PixelStorei = &glPixelStorei;
		Procedures.ReadPixels = &glReadPixels;
		Procedures.StencilFunc = &glStencilFunc;
		Procedures.StencilMask = &glStencilMask;
		Procedures.StencilOp = &glStencilOp;
		Procedures.TexSubImage2D = &glTexSubImage2D;
		Procedures.Uniform3f = &glUniform3f;
		Procedures.Uniform3fv = &glUniform3fv;
		Procedures.Uniform3i = &glUniform3i;
		Procedures.Uniform4fv = &glUniform4fv;
	}
#endif
}

bool gl_GLES_LoadContext(FGLESProcResolver resolver, int minimumMajor, int minimumMinor,
	bool requireGLES, FGLESContextInfo *info)
{
	ResetContextState();
	LoadAllProcedures(resolver);
	if (!HasRequiredProcedures())
	{
		gl_GLES_Report("context", "required GLES procedure is unavailable");
		return false;
	}
	FGLESContextInfo candidate = {};
	if (!FillContextInfo(minimumMajor, minimumMinor, requireGLES, candidate))
	{
		char message[256];
		snprintf(message, sizeof(message), "context rejected: %s %d.%d", candidate.isGLES ? "GLES" : "OpenGL",
			candidate.majorVersion, candidate.minorVersion);
		gl_GLES_Report("context", message);
		return false;
	}
	ContextInfo = candidate;
	ContextReady = true;
	if (info != nullptr) *info = ContextInfo;
	return true;
}

bool gl_GLES_InstallDirectContext(int minimumMajor, int minimumMinor, FGLESContextInfo *info)
{
	ResetContextState();
#if defined(__ANDROID__)
	InstallDirectProcedures();
	if (!HasRequiredProcedures()) return false;
	FGLESContextInfo candidate = {};
	if (!FillContextInfo(minimumMajor, minimumMinor, true, candidate)) return false;
	ContextInfo = candidate;
	ContextReady = true;
	if (info != nullptr) *info = ContextInfo;
	return true;
#else
	(void)minimumMajor;
	(void)minimumMinor;
	(void)info;
	return false;
#endif
}

void gl_GLES_ShutdownContext(bool preserveHostCallbacks)
{
	ResetContextState();
	if (!preserveHostCallbacks)
		HostCallbacks = {};
}

bool gl_GLES_HasContext()
{
	return ContextReady;
}

const FGLESProcTable &gl_GLES_GetProcTable()
{
	return Procedures;
}

const FGLESContextInfo &gl_GLES_GetContextInfo()
{
	return ContextInfo;
}

const char *gl_GLES_GetShaderVersion()
{
	return ContextInfo.isGLES ? "320 es" : "330 core";
}

bool gl_GLES_HasExtension(const char *name)
{
	return ContextReady && ExtensionPresent(name);
}

void gl_GLES_RegisterHostCallbacks(const FGLESHostCallbacks *callbacks)
{
	HostCallbacks = callbacks != nullptr ? *callbacks : FGLESHostCallbacks{};
}

void gl_GLES_PrepareHostFrame()
{
	if (HostCallbacks.prepareFrame != nullptr)
		HostCallbacks.prepareFrame(HostCallbacks.userData);
}

bool gl_GLES_SetHostTarget(const FGLESTargetDescriptor &target)
{
	if (target.renderWidth <= 0 || target.renderHeight <= 0 || !target.hostOwnsPresentation)
	{
		HostTarget = {};
		HostTargetReady = false;
		gl_GLES_Report("target", "host supplied an invalid presentation target");
		return false;
	}
	HostTarget = target;
	HostTargetReady = true;
	return true;
}

void gl_GLES_InvalidateHostTarget()
{
	HostTarget = {};
	HostTargetReady = false;
}

bool gl_GLES_GetHostTarget(FGLESTargetDescriptor *target)
{
	if (!HostTargetReady || target == nullptr)
		return false;
	*target = HostTarget;
	return true;
}

void gl_GLES_Report(const char *stage, const char *message)
{
	char formatted[2048];
	snprintf(formatted, sizeof(formatted), "GLES %s: %s", stage != nullptr ? stage : "diagnostic",
		message != nullptr ? message : "unknown error");
	LogMessage(formatted);
}

GLenum gl_GLES_CheckErrors(const char *stage)
{
	if (!ContextReady || Procedures.GetError == nullptr) return GL_NO_ERROR;
	GLenum firstError = GL_NO_ERROR;
	for (;;)
	{
		const GLenum error = Procedures.GetError();
		if (error == GL_NO_ERROR) break;
		if (firstError == GL_NO_ERROR) firstError = error;
		char message[128];
		snprintf(message, sizeof(message), "%s reported 0x%04x", stage != nullptr ? stage : "operation", error);
		gl_GLES_Report("error", message);
	}
	return firstError;
}

void gl_GLES_SetFrameContract(const FGLESFrameDescriptor &frame,
	const FGLESTargetDescriptor &target, const FGLESViewDescriptor &view)
{
	LastTarget = target;
	LastView = view;
	LastFrame = frame;
	LastFrame.target = &LastTarget;
	LastFrame.view = &LastView;
	FrameReady = true;
}

bool gl_GLES_PresentFrame()
{
	if (!FrameReady || LastFrame.target != &LastTarget || LastFrame.view != &LastView ||
		LastFrame.targetWidth != LastTarget.renderWidth || LastFrame.targetHeight != LastTarget.renderHeight ||
		!LastTarget.hostOwnsPresentation || HostCallbacks.present == nullptr)
	{
		if (!PresentationUnavailableReported)
		{
			gl_GLES_Report("present", "host presentation callback or frame contract is unavailable");
			PresentationUnavailableReported = true;
		}
		return false;
	}
	PresentationUnavailableReported = false;
	const bool presented = HostCallbacks.present(HostCallbacks.userData);
	return presented;
}

const FGLESFrameDescriptor &gl_GLES_GetFrameContract()
{
	return LastFrame;
}
