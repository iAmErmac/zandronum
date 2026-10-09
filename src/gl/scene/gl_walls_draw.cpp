/*
** gl_walls_draw.cpp
** Wall rendering
**
**---------------------------------------------------------------------------
** Copyright 2000-2005 Christoph Oelckers
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
#include "p_local.h"
#include "p_lnspec.h"
#include "a_sharedglobal.h"
#include "gl/gl_functions.h"

#include "gl/system/gl_interface.h"
#include "gl/system/gl_cvars.h"
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
#include <vector>
#include "gl/system/gl_gles_renderer.h"
#include "gl/system/gl_gles_scene.h"
#endif
#include "gl/renderer/gl_lightdata.h"
#include "gl/renderer/gl_renderstate.h"
#include "gl/data/gl_data.h"
#include "gl/dynlights/gl_dynlight.h"
#include "gl/dynlights/gl_glow.h"
#include "gl/scene/gl_drawinfo.h"
#include "gl/scene/gl_portal.h"
#include "gl/shaders/gl_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/utility/gl_clock.h"
#include "gl/utility/gl_templates.h"

EXTERN_CVAR(Bool, gl_seamless)

//==========================================================================
//
// Sets up the texture coordinates for one light to be rendered
//
//==========================================================================
bool GLWall::PrepareLight(texcoord * tcs, ADynamicLight * light)
{
	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;
	Vector nearPt, up, right;
	float scale;

	p.Init(vtx,4);

	if (!p.ValidNormal()) 
	{
		return false;
	}

	if (!gl_SetupLight(p, light, nearPt, up, right, scale, Colormap.colormap, true, !!(flags&GLWF_FOGGY))) 
	{
		return false;
	}

	if (tcs != NULL)
	{
		Vector t1;
		int outcnt[4]={0,0,0,0};

		for(int i=0;i<4;i++)
		{
			t1.Set(&vtx[i*3]);
			Vector nearToVert = t1 - nearPt;
			tcs[i].u = (nearToVert.Dot(right) * scale) + 0.5f;
			tcs[i].v = (nearToVert.Dot(up) * scale) + 0.5f;

			// quick check whether the light touches this polygon
			if (tcs[i].u<0) outcnt[0]++;
			if (tcs[i].u>1) outcnt[1]++;
			if (tcs[i].v<0) outcnt[2]++;
			if (tcs[i].v>1) outcnt[3]++;

		}
		// The light doesn't touch this polygon
		if (outcnt[0]==4 || outcnt[1]==4 || outcnt[2]==4 || outcnt[3]==4) return false;
	}

	draw_dlight++;
	return true;
}

//==========================================================================
//
// Collect lights for shader
//
//==========================================================================
FDynLightData lightdata;
static unsigned int nativeLightCounts[3] = { 0, 0, 0 };

void GLWall::SetupLights(bool collect)
{
	// [AK] Take care of gl_lights_size and ZADF_FORCE_VIDEO_DEFAULTS.
	OVERRIDE_LIGHTS_SIZE_IF_NECESSARY

	float vtx[]={glseg.x1,zbottom[0],glseg.y1, glseg.x1,ztop[0],glseg.y1, glseg.x2,ztop[1],glseg.y2, glseg.x2,zbottom[1],glseg.y2};
	Plane p;

	lightdata.Clear();
	nativeLightCounts[0] = nativeLightCounts[1] = nativeLightCounts[2] = 0;
	if (!collect) return;
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	const bool projected = gl_GLES_IsActive() && !gl_dynlight_shader;
	unsigned int projectedOrder = 0;
#endif
	p.Init(vtx,4);

	if (!p.ValidNormal()) 
	{
		return;
	}
	for(int i=0;i<2;i++)
	{
		FLightNode *node;
		if (seg->sidedef == NULL)
		{
			node = NULL;
		}
		else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
		{
			node = seg->sidedef->lighthead[i];
		}
		else if (sub)
		{
			// Polobject segs cannot be checked per sidedef so use the subsector instead.
			node = sub->lighthead[i];
		}
		else node = NULL;

		// Iterate through all dynamic lights which touch this wall and render them
		while (node)
		{
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
			if (projected && node->lightsource->owned && node->lightsource->target != NULL &&
				!node->lightsource->target->IsVisibleToPlayer())
			{
				node = node->nextLight;
				continue;
			}
#endif
			if (!(node->lightsource->flags2&MF2_DORMANT))
			{
				iter_dlight++;

				Vector fn, pos;

				float x = FIXED2FLOAT(node->lightsource->x);
				float y = FIXED2FLOAT(node->lightsource->y);
				float z = FIXED2FLOAT(node->lightsource->z);
				float dist = fabsf(p.DistToPoint(x, z, y));
				float radius = (node->lightsource->GetRadius() * gl_lights_size);
				float scale = 1.0f / ((2.f * radius) - dist);

				if (radius > 0.f && dist < radius)
				{
					Vector nearPt, up, right;

					pos.Set(x,z,y);
					fn=p.Normal();
					fn.GetRightUp(right, up);

					Vector tmpVec = fn * dist;
					nearPt = pos + tmpVec;

					Vector t1;
					int outcnt[4]={0,0,0,0};
					texcoord tcs[4];

					// do a quick check whether the light touches this polygon
					for(int i=0;i<4;i++)
					{
						t1.Set(&vtx[i*3]);
						Vector nearToVert = t1 - nearPt;
						tcs[i].u = (nearToVert.Dot(right) * scale) + 0.5f;
						tcs[i].v = (nearToVert.Dot(up) * scale) + 0.5f;

						if (tcs[i].u<0) outcnt[0]++;
						if (tcs[i].u>1) outcnt[1]++;
						if (tcs[i].v<0) outcnt[2]++;
						if (tcs[i].v>1) outcnt[3]++;

					}
					if (outcnt[0]!=4 && outcnt[1]!=4 && outcnt[2]!=4 && outcnt[3]!=4) 
					{
						bool forceAdditive = false;
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
						forceAdditive = projected && (flags & GLWF_FOGGY);
						const unsigned int previousSizes[3] = { lightdata.arrays[0].Size(), lightdata.arrays[1].Size(), lightdata.arrays[2].Size() };
#endif
						if (gl_GetLight(p, node->lightsource, Colormap.colormap, true, forceAdditive, lightdata))
						{
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
							if (projected)
								for (unsigned int kind = 0; kind < 3; ++kind)
									if (lightdata.arrays[kind].Size() > previousSizes[kind])
										gl_GLESInternalSceneMarkProjectedLight(&lightdata.arrays[kind][previousSizes[kind]], projectedOrder++, kind == 1 && i == 1 ? 3 : kind);
#endif
						}
					}
				}
			}
			node = node->nextLight;
		}
	}
	int numlights[3];
	int allNativeLights[3];

	lightdata.Combine(numlights, gl.MaxLights(), allNativeLights);
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (projected && allNativeLights[2] > 0)
		gl_GLESInternalSceneOrderProjectedLights(&lightdata.arrays[0][0], allNativeLights[2] / 2);
#endif
	nativeLightCounts[0] = static_cast<unsigned int>(allNativeLights[0]);
	nativeLightCounts[1] = static_cast<unsigned int>(allNativeLights[1]);
	nativeLightCounts[2] = static_cast<unsigned int>(allNativeLights[2]);
	if (numlights[2] > 0)
	{
		draw_dlight+=numlights[2]/2;
		gl_RenderState.EnableLight(true);
		gl_RenderState.SetLights(numlights, &lightdata.arrays[0][0]);
	}
}

//==========================================================================
//
// General purpose wall rendering function
// everything goes through here
//
//==========================================================================

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
static void BuildNativeWallEdges(const GLWall &wall, const texcoord *tcs, bool glow,
	std::vector<float> &positions, std::vector<float> &texcoords,
	std::vector<float> &glowDistances, std::vector<unsigned int> &indices)
{
	// Keep the four corners first for lighting-plane and sort calculations.
	std::vector<unsigned int> perimeter;
	auto append = [&](float x, float y, float z, float u, float v, float top, float bottom)
	{
		perimeter.push_back(static_cast<unsigned int>(positions.size() / 3));
		positions.push_back(x); positions.push_back(y); positions.push_back(z);
		texcoords.push_back(u); texcoords.push_back(v);
		glowDistances.push_back(glow ? top : 0.0f);
		glowDistances.push_back(glow ? bottom : 0.0f);
	};
	perimeter.push_back(0);
	if (wall.glseg.fracleft == 0 && wall.vertexes[0] != NULL)
	{
		const vertex_t *vertex = wall.vertexes[0];
		const float height = wall.ztop[0] - wall.zbottom[0];
		const float u = height ? (tcs[1].u - tcs[0].u) / height : 0.0f;
		const float v = height ? (tcs[1].v - tcs[0].v) / height : 0.0f;
		int index = 0;
		while (index < vertex->numheights && vertex->heightlist[index] <= wall.zbottom[0]) ++index;
		while (index < vertex->numheights && vertex->heightlist[index] < wall.ztop[0])
		{
			const float y = vertex->heightlist[index++];
			append(wall.glseg.x1, y, wall.glseg.y1,
				u * (y - wall.ztop[0]) + tcs[1].u, v * (y - wall.ztop[0]) + tcs[1].v,
				wall.zceil[0] - y, y - wall.zfloor[0]);
		}
	}
	perimeter.push_back(1);
	const side_t *side = wall.seg->sidedef;
	const float width = wall.glseg.fracright - wall.glseg.fracleft;
	if (!(wall.flags & GLWall::GLWF_NOSPLITUPPER) && side->numsegs > 1)
	{
		const float u = (tcs[2].u - tcs[1].u) / width;
		const float v = (tcs[2].v - tcs[1].v) / width;
		const float top = (wall.ztop[1] - wall.ztop[0]) / width;
		const float ceiling = (wall.zceil[1] - wall.zceil[0]) / width;
		const float floor = (wall.zfloor[1] - wall.zfloor[0]) / width;
		for (int index = 0; index < side->numsegs - 1; ++index)
		{
			const seg_t *segment = side->segs[index];
			if (segment->sidefrac <= wall.glseg.fracleft) continue;
			if (segment->sidefrac >= wall.glseg.fracright) break;
			const float fraction = segment->sidefrac - wall.glseg.fracleft;
			append(segment->v2->fx, wall.ztop[0] + top * fraction, segment->v2->fy,
				tcs[1].u + u * fraction, tcs[1].v + v * fraction,
				wall.zceil[0] - wall.ztop[0] + (ceiling - top) * fraction,
				wall.ztop[0] - wall.zfloor[0] + (top - floor) * fraction);
		}
	}
	perimeter.push_back(2);
	if (wall.glseg.fracright == 1 && wall.vertexes[1] != NULL)
	{
		const vertex_t *vertex = wall.vertexes[1];
		const float height = wall.ztop[1] - wall.zbottom[1];
		const float u = height ? (tcs[2].u - tcs[3].u) / height : 0.0f;
		const float v = height ? (tcs[2].v - tcs[3].v) / height : 0.0f;
		int index = vertex->numheights - 1;
		while (index > 0 && vertex->heightlist[index] >= wall.ztop[1]) --index;
		while (index > 0 && vertex->heightlist[index] > wall.zbottom[1])
		{
			const float y = vertex->heightlist[index--];
			append(wall.glseg.x2, y, wall.glseg.y2,
				u * (y - wall.ztop[1]) + tcs[2].u, v * (y - wall.ztop[1]) + tcs[2].v,
				wall.zceil[1] - y, y - wall.zfloor[1]);
		}
	}
	perimeter.push_back(3);
	if (!(wall.flags & GLWall::GLWF_NOSPLITLOWER) && side->numsegs > 1)
	{
		const float u = (tcs[3].u - tcs[0].u) / width;
		const float v = (tcs[3].v - tcs[0].v) / width;
		const float bottom = (wall.zbottom[1] - wall.zbottom[0]) / width;
		const float ceiling = (wall.zceil[1] - wall.zceil[0]) / width;
		const float floor = (wall.zfloor[1] - wall.zfloor[0]) / width;
		for (int index = side->numsegs - 2; index >= 0; --index)
		{
			const seg_t *segment = side->segs[index];
			if (segment->sidefrac >= wall.glseg.fracright) continue;
			if (segment->sidefrac <= wall.glseg.fracleft) break;
			const float fraction = segment->sidefrac - wall.glseg.fracleft;
			append(segment->v2->fx, wall.zbottom[0] + bottom * fraction, segment->v2->fy,
				tcs[0].u + u * fraction, tcs[0].v + v * fraction,
				wall.zceil[0] - wall.zbottom[0] + (ceiling - bottom) * fraction,
				wall.zbottom[0] - wall.zfloor[0] + (bottom - floor) * fraction);
		}
	}
	if (positions.size() == 12) return;
	for (size_t index = 1; index + 1 < perimeter.size(); ++index)
	{
		indices.push_back(0);
		indices.push_back(perimeter[index]);
		indices.push_back(perimeter[index + 1]);
	}
}
#endif

void GLWall::RenderWall(int textured, float * color2, ADynamicLight * light)
{
	texcoord tcs[4];
	bool glowing;
	bool split = (gl_seamless && !(textured&4) && seg->sidedef != NULL && !(seg->sidedef->Flags & WALLF_POLYOBJ));

	if (!light)
	{
		tcs[0]=lolft;
		tcs[1]=uplft;
		tcs[2]=uprgt;
		tcs[3]=lorgt;
		glowing = !!(flags&GLWF_GLOW) && (textured & 2);
	}
	else
	{
		if (!PrepareLight(tcs, light)) return;
		glowing = false;
	}

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		if (light != NULL) return;
		FColormap lightingColormap = Colormap;
		const bool unfoggedBase = textured == 3 && (flags & GLWF_FOGGY) && gl_GLES_UsesUntexturedBasePass();
		if (unfoggedBase) lightingColormap.FadeColor = 0;
		FShaderLightParameters lighting = gl_GetShaderLightParameters(lightlevel, rellight + getExtraLight(), &lightingColormap);
		if (unfoggedBase)
		{
			lighting.factor = 1.0f;
			lighting.distance = 0.0f;
		}
		float color[3];
		gl_GetLightColor(lightlevel, rellight + getExtraLight(), &Colormap,
			color + 0, color + 1, color + 2);
		float fogColor[3];
		float fogDensity;
		const bool nativeFog = gl_GetFogParameters(lightlevel, &Colormap,
			RenderStyle == STYLE_Add, fogColor, &fogDensity) && !unfoggedBase;
		float projectedFogDensity = 0.0f;
		if (!gl_dynlight_shader && gl_lights)
		{
			float lightFogColor[3];
			gl_GetFogParameters((flags & GLWF_FOGGY) ? lightlevel : (255 + lightlevel) >> 1,
				(flags & GLWF_FOGGY) ? &Colormap : NULL, true, lightFogColor, &projectedFogDensity);
		}
		const float positions[12] =
		{
			glseg.x1, zbottom[0], glseg.y1,
			glseg.x1, ztop[0], glseg.y1,
			glseg.x2, ztop[1], glseg.y2,
			glseg.x2, zbottom[1], glseg.y2
		};
		float texcoords[8] =
		{
			tcs[0].u, tcs[0].v, tcs[1].u, tcs[1].v,
			tcs[2].u, tcs[2].v, tcs[3].u, tcs[3].v
		};
		const unsigned int texture = gltexture != NULL ? gltexture->BindNative(Colormap.colormap, 0, true) : 0;
		const unsigned int brightmap = gltexture != NULL && gl_BrightmapsActive() &&
			gl_fixedcolormap == CM_DEFAULT && !(flags & GLWF_FOGGY) ?
			gltexture->BindNativeBrightmap(true) : 0;
		const bool transparent = textured == 5 && gltexture != NULL && gltexture->GetTransparent();
		EGLESBlendMode blendMode =
			(color2 == NULL && (alpha < 0.999f || transparent)) ? GLES_BLEND_ALPHA : GLES_BLEND_OPAQUE;
		if (RenderStyle == STYLE_Add) blendMode = GLES_BLEND_ADD;
		else if (RenderStyle == STYLE_Subtract) blendMode = GLES_BLEND_REVERSE_SUBTRACT;
		const float glowDistances[8] =
		{
			zceil[0] - zbottom[0], zbottom[0] - zfloor[0],
			zceil[0] - ztop[0], ztop[0] - zfloor[0],
			zceil[1] - ztop[1], ztop[1] - zfloor[1],
			zceil[1] - zbottom[1], zbottom[1] - zfloor[1]
		};
		bool projectedBase = false;
		if (textured == 3 && !gl_dynlight_shader && gl_lights)
		{
			projectedBase = gl_forcemultipass;
			if (!gl_fixedcolormap)
			{
				if (seg == NULL || seg->sidedef == NULL) projectedBase = false;
				else if (!(seg->sidedef->Flags & WALLF_POLYOBJ)) projectedBase = seg->sidedef->lighthead[0] != NULL;
				else if (sub != NULL) projectedBase = sub->lighthead[0] != NULL;
			}
		}
		const unsigned int materialFlags =
			(projectedBase ? GLES_MATERIAL_PROJECTED_BASE : 0) |
			((flags & GLWF_FOGGY) ? GLES_MATERIAL_PROJECTED_FOG : 0) |
			(unfoggedBase ? GLES_MATERIAL_INHERIT_FOG : 0) |
			(gl_GLES_MaskedTextureRGB() ? GLES_MATERIAL_MASK_TEXTURE_RGB : 0) |
			GLES_MATERIAL_WORLD_SURFACE | ((flags & GLT_CLAMPX) ? GLES_MATERIAL_CLAMP_X : 0) |
			((flags & GLT_CLAMPY) ? GLES_MATERIAL_CLAMP_Y : 0);
		std::vector<float> edgePositions, edgeTexcoords, edgeGlowDistances;
		std::vector<unsigned int> edgeIndices;
		if (split)
		{
			edgePositions.assign(positions, positions + 12);
			edgeTexcoords.assign(texcoords, texcoords + 8);
			edgeGlowDistances.assign(glowDistances, glowDistances + 8);
			BuildNativeWallEdges(*this, tcs, glowing, edgePositions, edgeTexcoords, edgeGlowDistances, edgeIndices);
		}
		const bool splitEdges = !edgeIndices.empty();
		gl_GLES_AddWall(splitEdges ? edgePositions.data() : positions,
			splitEdges ? edgeTexcoords.data() : texcoords, color, color2 != NULL ? 1.0f : alpha,
			texture, gltexture != NULL && gltexture->isMasked() &&
				type != RENDERWALL_TOP && type != RENDERWALL_M1S && type != RENDERWALL_BOTTOM,
			nativeFog, true, fogColor, fogDensity,
			blendMode, materialFlags, nativeLightCounts[2] > 0 ? &lightdata.arrays[0][0] : NULL, nativeLightCounts,
			brightmap, (Colormap.colormap >= CM_DESAT0 && Colormap.colormap <= CM_DESAT31) ?
			Colormap.colormap : 0, glowing ? topglowcolor : NULL, glowing ? bottomglowcolor : NULL,
			glowing ? (splitEdges ? edgeGlowDistances.data() : glowDistances) : NULL,
			&lighting, transparent ? 0.0f : 0.5f, false, 0, 0, projectedFogDensity,
			splitEdges ? edgeIndices.data() : NULL, splitEdges ? static_cast<unsigned int>(edgePositions.size() / 3) : 4,
			splitEdges ? static_cast<unsigned int>(edgeIndices.size()) : 6);
		vertexcount += splitEdges ? static_cast<int>(edgePositions.size() / 3) : 4;
		return;
	}
#endif
#if !defined(__ANDROID__)

	if (glowing) gl_RenderState.SetGlowParams(topglowcolor, bottomglowcolor);

	gl_RenderState.Apply();

	// the rest of the code is identical for textured rendering and lights

	glBegin(GL_TRIANGLE_FAN);

	// lower left corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[0] - zbottom[0], zbottom[0] - zfloor[0]);
	if (textured&1) glTexCoord2f(tcs[0].u,tcs[0].v);
	glVertex3f(glseg.x1,zbottom[0],glseg.y1);

	if (split && glseg.fracleft==0) SplitLeftEdge(tcs, glowing);

	// upper left corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[0] - ztop[0], ztop[0] - zfloor[0]);
	if (textured&1) glTexCoord2f(tcs[1].u,tcs[1].v);
	glVertex3f(glseg.x1,ztop[0],glseg.y1);

	if (split && !(flags & GLWF_NOSPLITUPPER)) SplitUpperEdge(tcs, glowing);

	// color for right side
	if (color2) glColor4fv(color2);

	// upper right corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[1] - ztop[1], ztop[1] - zfloor[1]);
	if (textured&1) glTexCoord2f(tcs[2].u,tcs[2].v);
	glVertex3f(glseg.x2,ztop[1],glseg.y2);

	if (split && glseg.fracright==1) SplitRightEdge(tcs, glowing);

	// lower right corner
	if (glowing) glVertexAttrib2f(VATTR_GLOWDISTANCE, zceil[1] - zbottom[1], zbottom[1] - zfloor[1]);
	if (textured&1) glTexCoord2f(tcs[3].u,tcs[3].v); 
	glVertex3f(glseg.x2,zbottom[1],glseg.y2);

	if (split && !(flags & GLWF_NOSPLITLOWER)) SplitLowerEdge(tcs, glowing);

	glEnd();

	vertexcount+=4;

#endif
}

//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderFogBoundary()
{
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		OVERRIDE_FOGMODE_IF_NECESSARY
		if (!gl_fogmode || gl_fixedcolormap != 0) return;

		float fogColor[3];
		float fogDensity = 0.0f;
		gl_GetFogParameters(lightlevel, &Colormap, false, fogColor, &fogDensity);
		const float color[3] = { 1.0f, 1.0f, 1.0f };
		const float positions[12] =
		{
			glseg.x1, zbottom[0], glseg.y1,
			glseg.x1, ztop[0], glseg.y1,
			glseg.x2, ztop[1], glseg.y2,
			glseg.x2, zbottom[1], glseg.y2
		};
		gl_GLES_AddWall(positions, NULL, color, 1.0f, 0, false, true, false,
			fogColor, fogDensity, GLES_BLEND_ALPHA, GLES_MATERIAL_FOG_BOUNDARY);
		return;
	}
#endif
#if !defined(__ANDROID__)
	// [BB/EP] Take care of gl_fogmode and ZADF_FORCE_VIDEO_DEFAULTS.
	OVERRIDE_FOGMODE_IF_NECESSARY

	if (gl_fogmode && gl_fixedcolormap == 0)
	{
		// with shaders this can be done properly
		if (gl.shadermodel == 4 || (gl.shadermodel == 3 && gl_fog_shader))
		{
			int rel = rellight + getExtraLight();
			gl_SetFog(lightlevel, rel, &Colormap, false);
			gl_RenderState.SetEffect(EFF_FOGBOUNDARY);
			gl_RenderState.EnableAlphaTest(false);
			RenderWall(0, NULL);
			gl_RenderState.EnableAlphaTest(true);
			gl_RenderState.SetEffect(EFF_NONE);
		}
		else
		{
			// otherwise some approximation is needed. This won't look as good
			// as the shader version but it's an acceptable compromise.
			float fogdensity=gl_GetFogDensity(lightlevel, Colormap.FadeColor);

			float xcamera=FIXED2FLOAT(viewx);
			float ycamera=FIXED2FLOAT(viewy);

			float dist1=Dist2(xcamera,ycamera, glseg.x1,glseg.y1);
			float dist2=Dist2(xcamera,ycamera, glseg.x2,glseg.y2);


			// these values were determined by trial and error and are scale dependent!
			float fogd1=(0.95f-exp(-fogdensity*dist1/62500.f)) * 1.05f;
			float fogd2=(0.95f-exp(-fogdensity*dist2/62500.f)) * 1.05f;

			gl_ModifyColor(Colormap.FadeColor.r, Colormap.FadeColor.g, Colormap.FadeColor.b, Colormap.colormap);
			float fc[4]={Colormap.FadeColor.r/255.0f,Colormap.FadeColor.g/255.0f,Colormap.FadeColor.b/255.0f,fogd2};

			gl_RenderState.EnableTexture(false);
			gl_RenderState.EnableFog(false);
			gl_RenderState.AlphaFunc(GL_GREATER,0);
			glDepthFunc(GL_LEQUAL);
			glColor4f(fc[0],fc[1],fc[2], fogd1);
			if (glset.lightmode == 8) glVertexAttrib1f(VATTR_LIGHTLEVEL, 1.0); // Korshun.

			flags &= ~GLWF_GLOW;
			RenderWall(4,fc);

			glDepthFunc(GL_LESS);
			gl_RenderState.EnableFog(true);
			gl_RenderState.AlphaFunc(GL_GEQUAL,0.5f);
			gl_RenderState.EnableTexture(true);
		}
	}
#endif
}


//==========================================================================
//
// 
//
//==========================================================================
void GLWall::RenderMirrorSurface()
{
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive())
	{
		OVERRIDE_FOGMODE_IF_NECESSARY
		if (GLRenderer->mirrortexture == NULL) return;
		FMaterial *material = FMaterial::ValidateTexture(GLRenderer->mirrortexture);
		if (material == NULL) return;
		const unsigned int texture = material->BindNative(Colormap.colormap, 0, false);
		if (texture == 0) return;
		float color[3];
		gl_GetLightColor(lightlevel, 0, &Colormap, color + 0, color + 1, color + 2);
		float fogColor[3] = { 0.0f, 0.0f, 0.0f };
		float fogDensity = 0.0f;
		const bool nativeFog = gl_GetFogParameters(lightlevel, &Colormap, true, fogColor, &fogDensity);
		FShaderLightParameters lighting = gl_GetShaderLightParameters(lightlevel, 0, &Colormap);
		// The sphere-map effect omits software lighting.
		lighting.software = 0.0f;
		const float positions[12] =
		{
			glseg.x1, zbottom[0], glseg.y1,
			glseg.x1, ztop[0], glseg.y1,
			glseg.x2, ztop[1], glseg.y2,
			glseg.x2, zbottom[1], glseg.y2
		};
		gl_GLES_AddWall(positions, NULL, color, 0.1f, texture, false, nativeFog, false,
			fogColor, fogDensity, GLES_BLEND_ADD, GLES_MATERIAL_SPHERE_MAP,
			NULL, NULL, 0, 0, NULL, NULL, NULL, &lighting);
		if (seg->sidedef->AttachedDecals) DoDrawDecals();
		return;
	}
#endif
#if !defined(__ANDROID__)
	if (GLRenderer->mirrortexture == NULL) return;

	// For the sphere map effect we need a normal of the mirror surface,
	Vector v(glseg.y2-glseg.y1, 0 ,-glseg.x2+glseg.x1);
	v.Normalize();
	glNormal3fv(&v[0]);

	// Use sphere mapping for this
	gl_RenderState.SetEffect(EFF_SPHEREMAP);

	gl_SetColor(lightlevel, 0, &Colormap ,0.1f);
	gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);
	gl_RenderState.AlphaFunc(GL_GREATER,0);
	glDepthFunc(GL_LEQUAL);
	gl_SetFog(lightlevel, getExtraLight(), &Colormap, true);

	FMaterial * pat=FMaterial::ValidateTexture(GLRenderer->mirrortexture);
	pat->BindPatch(Colormap.colormap, 0);

	flags &= ~GLWF_GLOW;
	//flags |= GLWF_NOSHADER;
	RenderWall(0,NULL);

	gl_RenderState.SetEffect(EFF_NONE);

	// Restore the defaults for the translucent pass
	gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	gl_RenderState.AlphaFunc(GL_GEQUAL,0.5f*gl_mask_sprite_threshold);
	glDepthFunc(GL_LESS);

	// This is drawn in the translucent pass which is done after the decal pass
	// As a result the decals have to be drawn here.
	if (seg->sidedef->AttachedDecals)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
		glPolygonOffset(-1.0f, -128.0f);
		glDepthMask(false);
		DoDrawDecals();
		glDepthMask(true);
		glPolygonOffset(0.0f, 0.0f);
		glDisable(GL_POLYGON_OFFSET_FILL);
		gl_RenderState.SetTextureMode(TM_MODULATE);
		gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
#endif
}


//==========================================================================
//
// 
//
//==========================================================================

void GLWall::RenderTranslucentWall()
{
	bool transparent = gltexture? gltexture->GetTransparent() : false;
	
	// currently the only modes possible are solid, additive or translucent
	// and until that changes I won't fix this code for the new blending modes!
	bool isadditive = RenderStyle == STYLE_Add;

	if (!transparent) gl_RenderState.AlphaFunc(GL_GEQUAL,gl_mask_threshold*fabs(alpha));
	else gl_RenderState.EnableAlphaTest(false);
	if (isadditive) gl_RenderState.BlendFunc(GL_SRC_ALPHA,GL_ONE);

	int extra;
	if (gltexture) 
	{
		if (flags&GLWF_FOGGY) gl_RenderState.EnableBrightmap(false);
		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		gltexture->Bind(Colormap.colormap, flags, 0);
		extra = getExtraLight();
	}
	else 
	{
		gl_RenderState.EnableTexture(false);
		extra = 0;
	}

	gl_SetColor(lightlevel, extra, &Colormap, fabsf(alpha));
	if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, extra, &Colormap, isadditive);
	else gl_SetFog(255, 0, NULL, false);

	RenderWall(5,NULL);

	// restore default settings
	if (isadditive) gl_RenderState.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	if (transparent) gl_RenderState.EnableAlphaTest(true);

	if (!gltexture)	
	{
		gl_RenderState.EnableTexture(true);
	}
	gl_RenderState.EnableBrightmap(true);
	gl_RenderState.EnableGlow(false);
}

//==========================================================================
//
// 
//
//==========================================================================
void GLWall::Draw(int pass)
{
	FLightNode * node;
	int rel;
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive() && pass != GLPASS_TRANSLUCENT)
	{
		if (pass == GLPASS_ALL || pass == GLPASS_PLAIN || pass == GLPASS_BASE || pass == GLPASS_BASE_MASKED)
		{
			SetupLights(gl_lights && GLRenderer->mLightCount > 0);
			RenderWall(3, NULL);
			if ((type != RENDERWALL_FFBLOCK || gltexture != NULL) && seg != NULL &&
				seg->sidedef != NULL && seg->sidedef->AttachedDecals != NULL)
				DoDrawDecals();
		}
		return;
	}
#endif
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive() && pass == GLPASS_TRANSLUCENT)
	{
		SetupLights(false);
		if (type == RENDERWALL_MIRRORSURFACE) RenderMirrorSurface();
		else if (type == RENDERWALL_FOGBOUNDARY) RenderFogBoundary();
		else
		{
			const int savedRelativeLight = rellight;
			rellight = 0;
			RenderWall(5, NULL);
			rellight = savedRelativeLight;
		}
		return;
	}
#endif
#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)
	if (gl_GLES_IsActive() && pass != GLPASS_ALL)
		nativeLightCounts[0] = nativeLightCounts[1] = nativeLightCounts[2] = 0;
#endif


	// This allows mid textures to be drawn on lines that might overlap a sky wall
	if ((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER)
	{
		if (pass != GLPASS_DECALS)
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -128.0f);
		}
	}

	switch (pass)
	{
	case GLPASS_ALL:			// Single-pass rendering
		SetupLights();
		// fall through
	case GLPASS_PLAIN:			// Single-pass rendering
		rel = rellight + getExtraLight();
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
		else gl_SetFog(255, 0, NULL, false);

		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		gltexture->Bind(Colormap.colormap, flags, 0);
		RenderWall(3, NULL);
		gl_RenderState.EnableGlow(false);
		gl_RenderState.EnableLight(false);
		break;

	case GLPASS_BASE:			// Base pass for non-masked polygons (all opaque geometry)
	case GLPASS_BASE_MASKED:	// Base pass for masked polygons (2sided mid-textures and transparent 3D floors)
		rel = rellight + getExtraLight();
		gl_SetColor(lightlevel, rel, &Colormap,1.0f);
		if (!(flags&GLWF_FOGGY)) 
		{
			if (type!=RENDERWALL_M2SNF) gl_SetFog(lightlevel, rel, &Colormap, false);
			else gl_SetFog(255, 0, NULL, false);
		}
		gl_RenderState.EnableGlow(!!(flags & GLWF_GLOW));
		// fall through

		if (pass != GLPASS_BASE)
		{
			gltexture->Bind(Colormap.colormap, flags, 0);
		}
		RenderWall(pass == GLPASS_BASE? 2:3, NULL);
		gl_RenderState.EnableGlow(false);
		gl_RenderState.EnableLight(false);
		break;

	case GLPASS_TEXTURE:		// modulated texture
		gltexture->Bind(Colormap.colormap, flags, 0);
		RenderWall(1, NULL);
		break;

	case GLPASS_LIGHT:
	case GLPASS_LIGHT_ADDITIVE:
		// black fog is diminishing light and should affect lights less than the rest!
		if (!(flags&GLWF_FOGGY)) gl_SetFog((255+lightlevel)>>1, 0, NULL, false);
		else gl_SetFog(lightlevel, 0, &Colormap, true);	

		if (seg->sidedef == NULL)
		{
			node = NULL;
		}
		else if (!(seg->sidedef->Flags & WALLF_POLYOBJ))
		{
			// Iterate through all dynamic lights which touch this wall and render them
			node = seg->sidedef->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		}
		else if (sub)
		{
			// To avoid constant rechecking for polyobjects use the subsector's lightlist instead
			node = sub->lighthead[pass==GLPASS_LIGHT_ADDITIVE];
		}
		else node = NULL;
		while (node)
		{
			if (!(node->lightsource->flags2&MF2_DORMANT))
			{
				iter_dlight++;
				RenderWall(1, NULL, node->lightsource);
			}
			node = node->nextLight;
		}
		break;

	case GLPASS_DECALS:
	case GLPASS_DECALS_NOFOG:
		if (seg->sidedef && seg->sidedef->AttachedDecals)
		{
			if (pass==GLPASS_DECALS) 
			{
				gl_SetFog(lightlevel, rellight + getExtraLight(), &Colormap, false);
			}
			DoDrawDecals();
		}
		break;

	case GLPASS_TRANSLUCENT:
		switch (type)
		{
		case RENDERWALL_MIRRORSURFACE:
			RenderMirrorSurface();
			break;

		case RENDERWALL_FOGBOUNDARY:
			RenderFogBoundary();
			break;

		default:
			RenderTranslucentWall();
			break;
		}
	}

	if ((flags&GLWF_SKYHACK && type==RENDERWALL_M2S) || type == RENDERWALL_COLORLAYER)
	{
		if (pass!=GLPASS_DECALS)
		{
			glDisable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(0, 0);
		}
	}
}
