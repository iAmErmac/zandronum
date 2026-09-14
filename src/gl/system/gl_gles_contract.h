#ifndef ZANDRONUM_GL_GLES_CONTRACT_H
#define ZANDRONUM_GL_GLES_CONTRACT_H

#include <stdint.h>

struct FGLESTargetDescriptor;

struct FGLESViewDescriptor
{
	int viewportX;
	int viewportY;
	int viewportWidth;
	int viewportHeight;
	float cameraPosition[3];
	float viewMatrix[16];
	float projectionMatrix[16];
	float viewProjectionMatrix[16];
	int viewIndex;
	int inactiveViewIndex;
	bool active;
};

struct FGLESFrameDescriptor
{
	uint64_t frameNumber;
	double timeSeconds;
	bool paused;
	int targetWidth;
	int targetHeight;
	const FGLESTargetDescriptor *target;
	const FGLESViewDescriptor *view;
};

#endif
