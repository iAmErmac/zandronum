#include "zandronum_android_video.h"

#ifdef __ANDROID__

#include <chrono>

#include "c_console.h"
#include "c_cvars.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_renderer.h"
#include "gl/renderer/gl_renderer.h"
#include "zandronum_android_host.h"

EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Bool, cl_capfps)

IMPLEMENT_ABSTRACT_CLASS(AndroidGLFB)

AndroidGLVideo::AndroidGLVideo(int)
	: IteratorMode(0)
{
}

void AndroidGLVideo::SetWindowedScale(float scale)
{
	if (scale <= 1.0f) return;
	static bool reported = false;
	if (!reported)
	{
		reported = true;
		Printf("Android GLES uses the host surface directly; window scaling is unsupported.\n");
	}
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
	: DFrameBuffer(width, height), m_supportsGamma(false)
{
}

bool AndroidGLFB::Lock(bool)
{
	// The shared framebuffer API requires this hook, but native GLES has no CPU surface.
	return false;
}

void AndroidGLFB::Unlock()
{
	// GLES draws directly to the active target; there is no CPU surface to unlock.
}

bool AndroidGLFB::IsLocked()
{
	// Rendering stays in the active GLES target for the entire frame.
	return false;
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
	const double waitMilliseconds = I_WaitForFPSLimit();
	const std::chrono::steady_clock::time_point presentStart = std::chrono::steady_clock::now();
	const bool presented = gl_GLES_PresentFrame();
	const std::chrono::duration<double, std::milli> presentElapsed =
		std::chrono::steady_clock::now() - presentStart;
	int displayLimit = 0;
	int effectiveLimit = 0;
	I_GetAndroidFPSLimitState(&displayLimit, &effectiveLimit);
	gl_GLES_RecordHostPresentation(waitMilliseconds, presentElapsed.count(), presented,
		static_cast<int>(vid_maxfps), cl_capfps ? 1 : 0, displayLimit, effectiveLimit);
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

#endif
