#include "zandronum_android_video.h"

#ifdef __ANDROID__

#include "c_console.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/system/gl_gles_context.h"
#include "gl/renderer/gl_renderer.h"
#include "zandronum_android_host.h"

IMPLEMENT_ABSTRACT_CLASS(AndroidGLFB)

AndroidGLVideo::AndroidGLVideo(int)
	: IteratorMode(0)
{
}

void AndroidGLVideo::SetWindowedScale(float)
{
}

DFrameBuffer *AndroidGLVideo::CreateFrameBuffer(int width, int height, bool, DFrameBuffer *old)
{
	if (old != nullptr && old->GetWidth() == width && old->GetHeight() == height)
		return old;
	if (old != nullptr)
	{
		old->ObjectFlags |= OF_YesReallyDelete;
		delete old;
	}
	return new OpenGLFrameBuffer(nullptr, width, height, 32, 60, true);
}

void AndroidGLVideo::StartModeIterator(int, bool)
{
	IteratorMode = 0;
}

bool AndroidGLVideo::NextMode(int *width, int *height, bool *letterbox)
{
	if (IteratorMode++ != 0)
		return false;
	if (width != nullptr)
		*width = Zandronum_AndroidHost_GetWidth();
	if (height != nullptr)
		*height = Zandronum_AndroidHost_GetHeight();
	if (letterbox != nullptr)
		*letterbox = false;
	return true;
}

bool AndroidGLVideo::SetResolution(int width, int height, int)
{
	if (GLRenderer != nullptr)
		GLRenderer->FlushTextures();
	I_ShutdownGraphics();
	Video = new AndroidGLVideo(0);
	if (Video == nullptr)
		I_FatalError("Failed to initialize display");
	return V_DoModeSetup(width, height, 32);
}

AndroidGLFB::AndroidGLFB(void *, int width, int height, int, int, bool)
	: DFrameBuffer(width, height), LockDepth(0), m_supportsGamma(false)
{
}

bool AndroidGLFB::Lock(bool)
{
	++LockDepth;
	return false;
}

bool AndroidGLFB::Lock()
{
	return Lock(true);
}

void AndroidGLFB::Unlock()
{
	if (LockDepth > 0)
		--LockDepth;
}

bool AndroidGLFB::IsLocked()
{
	return LockDepth != 0;
}

bool AndroidGLFB::IsValid()
{
	return Zandronum_AndroidHost_IsSurfaceReady();
}

bool AndroidGLFB::IsFullscreen()
{
	return true;
}

void AndroidGLFB::SwapBuffers()
{
	I_WaitForFPSLimit();
	gl_GLES_PresentFrame();
}

bool AndroidGLFB::CanUpdate()
{
	return true;
}

void AndroidGLFB::SetGammaTable(WORD *)
{
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		Printf("Android hardware gamma tables are unsupported; display correction stays in the GLES present pass.\n");
	}
}

void AndroidGLFB::InitializeState()
{
}

#endif
