#ifndef ZANDRONUM_GL_GLES_SHADER_H
#define ZANDRONUM_GL_GLES_SHADER_H

#include "gl/system/gl_gles_api.h"

GLuint gl_GLES_CompileShader(GLenum type, const char *source, const char *label,
	char *log, int logSize);
GLuint gl_GLES_LinkProgram(const char *vertexSource, const char *fragmentSource,
	const char *label, char *log, int logSize);

#endif
