#ifndef ZANDRONUM_GL_GLES_CONTEXT_H
#define ZANDRONUM_GL_GLES_CONTEXT_H

#include "gl/system/gl_gles_api.h"
#include "gl/system/gl_gles_contract.h"

struct FGLESContextInfo
{
	int majorVersion;
	int minorVersion;
	int maxTextureSize;
	int maxTextureUnits;
	int maxVertexUniformVectors;
	int maxFragmentUniformVectors;
	int extensionCount;
	const char *vendor;
	const char *renderer;
	const char *version;
	const char *shadingLanguageVersion;
	bool isGLES;
	bool hasDepthStencil;
	bool hasMultisample;
	bool hasDebugLabels;
	bool hasAnisotropicFiltering;
	bool hasMultiview;
};

using FGLESProcResolver = void *(*)(const char *name);
using FGLESFramePrepareCallback = void (*)(void *userData);
using FGLESLogCallback = void (*)(void *userData, const char *message);
using FGLESPresentCallback = bool (*)(void *userData);

struct FGLESHostCallbacks
{
	void *userData;
	FGLESFramePrepareCallback prepareFrame;
	FGLESPresentCallback present;
	FGLESLogCallback log;
};

bool gl_GLES_LoadContext(FGLESProcResolver resolver, int minimumMajor, int minimumMinor,
	bool requireGLES, FGLESContextInfo *info);
bool gl_GLES_InstallDirectContext(int minimumMajor, int minimumMinor, FGLESContextInfo *info);
void gl_GLES_ShutdownContext(bool preserveHostCallbacks = false);
bool gl_GLES_HasContext();
const FGLESProcTable &gl_GLES_GetProcTable();
const FGLESContextInfo &gl_GLES_GetContextInfo();
const char *gl_GLES_GetShaderVersion();
bool gl_GLES_HasExtension(const char *name);
void gl_GLES_RegisterHostCallbacks(const FGLESHostCallbacks *callbacks);
void gl_GLES_PrepareHostFrame();
bool gl_GLES_SetHostTarget(const FGLESTargetDescriptor &target);
void gl_GLES_InvalidateHostTarget();
bool gl_GLES_GetHostTarget(FGLESTargetDescriptor *target);
void gl_GLES_Report(const char *stage, const char *message);
GLenum gl_GLES_CheckErrors(const char *stage);

void gl_GLES_SetFrameContract(const FGLESFrameDescriptor &frame,
	const FGLESTargetDescriptor &target, const FGLESViewDescriptor &view);
bool gl_GLES_PresentFrame();
const FGLESFrameDescriptor &gl_GLES_GetFrameContract();

#endif
