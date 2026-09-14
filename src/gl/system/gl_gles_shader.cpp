#include "gl/system/gl_gles_shader.h"

#include <stdio.h>
#include <string.h>

#include "gl/system/gl_gles_context.h"

namespace
{
	void ResetLog(char *log, int logSize)
	{
		if (log != nullptr && logSize > 0) log[0] = '\0';
	}

	void CopyLog(char *destination, int destinationSize, const char *label,
		const char *sourceLog)
	{
		if (destination == nullptr || destinationSize <= 0) return;
		const char *safeLabel = label != nullptr ? label : "shader";
		const char *safeSourceLog = sourceLog != nullptr ? sourceLog : "no driver log";
		snprintf(destination, static_cast<size_t>(destinationSize), "%s: %s", safeLabel, safeSourceLog);
		destination[destinationSize - 1] = '\0';
	}

	void GetShaderLog(const FGLESProcTable &gles, GLuint shader, char *log, int logSize,
		const char *label)
	{
		char driverLog[1024] = {};
		GLsizei length = 0;
		gles.GetShaderInfoLog(shader, sizeof(driverLog) - 1, &length, driverLog);
		CopyLog(log, logSize, label, driverLog);
	}

	void GetProgramLog(const FGLESProcTable &gles, GLuint program, char *log, int logSize,
		const char *label)
	{
		char driverLog[1024] = {};
		GLsizei length = 0;
		gles.GetProgramInfoLog(program, sizeof(driverLog) - 1, &length, driverLog);
		CopyLog(log, logSize, label, driverLog);
	}
}

GLuint gl_GLES_CompileShader(GLenum type, const char *source, const char *label,
	char *log, int logSize)
{
	ResetLog(log, logSize);
	if (source == nullptr || !gl_GLES_HasContext())
	{
		CopyLog(log, logSize, label, "no active GLES context or source");
		return 0;
	}
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	GLuint shader = gles.CreateShader(type);
	if (shader == 0)
	{
		CopyLog(log, logSize, label, "glCreateShader returned zero");
		return 0;
	}
	const GLchar *sources[] = { source };
	gles.ShaderSource(shader, 1, sources, nullptr);
	gles.CompileShader(shader);
	GLint compiled = GL_FALSE;
	gles.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if (compiled == GL_FALSE)
	{
		GetShaderLog(gles, shader, log, logSize, label);
		gles.DeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint gl_GLES_LinkProgram(const char *vertexSource, const char *fragmentSource,
	const char *label, char *log, int logSize)
{
	ResetLog(log, logSize);
	const GLuint vertex = gl_GLES_CompileShader(GL_VERTEX_SHADER, vertexSource, label, log, logSize);
	if (vertex == 0) return 0;
	const GLuint fragment = gl_GLES_CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label, log, logSize);
	if (fragment == 0)
	{
		gl_GLES_GetProcTable().DeleteShader(vertex);
		return 0;
	}
	const FGLESProcTable &gles = gl_GLES_GetProcTable();
	const GLuint program = gles.CreateProgram();
	if (program == 0)
	{
		gles.DeleteShader(vertex);
		gles.DeleteShader(fragment);
		CopyLog(log, logSize, label, "glCreateProgram returned zero");
		return 0;
	}
	gles.AttachShader(program, vertex);
	gles.AttachShader(program, fragment);
	if (gles.BindAttribLocation != nullptr)
	{
		gles.BindAttribLocation(program, 0, "a_position");
		gles.BindAttribLocation(program, 1, "a_uv");
		gles.BindAttribLocation(program, 2, "a_color");
		gles.BindAttribLocation(program, 3, "a_normal");
		gles.BindAttribLocation(program, 4, "a_secondary");
	}
	gles.LinkProgram(program);
	GLint linked = GL_FALSE;
	gles.GetProgramiv(program, GL_LINK_STATUS, &linked);
	gles.DeleteShader(vertex);
	gles.DeleteShader(fragment);
	if (linked == GL_FALSE)
	{
		GetProgramLog(gles, program, log, logSize, label);
		gles.DeleteProgram(program);
		return 0;
	}
	return program;
}
