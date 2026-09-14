#ifndef ZANDRONUM_GL_GLES_API_H
#define ZANDRONUM_GL_GLES_API_H

#include <stddef.h>

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#else
#include <windows.h>
#include <GL/gl.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER 0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0 0x8CE0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif
#ifndef GL_NEAREST
#define GL_NEAREST 0x2600
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif
#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif
#ifndef GL_COLOR_BUFFER_BIT
#define GL_COLOR_BUFFER_BIT 0x00004000
#endif
#ifndef GL_RENDERBUFFER_BINDING
#define GL_RENDERBUFFER_BINDING 0x8CA7
#endif
#ifndef GL_MAX_SAMPLES
#define GL_MAX_SAMPLES 0x8D57
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif
#ifndef GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#endif
#ifndef GL_MAX_VERTEX_UNIFORM_VECTORS
#define GL_MAX_VERTEX_UNIFORM_VECTORS 0x8DFB
#endif
#ifndef GL_MAX_FRAGMENT_UNIFORM_VECTORS
#define GL_MAX_FRAGMENT_UNIFORM_VECTORS 0x8DFD
#endif
#ifndef GL_MAX_VERTEX_UNIFORM_COMPONENTS
#define GL_MAX_VERTEX_UNIFORM_COMPONENTS 0x8B4A
#endif
#ifndef GL_MAX_FRAGMENT_UNIFORM_COMPONENTS
#define GL_MAX_FRAGMENT_UNIFORM_COMPONENTS 0x8B49
#endif
#ifndef GL_MAJOR_VERSION
#define GL_MAJOR_VERSION 0x821B
#endif
#ifndef GL_MINOR_VERSION
#define GL_MINOR_VERSION 0x821C
#endif
#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
#define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_SHADING_LANGUAGE_VERSION
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#endif
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER 0x8892
#endif
#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
#ifndef GL_FLOAT
#define GL_FLOAT 0x1406
#endif
#ifndef GL_TRIANGLES
#define GL_TRIANGLES 0x0004
#endif

using FGLESGetError = GLenum (APIENTRY *)(void);
using FGLESGetString = const GLubyte *(APIENTRY *)(GLenum);
using FGLESGetStringi = const GLubyte *(APIENTRY *)(GLenum, GLuint);
using FGLESGetIntegerv = void (APIENTRY *)(GLenum, GLint *);
using FGLESActiveTexture = void (APIENTRY *)(GLenum);
using FGLESBindTexture = void (APIENTRY *)(GLenum, GLuint);
using FGLESGenTextures = void (APIENTRY *)(GLsizei, GLuint *);
using FGLESDeleteTextures = void (APIENTRY *)(GLsizei, const GLuint *);
using FGLESTexParameteri = void (APIENTRY *)(GLenum, GLenum, GLint);
using FGLESTexImage2D = void (APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using FGLESGenFramebuffers = void (APIENTRY *)(GLsizei, GLuint *);
using FGLESDeleteFramebuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using FGLESBindFramebuffer = void (APIENTRY *)(GLenum, GLuint);
using FGLESFramebufferTexture2D = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint, GLint);
using FGLESCheckFramebufferStatus = GLenum (APIENTRY *)(GLenum);
using FGLESGenRenderbuffers = void (APIENTRY *)(GLsizei, GLuint *);
using FGLESDeleteRenderbuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using FGLESBindRenderbuffer = void (APIENTRY *)(GLenum, GLuint);
using FGLESRenderbufferStorage = void (APIENTRY *)(GLenum, GLenum, GLsizei, GLsizei);
using FGLESRenderbufferStorageMultisample = void (APIENTRY *)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using FGLESFramebufferRenderbuffer = void (APIENTRY *)(GLenum, GLenum, GLenum, GLuint);
using FGLESBlitFramebuffer = void (APIENTRY *)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using FGLESCreateShader = GLuint (APIENTRY *)(GLenum);
using FGLESShaderSource = void (APIENTRY *)(GLuint, GLsizei, const GLchar *const *, const GLint *);
using FGLESCompileShader = void (APIENTRY *)(GLuint);
using FGLESGetShaderiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using FGLESGetShaderInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, GLchar *);
using FGLESDeleteShader = void (APIENTRY *)(GLuint);
using FGLESCreateProgram = GLuint (APIENTRY *)(void);
using FGLESAttachShader = void (APIENTRY *)(GLuint, GLuint);
using FGLESBindAttribLocation = void (APIENTRY *)(GLuint, GLuint, const GLchar *);
using FGLESLinkProgram = void (APIENTRY *)(GLuint);
using FGLESGetProgramiv = void (APIENTRY *)(GLuint, GLenum, GLint *);
using FGLESGetProgramInfoLog = void (APIENTRY *)(GLuint, GLsizei, GLsizei *, GLchar *);
using FGLESDeleteProgram = void (APIENTRY *)(GLuint);
using FGLESUseProgram = void (APIENTRY *)(GLuint);
using FGLESGetUniformLocation = GLint (APIENTRY *)(GLuint, const GLchar *);
using FGLESUniform1i = void (APIENTRY *)(GLint, GLint);
using FGLESUniform1f = void (APIENTRY *)(GLint, GLfloat);
using FGLESUniform4f = void (APIENTRY *)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
using FGLESUniformMatrix4fv = void (APIENTRY *)(GLint, GLsizei, GLboolean, const GLfloat *);
using FGLESGenVertexArrays = void (APIENTRY *)(GLsizei, GLuint *);
using FGLESDeleteVertexArrays = void (APIENTRY *)(GLsizei, const GLuint *);
using FGLESBindVertexArray = void (APIENTRY *)(GLuint);
using FGLESGenBuffers = void (APIENTRY *)(GLsizei, GLuint *);
using FGLESDeleteBuffers = void (APIENTRY *)(GLsizei, const GLuint *);
using FGLESBindBuffer = void (APIENTRY *)(GLenum, GLuint);
using FGLESBufferData = void (APIENTRY *)(GLenum, ptrdiff_t, const void *, GLenum);
using FGLESEnableVertexAttribArray = void (APIENTRY *)(GLuint);
using FGLESDisableVertexAttribArray = void (APIENTRY *)(GLuint);
using FGLESVertexAttribPointer = void (APIENTRY *)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
using FGLESDrawArrays = void (APIENTRY *)(GLenum, GLint, GLsizei);
using FGLESViewport = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using FGLESScissor = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei);
using FGLESClearColor = void (APIENTRY *)(GLfloat, GLfloat, GLfloat, GLfloat);
using FGLESClear = void (APIENTRY *)(GLbitfield);
using FGLESEnable = void (APIENTRY *)(GLenum);
using FGLESDisable = void (APIENTRY *)(GLenum);
using FGLESBlendFunc = void (APIENTRY *)(GLenum, GLenum);
using FGLESDepthFunc = void (APIENTRY *)(GLenum);
using FGLESDepthMask = void (APIENTRY *)(GLboolean);

struct FGLESProcTable
{
	FGLESGetError GetError;
	FGLESGetString GetString;
	FGLESGetStringi GetStringi;
	FGLESGetIntegerv GetIntegerv;
	FGLESActiveTexture ActiveTexture;
	FGLESBindTexture BindTexture;
	FGLESGenTextures GenTextures;
	FGLESDeleteTextures DeleteTextures;
	FGLESTexParameteri TexParameteri;
	FGLESTexImage2D TexImage2D;
	FGLESGenFramebuffers GenFramebuffers;
	FGLESDeleteFramebuffers DeleteFramebuffers;
	FGLESBindFramebuffer BindFramebuffer;
	FGLESFramebufferTexture2D FramebufferTexture2D;
	FGLESCheckFramebufferStatus CheckFramebufferStatus;
	FGLESGenRenderbuffers GenRenderbuffers;
	FGLESDeleteRenderbuffers DeleteRenderbuffers;
	FGLESBindRenderbuffer BindRenderbuffer;
	FGLESRenderbufferStorage RenderbufferStorage;
	FGLESRenderbufferStorageMultisample RenderbufferStorageMultisample;
	FGLESFramebufferRenderbuffer FramebufferRenderbuffer;
	FGLESBlitFramebuffer BlitFramebuffer;
	FGLESCreateShader CreateShader;
	FGLESShaderSource ShaderSource;
	FGLESCompileShader CompileShader;
	FGLESGetShaderiv GetShaderiv;
	FGLESGetShaderInfoLog GetShaderInfoLog;
	FGLESDeleteShader DeleteShader;
	FGLESCreateProgram CreateProgram;
	FGLESAttachShader AttachShader;
	FGLESBindAttribLocation BindAttribLocation;
	FGLESLinkProgram LinkProgram;
	FGLESGetProgramiv GetProgramiv;
	FGLESGetProgramInfoLog GetProgramInfoLog;
	FGLESDeleteProgram DeleteProgram;
	FGLESUseProgram UseProgram;
	FGLESGetUniformLocation GetUniformLocation;
	FGLESUniform1i Uniform1i;
	FGLESUniform1f Uniform1f;
	FGLESUniform4f Uniform4f;
	FGLESUniformMatrix4fv UniformMatrix4fv;
	FGLESGenVertexArrays GenVertexArrays;
	FGLESDeleteVertexArrays DeleteVertexArrays;
	FGLESBindVertexArray BindVertexArray;
	FGLESGenBuffers GenBuffers;
	FGLESDeleteBuffers DeleteBuffers;
	FGLESBindBuffer BindBuffer;
	FGLESBufferData BufferData;
	FGLESEnableVertexAttribArray EnableVertexAttribArray;
	FGLESDisableVertexAttribArray DisableVertexAttribArray;
	FGLESVertexAttribPointer VertexAttribPointer;
	FGLESDrawArrays DrawArrays;
	FGLESViewport Viewport;
	FGLESScissor Scissor;
	FGLESClearColor ClearColor;
	FGLESClear Clear;
	FGLESEnable Enable;
	FGLESDisable Disable;
	FGLESBlendFunc BlendFunc;
	FGLESDepthFunc DepthFunc;
	FGLESDepthMask DepthMask;
};

#endif
