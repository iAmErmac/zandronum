#ifndef ZANDRONUM_GL_GLES_PORTAL_H
#define ZANDRONUM_GL_GLES_PORTAL_H

#include "gl/system/gl_gles_targets.h"

#include <stddef.h>

class FMaterial;

struct FGLESSkyPrimitiveRange
{
	GLsizei firstIndex;
	GLsizei indexCount;
};

struct FGLESPortalComposite
{
	GLuint sourceTexture;
	GLuint sceneVertexArray;
	GLuint sceneSampler;
	GLsizei indexCount;
	size_t indexOffsetBytes;
	const float *viewProjection;
	int targetWidth;
	int targetHeight;
	GLuint parentStencilBit;
	bool solidColor;
};

struct FGLESPortalMask
{
	GLsizei indexCount;
	size_t indexOffsetBytes;
	const float *viewProjection;
};

struct FGLESPortalCompositeList
{
	GLuint sourceTexture;
	GLuint sceneVertexArray;
	GLuint sceneSampler;
	const FGLESPortalMask *masks;
	size_t maskCount;
	int targetWidth;
	int targetHeight;
	GLuint parentStencilBit;
	bool solidColor;
};

void gl_GLESInternalPortalBeginFrame();
bool gl_GLESInternalPortalAcquireFallbackTarget(int width, int height, int *index);
FGLESTargetDescriptor *gl_GLESInternalPortalGetFallbackTarget(int index);
bool gl_GLESInternalPortalInitialize();
void gl_GLESInternalPortalDestroy();
void gl_GLESInternalPortalContextLost();
bool gl_GLESInternalPortalComposite(const FGLESPortalComposite &config);
bool gl_GLESInternalPortalCompositeMasks(const FGLESPortalCompositeList &config);

GLsizei gl_GLESInternalPortalUploadSkyGeometry(FMaterial *material, float xOffset,
	float yOffset, bool mirrored, float cameraX, float cameraY, float cameraZ);
void gl_GLESInternalPortalUploadSkyboxGeometry(float xOffset, bool sky2, bool fliptop,
	float cameraX, float cameraY, float cameraZ);
GLuint gl_GLESInternalPortalSkyVertexArray();
FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyUpperCap();
FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyLowerCap();
FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyUpperStrip(int row);
FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyLowerStrip(int row);
FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyboxFace(int face);

#endif
