#ifndef __GL_PCH_H
#define __GL_PCH_H
#ifdef _WIN32
//#define __RPCNDR_H__		// this header causes problems!
//#define __wtypes_h__
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _WIN32_WINDOWS 0x410
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501			// Support the mouse wheel and session notification.
#define _WIN32_IE 0x0500
#endif
#define DIRECTINPUT_VERSION 0x800
#define DIRECTDRAW_VERSION 0x0300

#define DWORD WINDOWS_DWORD	// I don't want to depend on this throughout the GL code!

#ifdef _MSC_VER
#pragma warning(disable : 4995)     // MIPS
#endif

#include <windows.h>
#include <mmsystem.h>
#include <winsock.h>
#ifndef __WINE__
#include <dshow.h>
#endif
#include <d3d9.h>
//#include <dsound.h>
//#include <dinput.h>
//#include <lmcons.h>
//#include <shlobj.h>
#endif

#undef DWORD
#ifndef CALLBACK
#define CALLBACK
#endif
#include <math.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
//#include <direct.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#if !defined(__APPLE__)
#include <malloc.h>
#endif
#include <time.h>

#ifdef _MSC_VER
#define    F_OK    0    /* Check for file existence */
#define    W_OK    2    /* Check for write permission */
#define    R_OK    4    /* Check for read permission */
#include <io.h>
#else
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

//GL headers
#if defined(__APPLE__)
#include <GL/glew.h>
#include <OpenGL/OpenGL.h>
#elif defined(__unix__)
#ifdef __ANDROID__
#if defined(ZANDRONUM_NATIVE_GLES)
#include <GLES3/gl32.h>
#ifndef GL_CLAMP
#define GL_CLAMP 0x2900
#endif
#else
//#define GL_GLEXT_PROTOTYPES
//#include "GL/gl.h"
//#define GL_GLEXT_PROTOTYPES
//#include "GL/glext.h"
//#include "gl/api/glext.h"
#include "gl_load.h"

#define glDeleteRenderbuffers glDeleteRenderbuffersEXT
#define glGenRenderbuffers glGenRenderbuffersEXT
#define glBindRenderbuffer glBindRenderbufferEXT
#define glRenderbufferStorage glRenderbufferStorageEXT
#define glFramebufferTexture2D glFramebufferTexture2DEXT
#define glFramebufferRenderbuffer glFramebufferRenderbufferEXT
#define glGenFramebuffers glGenFramebuffersEXT
#define glDeleteFramebuffers glDeleteFramebuffersEXT
#define glBindFramebuffer glBindFramebufferEXT

#define GL_RENDERBUFFER GL_RENDERBUFFER_EXT
#define GL_FRAMEBUFFER GL_FRAMEBUFFER_EXT

#define GL_DEPTH24_STENCIL8 GL_DEPTH24_STENCIL8_EXT
#define GL_COLOR_ATTACHMENT0 GL_COLOR_ATTACHMENT0_EXT
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
//#include "gl/api/glext.h"
//#include "gl/api/gl_api.h"
#endif

#else
#include <GL/glew.h>
// #include "gl/api/glext.h" [AK] Commented this out to fix compile errors on Fedora.
#endif

#else // !__APPLE__ && !__unix__
#include <GL/gl.h>
#include <GL/glu.h>
#include "gl/api/glext.h"
#endif
//#include "gl/api/gl_api.h"

#ifdef _WIN32
#define DWORD WINDOWS_DWORD	// I don't want to depend on this throughout the GL code!
#include "gl/api/wglext.h"
#ifndef __WINE__
#undef DWORD
#endif
#else
typedef unsigned char 	byte;
typedef float		FLOAT;
template <typename T>
inline T max( T a, T b) { return (((a)>(b)) ? (a) : (b)); }
#define __cdecl
#define _access(a,b)	access(a,b)
#endif

#if defined(_WIN32) || defined(ZANDRONUM_GLES_BACKEND)
#include "gl/api/gl_api.h"
#endif
#if !defined(_WIN32) && !defined(__ANDROID__)
#include <SDL.h>
#endif


#ifdef LoadMenu
#undef LoadMenu
#endif
#ifdef DrawText
#undef DrawText
#endif
#ifdef GetCharWidth
#undef GetCharWidth
#endif

#undef S_NORMAL
#undef OPAQUE


#ifdef _MSC_VER
#pragma warning(disable : 4244)     // MIPS
#pragma warning(disable : 4136)     // X86
#pragma warning(disable : 4051)     // ALPHA

#pragma warning(disable : 4018)     // signed/unsigned mismatch
#pragma warning(disable : 4305)     // truncate from double to float
#endif

#ifdef WIN32
#undef WIN32
#endif
#endif //__GL_PCH_H
