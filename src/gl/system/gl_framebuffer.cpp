/*
** gl_framebuffer.cpp
** Implementation of the non-hardware specific parts of the
** OpenGL frame buffer
**
**---------------------------------------------------------------------------
** Copyright 2000-2007 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
** 4. When not used as part of GZDoom or a GZDoom derivative, this code will be
**    covered by the terms of the GNU Lesser General Public License as published
**    by the Free Software Foundation; either version 2.1 of the License, or (at
**    your option) any later version.
** 5. Full disclosure of the entire project's source code, except for third
**    party libraries is mandatory. (NOTE: This clause is non-negotiable!)
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include "gl/system/gl_system.h"
#include <chrono>
#include "files.h"
#include "m_swap.h"
#include "v_video.h"
#include "doomstat.h"
#include "m_png.h"
#include "m_crc32.h"
#include "vectors.h"
#include "v_palette.h"
#include "templates.h"
#include "farchive.h"

#include "gl/system/gl_interface.h"
#include "r_renderer.h"
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_renderer.h"
#include "gl/system/gl_gles_targets.h"
#endif
#include "gl/system/gl_framebuffer.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/data/gl_data.h"
#include "gl/textures/gl_hwtexture.h"
#include "gl/textures/gl_texture.h"
#include "gl/textures/gl_translate.h"
#include "gl/textures/gl_skyboxtexture.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"
#include "gl/gl_functions.h"

IMPLEMENT_CLASS(OpenGLFrameBuffer)
EXTERN_CVAR (Float, vid_brightness)
EXTERN_CVAR (Float, vid_contrast)
EXTERN_CVAR (Bool, vid_vsync)
#if defined(ZANDRONUM_GLES_BACKEND)
EXTERN_CVAR (Int, vid_renderer)
#endif
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
bool gl_GLES_IsProfileEnabled();
#endif

CVAR(Bool, gl_aalines, false, CVAR_ARCHIVE)

FGLRenderer *GLRenderer;

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
static bool DesktopProfileFrameReady = false;
static std::chrono::steady_clock::time_point DesktopProfileFrameStart;
static double DesktopProfileRenderMilliseconds = 0.0;
static unsigned int DesktopProfileFrame = 0;

void gl_GLES_DesktopProfileBegin()
{
	if (!gl_GLES_IsProfileEnabled()) return;
	DesktopProfileFrameStart = std::chrono::steady_clock::now();
	DesktopProfileFrameReady = false;
}

void gl_GLES_DesktopProfileEnd()
{
	if (!gl_GLES_IsProfileEnabled()) return;
	DesktopProfileRenderMilliseconds = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - DesktopProfileFrameStart).count();
	DesktopProfileFrameReady = true;
}
#endif

void gl_SetupMenu();
void gl_LoadExtensions();
void gl_PrintStartupLog();

//==========================================================================
//
//
//
//==========================================================================

OpenGLFrameBuffer::OpenGLFrameBuffer(void *hMonitor, int width, int height, int bits, int refreshHz, bool fullscreen) : 
	Super(hMonitor, width, height, bits, refreshHz, fullscreen) 
{
	GLRenderer = new FGLRenderer(this);
	memcpy (SourcePalette, GPalette.BaseColors, sizeof(PalEntry)*256);
	UpdatePalette ();
	ScreenshotBuffer = NULL;
	LastCamera = NULL;

	InitializeState();
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		gl_SetupMenu();
		gl_GenerateGlobalBrightmapFromColormap();
		needsetgamma = true;
		swapped = false;
		Accel2D = false;
		SetVSync(vid_vsync);
		return;
	}
	#endif
	gl_SetupMenu();
	gl_GenerateGlobalBrightmapFromColormap();
	DoSetGamma();
	needsetgamma = true;
	swapped = false;
	Accel2D = true;
	SetVSync(vid_vsync);
}

OpenGLFrameBuffer::~OpenGLFrameBuffer()
{
	delete GLRenderer;
	GLRenderer = NULL;
}

//==========================================================================
//
// Initializes the GL renderer
//
//==========================================================================

void OpenGLFrameBuffer::InitializeState()
{
	static bool first=true;

	gl_LoadExtensions();

#if defined(__ANDROID__)
	if (gl_GLES_IsActive() || gl_GLES_InitializeBootstrap(GetWidth(), GetHeight()))
	{
		if (first)
		{
			first = false;
			gl_GLES_PrintStartupLog();
		}
		GLRenderer->Initialize();
		return;
	}
	I_FatalError("Zandronum GLES bootstrap failed; refusing to enter the legacy renderer.");
	return;
#elif defined(ZANDRONUM_GLES_BACKEND)
	if (vid_renderer == RENDERER_GLES && gl_GLES_HasContext())
	{
		if (!gl_GLES_InitializeBootstrap(GetWidth(), GetHeight()))
		{
			I_FatalError("Zandronum GLES resource bootstrap failed.");
			return;
		}
		if (first)
		{
			first = false;
			gl_GLES_PrintStartupLog();
		}
		GLRenderer->Initialize();
		return;
	}
#endif

#if !defined(__ANDROID__)
	Super::InitializeState();
	if (first)
	{
		first=false;
		// [BB] For some reason this crashes, if compiled with MinGW and optimization. Has to be investigated.
#ifndef __MINGW32__
		gl_PrintStartupLog();
#endif

#ifdef __ANDROID__
        gl_PrintStartupLog();
#endif
		if (gl.flags&RFL_NPOT_TEXTURE)
		{
			Printf("Support for non power 2 textures enabled.\n");
		}
		if (gl.flags&RFL_OCCLUSION_QUERY)
		{
			Printf("Occlusion query enabled.\n");
		}
	}
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepth(1.0f);
	glDepthFunc(GL_LESS);
	glShadeModel(GL_SMOOTH);

	glEnable(GL_DITHER);
	glEnable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_POLYGON_OFFSET_FILL);
#ifndef __ANDROID__
	glEnable(GL_POLYGON_OFFSET_LINE);
#endif
	glEnable(GL_BLEND);
#ifndef __ANDROID__
	glEnable(GL_DEPTH_CLAMP_NV);
#endif
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
	glDisable(GL_LINE_SMOOTH);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glAlphaFunc(GL_GEQUAL,0.5f);
	glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
#ifndef __ANDROID__
	glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
#endif
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

	// This was to work around a bug in some older driver. Probably doesn't make sense anymore.
	glEnable(GL_FOG);
	glDisable(GL_FOG);

	glHint(GL_FOG_HINT, GL_FASTEST);
	glFogi(GL_FOG_MODE, GL_EXP);


	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	int trueH = GetTrueHeight();
	int h = GetHeight();
	glViewport(0, (trueH - h)/2, GetWidth(), GetHeight()); 

	Begin2D(false);
	GLRenderer->Initialize();
#endif
}

//==========================================================================
//
// Updates the screen
//
//==========================================================================

// Testing only for now. 
CVAR(Bool, gl_draw_sync, true, 0) //false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

void OpenGLFrameBuffer::Update()
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		static bool nativeUpdateInProgress = false;
		if (nativeUpdateInProgress) return;
		nativeUpdateInProgress = true;
		// Status-bar and message drawing appends to the native batch after the view.
		DrawRateStuff();
		if (GetTrueHeight() != GetHeight() && GLRenderer != NULL)
			GLRenderer->ClearBorders();
		gl_GLES_EndScene();
		Swap();
		swapped = false;
		Unlock();
		CheckBench();
		nativeUpdateInProgress = false;
		return;
	}
	#endif

	if (!CanUpdate()) 
	{
		GLRenderer->Flush();
		return;
	}
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	const bool profileDesktopFrame = DesktopProfileFrameReady;
#endif

	Begin2D(false);

	DrawRateStuff();
	GLRenderer->Flush();

	if (GetTrueHeight() != GetHeight())
	{
		if (GLRenderer != NULL) 
			GLRenderer->ClearBorders();

		Begin2D(false);
	}
	if (gl_draw_sync || !swapped)
	{
		Swap();
	}
	swapped = false;
	Unlock();
	CheckBench();
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (profileDesktopFrame)
	{
		++DesktopProfileFrame;
		if (DesktopProfileFrame == 1 || DesktopProfileFrame % 30 == 0)
		{
			const auto desktopFrameEnd = std::chrono::steady_clock::now();
			const double renderMilliseconds = DesktopProfileRenderMilliseconds;
			const double totalMilliseconds = std::chrono::duration<double, std::milli>(
				desktopFrameEnd - DesktopProfileFrameStart).count();
			DPrintf("desktop profile: frame=%u cpu=%.3f total=%.3f ms; renderer=desktop\n",
				DesktopProfileFrame, renderMilliseconds, totalMilliseconds);
		}
		DesktopProfileFrameReady = false;
	}
	#endif
}


//==========================================================================
//
// Swap the buffers
//
//==========================================================================
#include "gl/renderer/gl_renderstate.h"
void OpenGLFrameBuffer::Swap()
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		gl_GLES_PrepareHostFrame();
		FGLESTargetDescriptor presentationTarget = {};
		if (!gl_GLES_GetHostTarget(&presentationTarget))
		{
			gl_GLES_ClearScene();
			swapped = false;
			return;
		}
		gl_GLES_RenderBootstrap(presentationTarget.renderWidth, presentationTarget.renderHeight);
		SwapBuffers();
		gl_GLES_ClearScene();
		swapped = true;
		return;
	}
	#endif

	Finish.Reset();
	Finish.Clock();
#ifndef __ANDROID__
	glFinish();
#endif

#ifdef __ANDROID__
	gl_RenderState.EnableAlphaTest(false);
	gl_RenderState.Apply();
#endif

	if (needsetgamma) 
	{
		//DoSetGamma();
		needsetgamma = false;
	}
	SwapBuffers();
	Finish.Unclock();
	swapped = true;
	FHardwareTexture::UnbindAll();
}

//===========================================================================
//
// DoSetGamma
//
// (Unfortunately Windows has some safety precautions that block gamma ramps
//  that are considered too extreme. As a result this doesn't work flawlessly)
//
//===========================================================================

void OpenGLFrameBuffer::DoSetGamma()
{
	WORD gammaTable[768];

	if (m_supportsGamma)
	{
		// This formula is taken from Doomsday
		float gamma = clamp<float>(Gamma, 0.1f, 4.f);
		float contrast = clamp<float>(vid_contrast, 0.1f, 3.f);
		float bright = clamp<float>(vid_brightness, -0.8f, 0.8f);

		double invgamma = 1 / gamma;
		double norm = pow(255., invgamma - 1);

		for (int i = 0; i < 256; i++)
		{
			double val = i * contrast - (contrast - 1) * 127;
			if(gamma != 1) val = pow(val, invgamma) / norm;
			val += bright * 128;

			gammaTable[i] = gammaTable[i + 256] = gammaTable[i + 512] = (WORD)clamp<double>(val*256, 0, 0xffff);
		}
		SetGammaTable(gammaTable);
	}
}

bool OpenGLFrameBuffer::SetGamma(float gamma)
{
	DoSetGamma();
	return true;
}

bool OpenGLFrameBuffer::SetBrightness(float bright)
{
	DoSetGamma();
	return true;
}

bool OpenGLFrameBuffer::SetContrast(float contrast)
{
	DoSetGamma();
	return true;
}

//===========================================================================
//
//
//===========================================================================

void OpenGLFrameBuffer::UpdatePalette()
{
	int rr=0,gg=0,bb=0;
	for(int x=0;x<256;x++)
	{
		rr+=GPalette.BaseColors[x].r;
		gg+=GPalette.BaseColors[x].g;
		bb+=GPalette.BaseColors[x].b;
	}
	rr>>=8;
	gg>>=8;
	bb>>=8;

	palette_brightness = (rr*77 + gg*143 + bb*35)/255;
}

void OpenGLFrameBuffer::GetFlashedPalette (PalEntry pal[256])
{
	memcpy(pal, SourcePalette, 256*sizeof(PalEntry));
}

PalEntry *OpenGLFrameBuffer::GetPalette ()
{
	return SourcePalette;
}

bool OpenGLFrameBuffer::SetFlash(PalEntry rgb, int amount)
{
	Flash = PalEntry(amount, rgb.r, rgb.g, rgb.b);
	return true;
}

void OpenGLFrameBuffer::GetFlash(PalEntry &rgb, int &amount)
{
	rgb = Flash;
	rgb.a = 0;
	amount = Flash.a;
}

int OpenGLFrameBuffer::GetPageCount()
{
	return 1;
}


void OpenGLFrameBuffer::GetHitlist(BYTE *hitlist)
{
	Super::GetHitlist(hitlist);

	// check skybox textures and mark the separate faces as used
	for(int i=0;i<TexMan.NumTextures(); i++)
	{
		if (hitlist[i])
		{
			FTexture *tex = TexMan.ByIndex(i);
			if (tex->gl_info.bSkybox)
			{
				FSkyBox *sb = static_cast<FSkyBox*>(tex);
				for(int i=0;i<6;i++) 
				{
					if (sb->faces[i]) 
					{
						int index = sb->faces[i]->id.GetIndex();
						hitlist[index] |= 1;
					}
				}
			}
		}
	}


	// check model skins
}

//==========================================================================
//
// DFrameBuffer :: CreatePalette
//
// Creates a native palette from a remap table, if supported.
//
//==========================================================================

FNativePalette *OpenGLFrameBuffer::CreatePalette(FRemapTable *remap)
{
	return GLTranslationPalette::CreatePalette(remap);
}

//==========================================================================
//
//
//
//==========================================================================
bool OpenGLFrameBuffer::Begin2D(bool)
{
#if defined(__ANDROID__)
	return false;
#else
#if defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive()) return false;
#endif
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(
		(GLdouble) 0,
		(GLdouble) GetWidth(), 
		(GLdouble) GetHeight(), 
		(GLdouble) 0,
		(GLdouble) -1.0, 
		(GLdouble) 1.0 
		);
	glDisable(GL_DEPTH_TEST);

	// Korshun: ENABLE AUTOMAP ANTIALIASING!!!
	if (gl_aalines)
		glEnable(GL_LINE_SMOOTH);
	else
	{
		glDisable(GL_MULTISAMPLE);
		glDisable(GL_LINE_SMOOTH);
		glLineWidth(1.0);
	}

	if (GLRenderer != NULL)
			GLRenderer->Begin2D();
	return true;
#endif
}

//==========================================================================
//
// Draws a texture
//
//==========================================================================

void STACK_ARGS OpenGLFrameBuffer::DrawTextureV(FTexture *img, double x0, double y0, uint32 tag, va_list tags)
{
	DrawParms parms;

	if (ParseDrawTextureTags(img, x0, y0, tag, tags, &parms, true))
	{
		if (GLRenderer != NULL) GLRenderer->DrawTexture(img, parms);
	}
}

//==========================================================================
//
//
//
//==========================================================================
void OpenGLFrameBuffer::DrawLine(int x1, int y1, int x2, int y2, int palcolor, uint32 color)
{
	if (GLRenderer != NULL) 
		GLRenderer->DrawLine(x1, y1, x2, y2, palcolor, color);
}

//==========================================================================
//
//
//
//==========================================================================
void OpenGLFrameBuffer::DrawPixel(int x1, int y1, int palcolor, uint32 color)
{
	if (GLRenderer != NULL) 
		GLRenderer->DrawPixel(x1, y1, palcolor, color);
}

//==========================================================================
//
//
//
//==========================================================================
void OpenGLFrameBuffer::Dim(PalEntry color)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		DCanvas::Dim(color);
		return;
	}
	#endif
	// Unlike in the software renderer the color is being ignored here because
	// view blending only affects the actual view with the GL renderer.
	Super::Dim(0);
}

void OpenGLFrameBuffer::Dim(PalEntry color, float damount, int x1, int y1, int w, int h)
{
	if (GLRenderer != NULL) 
		GLRenderer->Dim(color, damount, x1, y1, w, h);
}

//==========================================================================
//
//
//
//==========================================================================
void OpenGLFrameBuffer::FlatFill (int left, int top, int right, int bottom, FTexture *src, bool local_origin)
{

	if (GLRenderer != NULL) 
		GLRenderer->FlatFill(left, top, right, bottom, src, local_origin);
}

//==========================================================================
//
//
//
//==========================================================================
void OpenGLFrameBuffer::Clear(int left, int top, int right, int bottom, int palcolor, uint32 color)
{
	if (GLRenderer != NULL) 
		GLRenderer->Clear(left, top, right, bottom, palcolor, color);
}

//==========================================================================
//
// D3DFB :: FillSimplePoly
//
// Here, "simple" means that a simple triangle fan can draw it.
//
//==========================================================================

void OpenGLFrameBuffer::FillSimplePoly(FTexture *texture, FVector2 *points, int npoints,
	double originx, double originy, double scalex, double scaley,
	angle_t rotation, FDynamicColormap *colormap, int lightlevel)
{
	if (GLRenderer != NULL)
	{
		GLRenderer->FillSimplePoly(texture, points, npoints, originx, originy, scalex, scaley,
			rotation, colormap, lightlevel);
	}
}


//===========================================================================
// 
//	Takes a screenshot
//
//===========================================================================

void OpenGLFrameBuffer::GetScreenshotBuffer(const BYTE *&buffer, int &pitch, ESSType &color_type)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	ReleaseScreenshotBuffer();
	ScreenshotBuffer = new BYTE[w * h * 3];

	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		BYTE *rgba = new BYTE[w * h * 4];
		if (!gl_GLES_ReadScreenshot(rgba, w, h))
		{
			delete [] rgba;
			delete [] ScreenshotBuffer;
			ScreenshotBuffer = nullptr;
			buffer = nullptr;
			pitch = 0;
			color_type = SS_RGB;
			return;
		}
		for (int pixel = 0; pixel < w * h; ++pixel)
		{
			ScreenshotBuffer[pixel * 3 + 0] = rgba[pixel * 4 + 0];
			ScreenshotBuffer[pixel * 3 + 1] = rgba[pixel * 4 + 1];
			ScreenshotBuffer[pixel * 3 + 2] = rgba[pixel * 4 + 2];
		}
		delete [] rgba;
		gl_GLES_ResetState(GetWidth(), GetHeight());
		pitch = -w*3;
		color_type = SS_RGB;
		buffer = ScreenshotBuffer + w * 3 * (h - 1);
		return;
	}
	#endif

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0,(GetTrueHeight() - GetHeight()) / 2,w,h,GL_RGB,GL_UNSIGNED_BYTE,ScreenshotBuffer);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	pitch = -w*3;
	color_type = SS_RGB;
	buffer = ScreenshotBuffer + w * 3 * (h - 1);
}

//===========================================================================
// 
// Releases the screenshot buffer.
//
//===========================================================================

void OpenGLFrameBuffer::ReleaseScreenshotBuffer()
{
	if (ScreenshotBuffer != NULL) delete [] ScreenshotBuffer;
	ScreenshotBuffer = NULL;
}


void OpenGLFrameBuffer::GameRestart()
{
	memcpy (SourcePalette, GPalette.BaseColors, sizeof(PalEntry)*256);
	UpdatePalette ();
	ScreenshotBuffer = NULL;
	LastCamera = NULL;
	gl_GenerateGlobalBrightmapFromColormap();
}
