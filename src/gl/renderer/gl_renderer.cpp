/*
** gl1_renderer.cpp
** Renderer interface
**
**---------------------------------------------------------------------------
** Copyright 2008 Christoph Oelckers
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
#include "files.h"
#include "m_swap.h"
#include "v_video.h"
#include "r_data/r_translate.h"
#include "m_png.h"
#include "m_crc32.h"
#include "w_wad.h"
//#include "gl/gl_intern.h"
#include "gl/gl_functions.h"
#include "vectors.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_framebuffer.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/data/gl_data.h"
#include "gl/data/gl_vertexbuffer.h"
#include "gl/dynlights/gl_lightbuffer.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_texture.h"
#include "gl/textures/gl_translate.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"
#include "gl/models/gl_models.h"
#include <math.h>
#include <vector>
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
#include "gl/system/gl_gles_renderer.h"
#endif
#if defined(ZANDRONUM_GLES_BACKEND)
#include "r_renderer.h"
EXTERN_CVAR(Int, vid_renderer)
#endif

//===========================================================================
// 
// Renderer interface
//
//===========================================================================

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
void gl_GLES_RegisterShaderPrograms()
{
	if (GLRenderer == NULL || GLRenderer->mShaderManager == NULL) return;
	const char *names[] =
	{
		"gles/opaque", "gles/masked", "gles/fog", "gles/palette", "gles/present"
	};
	for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
	{
		const unsigned int handle = gl_GLES_GetShaderProgram(names[i]);
		if (handle != 0) GLRenderer->mShaderManager->RegisterGLESProgram(names[i], handle);
	}
}

void gl_GLES_UnregisterShaderPrograms()
{
	if (GLRenderer != NULL && GLRenderer->mShaderManager != NULL)
		GLRenderer->mShaderManager->ClearGLESPrograms();
}

unsigned int gl_GLES_BindShaderProgram(const char *name, unsigned int fallback)
{
	if (GLRenderer != NULL && GLRenderer->mShaderManager != NULL)
	{
		const unsigned int handle = GLRenderer->mShaderManager->BindGLESProgram(name);
		if (handle != 0) return handle;
	}
	gl_GLES_UseProgram(fallback);
	return fallback;
}
#endif

EXTERN_CVAR(Bool, gl_render_segs)

//-----------------------------------------------------------------------------
//
// Initialize
//
//-----------------------------------------------------------------------------

FGLRenderer::FGLRenderer(OpenGLFrameBuffer *fb) 
{
	framebuffer = fb;
	mCurrentPortal = NULL;
	mMirrorCount = 0;
	mPlaneMirrorCount = 0;
	mLightCount = 0;
	mAngles = FRotator(0,0,0);
	mViewVector = FVector2(0,0);
	mCameraPos = FVector3(0,0,0);
	mVBO = NULL;
	gl_spriteindex = 0;
	mShaderManager = NULL;
	glpart2 = glpart = gllight = mirrortexture = NULL;
}

void FGLRenderer::Initialize()
{
	glpart2 = FTexture::CreateTexture(Wads.GetNumForFullName("glstuff/glpart2.png"), FTexture::TEX_MiscPatch);
	glpart = FTexture::CreateTexture(Wads.GetNumForFullName("glstuff/glpart.png"), FTexture::TEX_MiscPatch);
	mirrortexture = FTexture::CreateTexture(Wads.GetNumForFullName("glstuff/mirror.png"), FTexture::TEX_MiscPatch);
	gllight = FTexture::CreateTexture(Wads.GetNumForFullName("glstuff/gllight.png"), FTexture::TEX_MiscPatch);

	mVBO = new FFlatVertexBuffer;
	mFBID = 0;
	SetupLevel();
	mShaderManager = new FShaderManager;
	#if defined(__ANDROID__)
	if (gl_GLES_IsActive()) gl_GLES_RegisterShaderPrograms();
	#elif defined(ZANDRONUM_GLES_BACKEND)
	if (vid_renderer == RENDERER_GLES && gl_GLES_IsActive()) gl_GLES_RegisterShaderPrograms();
	#endif
}

FGLRenderer::~FGLRenderer() 
{
	gl_CleanModelData();
	gl_DeleteAllAttachedLights();
	FMaterial::FlushAll();
	if (mShaderManager != NULL) delete mShaderManager;
	if (mVBO != NULL) delete mVBO;
	if (glpart2) delete glpart2;
	if (glpart) delete glpart;
	if (mirrortexture) delete mirrortexture;
	if (gllight) delete gllight;
	if (mFBID != 0) glDeleteFramebuffers(1, &mFBID);
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::SetupLevel()
{
	mVBO->CreateVBO();
}

void FGLRenderer::Begin2D()
{
	gl_RenderState.EnableFog(false);
	gl_RenderState.Set2DMode(true);
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::ProcessLowerMiniseg(seg_t *seg, sector_t * frontsector, sector_t * backsector)
{
	GLWall wall;
	wall.ProcessLowerMiniseg(seg, frontsector, backsector);
	rendered_lines++;
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::ProcessSprite(AActor *thing, sector_t *sector)
{
	GLSprite glsprite;
	glsprite.Process(thing, sector);
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::ProcessParticle(particle_t *part, sector_t *sector)
{
	GLSprite glsprite;
	glsprite.ProcessParticle(part, sector);//, 0, 0);
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::ProcessSector(sector_t *fakesector)
{
	GLFlat glflat = {};
	glflat.ProcessSector(fakesector);
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::FlushTextures()
{
	FMaterial::FlushAll();
}

//===========================================================================
// 
//
//
//===========================================================================

bool FGLRenderer::StartOffscreen()
{
	if (gl.flags & RFL_FRAMEBUFFER)
	{
		if (mFBID == 0) glGenFramebuffers(1, &mFBID);
		glBindFramebuffer(GL_FRAMEBUFFER, mFBID);
		return true;
	}
	return false;
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::EndOffscreen()
{
	if (gl.flags & RFL_FRAMEBUFFER)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
}

//===========================================================================
// 
//
//
//===========================================================================

unsigned char *FGLRenderer::GetTextureBuffer(FTexture *tex, int &w, int &h)
{
	FMaterial * gltex = FMaterial::ValidateTexture(tex);
	if (gltex)
	{
		return gltex->CreateTexBuffer(CM_DEFAULT, 0, w, h);
	}
	return NULL;
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::ClearBorders()
{
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		OpenGLFrameBuffer *glscreen = static_cast<OpenGLFrameBuffer*>(screen);
		const int width = glscreen != NULL ? glscreen->GetWidth() : 0;
		const int height = glscreen != NULL ? glscreen->GetHeight() : 0;
		const int trueHeight = glscreen != NULL ? glscreen->GetTrueHeight() : 0;
		if (width <= 0 || height <= 0 || trueHeight <= height) return;
		const float border = static_cast<float>(trueHeight - height) /
			(2.0f * static_cast<float>(trueHeight));
		const float black[3] = { 0.0f, 0.0f, 0.0f };
		const float upper[12] =
		{
			-1.0f, 1.0f - 2.0f * border, 0.0f,
			-1.0f, 1.0f, 0.0f,
			 1.0f, 1.0f, 0.0f,
			 1.0f, 1.0f - 2.0f * border, 0.0f
		};
		const float lower[12] =
		{
			-1.0f, -1.0f, 0.0f,
			-1.0f, -1.0f + 2.0f * border, 0.0f,
			 1.0f, -1.0f + 2.0f * border, 0.0f,
			 1.0f, -1.0f, 0.0f
		};
		gl_GLES_AddHUDQuad(upper, NULL, black, 1.0f, false, 0, GLES_BLEND_OPAQUE);
		gl_GLES_AddHUDQuad(lower, NULL, black, 1.0f, false, 0, GLES_BLEND_OPAQUE);
		return;
	}
#endif
#if !defined(__ANDROID__)
	OpenGLFrameBuffer *glscreen = static_cast<OpenGLFrameBuffer*>(screen);

	// Letterbox time! Draw black top and bottom borders.
	int width = glscreen->GetWidth();
	int height = glscreen->GetHeight();
	int trueHeight = glscreen->GetTrueHeight();

	int borderHeight = (trueHeight - height) / 2;

	glViewport(0, 0, width, trueHeight);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, width * 1.0, 0.0, trueHeight, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glColor3f(0.f, 0.f, 0.f);
	gl_RenderState.Set2DMode(true);
	gl_RenderState.EnableTexture(false);
	gl_RenderState.Apply(true);

	glBegin(GL_QUADS);
	// upper quad
	glVertex2i(0, borderHeight);
	glVertex2i(0, 0);
	glVertex2i(width, 0);
	glVertex2i(width, borderHeight);

	// lower quad
	glVertex2i(0, trueHeight);
	glVertex2i(0, trueHeight - borderHeight);
	glVertex2i(width, trueHeight - borderHeight);
	glVertex2i(width, trueHeight);
	glEnd();

	gl_RenderState.EnableTexture(true);

	glViewport(0, (trueHeight - height) / 2, width, height); 
#endif
}

//==========================================================================
//
// Draws a texture
//
//==========================================================================

void FGLRenderer::DrawTexture(FTexture *img, DCanvas::DrawParms &parms)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		FMaterial *nativeMaterial = FMaterial::ValidateTexture(img);
		if (nativeMaterial == NULL) return;

		const double xscale = parms.texwidth > 0.0 ? parms.destwidth / parms.texwidth : 1.0;
		const double yscale = parms.texheight > 0.0 ? parms.destheight / parms.texheight : 1.0;
		double left = parms.x - parms.left * xscale;
		double top = parms.y - parms.top * yscale;
		double right = left + parms.destwidth;
		double bottom = top + parms.destheight;
		float u1 = 0.0f, v1 = 0.0f, u2 = 1.0f, v2 = 1.0f;
		if (parms.windowleft > 0.0 || parms.windowright < parms.texwidth)
		{
			const float texWidth = parms.texwidth > 0.0 ? static_cast<float>(parms.texwidth) : 1.0f;
			left += parms.windowleft * xscale;
			right -= (parms.texwidth - parms.windowright) * xscale;
			u1 = static_cast<float>(parms.windowleft) / texWidth;
			u2 = static_cast<float>(parms.windowright) / texWidth;
		}
		if (parms.flipX)
		{
			const float swap = u1;
			u1 = u2;
			u2 = swap;
		}
		const double clipLeft = std::max(left, static_cast<double>(parms.lclip));
		const double clipTop = std::max(top, static_cast<double>(parms.uclip));
		const double clipRight = std::min(right, static_cast<double>(parms.rclip));
		const double clipBottom = std::min(bottom, static_cast<double>(parms.dclip));
		if (clipLeft >= clipRight || clipTop >= clipBottom || right <= left || bottom <= top) return;
		const float clipU1 = u1 + (u2 - u1) * static_cast<float>((clipLeft - left) / (right - left));
		const float clipU2 = u1 + (u2 - u1) * static_cast<float>((clipRight - left) / (right - left));
		const float clipV1 = v1 + (v2 - v1) * static_cast<float>((clipTop - top) / (bottom - top));
		const float clipV2 = v1 + (v2 - v1) * static_cast<float>((clipBottom - top) / (bottom - top));
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float positions[12] =
		{
			2.0f * static_cast<float>(clipLeft) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(clipTop) / height, 0.0f,
			2.0f * static_cast<float>(clipLeft) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(clipBottom) / height, 0.0f,
			2.0f * static_cast<float>(clipRight) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(clipBottom) / height, 0.0f,
			2.0f * static_cast<float>(clipRight) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(clipTop) / height, 0.0f
		};
		const float texcoords[8] = { clipU1, clipV1, clipU1, clipV2, clipU2, clipV2, clipU2, clipV1 };
		uint32 colorOverlay = parms.colorOverlay;
		float light = 1.0f;
		if (colorOverlay != 0 && (colorOverlay & 0xffffff) == 0)
		{
			light = 1.0f - APART(colorOverlay) / 255.0f;
			colorOverlay = 0;
		}
		float color[3] = { light, light, light };
		if (parms.style.Flags & STYLEF_ColorIsFixed)
		{
			color[0] = RPART(parms.fillcolor) / 255.0f;
			color[1] = GPART(parms.fillcolor) / 255.0f;
			color[2] = BPART(parms.fillcolor) / 255.0f;
		}
		int translation = 0;
		if (parms.remap != NULL && !parms.remap->Inactive)
		{
			GLTranslationPalette *palette = static_cast<GLTranslationPalette *>(parms.remap->GetNative());
			if (palette != NULL) translation = -palette->GetIndex();
		}
		const bool alphaChannel = parms.alphaChannel;
		const unsigned int texture = nativeMaterial->BindNative(alphaChannel ? CM_SHADE : CM_DEFAULT,
			alphaChannel ? 0 : translation, false, false);
		int textureMode = 0;
		int sourceBlend = GL_SRC_ALPHA;
		int destinationBlend = GL_ONE_MINUS_SRC_ALPHA;
		int blendEquation = GL_FUNC_ADD;
		gl_GetRenderStyle(parms.style, !parms.masked, false, &textureMode, &sourceBlend,
			&destinationBlend, &blendEquation);
		EGLESBlendMode blendMode = GLES_BLEND_OPAQUE;
		if (parms.style.BlendOp == STYLEOP_Add)
			blendMode = parms.style.DestAlpha == STYLEALPHA_One ? GLES_BLEND_ADD : GLES_BLEND_ALPHA;
		else if (parms.style.BlendOp == STYLEOP_Sub) blendMode = GLES_BLEND_SUBTRACT;
		else if (parms.style.BlendOp == STYLEOP_RevSub) blendMode = GLES_BLEND_REVERSE_SUBTRACT;
		else if (parms.alpha < FRACUNIT || parms.style.BlendOp != STYLEOP_None) blendMode = GLES_BLEND_ALPHA;
		const bool customBlend = blendMode != GLES_BLEND_OPAQUE &&
			(parms.style.BlendOp < STYLEOP_Fuzz || parms.style.BlendOp > STYLEOP_FuzzOrRevSub);
		unsigned int materialFlags = 0;
		if (alphaChannel) materialFlags |= GLES_MATERIAL_RED_IS_ALPHA;
		if (parms.style.Flags & STYLEF_RedIsAlpha) materialFlags |= GLES_MATERIAL_RED_IS_ALPHA;
		if (parms.style.Flags & STYLEF_InvertOverlay) materialFlags |= GLES_MATERIAL_INVERT;
		if (parms.style.Flags & STYLEF_FadeToBlack) materialFlags |= GLES_MATERIAL_FADE_TO_BLACK;
		if (parms.style.Flags & STYLEF_InvertSource) materialFlags |= GLES_MATERIAL_INVERT_SOURCE;
		if (parms.style.Flags & STYLEF_ColorIsFixed) materialFlags |= GLES_MATERIAL_COLOR_FIXED;
		gl_GLES_AddHUDQuad(positions, texcoords, color, FIXED2FLOAT(parms.alpha),
			parms.masked != 0, texture, blendMode, materialFlags, customBlend,
			sourceBlend, destinationBlend);
		if (colorOverlay != 0 && APART(colorOverlay) != 0)
		{
			const float overlayColor[3] =
			{
				RPART(colorOverlay) / 255.0f,
				GPART(colorOverlay) / 255.0f,
				BPART(colorOverlay) / 255.0f
			};
			gl_GLES_AddHUDQuad(positions, texcoords, overlayColor,
				APART(colorOverlay) / 255.0f, false, texture, GLES_BLEND_ALPHA,
				GLES_MATERIAL_COLOR_OVERLAY);
		}
		return;
	}
#endif
#if !defined(__ANDROID__)

	double xscale = parms.destwidth / parms.texwidth;
	double yscale = parms.destheight / parms.texheight;
	double x = parms.x - parms.left * xscale;
	double y = parms.y - parms.top * yscale;
	double w = parms.destwidth;
	double h = parms.destheight;
	float u1, v1, u2, v2, r, g, b;
	float light = 1.f;

	FMaterial * gltex = FMaterial::ValidateTexture(img);

	if (parms.colorOverlay && (parms.colorOverlay & 0xffffff) == 0)
	{
		// Right now there's only black. Should be implemented properly later
		light = 1.f - APART(parms.colorOverlay)/255.f;
		parms.colorOverlay = 0;
	}

	if (!img->bHasCanvas)
	{
		if (!parms.alphaChannel) 
		{
			int translation = 0;
			if (parms.remap != NULL && !parms.remap->Inactive)
			{
				GLTranslationPalette * pal = static_cast<GLTranslationPalette*>(parms.remap->GetNative());
				if (pal) translation = -pal->GetIndex();
			}
			gltex->BindPatch(CM_DEFAULT, translation);
		}
		else 
		{
			// This is an alpha texture
			gltex->BindPatch(CM_SHADE, 0);
		}

		u1 = gltex->GetUL();
		v1 = gltex->GetVT();
		u2 = gltex->GetUR();
		v2 = gltex->GetVB();
	}
	else
	{
		gltex->Bind(CM_DEFAULT, 0, 0);
		u2=1.f;
		v2=-1.f;
		u1 = v1 = 0.f;
		gl_RenderState.SetTextureMode(TM_OPAQUE);
	}
	
	if (parms.flipX)
	{
		float temp = u1;
		u1 = u2;
		u2 = temp;
	}
	

	if (parms.windowleft > 0 || parms.windowright < parms.texwidth)
	{
		x += parms.windowleft * xscale;
		w -= (parms.texwidth - parms.windowright + parms.windowleft) * xscale;

		u1 = float(u1 + parms.windowleft / parms.texwidth);
		u2 = float(u2 - (parms.texwidth - parms.windowright) / parms.texwidth);
	}

	if (parms.style.Flags & STYLEF_ColorIsFixed)
	{
		r = RPART(parms.fillcolor)/255.0f;
		g = GPART(parms.fillcolor)/255.0f;
		b = BPART(parms.fillcolor)/255.0f;
	}
	else
	{
		r = g = b = light;
	}
	
	// scissor test doesn't use the current viewport for the coordinates, so use real screen coordinates
	int btm = (SCREENHEIGHT - screen->GetHeight()) / 2;
	btm = SCREENHEIGHT - btm;

	glEnable(GL_SCISSOR_TEST);
	int space = (static_cast<OpenGLFrameBuffer*>(screen)->GetTrueHeight()-screen->GetHeight())/2;
	glScissor(parms.lclip, btm - parms.dclip + space, parms.rclip - parms.lclip, parms.dclip - parms.uclip);
	
	gl_SetRenderStyle(parms.style, !parms.masked, false);
	if (img->bHasCanvas)
	{
		gl_RenderState.SetTextureMode(TM_OPAQUE);
	}

	glColor4f(r, g, b, FIXED2FLOAT(parms.alpha));
	
	gl_RenderState.EnableAlphaTest(false);
	gl_RenderState.Apply();
	glBegin(GL_TRIANGLE_STRIP);
	glTexCoord2f(u1, v1);
	glVertex2d(x, y);
	glTexCoord2f(u1, v2);
	glVertex2d(x, y + h);
	glTexCoord2f(u2, v1);
	glVertex2d(x + w, y);
	glTexCoord2f(u2, v2);
	glVertex2d(x + w, y + h);
	glEnd();

	if (parms.colorOverlay)
	{
		gl_RenderState.SetTextureMode(TM_MASK);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		gl_RenderState.BlendEquation(GL_FUNC_ADD);
		gl_RenderState.Apply();
		glColor4ub(RPART(parms.colorOverlay),GPART(parms.colorOverlay),BPART(parms.colorOverlay),APART(parms.colorOverlay));
		glBegin(GL_TRIANGLE_STRIP);
		glTexCoord2f(u1, v1);
		glVertex2d(x, y);
		glTexCoord2f(u1, v2);
		glVertex2d(x, y + h);
		glTexCoord2f(u2, v1);
		glVertex2d(x + w, y);
		glTexCoord2f(u2, v2);
		glVertex2d(x + w, y + h);
		glEnd();
	}

	gl_RenderState.EnableAlphaTest(true);
	
	glScissor(0, 0, screen->GetWidth(), screen->GetHeight());
	glDisable(GL_SCISSOR_TEST);
	gl_RenderState.SetTextureMode(TM_MODULATE);
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl_RenderState.BlendEquation(GL_FUNC_ADD);
#endif
}

//==========================================================================
//
//
//
//==========================================================================
void FGLRenderer::DrawLine(int x1, int y1, int x2, int y2, int palcolor, uint32 color)
{
	PalEntry p = color? (PalEntry)color : GPalette.BaseColors[palcolor];
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float dx = static_cast<float>(x2 - x1);
		const float dy = static_cast<float>(y2 - y1);
		const float length = sqrtf(dx * dx + dy * dy);
		if (length > 0.001f)
		{
			const float offsetX = -dy / length * 0.5f;
			const float offsetY = dx / length * 0.5f;
			const float points[8] =
			{
				x1 + offsetX, y1 + offsetY,
				x1 - offsetX, y1 - offsetY,
				x2 - offsetX, y2 - offsetY,
				x2 + offsetX, y2 + offsetY
			};
			float positions[12];
			for (int i = 0; i < 4; ++i)
			{
				positions[i * 3 + 0] = 2.0f * points[i * 2 + 0] / width - 1.0f;
				positions[i * 3 + 1] = 1.0f - 2.0f * points[i * 2 + 1] / height;
				positions[i * 3 + 2] = 0.0f;
			}
			const float rgb[3] = { p.r / 255.0f, p.g / 255.0f, p.b / 255.0f };
			const float alpha = color != 0 && p.a != 0 ? p.a / 255.0f : 1.0f;
			gl_GLES_AddHUDQuad(positions, NULL, rgb, alpha, false, 0,
				alpha < 0.999f ? GLES_BLEND_ALPHA : GLES_BLEND_OPAQUE);
		}
		else
		{
			DrawPixel(x1, y1, palcolor, color);
		}
		return;
	}
#endif
#if !defined(__ANDROID__)
	gl_RenderState.EnableTexture(false);
	gl_RenderState.Apply(true);
	glColor3ub(p.r, p.g, p.b);
	glBegin(GL_LINES);
	glVertex2i(x1, y1);
	glVertex2i(x2, y2);
	glEnd();
	gl_RenderState.EnableTexture(true);
#endif
}

//==========================================================================
//
//
//
//==========================================================================
void FGLRenderer::DrawPixel(int x1, int y1, int palcolor, uint32 color)
{
	PalEntry p = color? (PalEntry)color : GPalette.BaseColors[palcolor];
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float left = 2.0f * static_cast<float>(x1) / width - 1.0f;
		const float right = 2.0f * static_cast<float>(x1 + 1) / width - 1.0f;
		const float top = 1.0f - 2.0f * static_cast<float>(y1) / height;
		const float bottom = 1.0f - 2.0f * static_cast<float>(y1 + 1) / height;
		const float positions[12] =
		{
			left, top, 0.0f, left, bottom, 0.0f,
			right, bottom, 0.0f, right, top, 0.0f
		};
		const float rgb[3] = { p.r / 255.0f, p.g / 255.0f, p.b / 255.0f };
		const float alpha = color != 0 && p.a != 0 ? p.a / 255.0f : 1.0f;
		gl_GLES_AddHUDQuad(positions, NULL, rgb, alpha, false, 0,
			alpha < 0.999f ? GLES_BLEND_ALPHA : GLES_BLEND_OPAQUE);
		return;
	}
#endif
#if !defined(__ANDROID__)
	gl_RenderState.EnableTexture(false);
	gl_RenderState.Apply(true);
	glColor3ub(p.r, p.g, p.b);
	glBegin(GL_POINTS);
	glVertex2i(x1, y1);
	glEnd();
	gl_RenderState.EnableTexture(true);
#endif
}

//===========================================================================
// 
//
//
//===========================================================================

void FGLRenderer::Dim(PalEntry color, float damount, int x1, int y1, int w, int h)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		if (damount <= 0.0f || w <= 0 || h <= 0) return;
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float left = 2.0f * static_cast<float>(x1) / width - 1.0f;
		const float right = 2.0f * static_cast<float>(x1 + w) / width - 1.0f;
		const float top = 1.0f - 2.0f * static_cast<float>(y1) / height;
		const float bottom = 1.0f - 2.0f * static_cast<float>(y1 + h) / height;
		const float positions[12] =
		{
			left, top, 0.0f, left, bottom, 0.0f,
			right, bottom, 0.0f, right, top, 0.0f
		};
		const float rgb[3] = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f };
		gl_GLES_AddHUDQuad(positions, NULL, rgb, damount, false, 0,
			GLES_BLEND_ALPHA);
		return;
	}
#endif
#if !defined(__ANDROID__)
	float r, g, b;
	
	gl_RenderState.EnableTexture(false);
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl_RenderState.AlphaFunc(GL_GREATER,0);
	gl_RenderState.Apply(true);
	
	r = color.r/255.0f;
	g = color.g/255.0f;
	b = color.b/255.0f;
	
	glBegin(GL_TRIANGLE_FAN);
	glColor4f(r, g, b, damount);
	glVertex2i(x1, y1);
	glVertex2i(x1, y1 + h);
	glVertex2i(x1 + w, y1 + h);
	glVertex2i(x1 + w, y1);
	glEnd();
	
	gl_RenderState.EnableTexture(true);
#endif
}

//==========================================================================
//
//
//
//==========================================================================
void FGLRenderer::FlatFill (int left, int top, int right, int bottom, FTexture *src, bool local_origin)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		if (src == NULL || right <= left || bottom <= top) return;
		FMaterial *nativeMaterial = FMaterial::ValidateTexture(src);
		if (nativeMaterial == NULL) return;
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float u1 = local_origin ? 0.0f : static_cast<float>(left) / src->GetWidth();
		const float v1 = local_origin ? 0.0f : static_cast<float>(top) / src->GetHeight();
		const float u2 = static_cast<float>(local_origin ? right - left : right) / src->GetWidth();
		const float v2 = static_cast<float>(local_origin ? bottom - top : bottom) / src->GetHeight();
		const float positions[12] =
		{
			2.0f * static_cast<float>(left) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(top) / height, 0.0f,
			2.0f * static_cast<float>(left) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(bottom) / height, 0.0f,
			2.0f * static_cast<float>(right) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(bottom) / height, 0.0f,
			2.0f * static_cast<float>(right) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(top) / height, 0.0f
		};
		const float texcoords[8] = { u1, v1, u1, v2, u2, v2, u2, v1 };
		const unsigned int nativeTexture = nativeMaterial->BindNative(CM_DEFAULT, 0, true);
		gl_GLES_AddHUDPolygon(positions, texcoords, 4, NULL, 1.0f,
			nativeMaterial->isMasked(), nativeTexture, true, GLES_BLEND_OPAQUE);
		return;
	}
#endif
#if !defined(__ANDROID__)
	float fU1,fU2,fV1,fV2;

	FMaterial *gltexture=FMaterial::ValidateTexture(src);
	
	if (!gltexture) return;

	gltexture->Bind(CM_DEFAULT, 0, 0);
	
	// scaling is not used here.
	if (!local_origin)
	{
		fU1 = float(left) / src->GetWidth();
		fV1 = float(top) / src->GetHeight();
		fU2 = float(right) / src->GetWidth();
		fV2 = float(bottom) / src->GetHeight();
	}
	else
	{		
		fU1 = 0;
		fV1 = 0;
		fU2 = float(right-left) / src->GetWidth();
		fV2 = float(bottom-top) / src->GetHeight();
	}
	gl_RenderState.Apply();
	glBegin(GL_TRIANGLE_STRIP);
	glColor4f(1, 1, 1, 1);
	glTexCoord2f(fU1, fV1); glVertex2f(left, top);
	glTexCoord2f(fU1, fV2); glVertex2f(left, bottom);
	glTexCoord2f(fU2, fV1); glVertex2f(right, top);
	glTexCoord2f(fU2, fV2); glVertex2f(right, bottom);
	glEnd();
#endif
}

//==========================================================================
//
//
//
//==========================================================================
void FGLRenderer::Clear(int left, int top, int right, int bottom, int palcolor, uint32 color)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		if (right <= left || bottom <= top) return;
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float positions[12] =
		{
			2.0f * static_cast<float>(left) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(top) / height, 0.0f,
			2.0f * static_cast<float>(left) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(bottom) / height, 0.0f,
			2.0f * static_cast<float>(right) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(bottom) / height, 0.0f,
			2.0f * static_cast<float>(right) / width - 1.0f,
			1.0f - 2.0f * static_cast<float>(top) / height, 0.0f
		};
		const PalEntry fill = palcolor == -1 || color != 0 ?
			static_cast<PalEntry>(color) : GPalette.BaseColors[palcolor];
		const float rgb[3] = { fill.r / 255.0f, fill.g / 255.0f, fill.b / 255.0f };
		gl_GLES_AddHUDQuad(positions, NULL, rgb, 1.0f, false, 0,
			GLES_BLEND_OPAQUE);
		return;
	}
#endif
#if !defined(__ANDROID__)
	int rt;
	int offY = 0;
	PalEntry p = palcolor==-1 || color != 0? (PalEntry)color : GPalette.BaseColors[palcolor];
	int width = right-left;
	int height= bottom-top;
	
	
	rt = screen->GetHeight() - top;
	
	int space = (static_cast<OpenGLFrameBuffer*>(screen)->GetTrueHeight()-screen->GetHeight())/2;	// ugh...
	rt += space;
	/*
	if (!m_windowed && (m_trueHeight != m_height))
	{
		offY = (m_trueHeight - m_height) / 2;
		rt += offY;
	}
	*/
	
	glEnable(GL_SCISSOR_TEST);
	glScissor(left, rt - height, width, height);
	
	glClearColor(p.r/255.0f, p.g/255.0f, p.b/255.0f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(0.f, 0.f, 0.f, 0.f);
	
	glDisable(GL_SCISSOR_TEST);
#endif
}

//==========================================================================
//
// D3DFB :: FillSimplePoly
//
// Here, "simple" means that a simple triangle fan can draw it.
//
//==========================================================================

void FGLRenderer::FillSimplePoly(FTexture *texture, FVector2 *points, int npoints,
	double originx, double originy, double scalex, double scaley,
	angle_t rotation, FDynamicColormap *colormap, int lightlevel)
{
	#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		if (texture == NULL || points == NULL || npoints < 3) return;
		FMaterial *nativeMaterial = FMaterial::ValidateTexture(texture);
		if (nativeMaterial == NULL) return;
		FColormap cm;
		cm = colormap;
		lightlevel = gl_CalcLightLevel(lightlevel, 0, true);
		const PalEntry light = gl_CalcLightColor(lightlevel, cm.LightColor, cm.blendfactor, true);
		const float width = screen != NULL && screen->GetWidth() > 0 ?
			static_cast<float>(screen->GetWidth()) : static_cast<float>(SCREENWIDTH);
		const float height = screen != NULL && screen->GetHeight() > 0 ?
			static_cast<float>(screen->GetHeight()) : static_cast<float>(SCREENHEIGHT);
		const float uScale = 1.0f / static_cast<float>(texture->GetScaledWidth() * scalex);
		const float vScale = (nativeMaterial->tex->bHasCanvas ? -1.0f : 1.0f) /
			static_cast<float>(texture->GetScaledHeight() * scaley);
		const float radians = static_cast<float>(rotation * M_PI / float(1u << 31));
		const float cosRotation = cosf(radians);
		const float sinRotation = sinf(radians);
		const float ox = static_cast<float>(originx);
		const float oy = static_cast<float>(originy);
		std::vector<float> positions(static_cast<size_t>(npoints) * 3);
		std::vector<float> texcoords(static_cast<size_t>(npoints) * 2);
		for (int i = 0; i < npoints; ++i)
		{
			const float x = points[i].X;
			const float y = points[i].Y;
			positions[i * 3 + 0] = 2.0f * x / width - 1.0f;
			positions[i * 3 + 1] = 1.0f - 2.0f * y / height;
			positions[i * 3 + 2] = 0.0f;
			float u = x - 0.5f - ox;
			float v = y - 0.5f - oy;
			if (rotation != 0)
			{
				const float rotatedU = u;
				u = rotatedU * cosRotation - v * sinRotation;
				v = v * cosRotation + rotatedU * sinRotation;
			}
			texcoords[i * 2 + 0] = u * uScale;
			texcoords[i * 2 + 1] = v * vScale;
		}
		const float color[3] =
		{
			light.r / 255.0f,
			light.g / 255.0f,
			light.b / 255.0f
		};
		const unsigned int nativeTexture = nativeMaterial->BindNative(cm.colormap, 0, true);
		gl_GLES_AddHUDPolygon(&positions[0], &texcoords[0],
			static_cast<unsigned int>(npoints), color, 1.0f, nativeMaterial->isMasked(),
			nativeTexture, true, GLES_BLEND_OPAQUE);
		return;
	}
#endif
#if !defined(__ANDROID__)
	if (npoints < 3)
	{ // This is no polygon.
		return;
	}

	FMaterial *gltexture = FMaterial::ValidateTexture(texture);

	if (gltexture == NULL)
	{
		return;
	}

	FColormap cm;
	cm = colormap;

	lightlevel = gl_CalcLightLevel(lightlevel, 0, true);
	PalEntry pe = gl_CalcLightColor(lightlevel, cm.LightColor, cm.blendfactor, true);
	glColor3ub(pe.r, pe.g, pe.b);

	gltexture->Bind(cm.colormap);

	int i;
	float rot = float(rotation * M_PI / float(1u << 31));
	bool dorotate = rot != 0;

	float cosrot = cos(rot);
	float sinrot = sin(rot);

	//float yoffs = GatheringWipeScreen ? 0 : LBOffset;
	float uscale = float(1.f / (texture->GetScaledWidth() * scalex));
	float vscale = float(1.f / (texture->GetScaledHeight() * scaley));
	if (gltexture->tex->bHasCanvas)
	{
		vscale = 0 - vscale;
	}
	float ox = float(originx);
	float oy = float(originy);

	gl_RenderState.Apply();
	glBegin(GL_TRIANGLE_FAN);
	for (i = 0; i < npoints; ++i)
	{
		float u = points[i].X - 0.5f - ox;
		float v = points[i].Y - 0.5f - oy;
		if (dorotate)
		{
			float t = u;
			u = t * cosrot - v * sinrot;
			v = v * cosrot + t * sinrot;
		}
		glTexCoord2f(u * uscale, v * vscale);
		glVertex3f(points[i].X, points[i].Y /* + yoffs */, 0);
	}
	glEnd();
#endif
}

