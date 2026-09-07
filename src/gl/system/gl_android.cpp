#include "gl/system/gl_android.h"

#ifdef __ANDROID__

#include <GLES3/gl32.h>
#include <stdio.h>
#include <string.h>

#include "c_console.h"
#include "i_system.h"

static FAndroidGLESInfo Capabilities;
static bool CapabilitiesReady = false;
static bool BootstrapReady = false;
static bool NativeBackendEnabled = false;
static GLuint BootstrapProgram = 0;
static GLuint BootstrapBuffer = 0;
static GLuint BootstrapVertexArray = 0;
static unsigned int BootstrapFrame = 0;
static bool BootstrapPauseLogged = false;

static bool HasExtension(const char *name)
{
	for (GLint index = 0; index < Capabilities.extensionCount; ++index)
	{
		const char *extension = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, index));
		if (extension != NULL && strcmp(extension, name) == 0) return true;
	}
	return false;
}

static GLuint CompileShader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (!compiled)
	{
		char log[1024] = {};
		GLsizei length = 0;
		glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
		I_FatalError("Android GLES bootstrap shader failed: %s", log);
	}
	return shader;
}

static GLuint LinkBootstrapProgram()
{
	static const char *vertexSource =
		"#version 300 es\n"
		"layout(location = 0) in vec2 a_position;\n"
		"layout(location = 1) in vec3 a_color;\n"
		"out vec3 v_color;\n"
		"void main() { gl_Position = vec4(a_position, 0.0, 1.0); v_color = a_color; }\n";
	static const char *fragmentSource =
		"#version 300 es\n"
		"precision mediump float;\n"
		"in vec3 v_color;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"void main() { frag_color = vec4(v_color, 1.0); }\n";

	GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource);
	GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource);
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	if (!linked)
	{
		char log[1024] = {};
		GLsizei length = 0;
		glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
		I_FatalError("Android GLES bootstrap program failed: %s", log);
	}
	return program;
}

bool gl_AndroidNativeGLES_CollectCapabilities()
{
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	if (version == NULL || strncmp(version, "OpenGL ES ", 10) != 0)
		I_FatalError("Android GLES context did not report an OpenGL ES version.");

	int major = 0, minor = 0;
	if (sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2 || major < 3 || (major == 3 && minor < 2))
		I_FatalError("Android GLES 3.2 is required, got %s.", version);

	Capabilities.majorVersion = major;
	Capabilities.minorVersion = minor;
	Capabilities.vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
	Capabilities.renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
	Capabilities.version = version;
	Capabilities.shadingLanguageVersion = reinterpret_cast<const char *>(glGetString(GL_SHADING_LANGUAGE_VERSION));
	Capabilities.hasVertexBuffers = true;
	Capabilities.hasVertexArrays = true;
	Capabilities.hasUniformBuffers = true;
	Capabilities.hasFramebuffers = true;
	Capabilities.hasDepthStencil = true;
	Capabilities.hasBufferMapping = false;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &Capabilities.maxTextureSize);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &Capabilities.maxTextureUnits);
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &Capabilities.maxVertexUniformVectors);
	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &Capabilities.maxFragmentUniformVectors);
	glGetIntegerv(GL_NUM_EXTENSIONS, &Capabilities.extensionCount);
	if (Capabilities.maxTextureSize <= 0 || Capabilities.maxTextureUnits <= 0 ||
		Capabilities.maxVertexUniformVectors <= 0 || Capabilities.maxFragmentUniformVectors <= 0 ||
		Capabilities.extensionCount < 0)
		I_FatalError("Android GLES capability enumeration returned invalid limits.");
	Capabilities.hasAnisotropicFiltering = HasExtension("GL_EXT_texture_filter_anisotropic");
	Capabilities.hasAstcCompression = HasExtension("GL_KHR_texture_compression_astc_ldr");
	Capabilities.hasEtc2Compression = true;
	Capabilities.hasDebugLabels = HasExtension("GL_KHR_debug");
	Capabilities.hasMultiview = HasExtension("GL_OVR_multiview2") || HasExtension("GL_ANDROID_extension_pack_es31a");
	CapabilitiesReady = true;
	return true;
}

bool gl_AndroidNativeGLES_InitializeBootstrap(int width, int height)
{
	if (!CapabilitiesReady) return false;
	if (BootstrapProgram != 0) glDeleteProgram(BootstrapProgram);
	if (BootstrapBuffer != 0) glDeleteBuffers(1, &BootstrapBuffer);
	if (BootstrapVertexArray != 0) glDeleteVertexArrays(1, &BootstrapVertexArray);
	BootstrapProgram = LinkBootstrapProgram();
	glGenVertexArrays(1, &BootstrapVertexArray);
	glGenBuffers(1, &BootstrapBuffer);
	static const GLfloat vertices[] = {
		-0.72f, -0.58f, 0.95f, 0.25f, 0.18f,
		 0.72f, -0.58f, 0.18f, 0.85f, 0.32f,
		 0.00f,  0.72f, 0.20f, 0.42f, 0.95f
	};
	glBindVertexArray(BootstrapVertexArray);
	glBindBuffer(GL_ARRAY_BUFFER, BootstrapBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), reinterpret_cast<const void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(GLfloat), reinterpret_cast<const void *>(2 * sizeof(GLfloat)));
	glBindVertexArray(0);
	glViewport(0, 0, width, height);
	BootstrapReady = true;
	NativeBackendEnabled = true;
	return true;
}

void gl_AndroidNativeGLES_OnContextLost()
{
	BootstrapProgram = 0;
	BootstrapBuffer = 0;
	BootstrapVertexArray = 0;
	BootstrapReady = false;
	CapabilitiesReady = false;
	memset(&Capabilities, 0, sizeof(Capabilities));
}

bool gl_AndroidNativeGLES_OnContextRestored(int width, int height)
{
	if (!gl_AndroidNativeGLES_CollectCapabilities()) return false;
	return gl_AndroidNativeGLES_InitializeBootstrap(width, height);
}

void gl_AndroidNativeGLES_RenderBootstrap(int width, int height)
{
	if (!BootstrapReady)
	{
		if (!BootstrapPauseLogged)
		{
			BootstrapPauseLogged = true;
			Printf("Android GLES bootstrap paused while the surface is unavailable.\n");
		}
		return;
	}
	BootstrapPauseLogged = false;
	glViewport(0, 0, width, height);
	const float pulse = static_cast<float>(BootstrapFrame % 120) / 119.0f;
	glClearColor(0.025f, 0.045f + pulse * 0.03f, 0.075f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glUseProgram(BootstrapProgram);
	glBindVertexArray(BootstrapVertexArray);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);
	GLenum error = glGetError();
	if (error != GL_NO_ERROR)
		I_FatalError("Android GLES bootstrap produced GL error 0x%04x.", error);
	++BootstrapFrame;
	if (BootstrapFrame == 1 || (BootstrapFrame % 120) == 0)
		Printf("Android GLES bootstrap frame %u at %dx%d.\n", BootstrapFrame, width, height);
}

bool gl_AndroidNativeGLES_IsActive()
{
	return NativeBackendEnabled;
}

const FAndroidGLESInfo &gl_AndroidNativeGLES_GetCapabilities()
{
	return Capabilities;
}

void gl_AndroidNativeGLES_PrintStartupLog()
{
	if (!CapabilitiesReady) return;
	Printf("GL_VENDOR: %s\n", Capabilities.vendor);
	Printf("GL_RENDERER: %s\n", Capabilities.renderer);
	Printf("GL_VERSION: %s\n", Capabilities.version);
	Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", Capabilities.shadingLanguageVersion);
	Printf("GLES capabilities: ES %d.%d, max texture %d, texture units %d, extensions %d.\n",
		Capabilities.majorVersion, Capabilities.minorVersion, Capabilities.maxTextureSize,
		Capabilities.maxTextureUnits, Capabilities.extensionCount);
	Printf("GLES core: buffers=%s, arrays=%s, uniform-buffers=%s, framebuffers=%s, depth-stencil=%s, buffer-mapping=%s.\n",
		Capabilities.hasVertexBuffers ? "yes" : "no",
		Capabilities.hasVertexArrays ? "yes" : "no",
		Capabilities.hasUniformBuffers ? "yes" : "no",
		Capabilities.hasFramebuffers ? "yes" : "no",
		Capabilities.hasDepthStencil ? "yes" : "no",
		Capabilities.hasBufferMapping ? "yes" : "deferred");
	Printf("GLES optional: ETC2=%s, anisotropy=%s, ASTC=%s, debug=%s, multiview=%s.\n",
		Capabilities.hasEtc2Compression ? "yes" : "no",
		Capabilities.hasAnisotropicFiltering ? "yes" : "no",
		Capabilities.hasAstcCompression ? "yes" : "no",
		Capabilities.hasDebugLabels ? "yes" : "no",
		Capabilities.hasMultiview ? "yes" : "no");
	DPrintf("GLES extension list follows (%d entries):\n", Capabilities.extensionCount);
	for (GLint index = 0; index < Capabilities.extensionCount; ++index)
		DPrintf("  %s\n", glGetStringi(GL_EXTENSIONS, index));
}

#endif
