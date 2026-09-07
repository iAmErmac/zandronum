#ifndef ZANDRONUM_GL_ANDROID_H
#define ZANDRONUM_GL_ANDROID_H

struct FAndroidGLESInfo
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
	bool hasVertexBuffers;
	bool hasVertexArrays;
	bool hasUniformBuffers;
	bool hasFramebuffers;
	bool hasDepthStencil;
	bool hasBufferMapping;
	bool hasAnisotropicFiltering;
	bool hasAstcCompression;
	bool hasEtc2Compression;
	bool hasDebugLabels;
	bool hasMultiview;
};

#ifdef __ANDROID__
bool gl_AndroidNativeGLES_CollectCapabilities();
bool gl_AndroidNativeGLES_InitializeBootstrap(int width, int height);
void gl_AndroidNativeGLES_RenderBootstrap(int width, int height);
bool gl_AndroidNativeGLES_IsActive();
void gl_AndroidNativeGLES_OnContextLost();
bool gl_AndroidNativeGLES_OnContextRestored(int width, int height);
const FAndroidGLESInfo &gl_AndroidNativeGLES_GetCapabilities();
void gl_AndroidNativeGLES_PrintStartupLog();
#else
inline bool gl_AndroidNativeGLES_IsActive() { return false; }
#endif

#endif
