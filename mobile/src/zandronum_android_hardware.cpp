#include "hardware.h"

#include <atomic>
#include <stdint.h>
#include <time.h>

#include "c_console.h"
#include "c_dispatch.h"
#include "c_cvars.h"
#include "doomstat.h"
#include "gl/system/gl_framebuffer.h"
#include "m_argv.h"
#include "r_renderer.h"
#include "r_swrenderer.h"
#include "version.h"
#include "zandronum_android_host.h"
#include "zandronum_android_video.h"

EXTERN_CVAR(Bool, ticker)
EXTERN_CVAR(Bool, fullscreen)
EXTERN_CVAR(Float, vid_winscale)
EXTERN_CVAR(Int, vid_maxfps)
EXTERN_CVAR(Bool, cl_capfps)

extern int NewWidth, NewHeight, NewBits;

IVideo *Video;
int currentrenderer = RENDERER_GLES;
static std::atomic<int> AndroidFPSLimit(0);
static std::atomic<int64_t> AndroidNextFrameDeadline(0);
static std::atomic<int> AndroidDisplayFPSLimit(0);
static const int AndroidMaximumFPS = 240;

static int64_t AndroidMonotonicNanoseconds()
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return static_cast<int64_t>(now.tv_sec) * 1000000000LL + now.tv_nsec;
}

CUSTOM_CVAR(Int, gl_vid_multisample, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(Int, vid_renderer, RENDERER_GLES, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	if (self == RENDERER_OPENGL)
		self = RENDERER_GLES;
	if (self != RENDERER_GLES)
		self = RENDERER_GLES;
	currentrenderer = RENDERER_GLES;
}

void I_ShutdownGraphics()
{
	if (screen)
	{
		DFrameBuffer *old = screen;
		screen = nullptr;
		old->ObjectFlags |= OF_YesReallyDelete;
		delete old;
	}
	if (Video != nullptr)
	{
		delete Video;
		Video = nullptr;
	}
}

void I_InitGraphics()
{
	UCVarValue value;
	value.Bool = !!Args->CheckParm("-devparm");
	ticker.SetGenericRepDefault(value, CVAR_Bool);
	vid_renderer = RENDERER_GLES;
	currentrenderer = RENDERER_GLES;
	Video = new AndroidGLVideo(0);
	if (Video == nullptr)
		I_FatalError("Failed to initialize display");
	atterm(I_ShutdownGraphics);
	Video->SetWindowedScale(vid_winscale);
	I_SetFPSLimit(vid_maxfps);
}

static void I_DeleteRenderer()
{
	if (Renderer != nullptr)
		delete Renderer;
}

extern FRenderer *gl_CreateInterface();

void I_CreateRenderer()
{
	currentrenderer = RENDERER_GLES;
	if (Renderer == nullptr)
	{
		Renderer = gl_CreateInterface();
		atterm(I_DeleteRenderer);
		P_ResetPlayerPitchLimits();
	}
}

DFrameBuffer *I_SetMode(int &width, int &height, DFrameBuffer *old)
{
	DFrameBuffer *buffer = Video->CreateFrameBuffer(width, height, true, old);
	return buffer;
}

bool I_CheckResolution(int width, int height, int)
{
	return width > 0 && height > 0;
}

void I_ClosestResolution(int *width, int *height, int)
{
	if (width != nullptr)
		*width = Zandronum_AndroidHost_GetWidth();
	if (height != nullptr)
		*height = Zandronum_AndroidHost_GetHeight();
}

void I_SetFPSLimit(int limit)
{
	if (limit < 0)
		limit = vid_maxfps;
	if (cl_capfps || limit == 0)
		limit = 0;
	else
	{
		limit = clamp(limit, TICRATE, AndroidMaximumFPS);
		const int displayLimit = AndroidDisplayFPSLimit.load(std::memory_order_relaxed);
		if (displayLimit > 0)
			limit = MIN(limit, displayLimit);
	}
	AndroidFPSLimit.store(limit, std::memory_order_relaxed);
	AndroidNextFrameDeadline.store(0, std::memory_order_relaxed);
	static int lastConfiguredLimit = -1;
	static int lastDisplayLimit = -1;
	static int lastEffectiveLimit = -1;
	static bool lastCapFPS = false;
	const int displayLimit = AndroidDisplayFPSLimit.load(std::memory_order_relaxed);
	if (developer && (lastConfiguredLimit != vid_maxfps || lastDisplayLimit != displayLimit ||
		lastEffectiveLimit != limit || lastCapFPS != !!cl_capfps))
	{
		DPrintf("Android GLES FPS pacing: vid_maxfps=%d cl_capfps=%d display=%d effective=%d\n",
			static_cast<int>(vid_maxfps), cl_capfps ? 1 : 0, displayLimit, limit);
	}
	lastConfiguredLimit = vid_maxfps;
	lastDisplayLimit = displayLimit;
	lastEffectiveLimit = limit;
	lastCapFPS = !!cl_capfps;
}

void I_SetDisplayRefreshRate(int refreshHz)
{
	if (refreshHz <= 0)
		AndroidDisplayFPSLimit.store(0, std::memory_order_relaxed);
	else
		AndroidDisplayFPSLimit.store(MIN(refreshHz, AndroidMaximumFPS), std::memory_order_relaxed);
	I_SetFPSLimit(-1);
}

double I_WaitForFPSLimit()
{
	const int64_t waitStart = AndroidMonotonicNanoseconds();
	const int limit = AndroidFPSLimit.load(std::memory_order_relaxed);
	if (limit <= 0)
		return 0.0;

	const int64_t interval = 1000000000LL / limit;
	const int64_t now = AndroidMonotonicNanoseconds();
	int64_t deadline = AndroidNextFrameDeadline.load(std::memory_order_relaxed);
	if (deadline == 0 || deadline < now - interval * 4)
		deadline = now;
	deadline += interval;
	while (deadline > AndroidMonotonicNanoseconds())
	{
		const int64_t remaining = deadline - AndroidMonotonicNanoseconds();
		struct timespec sleepTime = {
			static_cast<time_t>(remaining / 1000000000LL),
			static_cast<long>(remaining % 1000000000LL)
		};
		nanosleep(&sleepTime, nullptr);
	}
	AndroidNextFrameDeadline.store(deadline, std::memory_order_relaxed);
	return static_cast<double>(AndroidMonotonicNanoseconds() - waitStart) / 1000000.0;
}

void I_GetAndroidFPSLimitState(int *displayLimit, int *effectiveLimit)
{
	if (displayLimit != nullptr)
		*displayLimit = AndroidDisplayFPSLimit.load(std::memory_order_relaxed);
	if (effectiveLimit != nullptr)
		*effectiveLimit = AndroidFPSLimit.load(std::memory_order_relaxed);
}

CUSTOM_CVAR(Int, vid_maxfps, 240, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (vid_maxfps < TICRATE && vid_maxfps != 0)
		vid_maxfps = TICRATE;
	else if (vid_maxfps > AndroidMaximumFPS)
		vid_maxfps = AndroidMaximumFPS;
	if (!cl_capfps)
		I_SetFPSLimit(vid_maxfps);
}

CUSTOM_CVAR(Bool, fullscreen, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	if (screen != nullptr)
	{
		NewWidth = screen->GetWidth();
		NewHeight = screen->GetHeight();
	}
	NewBits = DisplayBits;
	setmodeneeded = true;
}

CUSTOM_CVAR(Float, vid_winscale, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1.f)
		self = 1.f;
}

CCMD(vid_listmodes)
{
	Printf("Android GLES uses the active host surface; multiple-monitor selection is unsupported.\n");
	if (Video == nullptr)
		return;
	int width, height;
	bool letterbox;
	Video->StartModeIterator(DisplayBits, true);
	while (Video->NextMode(&width, &height, &letterbox))
		Printf("%4d x%5d x%3d\n", width, height, DisplayBits);
}

CCMD(vid_currentmode)
{
	Printf("%dx%dx%d\n", DisplayWidth, DisplayHeight, DisplayBits);
}
