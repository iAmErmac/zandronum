/*
** gltexture.cpp
** Low level OpenGL texture handling. These classes are also
** containers for the various translations a texture can have.
**
**---------------------------------------------------------------------------
** Copyright 2004-2005 Christoph Oelckers
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
#include "templates.h"
#include "m_crc32.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_palette.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/textures/gl_material.h"
#ifdef __ANDROID__
#include "gl/system/gl_android.h"
#endif


extern TexFilter_s TexFilter[];
extern int TexFormat[];
EXTERN_CVAR(Bool, gl_clamp_per_texture)


//===========================================================================
// 
//	Static texture data
//
//===========================================================================
unsigned int FHardwareTexture::lastbound[FHardwareTexture::MAX_TEXTURES];
#ifdef __ANDROID__
TArray<FHardwareTexture *> FHardwareTexture::android_textures;
#endif

//===========================================================================
// 
// STATIC - Gets the maximum size of hardware textures
//
//===========================================================================
int FHardwareTexture::GetTexDimension(int value)
{
	if (value > gl.max_texturesize) return gl.max_texturesize;
#ifdef __ANDROID__
	// Native GLES 3.2 supports non-power-of-two 2D textures directly.
	if (gl_AndroidNativeGLES_IsActive()) return value;
#endif
	if (gl.flags&RFL_NPOT_TEXTURE) return value;

	int i=1;
	while (i<value) i+=i;
	return i;
}


#ifdef __ANDROID__
struct BoxPrecalc
{
	int boxStart;
	int boxEnd;
};

static void ResampleBoxPrecalc(TArray<BoxPrecalc>& boxes, int oldDim)
{
	int newDim = boxes.Size();
	const double scale_factor_1 = double(oldDim) / newDim;
	const int scale_factor_2 = (int)(scale_factor_1 / 2);

	for (int dst = 0; dst < newDim; ++dst)
	{
		// Source pixel in the Y direction
		const int src_p = int(dst * scale_factor_1);

		BoxPrecalc& precalc = boxes[dst];
		precalc.boxStart = clamp<int>(int(src_p - scale_factor_1 / 2.0 + 1), 0, oldDim - 1);
		precalc.boxEnd = clamp<int>(MAX<int>(precalc.boxStart + 1, int(src_p + scale_factor_2)), 0, oldDim - 1);
	}
}

static void Resize(int texwidth, int texheight, int width, int height, unsigned char *src_data, unsigned char *dst_data)
{

	// This function implements a simple pre-blur/box averaging method for
	// downsampling that gives reasonably smooth results To scale the image
	// down we will need to gather a grid of pixels of the size of the scale
	// factor in each direction and then do an averaging of the pixels.

	TArray<BoxPrecalc> vPrecalcs(height);
	TArray<BoxPrecalc> hPrecalcs(width);

	ResampleBoxPrecalc(vPrecalcs, texheight);
	ResampleBoxPrecalc(hPrecalcs, texwidth);

	int averaged_pixels, averaged_alpha, src_pixel_index;
	double sum_r, sum_g, sum_b, sum_a;

	for (int y = 0; y < height; y++)         // Destination image - Y direction
	{
		// Source pixel in the Y direction
		const BoxPrecalc& vPrecalc = vPrecalcs[y];

		for (int x = 0; x < width; x++)      // Destination image - X direction
		{
			// Source pixel in the X direction
			const BoxPrecalc& hPrecalc = hPrecalcs[x];

			// Box of pixels to average
			averaged_pixels = 0;
			averaged_alpha = 0;
			sum_r = sum_g = sum_b = sum_a = 0.0;

			for (int j = vPrecalc.boxStart; j <= vPrecalc.boxEnd; ++j)
			{
				for (int i = hPrecalc.boxStart; i <= hPrecalc.boxEnd; ++i)
				{
					// Calculate the actual index in our source pixels
					src_pixel_index = j * texwidth + i;

					int a = src_data[src_pixel_index * 4 + 3];
					if (a > 0)	// do not use color from fully transparent pixels
					{
						sum_r += src_data[src_pixel_index * 4 + 0];
						sum_g += src_data[src_pixel_index * 4 + 1];
						sum_b += src_data[src_pixel_index * 4 + 2];
						sum_a += a;
						averaged_pixels++;
					}
					averaged_alpha++;

				}
			}

			// Calculate the average from the sum and number of averaged pixels
			dst_data[0] = (unsigned char)xs_CRoundToInt(sum_r / averaged_pixels);
			dst_data[1] = (unsigned char)xs_CRoundToInt(sum_g / averaged_pixels);
			dst_data[2] = (unsigned char)xs_CRoundToInt(sum_b / averaged_pixels);
			dst_data[3] = (unsigned char)xs_CRoundToInt(sum_a / averaged_alpha);
			dst_data += 4;
		}
	}
}

static void GL_ResampleTexture (uint32_t *in, uint32_t inwidth, uint32_t inheight, uint32_t *out,  uint32_t outwidth, uint32_t outheight)
{
	//LOGI("GL_ResampleTexture %dx%d -> %dx%d",inwidth,inheight,outwidth,outheight);

	int		i, j;
	uint32_t	*inrow, *inrow2;
	uint32_t	frac, fracstep;
	uint8_t		*pix1, *pix2, *pix3, *pix4;
	uint32_t	*p1 = (uint32_t*)malloc(sizeof(uint32_t) * outwidth );
	uint32_t	*p2 = (uint32_t*)malloc(sizeof(uint32_t) * outwidth );

	fracstep = inwidth*0x10000/outwidth;

	frac = fracstep>>2;
	for (i=0 ; i<outwidth ; i++)
	{
		p1[i] = 4*(frac>>16);
		frac += fracstep;
	}
	frac = 3*(fracstep>>2);
	for (i=0 ; i<outwidth ; i++)
	{
		p2[i] = 4*(frac>>16);
		frac += fracstep;
	}

	for (i=0 ; i<outheight ; i++, out += outwidth)
	{
		inrow = in + inwidth*(int)((i+0.25)*inheight/outheight);
		inrow2 = in + inwidth*(int)((i+0.75)*inheight/outheight);
		frac = fracstep >> 1;
		for (j=0 ; j<outwidth ; j++)
		{
			pix1 = (uint8_t *)inrow + p1[j];
			pix2 = (uint8_t *)inrow + p2[j];
			pix3 = (uint8_t *)inrow2 + p1[j];
			pix4 = (uint8_t *)inrow2 + p2[j];
			((uint8_t *)(out+j))[0] = (pix1[0] + pix2[0] + pix3[0] + pix4[0])>>2;
			((uint8_t *)(out+j))[1] = (pix1[1] + pix2[1] + pix3[1] + pix4[1])>>2;
			((uint8_t *)(out+j))[2] = (pix1[2] + pix2[2] + pix3[2] + pix4[2])>>2;
			((uint8_t *)(out+j))[3] = (pix1[3] + pix2[3] + pix3[3] + pix4[3])>>2;
		}
	}

	free(p1);
	free(p2);
}
#endif

//===========================================================================
// 
//	Loads the texture image into the hardware
//
// NOTE: For some strange reason I was unable to find the source buffer
// should be one line higher than the actual texture. I got extremely
// strange crashes deep inside the GL driver when I didn't do it!
//
//===========================================================================
void FHardwareTexture::LoadImage(unsigned char * buffer,int w, int h, unsigned int & glTexID,int wrapparam, bool alphatexture, int texunit)
{
	int rh,rw;
	int texformat=TexFormat[gl_texture_format];
	bool deletebuffer=false;
	bool use_mipmapping = TexFilter[gl_texture_filter].mipmapping;

	#ifdef __ANDROID__
	// Native GLES uses one sized RGBA format until compressed asset uploads are audited.
	texformat = GL_RGBA8;
	#else
	if (alphatexture) texformat=GL_ALPHA8;
	else if (forcenocompression) texformat = GL_RGBA8;
	#endif
	if (glTexID==0) glGenTextures(1,&glTexID);
	glBindTexture(GL_TEXTURE_2D, glTexID);
	lastbound[texunit]=glTexID;

	if (!buffer)
	{
		w=texwidth;
		h=abs(texheight);
		rw = GetTexDimension (w);
		rh = GetTexDimension (h);

		// The texture must at least be initialized if no data is present.
		mipmap=false;
		#ifndef __ANDROID__
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, false);
		#endif
		buffer=(unsigned char *)calloc(4,rw * (rh+1));
		deletebuffer=true;
		//texheight=-h;	
	}
	else
	{
		rw = GetTexDimension (w);
		rh = GetTexDimension (h);

		#ifndef __ANDROID__
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, (mipmap && use_mipmapping && !forcenofiltering));
		#endif

		if (rw == w && rh == h)
		{
		}
		else if (rw < w || rh < h)
		{
			// The image must be scaled to fit the texture
			unsigned char * scaledbuffer=(unsigned char *)calloc(4,rw * (rh+1));
			if (scaledbuffer)
			{
				//gluScaleImage(GL_RGBA,w, h,GL_UNSIGNED_BYTE,buffer, rw, rh, GL_UNSIGNED_BYTE,scaledbuffer);
				::Resize(w,h,rw,rh,buffer,scaledbuffer);
				deletebuffer=true;
				buffer=scaledbuffer;
			}
		}
        else if (wrapparam==GL_REPEAT) // Need to make bigger by resampling, for 3d textures
        {
            unsigned int * scaledbuffer=(unsigned int *)calloc(4,rw * (rh+1));
            GL_ResampleTexture((unsigned  *)buffer,w,h,(unsigned *)scaledbuffer,rw,rh);
            deletebuffer=true;
            buffer=(unsigned char *)scaledbuffer;
        }
		else
		{
            unsigned int * scaledbuffer=(unsigned int *)calloc(4,rw * (rh+1));
				for(int y=0;y<h;y++)
                for( int x = 0; x < w; x++ )
				{
                    scaledbuffer[x + y * rw] = ((unsigned int *)buffer)[x + y * w];
				}
				deletebuffer=true;
            buffer=(unsigned char *)scaledbuffer;
		}
	}
#ifdef __ANDROID__
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
#elif defined(__MOBILE__)
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
#else
	glTexImage2D(GL_TEXTURE_2D, 0, texformat, rw, rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
#endif
	#ifdef __ANDROID__
	if (mipmap && use_mipmapping && !forcenofiltering)
		gl_AndroidNativeGLES_GenerateMipmap();
	#endif


	if (deletebuffer) free(buffer);

	// When using separate samplers the stuff below is not needed.
	// if (gl.flags & RFL_SAMPLER_OBJECTS) return;

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapparam==GL_CLAMP? GL_CLAMP_TO_EDGE : GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapparam==GL_CLAMP? GL_CLAMP_TO_EDGE : GL_REPEAT);
	clampmode = wrapparam==GL_CLAMP? GLT_CLAMPX|GLT_CLAMPY : 0;

	if (forcenofiltering)
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
#ifndef __MOBILE__
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.f);
#endif
	}
	else
	{
		if (mipmap && use_mipmapping)
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TexFilter[gl_texture_filter].minfilter);
#ifndef __MOBILE__
			if (gl_texture_filter_anisotropic)
			{
				glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, gl_texture_filter_anisotropic);
			}
#endif
		}
		else
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, TexFilter[gl_texture_filter].magfilter);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, TexFilter[gl_texture_filter].magfilter);
	}
}


//===========================================================================
// 
//	Creates a texture
//
//===========================================================================
FHardwareTexture::FHardwareTexture(int _width, int _height, bool _mipmap, bool wrap, bool nofilter, bool nocompression) 
{
	forcenocompression = nocompression;
	mipmap=_mipmap;
	texwidth=_width;
	texheight=_height;

	int cm_arraysize = CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size();
	glTexID = new unsigned[cm_arraysize];
	memset(glTexID,0,sizeof(unsigned int)*cm_arraysize);
	clampmode=0;
	glDepthID = 0;
	forcenofiltering = nofilter;
#ifdef __ANDROID__
	android_textures.Push(this);
#endif
}

#ifdef __ANDROID__
void FHardwareTexture::RememberAndroidSource(const unsigned char *buffer, int w, int h, bool wrap, int cm, int translation)
{
	if (buffer == NULL || w <= 0 || h <= 0) return;
	for (unsigned int i = 0; i < android_sources.Size(); ++i)
	{
		AndroidTextureSource &source = android_sources[i];
		if (source.cm == cm && source.translation == translation)
		{
			free(source.pixels);
			source.pixels = (unsigned char *)malloc(w * h * 4);
			if (source.pixels != NULL) memcpy(source.pixels, buffer, w * h * 4);
			source.width = w;
			source.height = h;
			source.wrap = wrap;
			return;
		}
	}
	const int index = android_sources.Reserve(1);
	AndroidTextureSource &source = android_sources[index];
	source.cm = cm;
	source.translation = translation;
	source.width = w;
	source.height = h;
	source.wrap = wrap;
	source.pixels = (unsigned char *)malloc(w * h * 4);
	if (source.pixels != NULL) memcpy(source.pixels, buffer, w * h * 4);
}

bool FHardwareTexture::RestoreAndroidSource(int cm, int translation, unsigned int &texture, int texunit)
{
	for (unsigned int i = 0; i < android_sources.Size(); ++i)
	{
		AndroidTextureSource &source = android_sources[i];
		if (source.cm == cm && source.translation == translation && source.pixels != NULL)
		{
			LoadImage(source.pixels, source.width, source.height, texture,
				source.wrap ? GL_REPEAT : GL_CLAMP, cm == CM_SHADE, texunit);
			return texture != 0;
		}
	}
	return false;
}

void FHardwareTexture::ClearAndroidSources()
{
	for (unsigned int i = 0; i < android_sources.Size(); ++i)
		free(android_sources[i].pixels);
	android_sources.Clear();
}

void FHardwareTexture::AndroidContextLost()
{
	const int cm_arraysize = CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size();
	for (int i = 0; i < cm_arraysize; ++i) glTexID[i] = 0;
	for (unsigned int i = 0; i < glTexID_Translated.Size(); ++i)
		glTexID_Translated[i].glTexID = 0;
	glDepthID = 0;
}

void FHardwareTexture::AndroidContextLostAll()
{
	for (unsigned int i = 0; i < android_textures.Size(); ++i)
		android_textures[i]->AndroidContextLost();
	memset(lastbound, 0, sizeof(lastbound));
}

void gl_AndroidNativeGLES_InvalidateTextures()
{
	FHardwareTexture::AndroidContextLostAll();
}
#endif


//===========================================================================
// 
//	Deletes a texture id and unbinds it from the texture units
//
//===========================================================================
void FHardwareTexture::DeleteTexture(unsigned int texid)
{
	if (texid != 0) 
	{
		for(int i = 0; i < MAX_TEXTURES; i++)
		{
			if (lastbound[i] == texid)
			{
				lastbound[i] = 0;
			}
		}
		glDeleteTextures(1, &texid);
	}
}

//===========================================================================
// 
//	Frees all associated resources
//
//===========================================================================
void FHardwareTexture::Clean(bool all)
{
	int cm_arraysize = CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size();

	if (all)
	{
		for (int i=0;i<cm_arraysize;i++)
		{
			DeleteTexture(glTexID[i]);
		}
		//glDeleteTextures(cm_arraysize,glTexID);
		memset(glTexID,0,sizeof(unsigned int)*cm_arraysize);
	}
	else
	{
		for (int i=1;i<cm_arraysize;i++)
		{
			DeleteTexture(glTexID[i]);
		}
		//glDeleteTextures(cm_arraysize-1,glTexID+1);
		memset(glTexID+1,0,sizeof(unsigned int)*(cm_arraysize-1));
	}
	for(unsigned int i=0;i<glTexID_Translated.Size();i++)
	{
		DeleteTexture(glTexID_Translated[i].glTexID);
	}
	glTexID_Translated.Clear();
	if (glDepthID != 0) glDeleteRenderbuffers(1, &glDepthID);
}

//===========================================================================
// 
//	Destroys the texture
//
//===========================================================================
FHardwareTexture::~FHardwareTexture() 
{ 
	Clean(true); 
#ifdef __ANDROID__
	for (unsigned int i = 0; i < android_textures.Size(); ++i)
	{
		if (android_textures[i] == this)
		{
			android_textures.Delete(i);
			break;
		}
	}
	ClearAndroidSources();
#endif
	delete [] glTexID;
}


//===========================================================================
// 
//	Gets a texture ID address and validates all required data
//
//===========================================================================

unsigned * FHardwareTexture::GetTexID(int cm, int translation)
{
	if (cm < 0 || cm >= CM_MAXCOLORMAP) cm=CM_DEFAULT;

	if (translation==0)
	{
		return &glTexID[cm];
	}

	// normally there aren't more than very few different 
	// translations here so this isn't performance critical.
	for(unsigned int i=0;i<glTexID_Translated.Size();i++)
	{
		if (glTexID_Translated[i].cm == cm &&
			glTexID_Translated[i].translation == translation)
		{
			return &glTexID_Translated[i].glTexID;
		}
	}

	int add = glTexID_Translated.Reserve(1);
	glTexID_Translated[add].cm=cm;
	glTexID_Translated[add].translation=translation;
	glTexID_Translated[add].glTexID=0;
	return &glTexID_Translated[add].glTexID;
}

//===========================================================================
// 
//	Binds this patch
//
//===========================================================================
unsigned int FHardwareTexture::Bind(int texunit, int cm,int translation)
{
	unsigned int * pTexID=GetTexID(cm, translation);
	#ifdef __ANDROID__
	if (*pTexID == 0 && gl_AndroidNativeGLES_CanUseResources())
		RestoreAndroidSource(cm, translation, *pTexID, texunit);
	#endif

	if (*pTexID!=0)
	{
		if (lastbound[texunit]==*pTexID) return *pTexID;
		lastbound[texunit]=*pTexID;
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, *pTexID);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		return *pTexID;
	}
	return 0;
}


void FHardwareTexture::Unbind(int texunit)
{
	if (lastbound[texunit] != 0)
	{
		if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (texunit != 0) glActiveTexture(GL_TEXTURE0);
		lastbound[texunit] = 0;
	}
}

void FHardwareTexture::UnbindAll()
{
	for(int texunit = 0; texunit < 16; texunit++)
	{
		Unbind(texunit);
	}
}

//===========================================================================
// 
//	Creates a depth buffer for this texture
//
//===========================================================================

int FHardwareTexture::GetDepthBuffer()
{
	if (gl.flags & RFL_FRAMEBUFFER)
	{
		if (glDepthID == 0)
		{
			glGenRenderbuffers(1, &glDepthID);
			glBindRenderbuffer(GL_RENDERBUFFER, glDepthID);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 
				GetTexDimension(texwidth), GetTexDimension(texheight));
			glBindRenderbuffer(GL_RENDERBUFFER, 0);
		}
		return glDepthID;
	}
	return 0;
}


//===========================================================================
// 
//	Binds this texture's surfaces to the current framrbuffer
//
//===========================================================================

void FHardwareTexture::BindToFrameBuffer()
{
	if (gl.flags & RFL_FRAMEBUFFER)
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTexID[0], 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GetDepthBuffer()); 
	}
}


//===========================================================================
// 
//	(re-)creates the texture
//
//===========================================================================
unsigned int FHardwareTexture::CreateTexture(unsigned char * buffer, int w, int h, bool wrap, int texunit,
									  int cm, int translation)
{
	if (cm < 0 || cm >= CM_MAXCOLORMAP) cm=CM_DEFAULT;

	unsigned int * pTexID=GetTexID(cm, translation);
	#ifdef __ANDROID__
	RememberAndroidSource(buffer, w, h, wrap, cm, translation);
	#endif

	if (texunit != 0) glActiveTexture(GL_TEXTURE0+texunit);
	LoadImage(buffer, w, h, *pTexID, wrap? GL_REPEAT:GL_CLAMP, cm==CM_SHADE, texunit);
	if (texunit != 0) glActiveTexture(GL_TEXTURE0);
	return *pTexID;
}


