#include "gl/system/gl_gles_renderer.h"
#include "gl/system/gl_gles_targets.h"
#include "gl/system/gl_gles_internal.h"
#include "gl/system/gl_gles_portal.h"
#include "gl/system/gl_gles_present.h"
#include "gl/system/gl_gles_scene.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif
#include <chrono>
#include <algorithm>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <string>
#include "w_wad.h"
#include "gl/shaders/gl_shaderdefs.h"

#include "c_console.h"
#include "i_system.h"
#include "r_defs.h"
#include "r_utility.h"
#include "v_palette.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_colormap.h"
#include "gl/renderer/gl_lightdata.h"
#include "gl/data/gl_data.h"
#include "gl/textures/gl_bitmap.h"
#include "gl/textures/gl_material.h"
#include "gl/textures/gl_skyboxtexture.h"
#include "gl/system/gl_cvars.h"
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_gles_shader.h"
#include "gl/system/gl_gles_dispatch.h"
#include "f_wipe.h"
#include "m_random.h"
#include "m_misc.h"
#include "m_png.h"

extern TexFilter_s TexFilter[];
EXTERN_CVAR(Float, skyoffset)
EXTERN_CVAR(Float, Gamma)
EXTERN_CVAR(Float, vid_brightness)
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Int, gl_vid_multisample)
EXTERN_CVAR(Bool, gl_no_skyclear)
EXTERN_CVAR(Int, screenblocks)
extern int skyfog;
extern long gl_frameMS;

namespace
{
	struct FBootstrapVertex
	{
		GLfloat x, y, z;
		GLfloat u, v;
	};

	struct FSceneVertex
	{
		GLfloat x, y, z;
		GLfloat nx, ny, nz;
		GLfloat u, v;
		GLfloat r, g, b, a;
		GLfloat glowTopDistance, glowBottomDistance;
	};

	struct FSceneViewSnapshot
	{
		float viewProjection[16];
		float cameraPosition[3];
		bool clipPlaneEnabled;
		float clipPlane[4];
	};

	struct FSceneBatch
	{
		GLsizei firstIndex;
		GLsizei indexCount;
		GLenum primitiveMode = GL_TRIANGLES;
		GLuint texture;
		FGLESMaterialEffect materialEffect;
		GLuint brightmap;
		int brightmapDesaturation;
		bool masked;
		bool fog;
		bool translucent;
		bool sourceOrdered;
		bool repeat;
		bool cameraTexture;
		bool palette;
		bool flat;
		bool hud;
		bool wipeOverlay;
		bool model;
		bool cullBackFaces;
		bool skyMask;
		bool flood;
		GLsizei floodWallFirstIndex;
		unsigned int materialFlags;
		EGLESBlendMode blendMode;
		bool customBlend;
		GLenum sourceBlend;
		GLenum destinationBlend;
		float alphaCutoff = 0.5f;
		float sortDepth;
		float fogColor[3];
		float fogDensity;
		FShaderLightParameters lighting;
		float glowTopColor[4];
		float glowBottomColor[4];
		size_t lightOffset;
		unsigned int lightCount;
		unsigned int lightNormalCount;
		unsigned int lightSubtractiveCount;
		float lightPlaneNormal[3];
		// Keep the view association with each batch. Nested portal collection must
		// not replace the outer scene's view uniforms.
		unsigned int viewSerial;
		unsigned int clipSerial;
		unsigned int viewSnapshot;
		int portalId;
		bool portalMask;
	};

	struct FNativeSkyRecord
	{
		int portalId;
		std::vector<size_t> maskBatches;
		float viewProjection[16];
		float cameraPosition[3];
		GLuint stencilBit;
		GLuint stencilRef;
		GLuint stencilMask;
		bool viewValid;
		bool capEligible;
		bool capsAdded;
		unsigned int submitted;
		unsigned int clipped;
		unsigned int rejected;
		float clipMin[4];
		float clipMax[4];
	};

	struct FNativePortalTarget
	{
		unsigned int id;
		unsigned int stencilSlot;
		bool framebufferFallback;
		int fallbackTargetIndex;
		bool fallbackTargetReady;
		int parentId;
		size_t firstBatch;
		size_t endBatch;
		std::vector<size_t> maskBatches;
		float savedViewProjection[16];
		float savedCameraPosition[3];
		float savedCameraYaw;
		float savedCameraPitch;
		float savedCameraFieldOfView;
		float savedCameraAspect;
		float savedCameraFovRatio;
		FMaterial *savedSkyMaterial;
		float savedSkyXOffset;
		float savedSkyYOffset;
		FMaterial *savedSkyLayerMaterial;
		float savedSkyLayerXOffset;
		float savedSkyLayerYOffset;
		bool savedSkyMirrored;
		bool savedSkyLayerMirrored;
		bool savedSky2;
		PalEntry savedSkyUpperCapColor;
		PalEntry savedSkyLowerCapColor;
		PalEntry savedSkyFogColor;
		bool savedSkyFogEnabled;
		bool savedClipPlaneEnabled;
		unsigned int savedViewSerial;
		unsigned int savedClipSerial;
		float savedClipPlane[4];
		FNativeSkyRecord sky;
		bool clearScreen;
		FMaterial *skyMaterial;
		float skyXOffset;
		float skyYOffset;
		FMaterial *skyLayerMaterial;
		float skyLayerXOffset;
		float skyLayerYOffset;
		bool skyMirrored;
		bool skyLayerMirrored;
		bool sky2;
		PalEntry skyUpperCapColor;
		PalEntry skyLowerCapColor;
		PalEntry skyFogColor;
		bool skyFogEnabled;
	};

	struct FGLESResources
	{
		GLuint sceneProgram;
		GLuint simpleProgram;
		GLuint fogProgram;
		GLuint fogMaskedProgram;
		GLuint maskedProgram;
		GLuint paletteProgram;
		GLuint sceneVertexArray;
		GLuint sceneVertexBuffer;
		GLuint sceneIndexBuffer;
		GLuint vertexArray;
		GLuint vertexBuffer;
		GLuint indexBuffer;
		GLuint checkerTexture;
		GLuint worldSamplers[4];
		GLuint sceneSamplers[4];
		GLuint nearestSamplers[4];
		int textureFilter;
		float textureAnisotropy;
		GLint maximumAnisotropy;
		FGLESTargetDescriptor sceneTarget;
		FGLESTargetDescriptor wipeOverlayTarget;
		FGLESTargetDescriptor cameraTarget;
		FGLESViewDescriptor viewContract;
		FGLESViewArea nativeViewArea;
		GLint sceneTextureUniform;
		GLint sceneBrightmapUniform;
		GLint sceneUseBrightmap;
		GLint sceneBrightmapDesaturation;
		GLint sceneViewProjection;
		GLint sceneUseTexture;
		GLint sceneModel;
		GLint sceneTextureTransform;
		GLint sceneCameraPosition;
		GLint sceneObjectColor;
		GLint sceneSkyDepth;
		GLint sceneSkyFog;
		GLint sceneMaterialFlags;
		GLint sceneLightCounts;
		GLint sceneProjectedLights;
		GLint sceneClipPlane;
		GLint sceneClipPlaneEnabled;
		GLint fogColor;
		GLint fogDensity;
		unsigned int frame;
		GLsizei sceneIndexCount;
		std::vector<FSceneVertex> sceneVertices;
		// GLES 3.2 core indices keep large model surfaces in the shared stream.
		std::vector<GLuint> sceneIndices;
		std::vector<GLushort> sceneShortIndices;
		GLenum sceneIndexType;
		size_t sceneIndexStride;
		std::vector<FSceneViewSnapshot> sceneViewSnapshots;
		std::vector<FSceneBatch> sceneBatches;
		std::vector<FGLESSceneOrderRecord> sceneOrderRecords;
		std::vector<FGLESSceneOrderRecord> portalOrderRecords;
		std::vector<FNativePortalTarget> portalTargets;
		FNativeSkyRecord outerSky;
		FMaterial *skyMaterial;
		float skyXOffset;
		float skyYOffset;
		FMaterial *skyLayerMaterial;
		float skyLayerXOffset;
		float skyLayerYOffset;
		bool skyMirrored;
		bool skyLayerMirrored;
		bool sky2;
		PalEntry skyUpperCapColor;
		PalEntry skyLowerCapColor;
		PalEntry skyFogColor;
		bool skyFogEnabled;
		unsigned int sceneWallCount;
		unsigned int sceneFlatCount;
		unsigned int sceneSpriteCount;
		unsigned int sceneBatchSubmissions;
		unsigned int sceneOpaqueBatchMerges;
		unsigned int sceneProgramBinds;
		unsigned int sceneProgramBindSkips;
		unsigned int sceneTextureBinds;
		unsigned int sceneTextureBindSkips;
		unsigned int sceneActiveTextureSets;
		unsigned int sceneActiveTextureSkips;
		unsigned int sceneSamplerBinds;
		unsigned int sceneSamplerBindSkips;
		unsigned int sceneOpaqueStateSets;
		unsigned int sceneOpaqueStateSkips;
		unsigned int sceneLightUniformUploads;
		unsigned int sceneLightUniformUploadSkips;
		unsigned int sceneGlowUniformUploads;
		unsigned int sceneGlowUniformUploadSkips;
		uint64_t sceneOrderHash;
		float viewProjection[16];
		float cameraX;
		float cameraY;
		float cameraZ;
		float cameraYaw;
		float cameraPitch;
		float cameraFieldOfView;
		float cameraAspect;
		float cameraFovRatio;
		unsigned int viewSerial;
		unsigned int clipSerial;
		bool clipPlaneEnabled;
		float clipPlane[4];
		bool sceneReady;
		bool ready;
		bool framebufferWarningLogged;
	};

	static FGLESNativeCapabilities Capabilities = {};
	static FGLESResources Resources = {};
	static bool CapabilitiesReady = false;
	static bool NativeBackendEnabled = false;
	static bool BootstrapPauseLogged = false;
	static bool SceneInputLogged = false;
	static bool SkyLogged = false;
	static std::chrono::steady_clock::time_point NativeFrameStart;
	struct FGLESProfileSample
	{
		bool active;
		std::chrono::steady_clock::time_point frameStart;
		double collectionMilliseconds;
		double orderingMilliseconds;
		double uploadMilliseconds;
		double portalSkyMilliseconds;
		double worldMilliseconds;
		double overlayMilliseconds;
		double resolveMilliseconds;
		unsigned int drawCalls;
		unsigned int triangles;
		unsigned int bufferReallocations;
		unsigned int materialHits;
		unsigned int materialMisses;
		unsigned int materialUploads;
		unsigned int stateCalls;
		unsigned int stateSkips;
		unsigned int portalStateSets;
		unsigned int portalStateSkips;
		unsigned int portalActiveTextureSets;
		unsigned int portalActiveTextureSkips;
		unsigned int portalUniformSets;
		unsigned int portalUniformSkips;
		unsigned int portalSkyUploadBuilds;
		unsigned int portalSkyUploadSkips;
		unsigned int lightSelectionRequests;
		unsigned int lightSelectionReuses;
		unsigned int simpleShaderDraws;
		unsigned int resolveCount;
		unsigned int readbackCount;
		unsigned int wipeCaptureCount;
		unsigned int cameraTargetCreates;
		unsigned int cameraCopies;
		double wallCollectionMilliseconds;
		double flatCollectionMilliseconds;
		double spriteCollectionMilliseconds;
		double modelCollectionMilliseconds;
		double materialResolutionMilliseconds;
		double lightSelectionMilliseconds;
		double batchRecordMilliseconds;
		double indexEmissionMilliseconds;
		size_t vertexBytes;
		size_t indexBytes;
	};
	static FGLESProfileSample Profile = {};
	struct FScopedProfileTimer
	{
		double *target;
		std::chrono::steady_clock::time_point start;

		explicit FScopedProfileTimer(double &milliseconds)
			: target(Profile.active ? &milliseconds : NULL),
			start(Profile.active ? std::chrono::steady_clock::now() :
				std::chrono::steady_clock::time_point())
		{
		}

		~FScopedProfileTimer()
		{
			if (target != NULL)
				*target += std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - start).count();
		}
	};
	static unsigned int PendingCameraTargetCreates = 0;
	static unsigned int PendingCameraCopies = 0;
	static bool NativePortalCapacityLogged = false;
	static bool FlatCollectionDeferred = false;
	static FGLESTargetDescriptor *NativeActiveTarget = NULL;
	static bool NativeOffscreenRender = false;
	static bool NativeWipeOverlayCollecting = false;
	static GLuint NativeBoundProgram = 0;
	static bool NativeProgramBindingKnown = false;
	static void SetNativeFullViewport(int width, int height)
	{
		glViewport(0, 0, width, height);
		glScissor(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
	}
	static void SetNativeHUDViewport(int width, int height)
	{
		const int logicalHeight = screen != NULL ? screen->GetHeight() : height;
		const int trueHeight = screen != NULL ? screen->GetTrueHeight() : height;
		if (logicalHeight <= 0 || trueHeight < logicalHeight)
		{
			SetNativeFullViewport(width, height);
			return;
		}
		const int bottom = static_cast<int>(
			static_cast<int64_t>((trueHeight - logicalHeight) / 2) * height / trueHeight);
		const int viewHeight = static_cast<int>(static_cast<int64_t>(logicalHeight) * height / trueHeight);
		glViewport(0, bottom, width, viewHeight);
		glScissor(0, bottom, width, viewHeight);
		glDisable(GL_SCISSOR_TEST);
	}

	struct FNativeDrawUniforms
	{
		GLint viewProjection;
		GLint skyDepth, skyFog;
		GLint textureUniform;
		GLint brightmapUniform;
		GLint useBrightmap;
		GLint brightmapDesaturation;
		GLint useTexture;
		GLint model;
		GLint textureTransform;
		GLint cameraPosition;
		GLint objectColor;
		GLint materialFlags;
		GLint fuzzTime;
		GLint alphaCutoff;
		GLint depthBias;
		GLint fogColor;
		GLint fogDensity;
		GLint lightDataSampler;
		GLint lightDataOffset;
		GLint lightCounts;
		GLint lightPlaneNormal;
		GLint projectedLights;
		GLint dynamicLightSampler;
		GLint clipPlaneUniform;
		GLint clipPlaneEnabledUniform;
		GLint glowTop, glowBottom, staticLight, spriteLight, fogEnabled;
		float top[4], bottom[4];
		FShaderLightParameters lighting;
		bool glowKnown, lightingKnown;
	};
	static std::unordered_map<GLuint, FNativeDrawUniforms> NativeDrawUniforms;

	static FNativeDrawUniforms &GetNativeDrawUniforms(GLuint program)
	{
		auto found = NativeDrawUniforms.find(program);
		if (found == NativeDrawUniforms.end())
		{
			FNativeDrawUniforms value = {};
			value.viewProjection = glGetUniformLocation(program, "u_view_projection");
			value.skyDepth = glGetUniformLocation(program, "u_sky_depth");
			value.skyFog = glGetUniformLocation(program, "u_sky_fog");
			value.textureUniform = glGetUniformLocation(program, "u_texture");
			value.brightmapUniform = glGetUniformLocation(program, "u_brightmap");
			value.useBrightmap = glGetUniformLocation(program, "u_use_brightmap");
			value.brightmapDesaturation = glGetUniformLocation(program, "u_brightmap_desaturation");
			value.useTexture = glGetUniformLocation(program, "u_use_texture");
			value.model = glGetUniformLocation(program, "u_model");
			value.textureTransform = glGetUniformLocation(program, "u_texture_transform");
			value.cameraPosition = glGetUniformLocation(program, "u_camera_position");
			value.objectColor = glGetUniformLocation(program, "u_object_color");
			value.materialFlags = glGetUniformLocation(program, "u_material_flags");
			value.fuzzTime = glGetUniformLocation(program, "u_fuzz_time");
			value.alphaCutoff = glGetUniformLocation(program, "u_alpha_cutoff");
			value.depthBias = glGetUniformLocation(program, "u_depth_bias");
			value.fogColor = glGetUniformLocation(program, "u_fog_color");
			value.fogDensity = glGetUniformLocation(program, "u_fog_density");
			value.lightDataSampler = glGetUniformLocation(program, "u_light_data");
			value.lightDataOffset = glGetUniformLocation(program, "u_light_offset");
			value.lightCounts = glGetUniformLocation(program, "u_light_counts");
			value.lightPlaneNormal = glGetUniformLocation(program, "u_light_plane_normal");
			value.projectedLights = glGetUniformLocation(program, "u_projected_lights");
			value.dynamicLightSampler = glGetUniformLocation(program, "u_dynamic_light_texture");
			value.clipPlaneUniform = glGetUniformLocation(program, "u_clip_plane");
			value.clipPlaneEnabledUniform = glGetUniformLocation(program, "u_clip_plane_enabled");
			value.glowTop = glGetUniformLocation(program, "u_glow_top_color");
			value.glowBottom = glGetUniformLocation(program, "u_glow_bottom_color");
			value.staticLight = glGetUniformLocation(program, "u_static_light");
			value.spriteLight = glGetUniformLocation(program, "u_sprite_light");
			value.fogEnabled = glGetUniformLocation(program, "fogenabled");
			found = NativeDrawUniforms.emplace(program, value).first;
		}
		return found->second;
	}

	static void ResetNativeDrawUniforms()
	{
		NativeDrawUniforms.clear();
	}

	static void SetNativeGlowUniforms(GLuint program, const FSceneBatch &batch)
	{
		FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
		if (uniforms.glowKnown && memcmp(uniforms.top, batch.glowTopColor, sizeof(uniforms.top)) == 0 &&
			memcmp(uniforms.bottom, batch.glowBottomColor, sizeof(uniforms.bottom)) == 0)
		{
			++Resources.sceneGlowUniformUploadSkips;
			return;
		}
		if (uniforms.glowTop >= 0) glUniform4fv(uniforms.glowTop, 1, batch.glowTopColor);
		if (uniforms.glowBottom >= 0) glUniform4fv(uniforms.glowBottom, 1, batch.glowBottomColor);
		memcpy(uniforms.top, batch.glowTopColor, sizeof(uniforms.top));
		memcpy(uniforms.bottom, batch.glowBottomColor, sizeof(uniforms.bottom));
		uniforms.glowKnown = true;
		++Resources.sceneGlowUniformUploads;
	}
	struct FNativeMaterialUniforms
	{
		GLint effect;
		GLint time;
		GLint userTime, desaturation, colormapEnabled, colormapStart, colormapRange;
		int shaderIndex;
		float animationTime;
		bool known;
	};
	static std::unordered_map<GLuint, FNativeMaterialUniforms> NativeMaterialUniforms;
	static std::unordered_map<uint64_t, float> NativeMaterialTimes;

	static void SetNativeMaterialUniforms(GLuint program, const FSceneBatch &batch)
	{
		auto found = NativeMaterialUniforms.find(program);
		if (found == NativeMaterialUniforms.end())
		{
			FNativeMaterialUniforms uniforms = {};
			uniforms.effect = glGetUniformLocation(program, "u_material_effect");
			uniforms.time = glGetUniformLocation(program, "u_material_time");
			uniforms.userTime = glGetUniformLocation(program, "timer");
			uniforms.desaturation = glGetUniformLocation(program, "desaturation_factor");
			uniforms.colormapEnabled = glGetUniformLocation(program, "u_material_colormap");
			uniforms.colormapStart = glGetUniformLocation(program, "colormapstart");
			uniforms.colormapRange = glGetUniformLocation(program, "colormaprange");
			found = NativeMaterialUniforms.emplace(program, uniforms).first;
		}
		FNativeMaterialUniforms &uniforms = found->second;
		const unsigned int materialFlags = batch.materialFlags;
		FGLESMaterialEffect effect = batch.materialEffect;
		if ((materialFlags & GLES_MATERIAL_FUZZ) != 0)
		{
			effect.shaderIndex = 0;
			effect.speed = 0.0f;
		}
		if ((materialFlags & GLES_MATERIAL_COLOR_FIXED) != 0)
			effect.colormap = CM_DEFAULT;
		unsigned int timerVariant = ((materialFlags & GLES_MATERIAL_GLOW) != 0 ? 1 : 0) |
			(effect.colormap >= CM_DESAT1 && effect.colormap <= CM_DESAT31 ? 2 : 0) |
			(batch.lightCount > 0 ? 4 : 0) | (glset.lightmode & 8);
		if (effect.colormap >= CM_FIRSTSPECIALCOLORMAP && effect.colormap < CM_MAXCOLORMAP)
			timerVariant = 16;
		if (effect.colormap == CM_FOGLAYER || (materialFlags & GLES_MATERIAL_SPRITE_FOG_LAYER) != 0)
			timerVariant = 17;
		const uint64_t timerKey = (static_cast<uint64_t>(effect.shaderIndex) << 5) | timerVariant;
		float &retainedTime = NativeMaterialTimes[timerKey];
		if (effect.speed > 0.0f) retainedTime = gl_frameMS * effect.speed / 1000.0f;
		const float animationTime = retainedTime;
		if (!uniforms.known || uniforms.shaderIndex != effect.shaderIndex)
			if (uniforms.effect >= 0) glUniform1i(uniforms.effect, effect.shaderIndex);
		if (!uniforms.known || uniforms.animationTime != animationTime)
			{
				if (uniforms.time >= 0) glUniform1f(uniforms.time, animationTime);
				if (uniforms.userTime >= 0) glUniform1f(uniforms.userTime, animationTime);
			}
		if (uniforms.desaturation >= 0) glUniform1f(uniforms.desaturation,
			effect.colormap >= CM_DESAT1 && effect.colormap <= CM_DESAT31 ?
				1.0f - float(effect.colormap - CM_DESAT0) / (CM_DESAT31 - CM_DESAT0) : 1.0f);
		const bool special = effect.colormap >= CM_FIRSTSPECIALCOLORMAP && effect.colormap < CM_MAXCOLORMAP;
		if (uniforms.colormapEnabled >= 0) glUniform1i(uniforms.colormapEnabled, special ? 1 : 0);
		if (special)
		{
			const FSpecialColormap &map = SpecialColormaps[effect.colormap - CM_FIRSTSPECIALCOLORMAP];
			const float range[3] = { map.ColorizeEnd[0] - map.ColorizeStart[0],
				map.ColorizeEnd[1] - map.ColorizeStart[1], map.ColorizeEnd[2] - map.ColorizeStart[2] };
			if (uniforms.colormapStart >= 0) glUniform3fv(uniforms.colormapStart, 1, map.ColorizeStart);
			if (uniforms.colormapRange >= 0) glUniform3fv(uniforms.colormapRange, 1, range);
		}
		uniforms.shaderIndex = effect.shaderIndex;
		uniforms.animationTime = animationTime;
		uniforms.known = true;
	}

	static void SetNativeLightingUniforms(GLuint program, const FSceneBatch &batch)
	{
		const GLint fogEnabled = GetNativeDrawUniforms(program).fogEnabled;
		if (fogEnabled >= 0) glUniform1i(fogEnabled, batch.fog ?
			(batch.fogColor[0] == 0.0f && batch.fogColor[1] == 0.0f && batch.fogColor[2] == 0.0f ? gl_fogmode : -gl_fogmode) : 0);

		FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
		if (uniforms.lightingKnown && memcmp(&uniforms.lighting, &batch.lighting, sizeof(batch.lighting)) == 0) return;
		if (uniforms.staticLight >= 0) glUniform4fv(uniforms.staticLight, 1, &batch.lighting.level);
		if (uniforms.spriteLight >= 0) glUniform3fv(uniforms.spriteLight, 1, batch.lighting.dynamic);
		uniforms.lighting = batch.lighting;
		uniforms.lightingKnown = true;
	}
	static std::vector<unsigned int> NativePortalCaptureStack;
	// Bits 0 and 1 belong to the outer sky and flood masks. The remaining
	// stencil pairs are reused after a portal capture is closed.
	static const unsigned int GLESNativePortalSlots = 3;
	static GLuint NativePortalStencilBit(unsigned int id)
	{
		return 1u << (2u + id * 2u);
	}
	static GLuint NativePortalSkyStencilBit(unsigned int id)
	{
		return 1u << (3u + id * 2u);
	}
	static unsigned int FindNativePortalStencilSlot()
	{
		bool used[GLESNativePortalSlots] = {};
		size_t scopeStart = 0;
		for (size_t index = 0; index < NativePortalCaptureStack.size(); ++index)
		{
			const unsigned int portalId = NativePortalCaptureStack[index];
			if (portalId < Resources.portalTargets.size() &&
				Resources.portalTargets[portalId].framebufferFallback)
				scopeStart = index;
		}
		for (size_t index = scopeStart; index < NativePortalCaptureStack.size(); ++index)
		{
			const unsigned int portalId = NativePortalCaptureStack[index];
			if (portalId < Resources.portalTargets.size())
			{
				const unsigned int slot = Resources.portalTargets[portalId].stencilSlot;
				if (slot < GLESNativePortalSlots) used[slot] = true;
			}
		}
		for (unsigned int slot = 0; slot < GLESNativePortalSlots; ++slot)
			if (!used[slot]) return slot;
		if (developer && !NativePortalCapacityLogged)
		{
			NativePortalCapacityLogged = true;
			DPrintf("Zandronum GLES portal nesting reached three stencil pairs; deeper captures use isolated framebuffer targets.\n");
		}
		return ~0u;
	}
	static bool CanAppendSceneGeometry(size_t vertexCount, size_t indexCount, const char *kind)
	{
		return gl_GLESInternalSceneCanAppend(Resources.sceneVertices.size(),
			Resources.sceneIndices.size(), vertexCount, indexCount, kind);
	}
	static GLenum NativeSceneIndexType()
	{
		return Resources.sceneIndexType != 0 ? Resources.sceneIndexType : GL_UNSIGNED_INT;
	}
	static size_t NativeSceneIndexStride()
	{
		return Resources.sceneIndexStride != 0 ? Resources.sceneIndexStride : sizeof(GLuint);
	}
	static const void *NativeSceneIndexOffset(size_t firstIndex)
	{
		return reinterpret_cast<const void *>(firstIndex * NativeSceneIndexStride());
	}
	static void ResetNativeSkyRecord(FNativeSkyRecord &record, int portalId, GLuint stencilBit)
	{
		record = {};
		record.portalId = portalId;
		record.stencilBit = stencilBit;
		record.stencilRef = stencilBit;
		record.stencilMask = stencilBit;
		for (int axis = 0; axis < 4; ++axis)
		{
			record.clipMin[axis] = 1.0e30f;
			record.clipMax[axis] = -1.0e30f;
		}
	}
	static FNativeSkyRecord *ActiveNativeSkyRecord()
	{
		if (NativePortalCaptureStack.empty()) return &Resources.outerSky;
		const unsigned int portalId = NativePortalCaptureStack.back();
		if (portalId >= Resources.portalTargets.size()) return NULL;
		return &Resources.portalTargets[portalId].sky;
	}

	CVAR(Bool, gl_gles_shader_test_failure, false, CVAR_DEBUGONLY)
	CVAR(Bool, gl_gles_profile, false, CVAR_DEBUGONLY)
	CVAR(Bool, gl_gles_validate_order, false, CVAR_DEBUGONLY)

	static double ProfileMilliseconds(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
	}

	static bool ProfileSampleDue()
	{
		return Profile.active && (Resources.frame == 1 || Resources.frame % 30 == 0);
	}

	static const uint64_t SceneOrderHashOffset = 1469598103934665603ULL;
	static const uint64_t SceneOrderHashPrime = 1099511628211ULL;

	template<class T>
	static void HashSceneOrderValue(const T &value)
	{
		const unsigned char *bytes = reinterpret_cast<const unsigned char *>(&value);
		for (size_t index = 0; index < sizeof(value); ++index)
		{
			Resources.sceneOrderHash ^= bytes[index];
			Resources.sceneOrderHash *= SceneOrderHashPrime;
		}
	}

	static inline void ProfileDrawElements(GLenum mode, GLsizei count, GLenum type,
		const void *indices)
	{
		gl_GLES_RecordProfileDraw(mode == GL_TRIANGLE_STRIP, count);
		glDrawElements(mode, count, type, indices);
	}

	static bool IsPaletteTexture(GLuint texture)
	{
		return gl_GLESInternalIsPaletteTexture(texture);
	}

	static GLuint NativeSamplerForBatch(const FSceneBatch &batch)
	{
		unsigned int clamp = batch.repeat ? 0 : 3;
		if (batch.materialFlags & GLES_MATERIAL_CLAMP_X) clamp |= 1;
		if (batch.materialFlags & GLES_MATERIAL_CLAMP_Y) clamp |= 2;
		if (batch.cameraTexture) clamp = 0;
		if ((gl_GLESInternalGetMaterialFlags(batch.texture) & GLES_TEXTURE_FLAG_NOFILTER) != 0)
			return Resources.nearestSamplers[clamp];
		return batch.cameraTexture || !batch.repeat ?
			Resources.sceneSamplers[clamp] : Resources.worldSamplers[clamp];
	}

	static GLuint BindNativeProgram(GLuint fallback, const char *name)
	{
		return static_cast<GLuint>(gl_GLES_BindShaderProgram(name, fallback));
	}

	static GLenum CheckError(const char *site)
	{
		return gl_GLES_CheckErrors(site);
	}

	static float ClampUnit(float value)
	{
		return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	}

	static GLuint LinkProgram(const char *vertexSource, const char *fragmentSource, const char *label,
		const char *defines = "")
	{
		char log[1024] = {};
		GLuint program = gl_GLES_LinkProgram(vertexSource, fragmentSource, label, log, sizeof(log), true);
		if (program == 0)
		{
			DPrintf("Zandronum GLES %s defines:\n%s\nvertex source:\n%s\nfragment source:\n%s\n",
				label, defines, vertexSource, fragmentSource);
			Printf("Zandronum GLES %s program failed: %s\n", label, log);
			I_FatalError("Zandronum GLES %s program failed: %s", label, log);
		}
		return program;
	}

	static std::string NativeMaterialVertexSource;
	static std::string NativeMaterialFragmentSources[5];
	static std::unordered_map<unsigned int, GLuint> NativeUserPrograms;

	static GLuint ResolveNativeMaterialProgram(GLuint base, const FSceneBatch &batch)
	{
		const FGLESMaterialEffect effect = batch.materialEffect;
		if (effect.shaderIndex < FIRST_USER_SHADER || (batch.materialFlags & GLES_MATERIAL_FUZZ) != 0) return base;
		const unsigned int variant = batch.fog ? (batch.masked ? 4 : 3) : (batch.masked ? 1 : (batch.palette ? 2 : 0));
		const unsigned int key = static_cast<unsigned int>(effect.shaderIndex - FIRST_USER_SHADER) * 5 + variant;
		const auto found = NativeUserPrograms.find(key);
		if (found != NativeUserPrograms.end()) return found->second;
		const char *path = gl_GetUserShaderPath(effect.shaderIndex);
		if (path == NULL) I_FatalError("Zandronum GLES material shader index %d is invalid.", effect.shaderIndex);
		const int lump = Wads.CheckNumForFullName(path);
		if (lump < 0) I_FatalError("Zandronum GLES material shader '%s' is missing.", path);
		FMemLump data = Wads.ReadLump(lump);
		std::string fragment = NativeMaterialFragmentSources[variant];
		if (fragment.find("in float v_camera_distance;") == std::string::npos)
			fragment.insert(fragment.find("void main()"), "in float v_camera_distance;\n");
		if (fragment.find("uniform vec3 u_camera_position;") == std::string::npos)
			fragment.insert(fragment.find("void main()"), "uniform vec3 u_camera_position;\n");
		if (fragment.find("uniform vec4 u_fog_color;") == std::string::npos)
			fragment.insert(fragment.find("void main()"), "uniform vec4 u_fog_color;\n");
		if (fragment.find("uniform float u_fog_density;") == std::string::npos)
			fragment.insert(fragment.find("void main()"), "uniform float u_fog_density;\n");
		if (fragment.find("uniform float u_alpha_cutoff;") == std::string::npos)
			fragment.insert(fragment.find("void main()"), "uniform float u_alpha_cutoff;\n");
		const std::string contract =
			"uniform int fogenabled;\n"
			"#define DOOMLIGHTFACTOR 232.0\n"
			"#define fogcolor u_fog_color\n"
			"#define fogparm vec4(u_static_light.y, u_static_light.z, -u_fog_density * 1.44269504089, 0.0)\n"
			"#define topglowcolor u_glow_top_color\n"
			"#define bottomglowcolor u_glow_bottom_color\n"
			"#define glowdist v_glow_distance\n"
			"#define texturemode ((u_material_flags & 64) != 0 ? 1 : ((u_material_flags & 4) != 0 ? 2 : 0))\n"
			"vec4 zandronum_texcoord[1];\n"
			"#define tex u_texture\n"
			"#define texture2 u_brightmap\n"
			"#define camerapos u_camera_position\n"
			"#define pixelpos vec4(v_world_position, v_camera_distance)\n"
			"#define lightlevel u_static_light.x\n"
			"#define dlightcolor u_sprite_light\n"
			"vec4 getLightColor(float fogdist, float fogfactor) { vec3 color = apply_static_light(v_color.rgb, fogdist); if (fogenabled > 0) color *= fogfactor; return vec4(apply_glow(color), v_color.a); }\n"
			"vec4 applyFog(vec4 color, float factor) { return vec4(mix(u_fog_color.rgb, color.rgb, factor), color.a); }\n"
			"vec4 getTexel(vec2 coords) { vec4 value = u_use_texture ? texture(u_texture, coords) : vec4(1.0); if ((u_material_flags & 4) != 0) value.a = 1.0; if ((u_material_flags & 64) != 0) value.rgb = vec3(1.0); return desaturate(value); }\n";
		const std::string mainStart = "void main() {";
		fragment.replace(fragment.find(mainStart), mainStart.size(), mainStart + " zandronum_texcoord[0] = vec4(v_uv, 0.0, 1.0);");
		const std::string original = "vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a));";
		fragment.replace(fragment.find(original), original.size(),
			"vec4 color = Process(vec4(lighting, u_material_colormap ? 1.0 : v_color.a));");
		const std::string earlyTest = "if ((u_material_flags & 1) != 0 ? texel.a <= 0.0 : texel.a <= 0.0 || texel.a < u_alpha_cutoff) discard;";
		const size_t alphaTest = fragment.find(earlyTest);
		if (alphaTest != std::string::npos)
		{
			fragment.erase(alphaTest, earlyTest.size());
			fragment.insert(fragment.find("color.rgb += additive;"), "if (color.a <= 0.0 || color.a < u_alpha_cutoff) discard; ");
		}
		const std::string fogAlpha = "(1.0 - fog) * texel.a * 0.75 * v_color.a";
		const size_t fogLayer = fragment.find(fogAlpha);
		if (fogLayer != std::string::npos)
			fragment.replace(fogLayer, fogAlpha.size(), "(1.0 - fog) * Process(vec4(1.0)).a * 0.75 * v_color.a");
		fragment.insert(fragment.find("void main()"), contract + gl_GLES_LowerMaterialShader(data.GetString().GetChars()) + "\n");
		const GLuint program = LinkProgram(NativeMaterialVertexSource.c_str(), fragment.c_str(), path);
		NativeUserPrograms.emplace(key, program);
		return program;
	}


	static FNativeDrawUniforms &BindNativeSkyMaterial(GLuint texture, const float *viewProjection, bool fog = false)
	{
		FSceneBatch batch = {};
		batch.texture = texture;
		batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
		batch.lighting.level = batch.lighting.factor = 1.0f;
		const GLuint program = ResolveNativeMaterialProgram(Resources.sceneProgram, batch);
		BindNativeProgram(program, program == Resources.sceneProgram ? "gles/opaque" : NULL);
		SetNativeMaterialUniforms(program, batch);
		SetNativeLightingUniforms(program, batch);
		SetNativeGlowUniforms(program, batch);
		FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
		static const GLfloat identity[16] =
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		if (uniforms.viewProjection >= 0) glUniformMatrix4fv(uniforms.viewProjection, 1, GL_FALSE, viewProjection);
		if (uniforms.model >= 0) glUniformMatrix4fv(uniforms.model, 1, GL_FALSE, identity);
		if (uniforms.textureTransform >= 0) glUniform4f(uniforms.textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
		if (uniforms.cameraPosition >= 0) glUniform3f(uniforms.cameraPosition, Resources.cameraX, Resources.cameraY, Resources.cameraZ);
		if (uniforms.skyDepth >= 0) glUniform1i(uniforms.skyDepth, 1);
		if (uniforms.skyFog >= 0) glUniform1i(uniforms.skyFog, fog ? 1 : 0);
		if (uniforms.materialFlags >= 0) glUniform1i(uniforms.materialFlags, 0);
		if (uniforms.lightCounts >= 0) glUniform3i(uniforms.lightCounts, 0, 0, 0);
		if (uniforms.projectedLights >= 0) glUniform1i(uniforms.projectedLights, 0);
		if (uniforms.clipPlaneEnabledUniform >= 0) glUniform1i(uniforms.clipPlaneEnabledUniform, 0);
		if (uniforms.useBrightmap >= 0) glUniform1i(uniforms.useBrightmap, 0);
		if (uniforms.brightmapDesaturation >= 0) glUniform1i(uniforms.brightmapDesaturation, 0);
		if (uniforms.textureUniform >= 0) glUniform1i(uniforms.textureUniform, 0);
		if (uniforms.brightmapUniform >= 0) glUniform1i(uniforms.brightmapUniform, 1);
		if (uniforms.fogColor >= 0) glUniform4f(uniforms.fogColor, 0.0f, 0.0f, 0.0f, 0.0f);
		if (uniforms.fogDensity >= 0) glUniform1f(uniforms.fogDensity, 0.0f);
		return uniforms;
	}

	static void DeleteResources(bool clearTextureCache)
	{
		for (const auto &program : NativeUserPrograms) glDeleteProgram(program.second);
		NativeUserPrograms.clear();
		gl_GLESInternalSceneDeleteLights();
		gl_GLESInternalSceneInvalidateBuffers();
		if (Resources.sceneProgram != 0) glDeleteProgram(Resources.sceneProgram);
		if (Resources.simpleProgram != 0) glDeleteProgram(Resources.simpleProgram);
		if (Resources.fogProgram != 0) glDeleteProgram(Resources.fogProgram);
		if (Resources.fogMaskedProgram != 0) glDeleteProgram(Resources.fogMaskedProgram);
		if (Resources.maskedProgram != 0) glDeleteProgram(Resources.maskedProgram);
		if (Resources.paletteProgram != 0) glDeleteProgram(Resources.paletteProgram);
		gl_GLESInternalPresentDestroy();
		gl_GLESInternalPortalDestroy();
		if (Resources.sceneVertexBuffer != 0) glDeleteBuffers(1, &Resources.sceneVertexBuffer);
		if (Resources.sceneIndexBuffer != 0) glDeleteBuffers(1, &Resources.sceneIndexBuffer);
		if (Resources.sceneVertexArray != 0) glDeleteVertexArrays(1, &Resources.sceneVertexArray);
		if (Resources.vertexBuffer != 0) glDeleteBuffers(1, &Resources.vertexBuffer);
		if (Resources.indexBuffer != 0) glDeleteBuffers(1, &Resources.indexBuffer);
		if (Resources.vertexArray != 0) glDeleteVertexArrays(1, &Resources.vertexArray);
		if (Resources.checkerTexture != 0) glDeleteTextures(1, &Resources.checkerTexture);
		gl_GLESInternalWipeDestroy();
		gl_GLESInternalDeleteMaterialTextures();
		glDeleteSamplers(4, Resources.worldSamplers);
		glDeleteSamplers(4, Resources.sceneSamplers);
		glDeleteSamplers(4, Resources.nearestSamplers);
		gl_GLES_DestroyRenderTarget(&Resources.sceneTarget);
		gl_GLES_DestroyRenderTarget(&Resources.wipeOverlayTarget);
		gl_GLES_DestroyRenderTarget(&Resources.cameraTarget);
		Resources.sceneProgram = 0;
		Resources.simpleProgram = 0;
		Resources.fogProgram = 0;
		Resources.fogMaskedProgram = 0;
		Resources.maskedProgram = 0;
		Resources.paletteProgram = 0;
		Resources.sceneVertexBuffer = 0;
		Resources.sceneIndexBuffer = 0;
		Resources.sceneVertexArray = 0;
		Resources.vertexBuffer = 0;
		Resources.indexBuffer = 0;
		Resources.vertexArray = 0;
		Resources.checkerTexture = 0;
		memset(Resources.worldSamplers, 0, sizeof(Resources.worldSamplers));
		memset(Resources.sceneSamplers, 0, sizeof(Resources.sceneSamplers));
		memset(Resources.nearestSamplers, 0, sizeof(Resources.nearestSamplers));
		Resources.textureFilter = -1;
		Resources.textureAnisotropy = -1.0f;
		Resources.sceneTarget = {};
		Resources.wipeOverlayTarget = {};
		Resources.cameraTarget = {};
		Resources.viewContract = {};
		NativeActiveTarget = NULL;
		NativeOffscreenRender = false;
		PendingCameraTargetCreates = 0;
		PendingCameraCopies = 0;
		ResetNativeDrawUniforms();
		NativeMaterialUniforms.clear();
		NativeMaterialTimes.clear();
		Resources.sceneTextureUniform = -1;
		Resources.sceneBrightmapUniform = -1;
		Resources.sceneUseBrightmap = -1;
		Resources.sceneBrightmapDesaturation = -1;
		Resources.sceneViewProjection = -1;
		Resources.sceneUseTexture = -1;
		Resources.sceneModel = -1;
		Resources.sceneTextureTransform = -1;
		Resources.sceneCameraPosition = -1;
		Resources.sceneObjectColor = -1;
		Resources.sceneSkyDepth = -1;
		Resources.sceneSkyFog = -1;
		Resources.sceneMaterialFlags = -1;
		Resources.sceneLightCounts = -1;
		Resources.sceneProjectedLights = -1;
		Resources.sceneClipPlane = -1;
		Resources.sceneClipPlaneEnabled = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
		Resources.sceneShortIndices.clear();
		Resources.sceneIndexType = GL_UNSIGNED_INT;
		Resources.sceneIndexStride = sizeof(GLuint);
		Resources.sceneViewSnapshots.clear();
		Resources.sceneBatches.clear();
		Resources.sceneOrderRecords.clear();
		Resources.portalOrderRecords.clear();
		gl_GLESInternalSceneClearLights();
		Resources.portalTargets.clear();
		NativePortalCaptureStack.clear();
		Resources.skyMaterial = NULL;
		Resources.skyXOffset = 0.0f;
		Resources.skyYOffset = 0.0f;
		Resources.skyLayerMaterial = NULL;
		Resources.skyLayerXOffset = 0.0f;
		Resources.skyLayerYOffset = 0.0f;
		Resources.sky2 = false;
		if (clearTextureCache) gl_GLESInternalClearMaterials(false);
		Resources.sceneReady = false;
		memset(Resources.viewProjection, 0, sizeof(Resources.viewProjection));
		Resources.ready = false;
	}

	static void InvalidateResources()
	{
		NativeUserPrograms.clear();
		gl_GLESInternalSceneInvalidateBuffers();
		Resources.sceneProgram = 0;
		Resources.simpleProgram = 0;
		Resources.fogProgram = 0;
		Resources.fogMaskedProgram = 0;
		Resources.maskedProgram = 0;
		Resources.paletteProgram = 0;
		gl_GLESInternalPresentContextLost();
		gl_GLESInternalPortalContextLost();
		Resources.sceneVertexBuffer = 0;
		Resources.sceneIndexBuffer = 0;
		Resources.sceneVertexArray = 0;
		Resources.vertexBuffer = 0;
		Resources.indexBuffer = 0;
		Resources.vertexArray = 0;
		Resources.checkerTexture = 0;
		gl_GLES_InvalidateRenderTarget(&Resources.sceneTarget);
		gl_GLES_InvalidateRenderTarget(&Resources.wipeOverlayTarget);
		gl_GLES_InvalidateRenderTarget(&Resources.cameraTarget);
		Resources.viewContract = {};
		NativeActiveTarget = NULL;
		NativeOffscreenRender = false;
		ResetNativeDrawUniforms();
		NativeMaterialUniforms.clear();
		NativeMaterialTimes.clear();
		gl_GLESInternalWipeContextLost();
		memset(Resources.worldSamplers, 0, sizeof(Resources.worldSamplers));
		memset(Resources.sceneSamplers, 0, sizeof(Resources.sceneSamplers));
		memset(Resources.nearestSamplers, 0, sizeof(Resources.nearestSamplers));
		Resources.textureFilter = -1;
		Resources.textureAnisotropy = -1.0f;
		Resources.sceneTextureUniform = -1;
		Resources.sceneBrightmapUniform = -1;
		Resources.sceneUseBrightmap = -1;
		Resources.sceneBrightmapDesaturation = -1;
		Resources.sceneViewProjection = -1;
		Resources.sceneUseTexture = -1;
		Resources.sceneModel = -1;
		Resources.sceneTextureTransform = -1;
		Resources.sceneCameraPosition = -1;
		Resources.sceneObjectColor = -1;
		Resources.sceneSkyDepth = -1;
		Resources.sceneSkyFog = -1;
		Resources.sceneMaterialFlags = -1;
		Resources.sceneLightCounts = -1;
		Resources.sceneProjectedLights = -1;
		Resources.sceneClipPlane = -1;
		Resources.sceneClipPlaneEnabled = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
		Resources.sceneShortIndices.clear();
		Resources.sceneIndexType = GL_UNSIGNED_INT;
		Resources.sceneIndexStride = sizeof(GLuint);
		Resources.sceneViewSnapshots.clear();
		Resources.sceneBatches.clear();
		Resources.sceneOrderRecords.clear();
		Resources.portalOrderRecords.clear();
		gl_GLESInternalSceneClearLights();
		Resources.portalTargets.clear();
		NativePortalCaptureStack.clear();
		Resources.skyMaterial = NULL;
		Resources.skyXOffset = 0.0f;
		Resources.skyYOffset = 0.0f;
		Resources.skyLayerMaterial = NULL;
		Resources.skyLayerXOffset = 0.0f;
		Resources.skyLayerYOffset = 0.0f;
		Resources.sky2 = false;
		gl_GLESInternalInvalidateMaterials();
		Resources.sceneReady = false;
		memset(Resources.viewProjection, 0, sizeof(Resources.viewProjection));
		Resources.ready = false;
		gl_GLESInternalStateContextLost();
		Resources.framebufferWarningLogged = false;
	}

	static void BuildCheckerTexture()
	{
		static unsigned char pixels[64 * 64 * 4];
		for (int y = 0; y < 64; ++y)
		{
			for (int x = 0; x < 64; ++x)
			{
				const bool dark = ((x / 8) ^ (y / 8)) & 1;
				const int offset = (y * 64 + x) * 4;
				pixels[offset + 0] = dark ? 32 : 220;
				pixels[offset + 1] = dark ? 100 : 220;
				pixels[offset + 2] = dark ? 190 : 245;
				pixels[offset + 3] = 255;
			}
		}
		glGenTextures(1, &Resources.checkerTexture);
		glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		CheckError("texture upload");

		Resources.maximumAnisotropy = 1;
		if (Capabilities.hasAnisotropicFiltering)
			glGetIntegerv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &Resources.maximumAnisotropy);
		glGenSamplers(4, Resources.worldSamplers);
		glGenSamplers(4, Resources.sceneSamplers);
		glGenSamplers(4, Resources.nearestSamplers);
		for (unsigned int clamp = 0; clamp < 4; ++clamp)
		{
			const GLuint samplers[3] = { Resources.worldSamplers[clamp],
				Resources.sceneSamplers[clamp], Resources.nearestSamplers[clamp] };
			for (int kind = 0; kind < 3; ++kind)
			{
				glSamplerParameteri(samplers[kind], GL_TEXTURE_MIN_FILTER,
					kind == 0 ? GL_LINEAR_MIPMAP_LINEAR : kind == 1 ? GL_LINEAR : GL_NEAREST);
				glSamplerParameteri(samplers[kind], GL_TEXTURE_MAG_FILTER, kind == 2 ? GL_NEAREST : GL_LINEAR);
				glSamplerParameteri(samplers[kind], GL_TEXTURE_WRAP_S, clamp & 1 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
				glSamplerParameteri(samplers[kind], GL_TEXTURE_WRAP_T, clamp & 2 ? GL_CLAMP_TO_EDGE : GL_REPEAT);
			}
		}
		CheckError("sampler setup");
	}

	static void ConfigureNativeSamplers()
	{
		if (Resources.worldSamplers[0] == 0 || Resources.sceneSamplers[0] == 0) return;
		int filter = gl_texture_filter;
		if (filter < 0 || filter >= 6) filter = 0;
		const float anisotropy = std::max(1.0f, std::min(static_cast<float>(gl_texture_filter_anisotropic),
			static_cast<float>(Resources.maximumAnisotropy)));
		if (Resources.textureFilter == filter && Resources.textureAnisotropy == anisotropy) return;
		const TexFilter_s &settings = TexFilter[filter];
		for (unsigned int clamp = 0; clamp < 4; ++clamp)
		{
			glSamplerParameteri(Resources.worldSamplers[clamp], GL_TEXTURE_MIN_FILTER, settings.minfilter);
			glSamplerParameteri(Resources.worldSamplers[clamp], GL_TEXTURE_MAG_FILTER, settings.magfilter);
			glSamplerParameteri(Resources.sceneSamplers[clamp], GL_TEXTURE_MIN_FILTER, settings.magfilter);
			glSamplerParameteri(Resources.sceneSamplers[clamp], GL_TEXTURE_MAG_FILTER, settings.magfilter);
			if (Capabilities.hasAnisotropicFiltering)
			{
				glSamplerParameterf(Resources.worldSamplers[clamp], GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
				glSamplerParameterf(Resources.sceneSamplers[clamp], GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.0f);
			}
		}
		Resources.textureFilter = filter;
		Resources.textureAnisotropy = anisotropy;
		CheckError("native texture filter setup");
	}

	static bool BuildFramebuffer(int width, int height)
	{
		const int requestedSamples = std::max(static_cast<int>(gl_vid_multisample), 0);
		if (!gl_GLES_CreateRenderTarget(&Resources.sceneTarget, width, height, requestedSamples))
		{
			if (!Resources.framebufferWarningLogged)
			{
				Resources.framebufferWarningLogged = true;
				Printf("Zandronum GLES render target could not be allocated at %dx%d.\n", width, height);
			}
			return false;
		}
		Resources.sceneTarget.hostOwnsPresentation = true;
		return true;
	}

	static void BuildViewProjection(float cameraX, float cameraY, float cameraZ,
		float cameraYaw, float cameraPitch, float cameraRoll, float fieldOfView, float aspect, float fovRatio,
		bool mirrored = false, bool planeMirrored = false)
	{
		const float pitch = cameraPitch * 3.14159265359f / 180.0f;
		Resources.cameraX = cameraX;
		Resources.cameraY = cameraY;
		Resources.cameraZ = cameraZ;
		Resources.cameraYaw = cameraYaw * 3.14159265359f / 180.0f;
		Resources.cameraPitch = pitch;
		Resources.cameraFieldOfView = fieldOfView;
		Resources.cameraAspect = aspect;
		Resources.cameraFovRatio = fovRatio;
		// Match SetViewMatrix: native vertices are supplied as (x, z, y),
		// then receive roll, pitch, yaw, translation, and the x mirror.
		const float viewYaw = (270.0f - cameraYaw) * 3.14159265359f / 180.0f;
		const float roll = cameraRoll * 3.14159265359f / 180.0f;
		const float mirrorSign = mirrored ? -1.0f : 1.0f;
		const float planeMirrorSign = planeMirrored ? -1.0f : 1.0f;
		const float viewMatrices[4][16] =
		{
			{
				cosf(roll), sinf(roll), 0.0f, 0.0f,
				-sinf(roll), cosf(roll), 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			},
			{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, cosf(pitch), sinf(pitch), 0.0f,
				0.0f, -sinf(pitch), cosf(pitch), 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			},
			{
				cosf(viewYaw), 0.0f, -mirrorSign * sinf(viewYaw), 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				mirrorSign * sinf(viewYaw), 0.0f, cosf(viewYaw), 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			},
			{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				cameraX * mirrorSign, -cameraZ * planeMirrorSign, -cameraY, 1.0f
			}
		};
		const float viewScale[16] =
		{
			-mirrorSign, 0.0f, 0.0f, 0.0f,
			0.0f, planeMirrorSign, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		auto multiply = [](const float *left, const float *right, float *result)
		{
			float value[16];
			for (int column = 0; column < 4; ++column)
				for (int row = 0; row < 4; ++row)
					value[column * 4 + row] = left[row] * right[column * 4] +
						left[4 + row] * right[column * 4 + 1] +
						left[8 + row] * right[column * 4 + 2] +
						left[12 + row] * right[column * 4 + 3];
			memcpy(result, value, sizeof(value));
		};
		float view[16];
		float intermediate[16];
		memcpy(view, viewMatrices[0], sizeof(view));
		multiply(view, viewMatrices[1], intermediate);
		multiply(intermediate, viewMatrices[2], view);
		multiply(view, viewMatrices[3], intermediate);
		multiply(intermediate, viewScale, view);
		const float verticalDegrees = 2 * RAD2DEG(atan(tan(DEG2RAD(fieldOfView) / 2) /
			std::max(fovRatio, 0.01f)));
		const double focal = 1.0 / tan(DEG2RAD(verticalDegrees) / 2);
		const float nearPlane = 5.0f;
		const float farPlane = 65536.0f;
		const float invRange = 1.0f / (nearPlane - farPlane);
		float projection[16] =
		{
			static_cast<float>(focal / (aspect > 0.01f ? aspect : 1.0f)), 0.0f, 0.0f, 0.0f,
			0.0f, static_cast<float>(focal), 0.0f, 0.0f,
			0.0f, 0.0f, (farPlane + nearPlane) * invRange, -1.0f,
			0.0f, 0.0f, 2.0f * farPlane * nearPlane * invRange, 0.0f
		};
		for (int column = 0; column < 4; ++column)
		{
			for (int row = 0; row < 4; ++row)
			{
				Resources.viewProjection[column * 4 + row] =
					projection[row] * view[column * 4] +
					projection[4 + row] * view[column * 4 + 1] +
					projection[8 + row] * view[column * 4 + 2] +
					projection[12 + row] * view[column * 4 + 3];
			}
		}
		memcpy(Resources.viewContract.viewMatrix, view, sizeof(view));
		memcpy(Resources.viewContract.projectionMatrix, projection, sizeof(projection));
		memcpy(Resources.viewContract.viewProjectionMatrix, Resources.viewProjection,
			sizeof(Resources.viewProjection));
		Resources.viewContract.cameraPosition[0] = cameraX;
		Resources.viewContract.cameraPosition[1] = cameraY;
		Resources.viewContract.cameraPosition[2] = cameraZ;
		Resources.viewContract.viewIndex = 0;
		Resources.viewContract.inactiveViewIndex = 1;
		Resources.viewContract.active = true;
		++Resources.viewSerial;
	}

	static unsigned int AddSceneVertex(const FSceneVertex &vertex)
	{
		if (!CanAppendSceneGeometry(1, 0, "vertex")) return 0xffffffffu;
		Resources.sceneVertices.push_back(vertex);
		return static_cast<unsigned int>(Resources.sceneVertices.size() - 1);
	}

	static void CopyNativeLightData(FSceneBatch &batch, const float *lightData,
		const unsigned int *lightCounts)
	{
		FScopedProfileTimer timer(Profile.lightSelectionMilliseconds);
		if (lightData == NULL || lightCounts == NULL)
		{
			batch.lightOffset = 0;
			batch.lightCount = 0;
			batch.lightNormalCount = 0;
			batch.lightSubtractiveCount = 0;
			return;
		}
		FGLESSceneLightSelection selection = {};
		if (!gl_GLESInternalSceneAppendLights(lightData, lightCounts, &selection))
			I_FatalError("Zandronum GLES selected light data could not be retained.");
		batch.lightOffset = selection.streamOffset;
		batch.lightCount = selection.lightCount;
		batch.lightNormalCount = selection.normalCount;
		batch.lightSubtractiveCount = selection.subtractiveCount;
	}

	static void SetLightPlaneNormal(FSceneBatch &batch, const FSceneVertex &a,
		const FSceneVertex &b, const FSceneVertex &c)
	{
		const float abX = b.x - a.x;
		const float abY = b.y - a.y;
		const float abZ = b.z - a.z;
		const float acX = c.x - a.x;
		const float acY = c.y - a.y;
		const float acZ = c.z - a.z;
		const float nx = abY * acZ - abZ * acY;
		const float ny = abZ * acX - abX * acZ;
		const float nz = abX * acY - abY * acX;
		const float length = sqrtf(nx * nx + ny * ny + nz * nz);
		if (length <= 0.0001f) return;
		batch.lightPlaneNormal[0] = nx / length;
		batch.lightPlaneNormal[1] = ny / length;
		batch.lightPlaneNormal[2] = nz / length;
	}

	static const FSceneViewSnapshot &BatchView(const FSceneBatch &batch)
	{
		return Resources.sceneViewSnapshots[batch.viewSnapshot];
	}

	static unsigned int CreateSceneViewSnapshot()
	{
		FSceneViewSnapshot snapshot = {};
		memcpy(snapshot.viewProjection, Resources.viewProjection, sizeof(snapshot.viewProjection));
		snapshot.cameraPosition[0] = Resources.cameraX;
		snapshot.cameraPosition[1] = Resources.cameraY;
		snapshot.cameraPosition[2] = Resources.cameraZ;
		snapshot.clipPlaneEnabled = Resources.clipPlaneEnabled;
		memcpy(snapshot.clipPlane, Resources.clipPlane, sizeof(snapshot.clipPlane));
		Resources.sceneViewSnapshots.push_back(snapshot);
		return static_cast<unsigned int>(Resources.sceneViewSnapshots.size() - 1);
	}

	static bool NativeSourceOrder = false;

	static void CaptureBatchView(FSceneBatch &batch)
	{
		batch.sourceOrdered = NativeSourceOrder;
		batch.viewSerial = Resources.viewSerial;
		batch.clipSerial = Resources.clipSerial;
		if (!Resources.sceneBatches.empty())
		{
			const FSceneBatch &previous = Resources.sceneBatches.back();
			if (previous.viewSerial == batch.viewSerial && previous.clipSerial == batch.clipSerial &&
				previous.viewSnapshot < Resources.sceneViewSnapshots.size())
			{
				batch.viewSnapshot = previous.viewSnapshot;
			}
			else
				batch.viewSnapshot = CreateSceneViewSnapshot();
		}
		else
			batch.viewSnapshot = CreateSceneViewSnapshot();
		batch.portalId = NativePortalCaptureStack.empty() ? -1 :
			static_cast<int>(NativePortalCaptureStack.back());
		batch.wipeOverlay = NativeWipeOverlayCollecting;
	}

	static bool HasNativeWipeOverlay()
	{
		for (size_t i = 0; i < Resources.sceneBatches.size(); ++i)
			if (Resources.sceneBatches[i].wipeOverlay) return true;
		return false;
	}

	static bool CanMergeOpaqueBatches(const FSceneBatch &previous, const FSceneBatch &batch)
	{
		const FSceneViewSnapshot &previousView = BatchView(previous);
		const FSceneViewSnapshot &batchView = BatchView(batch);
		// Keep every order-sensitive path as its own command. Consecutive opaque
		// world quads sharing the exact selected-light payload are safe to submit together.
		return !previous.sourceOrdered && !batch.sourceOrdered &&
			!previous.translucent && !batch.translucent &&
			previous.flat == batch.flat && !previous.hud && !batch.hud &&
			!previous.wipeOverlay && !batch.wipeOverlay && !previous.model && !batch.model &&
			!previous.cullBackFaces && !batch.cullBackFaces && !previous.skyMask && !batch.skyMask &&
			!previous.flood && !batch.flood && !previous.portalMask && !batch.portalMask &&
			previous.portalId < 0 && batch.portalId < 0 &&
			previous.firstIndex + previous.indexCount == batch.firstIndex &&
			previous.primitiveMode == batch.primitiveMode && previous.texture == batch.texture &&
			previous.materialEffect.shaderIndex == batch.materialEffect.shaderIndex &&
			previous.materialEffect.speed == batch.materialEffect.speed &&
			previous.materialEffect.colormap == batch.materialEffect.colormap && previous.brightmap == batch.brightmap &&
			previous.brightmapDesaturation == batch.brightmapDesaturation &&
			previous.masked == batch.masked && previous.fog == batch.fog &&
			previous.cameraTexture == batch.cameraTexture &&
			previous.repeat == batch.repeat && previous.palette == batch.palette &&
			previous.materialFlags == batch.materialFlags && previous.blendMode == batch.blendMode &&
			previous.customBlend == batch.customBlend && previous.sourceBlend == batch.sourceBlend &&
			previous.destinationBlend == batch.destinationBlend &&
			previous.alphaCutoff == batch.alphaCutoff &&
			((previous.lightCount == 0 && batch.lightCount == 0) ||
				(previous.lightOffset == batch.lightOffset &&
				previous.lightNormalCount == batch.lightNormalCount &&
				previous.lightSubtractiveCount == batch.lightSubtractiveCount &&
				previous.lightCount == batch.lightCount &&
				memcmp(previous.lightPlaneNormal, batch.lightPlaneNormal, sizeof(batch.lightPlaneNormal)) == 0)) &&
			previousView.clipPlaneEnabled == batchView.clipPlaneEnabled &&
			memcmp(previous.fogColor, batch.fogColor, sizeof(batch.fogColor)) == 0 &&
			previous.fogDensity == batch.fogDensity &&
			memcmp(&previous.lighting, &batch.lighting, sizeof(batch.lighting)) == 0 &&
			memcmp(previous.glowTopColor, batch.glowTopColor, sizeof(batch.glowTopColor)) == 0 &&
			memcmp(previous.glowBottomColor, batch.glowBottomColor, sizeof(batch.glowBottomColor)) == 0 &&
			previous.viewSerial == batch.viewSerial &&
			previous.clipSerial == batch.clipSerial;
	}

	static void RecordSceneBatchSubmission(const FSceneBatch &batch)
	{
		if (!Profile.active && !gl_gles_validate_order) return;
		++Resources.sceneBatchSubmissions;
		HashSceneOrderValue(batch.firstIndex);
		HashSceneOrderValue(batch.indexCount);
		HashSceneOrderValue(batch.primitiveMode);
		HashSceneOrderValue(batch.texture);
		HashSceneOrderValue(batch.brightmap);
		HashSceneOrderValue(batch.masked);
		HashSceneOrderValue(batch.fog);
		HashSceneOrderValue(batch.translucent);
		HashSceneOrderValue(batch.sourceOrdered);
		HashSceneOrderValue(batch.flat);
		HashSceneOrderValue(batch.hud);
		HashSceneOrderValue(batch.wipeOverlay);
		HashSceneOrderValue(batch.model);
		HashSceneOrderValue(batch.skyMask);
		HashSceneOrderValue(batch.flood);
		HashSceneOrderValue(batch.portalId);
		HashSceneOrderValue(batch.portalMask);
		HashSceneOrderValue(batch.materialFlags);
		HashSceneOrderValue(batch.blendMode);
		HashSceneOrderValue(batch.customBlend);
		HashSceneOrderValue(batch.sourceBlend);
		HashSceneOrderValue(batch.destinationBlend);
		HashSceneOrderValue(batch.alphaCutoff);
		HashSceneOrderValue(batch.lightOffset);
		HashSceneOrderValue(batch.lightCount);
		HashSceneOrderValue(batch.lightNormalCount);
		HashSceneOrderValue(batch.lightSubtractiveCount);
		HashSceneOrderValue(BatchView(batch).clipPlaneEnabled);
	}

	static void PushNativeSceneBatch(const FSceneBatch &batch)
	{
		FScopedProfileTimer timer(Profile.batchRecordMilliseconds);
		Resources.sceneBatches.push_back(batch);
	}

	static void AppendOpaqueBatch(FSceneBatch &batch)
	{
		RecordSceneBatchSubmission(batch);
		if (!gl_gles_validate_order && !Resources.sceneBatches.empty() &&
			CanMergeOpaqueBatches(Resources.sceneBatches.back(), batch))
		{
			Resources.sceneBatches.back().indexCount += batch.indexCount;
			++Resources.sceneOpaqueBatchMerges;
			return;
		}
		PushNativeSceneBatch(batch);
	}

	static void AddSceneQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked, bool fog, bool translucent, bool repeat,
		EGLESBlendMode blendMode, const float *fogColor, float fogDensity, unsigned int materialFlags,
		const float *lightData, const unsigned int *lightCounts, GLuint brightmap = 0,
		int brightmapDesaturation = 0, const float *topGlowColor = NULL,
		const float *bottomGlowColor = NULL, bool customBlend = false,
		GLenum sourceBlend = GL_SRC_ALPHA, GLenum destinationBlend = GL_ONE_MINUS_SRC_ALPHA,
	float alphaCutoff = 0.5f, bool hud = false, const FShaderLightParameters *lighting = NULL)
	{
		if (!CanAppendSceneGeometry(4, 6, "quad")) return;
		const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
		Resources.sceneVertices.resize(Resources.sceneVertices.size() + 4);
		FSceneVertex *vertexData = Resources.sceneVertices.data() + first;
		vertexData[0] = a;
		vertexData[1] = b;
		vertexData[2] = c;
		vertexData[3] = d;
		const unsigned int second = first + 1;
		const unsigned int third = first + 2;
		const unsigned int fourth = first + 3;
		const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
		Resources.sceneIndices.resize(Resources.sceneIndices.size() + 6);
		GLuint *indexData = Resources.sceneIndices.data() + firstIndex;
		{
			FScopedProfileTimer timer(Profile.indexEmissionMilliseconds);
			indexData[0] = first;
			indexData[1] = second;
			indexData[2] = third;
			indexData[3] = third;
			indexData[4] = fourth;
			indexData[5] = first;
		}
		const float centerX = (a.x + b.x + c.x + d.x) * 0.25f;
		const float centerY = (a.y + b.y + c.y + d.y) * 0.25f;
		const float centerZ = (a.z + b.z + c.z + d.z) * 0.25f;
		const float dx = centerX - Resources.cameraX;
		const float dy = centerY - Resources.cameraY;
		const float dz = centerZ - Resources.cameraZ;
		FSceneBatch batch = {};
		CaptureBatchView(batch);
		if (lighting != NULL) batch.lighting = *lighting;
		batch.firstIndex = firstIndex;
		batch.indexCount = 6;
		batch.texture = texture;
		batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
		batch.brightmap = brightmap;
		batch.brightmapDesaturation = brightmapDesaturation;
		batch.masked = masked;
		batch.fog = fog;
		batch.translucent = translucent || blendMode != GLES_BLEND_OPAQUE;
		batch.repeat = repeat;
		const unsigned int textureFlags = gl_GLESInternalGetMaterialFlags(texture);
		batch.cameraTexture = (textureFlags & GLES_TEXTURE_FLAG_FRAMEBUFFER) != 0;
		batch.palette = (textureFlags & GLES_TEXTURE_FLAG_PALETTE) != 0;
		batch.materialFlags = materialFlags;
		batch.blendMode = blendMode;
		batch.customBlend = customBlend;
		batch.sourceBlend = sourceBlend;
		batch.destinationBlend = destinationBlend;
		batch.alphaCutoff = alphaCutoff;
		batch.hud = hud;
		if (lightData != NULL && lightCounts != NULL)
			CopyNativeLightData(batch, lightData, lightCounts);
		if (batch.lightCount > 0)
			SetLightPlaneNormal(batch, a, b, c);
		batch.sortDepth = dx * dx + dy * dy + dz * dz;
		if ((materialFlags & GLES_MATERIAL_MIRROR_DECAL) != 0 && !Resources.sceneBatches.empty())
		{
			const FSceneBatch &surface = Resources.sceneBatches.back();
			if (surface.portalId == batch.portalId && surface.viewSerial == batch.viewSerial &&
				(surface.materialFlags & (GLES_MATERIAL_SPHERE_MAP | GLES_MATERIAL_MIRROR_DECAL)) != 0)
				batch.sortDepth = surface.sortDepth;
		}
		if (fogColor != NULL)
		{
			batch.fogColor[0] = ClampUnit(fogColor[0]);
			batch.fogColor[1] = ClampUnit(fogColor[1]);
			batch.fogColor[2] = ClampUnit(fogColor[2]);
		}
		batch.fogDensity = std::max(0.0f, fogDensity);
		if (topGlowColor != NULL || bottomGlowColor != NULL) batch.materialFlags |= GLES_MATERIAL_GLOW;
		if (topGlowColor != NULL) memcpy(batch.glowTopColor, topGlowColor, sizeof(batch.glowTopColor));
		if (bottomGlowColor != NULL) memcpy(batch.glowBottomColor, bottomGlowColor, sizeof(batch.glowBottomColor));
		AppendOpaqueBatch(batch);
	}

	static void AddHUDQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked,
		EGLESBlendMode blendMode, unsigned int materialFlags, bool customBlend = false,
		GLenum sourceBlend = GL_SRC_ALPHA, GLenum destinationBlend = GL_ONE_MINUS_SRC_ALPHA,
		float alphaCutoff = 0.5f, GLuint brightmap = 0, int brightmapDesaturation = 0)
	{
		const size_t batchCount = Resources.sceneBatches.size();
		const bool translucent = masked || blendMode != GLES_BLEND_OPAQUE || a.a < 0.999f ||
			b.a < 0.999f || c.a < 0.999f || d.a < 0.999f;
		AddSceneQuad(a, b, c, d, texture, masked, false, translucent, false, blendMode, NULL, 0.0f, materialFlags,
			NULL, NULL, brightmap, brightmapDesaturation, NULL, NULL, customBlend, sourceBlend, destinationBlend, alphaCutoff, true);
		if (Resources.sceneBatches.size() <= batchCount) return;
		FSceneBatch &batch = Resources.sceneBatches.back();
		if (!batch.wipeOverlay || Resources.sceneBatches.size() < 2) return;

		FSceneBatch &previous = Resources.sceneBatches[Resources.sceneBatches.size() - 2];
		const FSceneViewSnapshot &previousView = BatchView(previous);
		const FSceneViewSnapshot &batchView = BatchView(batch);
		const bool compatible = previous.hud && previous.wipeOverlay &&
			previous.firstIndex + previous.indexCount == batch.firstIndex &&
			previous.texture == batch.texture &&
			previous.materialEffect.shaderIndex == batch.materialEffect.shaderIndex &&
			previous.materialEffect.speed == batch.materialEffect.speed &&
			previous.materialEffect.colormap == batch.materialEffect.colormap && previous.brightmap == batch.brightmap &&
			previous.brightmapDesaturation == batch.brightmapDesaturation &&
			previous.masked == batch.masked && previous.fog == batch.fog &&
			previous.cameraTexture == batch.cameraTexture &&
			previous.translucent == batch.translucent && previous.repeat == batch.repeat &&
			previous.palette == batch.palette && previous.model == batch.model &&
			previous.cullBackFaces == batch.cullBackFaces &&
			previous.materialFlags == batch.materialFlags &&
			previous.customBlend == batch.customBlend && previous.sourceBlend == batch.sourceBlend &&
			previous.destinationBlend == batch.destinationBlend && previous.alphaCutoff == batch.alphaCutoff &&
			previous.blendMode == batch.blendMode && previous.portalId == batch.portalId &&
			previousView.clipPlaneEnabled == batchView.clipPlaneEnabled &&
			previous.viewSerial == batch.viewSerial &&
			previous.clipSerial == batch.clipSerial;
		if (compatible)
		{
			previous.indexCount += batch.indexCount;
			Resources.sceneBatches.pop_back();
		}
	}

	static void SetNativeDecalDepthBias(const FSceneBatch &batch, bool enabled)
	{
		if ((batch.materialFlags & GLES_MATERIAL_DECAL) == 0) return;
		if (!gl_GLES_GetContextInfo().hasDepthClamp)
		{
			const GLint bias = GetNativeDrawUniforms(NativeBoundProgram).depthBias;
			if (bias >= 0) glUniform4f(bias, enabled ? -1.0f : 0.0f, enabled ? -128.0f : 0.0f, 0.0f, 0.0f);
			return;
		}
		if (enabled)
		{
			glEnable(GL_POLYGON_OFFSET_FILL);
			glPolygonOffset(-1.0f, -128.0f);
		}
		else
		{
			glPolygonOffset(0.0f, 0.0f);
			glDisable(GL_POLYGON_OFFSET_FILL);
		}
	}

	static bool IsNativeDecal(const FSceneBatch &batch)
	{
		return (batch.materialFlags & GLES_MATERIAL_DECAL) != 0;
	}

	static bool CanUseNativeSimpleProgram(const FSceneBatch &batch)
	{
		return Resources.simpleProgram != 0 && !batch.fog && !batch.masked && !batch.palette &&
			!batch.translucent && !batch.hud && !batch.model && !batch.flood &&
			!batch.skyMask && !batch.portalMask && !batch.wipeOverlay && batch.portalId < 0 &&
			batch.materialFlags == 0 && batch.brightmap == 0 && batch.lightCount == 0 &&
			batch.lighting.software == 0.0f && batch.lighting.distance == 0.0f &&
			batch.materialEffect.shaderIndex == 0 &&
			batch.materialEffect.colormap == CM_DEFAULT &&
			!BatchView(batch).clipPlaneEnabled && !batch.customBlend && batch.blendMode == GLES_BLEND_OPAQUE &&
			batch.glowTopColor[3] <= 0.0f && batch.glowBottomColor[3] <= 0.0f;
	}

	static void UploadSceneGeometry()
	{
		if (!gl_GLESInternalSceneUploadLights())
			I_FatalError("Zandronum GLES light data could not be uploaded.");
		Resources.sceneIndexCount = static_cast<GLsizei>(Resources.sceneIndices.size());
		Resources.sceneReady = Resources.sceneIndexCount > 0;
		if (!Resources.sceneReady) return;
		Resources.sceneIndexType = GL_UNSIGNED_INT;
		Resources.sceneIndexStride = sizeof(GLuint);
		const void *indexData = Resources.sceneIndices.data();
		size_t indexBytes = Resources.sceneIndices.size() * sizeof(GLuint);
		if (Resources.sceneVertices.size() <= 65536u)
		{
			bool fitsShort = true;
			Resources.sceneShortIndices.resize(Resources.sceneIndices.size());
			for (size_t index = 0; index < Resources.sceneIndices.size(); ++index)
			{
				if (Resources.sceneIndices[index] > 0xffffu)
				{
					fitsShort = false;
					break;
				}
				Resources.sceneShortIndices[index] = static_cast<GLushort>(Resources.sceneIndices[index]);
			}
			if (fitsShort)
			{
				Resources.sceneIndexType = GL_UNSIGNED_SHORT;
				Resources.sceneIndexStride = sizeof(GLushort);
				indexData = Resources.sceneShortIndices.data();
				indexBytes = Resources.sceneShortIndices.size() * sizeof(GLushort);
			}
			else
				Resources.sceneShortIndices.clear();
		}
		else
			Resources.sceneShortIndices.clear();
		const auto uploadStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		const unsigned int reallocations = gl_GLESInternalSceneUploadGeometry(Resources.sceneVertexArray,
			Resources.sceneVertexBuffer, Resources.sceneIndexBuffer,
			&Resources.sceneVertices[0], Resources.sceneVertices.size() * sizeof(FSceneVertex),
			indexData, indexBytes);
		if (Profile.active)
		{
			Profile.bufferReallocations += reallocations;
			Profile.uploadMilliseconds += ProfileMilliseconds(uploadStart);
			Profile.vertexBytes = Resources.sceneVertices.size() * sizeof(FSceneVertex);
			Profile.indexBytes = indexBytes;
		}
	}

	static void AddSkyMaskCaps(FNativeSkyRecord &sky)
	{
		if (sky.capsAdded || !sky.capEligible) return;
		const float extent = 32767.0f;
		const float heights[2] = { extent, -extent };
		for (int cap = 0; cap < 2; ++cap)
		{
			if (!CanAppendSceneGeometry(4, 6, "sky cap"))
			{
				++sky.rejected;
				return;
			}
			const unsigned int firstVertex = static_cast<unsigned int>(Resources.sceneVertices.size());
			const float y = heights[cap];
			const float positions[12] =
			{
				-extent, y, -extent,
				-extent, y,  extent,
				 extent, y,  extent,
				 extent, y, -extent
			};
			for (int vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
			{
				FSceneVertex vertex = {};
				vertex.x = positions[vertexIndex * 3 + 0];
				vertex.y = positions[vertexIndex * 3 + 1];
				vertex.z = positions[vertexIndex * 3 + 2];
				vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
				if (AddSceneVertex(vertex) == 0xffffffffu) return;
			}
			const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
			Resources.sceneIndices.push_back(firstVertex);
			Resources.sceneIndices.push_back(firstVertex + 1);
			Resources.sceneIndices.push_back(firstVertex + 2);
			Resources.sceneIndices.push_back(firstVertex);
			Resources.sceneIndices.push_back(firstVertex + 2);
			Resources.sceneIndices.push_back(firstVertex + 3);
			FSceneBatch batch = {};
			CaptureBatchView(batch);
			batch.firstIndex = firstIndex;
			batch.indexCount = 6;
			batch.skyMask = true;
			batch.portalId = sky.portalId;
			RecordSceneBatchSubmission(batch);
			PushNativeSceneBatch(batch);
			sky.maskBatches.push_back(Resources.sceneBatches.size() - 1);
		}
		sky.capsAdded = true;
	}

	static bool BuildResources(int width, int height)
	{
		if (developer) DPrintf("GLES depth: %s\n", gl_GLES_GetContextInfo().hasDepthClamp ?
			"native depth clamping available" : "shader depth clamping enabled");
		static const char *sceneVertexSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"uniform mat4 u_view_projection;\n"
			"uniform mat4 u_model;\n"
			"uniform vec4 u_texture_transform;\n"
			"uniform vec3 u_camera_position;\n"
			"uniform vec4 u_object_color;\n"
			"uniform bool u_sky_depth;\n"
			"uniform bool u_sky_fog;\n"
			"layout(location = 0) in vec3 a_position;\n"
			"layout(location = 1) in vec2 a_uv;\n"
			"layout(location = 2) in vec4 a_color;\n"
			"layout(location = 3) in vec3 a_normal;\n"
			"layout(location = 4) in vec2 a_secondary;\n"
			"out vec2 v_uv;\n"
			"out vec4 v_color;\n"
			"out vec3 v_world_position;\n"
			"out float v_camera_distance;\n"
			"out vec2 v_glow_distance;\n"
			"void main() { vec4 world_position = u_model * vec4(a_position, 1.0); gl_Position = u_view_projection * world_position; gl_PointSize = 1.0; if (u_sky_depth) gl_Position.z = gl_Position.w; v_world_position = world_position.xyz; v_uv = a_uv * u_texture_transform.xy + u_texture_transform.zw; v_color = vec4(a_color.rgb * u_object_color.rgb, (u_sky_fog ? 1.0 : a_color.a) * u_object_color.a); v_camera_distance = gl_Position.w; v_glow_distance = a_secondary; }\n";
		static const char *sceneFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"in vec2 v_glow_distance;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float desaturation_factor;\n"
			"uniform bool u_material_colormap;\n"
			"uniform vec3 colormapstart;\n"
			"uniform vec3 colormaprange;\n"
			"vec4 desaturate(vec4 value) { float gray = dot(value.rgb, vec3(0.3, 0.56, 0.14)); return mix(vec4(gray, gray, gray, value.a), value, desaturation_factor); }\n"
			"uniform highp int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform highp sampler2D u_light_data;\n"
			"uniform highp int u_light_offset;\n"
			"uniform highp ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"uniform vec4 u_static_light;\n"
			"uniform vec3 u_sprite_light;\n"
			"uniform bool u_sky_depth;\n"
			"uniform highp int u_material_effect;\n"
			"uniform float u_material_time;\n"
			"vec2 material_uv(vec2 coords) { const float pi = 3.14159265358979323846; vec2 offset = vec2(0.0); if (u_material_effect == 1) { offset.y = sin(pi * 2.0 * (coords.x + u_material_time * 0.125)) * 0.1; offset.x = sin(pi * 2.0 * (coords.y + u_material_time * 0.125)) * 0.1; } else if (u_material_effect == 2) { float siny = sin(pi * 2.0 * (coords.y * 2.2 + u_material_time * 0.75)) * 0.03; offset.y = siny + sin(pi * 2.0 * (coords.x * 0.75 + u_material_time * 0.75)) * 0.03; offset.x = siny + sin(pi * 2.0 * (coords.x * 1.1 + u_material_time * 0.45)) * 0.02; } return coords + offset; }\n"
			"vec3 apply_static_light(vec3 color, float fogDistance) { if (u_sky_depth) return color; if (u_static_light.w > 0.0) { float L = u_static_light.x * 63.0 / 31.0; float minL = clamp(36.0 / 31.0 - L, 0.0, 1.0); float scale = 1.0 / max(gl_FragCoord.z, 0.0001); float index = (59.0 / 31.0 - L) - (scale * 232.0 / 31.0 - 232.0 / 31.0); float light = 1.0 - clamp(index, minL, 1.0); color *= clamp(vec3(light) + u_sprite_light, vec3(0.0), vec3(1.0)); } else if (u_static_light.z > 0.0 && fogDistance < u_static_light.z) color *= u_static_light.y - (fogDistance / u_static_light.z) * (u_static_light.y - 1.0); return color; }\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if ((u_material_flags & 65536) == 0) return color; if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += desaturate(u_glow_top_color * (1.0 - v_glow_distance.x / u_glow_top_color.a)).rgb; if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += desaturate(u_glow_bottom_color * (1.0 - v_glow_distance.y / u_glow_bottom_color.a)).rgb; return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (highp int i = 0; i < u_light_counts.z; ++i) { highp int index = (u_light_offset + i) * 2; highp int width = textureSize(u_light_data, 0).x; vec4 lightPosition = texelFetch(u_light_data, ivec2(index % width, index / width), 0); index += 1; vec3 lightColor = texelFetch(u_light_data, ivec2(index % width, index / width), 0).rgb; vec3 delta = v_world_position - lightPosition.xyz; float radius = max(lightPosition.w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = lightColor * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = u_light_counts.z > 0 ? clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)) : base; }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec2 sampleUV = v_uv; if ((u_material_flags & 8) != 0 && ((u_material_flags >> 10) & 7) == 5) sampleUV += vec2(mod(sin(6.28318530718 * (v_uv.y + u_fuzz_time * 2.0)), 0.1), mod(cos(6.28318530718 * (v_uv.x + u_fuzz_time * 2.0)), 0.1)) * 0.1; vec4 texel = u_use_texture ? texture(u_texture, material_uv(sampleUV)) : vec4(1.0); if ((u_material_flags & 4) != 0) texel.a = 1.0; if ((u_material_flags & 2) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel = desaturate(texel); vec3 lighting; vec3 additive; apply_dynamic_lights(apply_glow(apply_static_light(v_color.rgb, 0.0)), lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); if (u_material_colormap) { lighting = vec3(1.0); additive = vec3(0.0); } vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a)); if ((u_material_flags & 1) != 0) { color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); color.rgb = lighting; } if ((u_material_flags & 16) != 0) { color.rgb = u_material_colormap ? vec3(1.0) : v_color.rgb; color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); } if ((u_material_flags & 8) != 0) { int fuzzType = (u_material_flags >> 10) & 7; vec2 texCoord = v_uv; if (fuzzType == 1 || fuzzType == 6) texCoord = trunc(texCoord * 128.0) / 128.0; float texX; float texY; if (fuzzType <= 2) { texX = texCoord.x / 3.0 + 0.66; texY = 0.34 - texCoord.y / 3.0; } else if (fuzzType <= 5) { texX = sin(texCoord.x * 100.0 + u_fuzz_time * 5.0); texY = cos(texCoord.x * 100.0 + u_fuzz_time * 5.0); } else { texX = sin(mod(texCoord.x * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.x / 4.0; texY = cos(mod(texCoord.y * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.y / 4.0; } float fuzz = mod(u_fuzz_time * 2.0 + ((texX / texY) * 21.0 + (texY / texX) * 13.0), 0.5); if (fuzzType != 4 && fuzzType != 5) color.rgb = vec3(0.0); color.a *= fuzz; } if (u_material_colormap) { float gray = dot(color.rgb, vec3(0.3, 0.56, 0.14)); color = vec4(clamp(colormapstart + gray * colormaprange, vec3(0.0), vec3(1.0)), color.a) * v_color; } color.rgb += additive;  frag_color = color; }\n";
		static const char *simpleFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform bool u_use_texture;\n"
			"void main() { vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); frag_color = vec4(texel.rgb * clamp(v_color.rgb, vec3(0.0), vec3(1.4)), texel.a * v_color.a); }\n";
		static const char *maskedFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"in vec2 v_glow_distance;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float desaturation_factor;\n"
			"uniform bool u_material_colormap;\n"
			"uniform vec3 colormapstart;\n"
			"uniform vec3 colormaprange;\n"
			"vec4 desaturate(vec4 value) { float gray = dot(value.rgb, vec3(0.3, 0.56, 0.14)); return mix(vec4(gray, gray, gray, value.a), value, desaturation_factor); }\n"
			"uniform float u_alpha_cutoff;\n"
			"uniform highp int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform highp sampler2D u_light_data;\n"
			"uniform highp int u_light_offset;\n"
			"uniform highp ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"uniform vec4 u_static_light;\n"
			"uniform vec3 u_sprite_light;\n"
			"uniform bool u_sky_depth;\n"
			"uniform highp int u_material_effect;\n"
			"uniform float u_material_time;\n"
			"vec2 material_uv(vec2 coords) { const float pi = 3.14159265358979323846; vec2 offset = vec2(0.0); if (u_material_effect == 1) { offset.y = sin(pi * 2.0 * (coords.x + u_material_time * 0.125)) * 0.1; offset.x = sin(pi * 2.0 * (coords.y + u_material_time * 0.125)) * 0.1; } else if (u_material_effect == 2) { float siny = sin(pi * 2.0 * (coords.y * 2.2 + u_material_time * 0.75)) * 0.03; offset.y = siny + sin(pi * 2.0 * (coords.x * 0.75 + u_material_time * 0.75)) * 0.03; offset.x = siny + sin(pi * 2.0 * (coords.x * 1.1 + u_material_time * 0.45)) * 0.02; } return coords + offset; }\n"
			"vec3 apply_static_light(vec3 color, float fogDistance) { if (u_sky_depth) return color; if (u_static_light.w > 0.0) { float L = u_static_light.x * 63.0 / 31.0; float minL = clamp(36.0 / 31.0 - L, 0.0, 1.0); float scale = 1.0 / max(gl_FragCoord.z, 0.0001); float index = (59.0 / 31.0 - L) - (scale * 232.0 / 31.0 - 232.0 / 31.0); float light = 1.0 - clamp(index, minL, 1.0); color *= clamp(vec3(light) + u_sprite_light, vec3(0.0), vec3(1.0)); } else if (u_static_light.z > 0.0 && fogDistance < u_static_light.z) color *= u_static_light.y - (fogDistance / u_static_light.z) * (u_static_light.y - 1.0); return color; }\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if ((u_material_flags & 65536) == 0) return color; if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += desaturate(u_glow_top_color * (1.0 - v_glow_distance.x / u_glow_top_color.a)).rgb; if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += desaturate(u_glow_bottom_color * (1.0 - v_glow_distance.y / u_glow_bottom_color.a)).rgb; return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (highp int i = 0; i < u_light_counts.z; ++i) { highp int index = (u_light_offset + i) * 2; highp int width = textureSize(u_light_data, 0).x; vec4 lightPosition = texelFetch(u_light_data, ivec2(index % width, index / width), 0); index += 1; vec3 lightColor = texelFetch(u_light_data, ivec2(index % width, index / width), 0).rgb; vec3 delta = v_world_position - lightPosition.xyz; float radius = max(lightPosition.w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = lightColor * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = u_light_counts.z > 0 ? clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)) : base; }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec2 sampleUV = v_uv; if ((u_material_flags & 8) != 0 && ((u_material_flags >> 10) & 7) == 5) sampleUV += vec2(mod(sin(6.28318530718 * (v_uv.y + u_fuzz_time * 2.0)), 0.1), mod(cos(6.28318530718 * (v_uv.x + u_fuzz_time * 2.0)), 0.1)) * 0.1; vec4 texel = u_use_texture ? texture(u_texture, material_uv(sampleUV)) : vec4(1.0); if ((u_material_flags & 4) != 0) texel.a = 1.0; if ((u_material_flags & 2) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel = desaturate(texel); if ((u_material_flags & 1) != 0 ? texel.a <= 0.0 : texel.a <= 0.0 || texel.a < u_alpha_cutoff) discard; vec3 lighting; vec3 additive; apply_dynamic_lights(apply_glow(apply_static_light(v_color.rgb, 0.0)), lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); if (u_material_colormap) { lighting = vec3(1.0); additive = vec3(0.0); } vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a)); if ((u_material_flags & 1) != 0) { color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); color.rgb = lighting; } if ((u_material_flags & 16) != 0) { color.rgb = u_material_colormap ? vec3(1.0) : v_color.rgb; color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); } if ((u_material_flags & 8) != 0) { int fuzzType = (u_material_flags >> 10) & 7; vec2 texCoord = v_uv; if (fuzzType == 1 || fuzzType == 6) texCoord = trunc(texCoord * 128.0) / 128.0; float texX; float texY; if (fuzzType <= 2) { texX = texCoord.x / 3.0 + 0.66; texY = 0.34 - texCoord.y / 3.0; } else if (fuzzType <= 5) { texX = sin(texCoord.x * 100.0 + u_fuzz_time * 5.0); texY = cos(texCoord.x * 100.0 + u_fuzz_time * 5.0); } else { texX = sin(mod(texCoord.x * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.x / 4.0; texY = cos(mod(texCoord.y * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.y / 4.0; } float fuzz = mod(u_fuzz_time * 2.0 + ((texX / texY) * 21.0 + (texY / texX) * 13.0), 0.5); if (fuzzType != 4 && fuzzType != 5) color.rgb = vec3(0.0); color.a *= fuzz; } if (u_material_colormap) { float gray = dot(color.rgb, vec3(0.3, 0.56, 0.14)); color = vec4(clamp(colormapstart + gray * colormaprange, vec3(0.0), vec3(1.0)), color.a) * v_color; } color.rgb += additive;  frag_color = color; }\n";
		static const char *fogFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"in vec2 v_glow_distance;\n"
			"in float v_camera_distance;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float desaturation_factor;\n"
			"uniform bool u_material_colormap;\n"
			"uniform vec3 colormapstart;\n"
			"uniform vec3 colormaprange;\n"
			"vec4 desaturate(vec4 value) { float gray = dot(value.rgb, vec3(0.3, 0.56, 0.14)); return mix(vec4(gray, gray, gray, value.a), value, desaturation_factor); }\n"
			"uniform vec4 u_fog_color;\n"
			"uniform float u_fog_density;\n"
			"uniform vec3 u_camera_position;\n"
			"uniform highp int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform highp sampler2D u_light_data;\n"
			"uniform highp int u_light_offset;\n"
			"uniform highp ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"uniform vec4 u_static_light;\n"
			"uniform vec3 u_sprite_light;\n"
			"uniform bool u_sky_depth;\n"
			"uniform highp int u_material_effect;\n"
			"uniform float u_material_time;\n"
			"vec2 material_uv(vec2 coords) { const float pi = 3.14159265358979323846; vec2 offset = vec2(0.0); if (u_material_effect == 1) { offset.y = sin(pi * 2.0 * (coords.x + u_material_time * 0.125)) * 0.1; offset.x = sin(pi * 2.0 * (coords.y + u_material_time * 0.125)) * 0.1; } else if (u_material_effect == 2) { float siny = sin(pi * 2.0 * (coords.y * 2.2 + u_material_time * 0.75)) * 0.03; offset.y = siny + sin(pi * 2.0 * (coords.x * 0.75 + u_material_time * 0.75)) * 0.03; offset.x = siny + sin(pi * 2.0 * (coords.x * 1.1 + u_material_time * 0.45)) * 0.02; } return coords + offset; }\n"
			"vec3 apply_static_light(vec3 color, float fogDistance) { if (u_sky_depth) return color; if (u_static_light.w > 0.0) { float L = u_static_light.x * 63.0 / 31.0; float minL = clamp(36.0 / 31.0 - L, 0.0, 1.0); float scale = 1.0 / max(gl_FragCoord.z, 0.0001); float index = (59.0 / 31.0 - L) - (scale * 232.0 / 31.0 - 232.0 / 31.0); float light = 1.0 - clamp(index, minL, 1.0); color *= clamp(vec3(light) + u_sprite_light, vec3(0.0), vec3(1.0)); } else if (u_static_light.z > 0.0 && fogDistance < u_static_light.z) color *= u_static_light.y - (fogDistance / u_static_light.z) * (u_static_light.y - 1.0); return color; }\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if ((u_material_flags & 65536) == 0) return color; if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += desaturate(u_glow_top_color * (1.0 - v_glow_distance.x / u_glow_top_color.a)).rgb; if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += desaturate(u_glow_bottom_color * (1.0 - v_glow_distance.y / u_glow_bottom_color.a)).rgb; return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (highp int i = 0; i < u_light_counts.z; ++i) { highp int index = (u_light_offset + i) * 2; highp int width = textureSize(u_light_data, 0).x; vec4 lightPosition = texelFetch(u_light_data, ivec2(index % width, index / width), 0); index += 1; vec3 lightColor = texelFetch(u_light_data, ivec2(index % width, index / width), 0).rgb; vec3 delta = v_world_position - lightPosition.xyz; float radius = max(lightPosition.w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = lightColor * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = u_light_counts.z > 0 ? clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)) : base; }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec2 sampleUV = v_uv; if ((u_material_flags & 8) != 0 && ((u_material_flags >> 10) & 7) == 5) sampleUV += vec2(mod(sin(6.28318530718 * (v_uv.y + u_fuzz_time * 2.0)), 0.1), mod(cos(6.28318530718 * (v_uv.x + u_fuzz_time * 2.0)), 0.1)) * 0.1; vec4 texel = u_use_texture ? texture(u_texture, material_uv(sampleUV)) : vec4(1.0); if ((u_material_flags & 4) != 0) texel.a = 1.0; if ((u_material_flags & 2) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel = desaturate(texel); float fogDistance = (u_material_flags & 8192) != 0 ? max(16.0, distance(v_world_position, u_camera_position)) : v_camera_distance; float fog = clamp(exp(-u_fog_density * fogDistance), 0.0, 1.0); if ((u_material_flags & 262144) != 0) { frag_color = vec4(u_fog_color.rgb, (1.0 - fog) * texel.a * 0.75 * v_color.a); return; } vec3 lighting; vec3 additive; vec3 baseLighting = apply_static_light(v_color.rgb, fogDistance); if (all(equal(u_fog_color.rgb, vec3(0.0)))) baseLighting *= fog; apply_dynamic_lights(apply_glow(baseLighting), lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); if (u_material_colormap) { lighting = vec3(1.0); additive = vec3(0.0); } vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a)); if ((u_material_flags & 1) != 0) { color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); color.rgb = lighting; } if ((u_material_flags & 16) != 0) { color.rgb = u_material_colormap ? vec3(1.0) : v_color.rgb; color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); } if ((u_material_flags & 8) != 0) { int fuzzType = (u_material_flags >> 10) & 7; vec2 texCoord = v_uv; if (fuzzType == 1 || fuzzType == 6) texCoord = trunc(texCoord * 128.0) / 128.0; float texX; float texY; if (fuzzType <= 2) { texX = texCoord.x / 3.0 + 0.66; texY = 0.34 - texCoord.y / 3.0; } else if (fuzzType <= 5) { texX = sin(texCoord.x * 100.0 + u_fuzz_time * 5.0); texY = cos(texCoord.x * 100.0 + u_fuzz_time * 5.0); } else { texX = sin(mod(texCoord.x * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.x / 4.0; texY = cos(mod(texCoord.y * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.y / 4.0; } float fuzz = mod(u_fuzz_time * 2.0 + ((texX / texY) * 21.0 + (texY / texX) * 13.0), 0.5); if (fuzzType != 4 && fuzzType != 5) color.rgb = vec3(0.0); color.a *= fuzz; } if (u_material_colormap) { float gray = dot(color.rgb, vec3(0.3, 0.56, 0.14)); color = vec4(clamp(colormapstart + gray * colormaprange, vec3(0.0), vec3(1.0)), color.a) * v_color; } color.rgb += additive;  if ((u_material_flags & 256) != 0) { frag_color = vec4(u_fog_color.rgb, 1.0 - fog); return; } if (!u_material_colormap && any(notEqual(u_fog_color.rgb, vec3(0.0)))) color.rgb = mix(u_fog_color.rgb, color.rgb, fog); frag_color = color; }\n";
		static const char *fogMaskedFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"in vec2 v_glow_distance;\n"
			"in float v_camera_distance;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float desaturation_factor;\n"
			"uniform bool u_material_colormap;\n"
			"uniform vec3 colormapstart;\n"
			"uniform vec3 colormaprange;\n"
			"vec4 desaturate(vec4 value) { float gray = dot(value.rgb, vec3(0.3, 0.56, 0.14)); return mix(vec4(gray, gray, gray, value.a), value, desaturation_factor); }\n"
			"uniform vec4 u_fog_color;\n"
			"uniform float u_fog_density;\n"
			"uniform vec3 u_camera_position;\n"
			"uniform float u_alpha_cutoff;\n"
			"uniform highp int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform highp sampler2D u_light_data;\n"
			"uniform highp int u_light_offset;\n"
			"uniform highp ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"uniform vec4 u_static_light;\n"
			"uniform vec3 u_sprite_light;\n"
			"uniform bool u_sky_depth;\n"
			"uniform highp int u_material_effect;\n"
			"uniform float u_material_time;\n"
			"vec2 material_uv(vec2 coords) { const float pi = 3.14159265358979323846; vec2 offset = vec2(0.0); if (u_material_effect == 1) { offset.y = sin(pi * 2.0 * (coords.x + u_material_time * 0.125)) * 0.1; offset.x = sin(pi * 2.0 * (coords.y + u_material_time * 0.125)) * 0.1; } else if (u_material_effect == 2) { float siny = sin(pi * 2.0 * (coords.y * 2.2 + u_material_time * 0.75)) * 0.03; offset.y = siny + sin(pi * 2.0 * (coords.x * 0.75 + u_material_time * 0.75)) * 0.03; offset.x = siny + sin(pi * 2.0 * (coords.x * 1.1 + u_material_time * 0.45)) * 0.02; } return coords + offset; }\n"
			"vec3 apply_static_light(vec3 color, float fogDistance) { if (u_sky_depth) return color; if (u_static_light.w > 0.0) { float L = u_static_light.x * 63.0 / 31.0; float minL = clamp(36.0 / 31.0 - L, 0.0, 1.0); float scale = 1.0 / max(gl_FragCoord.z, 0.0001); float index = (59.0 / 31.0 - L) - (scale * 232.0 / 31.0 - 232.0 / 31.0); float light = 1.0 - clamp(index, minL, 1.0); color *= clamp(vec3(light) + u_sprite_light, vec3(0.0), vec3(1.0)); } else if (u_static_light.z > 0.0 && fogDistance < u_static_light.z) color *= u_static_light.y - (fogDistance / u_static_light.z) * (u_static_light.y - 1.0); return color; }\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if ((u_material_flags & 65536) == 0) return color; if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += desaturate(u_glow_top_color * (1.0 - v_glow_distance.x / u_glow_top_color.a)).rgb; if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += desaturate(u_glow_bottom_color * (1.0 - v_glow_distance.y / u_glow_bottom_color.a)).rgb; return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (highp int i = 0; i < u_light_counts.z; ++i) { highp int index = (u_light_offset + i) * 2; highp int width = textureSize(u_light_data, 0).x; vec4 lightPosition = texelFetch(u_light_data, ivec2(index % width, index / width), 0); index += 1; vec3 lightColor = texelFetch(u_light_data, ivec2(index % width, index / width), 0).rgb; vec3 delta = v_world_position - lightPosition.xyz; float radius = max(lightPosition.w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = lightColor * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = u_light_counts.z > 0 ? clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)) : base; }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec2 sampleUV = v_uv; if ((u_material_flags & 8) != 0 && ((u_material_flags >> 10) & 7) == 5) sampleUV += vec2(mod(sin(6.28318530718 * (v_uv.y + u_fuzz_time * 2.0)), 0.1), mod(cos(6.28318530718 * (v_uv.x + u_fuzz_time * 2.0)), 0.1)) * 0.1; vec4 texel = u_use_texture ? texture(u_texture, material_uv(sampleUV)) : vec4(1.0); if ((u_material_flags & 4) != 0) texel.a = 1.0; if ((u_material_flags & 2) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel = desaturate(texel); if ((u_material_flags & 1) != 0 ? texel.a <= 0.0 : texel.a <= 0.0 || texel.a < u_alpha_cutoff) discard; float fogDistance = (u_material_flags & 8192) != 0 ? max(16.0, distance(v_world_position, u_camera_position)) : v_camera_distance; float fog = clamp(exp(-u_fog_density * fogDistance), 0.0, 1.0); if ((u_material_flags & 262144) != 0) { frag_color = vec4(u_fog_color.rgb, (1.0 - fog) * texel.a * 0.75 * v_color.a); return; } vec3 lighting; vec3 additive; vec3 baseLighting = apply_static_light(v_color.rgb, fogDistance); if (all(equal(u_fog_color.rgb, vec3(0.0)))) baseLighting *= fog; apply_dynamic_lights(apply_glow(baseLighting), lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); if (u_material_colormap) { lighting = vec3(1.0); additive = vec3(0.0); } vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a)); if ((u_material_flags & 1) != 0) { color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); color.rgb = lighting; } if ((u_material_flags & 16) != 0) { color.rgb = u_material_colormap ? vec3(1.0) : v_color.rgb; color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); } if ((u_material_flags & 8) != 0) { int fuzzType = (u_material_flags >> 10) & 7; vec2 texCoord = v_uv; if (fuzzType == 1 || fuzzType == 6) texCoord = trunc(texCoord * 128.0) / 128.0; float texX; float texY; if (fuzzType <= 2) { texX = texCoord.x / 3.0 + 0.66; texY = 0.34 - texCoord.y / 3.0; } else if (fuzzType <= 5) { texX = sin(texCoord.x * 100.0 + u_fuzz_time * 5.0); texY = cos(texCoord.x * 100.0 + u_fuzz_time * 5.0); } else { texX = sin(mod(texCoord.x * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.x / 4.0; texY = cos(mod(texCoord.y * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.y / 4.0; } float fuzz = mod(u_fuzz_time * 2.0 + ((texX / texY) * 21.0 + (texY / texX) * 13.0), 0.5); if (fuzzType != 4 && fuzzType != 5) color.rgb = vec3(0.0); color.a *= fuzz; } if (u_material_colormap) { float gray = dot(color.rgb, vec3(0.3, 0.56, 0.14)); color = vec4(clamp(colormapstart + gray * colormaprange, vec3(0.0), vec3(1.0)), color.a) * v_color; } color.rgb += additive; if (!u_material_colormap && any(notEqual(u_fog_color.rgb, vec3(0.0)))) color.rgb = mix(u_fog_color.rgb, color.rgb, fog); frag_color = color; }\n";
		static const char *paletteFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"in vec2 v_glow_distance;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float desaturation_factor;\n"
			"uniform bool u_material_colormap;\n"
			"uniform vec3 colormapstart;\n"
			"uniform vec3 colormaprange;\n"
			"vec4 desaturate(vec4 value) { float gray = dot(value.rgb, vec3(0.3, 0.56, 0.14)); return mix(vec4(gray, gray, gray, value.a), value, desaturation_factor); }\n"
			"uniform highp int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform highp sampler2D u_light_data;\n"
			"uniform highp int u_light_offset;\n"
			"uniform highp ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"uniform vec4 u_static_light;\n"
			"uniform vec3 u_sprite_light;\n"
			"uniform bool u_sky_depth;\n"
			"uniform highp int u_material_effect;\n"
			"uniform float u_material_time;\n"
			"vec2 material_uv(vec2 coords) { const float pi = 3.14159265358979323846; vec2 offset = vec2(0.0); if (u_material_effect == 1) { offset.y = sin(pi * 2.0 * (coords.x + u_material_time * 0.125)) * 0.1; offset.x = sin(pi * 2.0 * (coords.y + u_material_time * 0.125)) * 0.1; } else if (u_material_effect == 2) { float siny = sin(pi * 2.0 * (coords.y * 2.2 + u_material_time * 0.75)) * 0.03; offset.y = siny + sin(pi * 2.0 * (coords.x * 0.75 + u_material_time * 0.75)) * 0.03; offset.x = siny + sin(pi * 2.0 * (coords.x * 1.1 + u_material_time * 0.45)) * 0.02; } return coords + offset; }\n"
			"vec3 apply_static_light(vec3 color, float fogDistance) { if (u_sky_depth) return color; if (u_static_light.w > 0.0) { float L = u_static_light.x * 63.0 / 31.0; float minL = clamp(36.0 / 31.0 - L, 0.0, 1.0); float scale = 1.0 / max(gl_FragCoord.z, 0.0001); float index = (59.0 / 31.0 - L) - (scale * 232.0 / 31.0 - 232.0 / 31.0); float light = 1.0 - clamp(index, minL, 1.0); color *= clamp(vec3(light) + u_sprite_light, vec3(0.0), vec3(1.0)); } else if (u_static_light.z > 0.0 && fogDistance < u_static_light.z) color *= u_static_light.y - (fogDistance / u_static_light.z) * (u_static_light.y - 1.0); return color; }\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if ((u_material_flags & 65536) == 0) return color; if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += desaturate(u_glow_top_color * (1.0 - v_glow_distance.x / u_glow_top_color.a)).rgb; if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += desaturate(u_glow_bottom_color * (1.0 - v_glow_distance.y / u_glow_bottom_color.a)).rgb; return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (highp int i = 0; i < u_light_counts.z; ++i) { highp int index = (u_light_offset + i) * 2; highp int width = textureSize(u_light_data, 0).x; vec4 lightPosition = texelFetch(u_light_data, ivec2(index % width, index / width), 0); index += 1; vec3 lightColor = texelFetch(u_light_data, ivec2(index % width, index / width), 0).rgb; vec3 delta = v_world_position - lightPosition.xyz; float radius = max(lightPosition.w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = lightColor * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = u_light_counts.z > 0 ? clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)) : base; }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec2 sampleUV = v_uv; if ((u_material_flags & 8) != 0 && ((u_material_flags >> 10) & 7) == 5) sampleUV += vec2(mod(sin(6.28318530718 * (v_uv.y + u_fuzz_time * 2.0)), 0.1), mod(cos(6.28318530718 * (v_uv.x + u_fuzz_time * 2.0)), 0.1)) * 0.1; vec4 texel = u_use_texture ? texture(u_texture, material_uv(sampleUV)) : vec4(1.0); if ((u_material_flags & 4) != 0) texel.a = 1.0; if ((u_material_flags & 2) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel = desaturate(texel); texel.rgb = clamp(texel.rgb, vec3(0.0), vec3(1.0)); vec3 lighting; vec3 additive; apply_dynamic_lights(apply_glow(apply_static_light(v_color.rgb, 0.0)), lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); if (u_material_colormap) { lighting = vec3(1.0); additive = vec3(0.0); } vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * (u_material_colormap ? 1.0 : v_color.a)); if ((u_material_flags & 1) != 0) { color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); color.rgb = lighting; } if ((u_material_flags & 16) != 0) { color.rgb = u_material_colormap ? vec3(1.0) : v_color.rgb; color.a = texel.a * (u_material_colormap ? 1.0 : v_color.a); } if ((u_material_flags & 8) != 0) { int fuzzType = (u_material_flags >> 10) & 7; vec2 texCoord = v_uv; if (fuzzType == 1 || fuzzType == 6) texCoord = trunc(texCoord * 128.0) / 128.0; float texX; float texY; if (fuzzType <= 2) { texX = texCoord.x / 3.0 + 0.66; texY = 0.34 - texCoord.y / 3.0; } else if (fuzzType <= 5) { texX = sin(texCoord.x * 100.0 + u_fuzz_time * 5.0); texY = cos(texCoord.x * 100.0 + u_fuzz_time * 5.0); } else { texX = sin(mod(texCoord.x * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.x / 4.0; texY = cos(mod(texCoord.y * 100.0 + u_fuzz_time * 5.0, 3.489)) + texCoord.y / 4.0; } float fuzz = mod(u_fuzz_time * 2.0 + ((texX / texY) * 21.0 + (texY / texX) * 13.0), 0.5); if (fuzzType != 4 && fuzzType != 5) color.rgb = vec3(0.0); color.a *= fuzz; } if (u_material_colormap) { float gray = dot(color.rgb, vec3(0.3, 0.56, 0.14)); color = vec4(clamp(colormapstart + gray * colormaprange, vec3(0.0), vec3(1.0)), color.a) * v_color; } color.rgb += additive;  frag_color = color; }\n";
		NativeMaterialVertexSource = sceneVertexSource;
		const char *materialSources[] = { sceneFragmentSource, maskedFragmentSource, paletteFragmentSource, fogFragmentSource, fogMaskedFragmentSource };
		for (int i = 0; i < 5; ++i) NativeMaterialFragmentSources[i] = materialSources[i];
		Resources.sceneProgram = LinkProgram(sceneVertexSource, sceneFragmentSource, "opaque scene");
		Resources.simpleProgram = LinkProgram(sceneVertexSource, simpleFragmentSource, "simple opaque scene");
		Resources.maskedProgram = LinkProgram(sceneVertexSource, maskedFragmentSource, "masked scene");
		Resources.fogProgram = LinkProgram(sceneVertexSource, fogFragmentSource, "fogged scene");
		Resources.fogMaskedProgram = LinkProgram(sceneVertexSource, fogMaskedFragmentSource, "fogged masked scene");
		const char *paletteSource = gl_gles_shader_test_failure ?
			"#version 320 es\nthis is an intentional shader test failure\n" : paletteFragmentSource;
		Resources.paletteProgram = LinkProgram(sceneVertexSource, paletteSource, "paletted/translated scene", "MATERIAL_PALETTE");
		if (!gl_GLESInternalPresentInitialize() || !gl_GLESInternalPortalInitialize()) return false;
		Resources.sceneTextureUniform = glGetUniformLocation(Resources.sceneProgram, "u_texture");
		Resources.sceneBrightmapUniform = glGetUniformLocation(Resources.sceneProgram, "u_brightmap");
		Resources.sceneUseBrightmap = glGetUniformLocation(Resources.sceneProgram, "u_use_brightmap");
		Resources.sceneBrightmapDesaturation = glGetUniformLocation(Resources.sceneProgram, "u_brightmap_desaturation");
		Resources.sceneViewProjection = glGetUniformLocation(Resources.sceneProgram, "u_view_projection");
		Resources.sceneUseTexture = glGetUniformLocation(Resources.sceneProgram, "u_use_texture");
		Resources.sceneModel = glGetUniformLocation(Resources.sceneProgram, "u_model");
		Resources.sceneTextureTransform = glGetUniformLocation(Resources.sceneProgram, "u_texture_transform");
		Resources.sceneCameraPosition = glGetUniformLocation(Resources.sceneProgram, "u_camera_position");
		Resources.sceneObjectColor = glGetUniformLocation(Resources.sceneProgram, "u_object_color");
		Resources.sceneSkyDepth = glGetUniformLocation(Resources.sceneProgram, "u_sky_depth");
		Resources.sceneSkyFog = glGetUniformLocation(Resources.sceneProgram, "u_sky_fog");
		Resources.sceneMaterialFlags = glGetUniformLocation(Resources.sceneProgram, "u_material_flags");
		Resources.sceneLightCounts = glGetUniformLocation(Resources.sceneProgram, "u_light_counts");
		Resources.sceneProjectedLights = glGetUniformLocation(Resources.sceneProgram, "u_projected_lights");
		Resources.sceneClipPlane = glGetUniformLocation(Resources.sceneProgram, "u_clip_plane");
		Resources.sceneClipPlaneEnabled = glGetUniformLocation(Resources.sceneProgram, "u_clip_plane_enabled");
		Resources.fogColor = glGetUniformLocation(Resources.fogProgram, "u_fog_color");
		Resources.fogDensity = glGetUniformLocation(Resources.fogProgram, "u_fog_density");

		static const FBootstrapVertex vertices[] =
		{
			{ -1.0f, -1.0f, 0.70f, 0.0f, 0.0f },
			{  1.0f, -1.0f, 0.70f, 1.0f, 0.0f },
			{  1.0f,  1.0f, 0.70f, 1.0f, 1.0f },
			{ -1.0f,  1.0f, 0.70f, 0.0f, 1.0f },
			{ -0.62f, -0.48f, 0.20f, 0.0f, 0.0f },
			{  0.62f, -0.48f, 0.20f, 2.0f, 0.0f },
			{  0.62f,  0.48f, 0.20f, 2.0f, 2.0f },
			{ -0.62f,  0.48f, 0.20f, 0.0f, 2.0f }
		};
		static const GLushort indices[] =
		{
			0, 1, 2, 2, 3, 0,
			4, 5, 6, 6, 7, 4
		};
		glGenVertexArrays(1, &Resources.vertexArray);
		glGenBuffers(1, &Resources.vertexBuffer);
		glGenBuffers(1, &Resources.indexBuffer);
		glBindVertexArray(Resources.vertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.vertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.indexBuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FBootstrapVertex), reinterpret_cast<const void *>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FBootstrapVertex), reinterpret_cast<const void *>(3 * sizeof(GLfloat)));
		glBindVertexArray(0);
		glGenVertexArrays(1, &Resources.sceneVertexArray);
		glGenBuffers(1, &Resources.sceneVertexBuffer);
		glGenBuffers(1, &Resources.sceneIndexBuffer);
		glBindVertexArray(Resources.sceneVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.sceneVertexBuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.sceneIndexBuffer);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(6 * sizeof(GLfloat)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(8 * sizeof(GLfloat)));
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(3 * sizeof(GLfloat)));
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(12 * sizeof(GLfloat)));
		glBindVertexArray(0);
		BuildCheckerTexture();
		ConfigureNativeSamplers();
		if (!BuildFramebuffer(width, height)) return false;
		Resources.viewProjection[0] = 1.0f;
		Resources.viewProjection[5] = 1.0f;
		Resources.viewProjection[10] = 1.0f;
		Resources.viewProjection[15] = 1.0f;
		gl_GLESInternalResetState(width, height);
		Resources.ready = true;
		CheckError("native GLES resource setup");
		return Resources.ready;
}

static bool InitializeResources(int width, int height, bool preserveTextureCache)
{
	if (!CapabilitiesReady || width <= 0 || height <= 0) return false;
	DeleteResources(!preserveTextureCache);
	NativeBackendEnabled = true;
	Resources.frame = 0;
	gl_GLESInternalPortalBeginFrame();
	if (!BuildResources(width, height))
	{
		DeleteResources(!preserveTextureCache);
		I_FatalError("Zandronum GLES resources could not be created.");
	}
	return true;
}
}

unsigned int gl_GLES_GetShaderProgram(const char *name)
{
	if (name == NULL || !Resources.ready) return 0;
	if (strcmp(name, "gles/opaque") == 0) return Resources.sceneProgram;
	if (strcmp(name, "gles/simple-opaque") == 0) return Resources.simpleProgram;
	if (strcmp(name, "gles/masked") == 0) return Resources.maskedProgram;
	if (strcmp(name, "gles/fog") == 0) return Resources.fogProgram;
	if (strcmp(name, "gles/fog-masked") == 0) return Resources.fogMaskedProgram;
	if (strcmp(name, "gles/palette") == 0) return Resources.paletteProgram;
	if (strcmp(name, "gles/present") == 0) return gl_GLESInternalPresentGetProgram();
	return 0;
}

bool gl_GLES_IsProfileEnabled()
{
	return gl_gles_profile || developer;
}

void gl_GLES_UseProgram(unsigned int handle)
{
	const GLuint program = static_cast<GLuint>(handle);
	if (NativeProgramBindingKnown && NativeBoundProgram == program)
	{
		++Resources.sceneProgramBindSkips;
		return;
	}
	glUseProgram(program);
	NativeBoundProgram = program;
	NativeProgramBindingKnown = true;
	++Resources.sceneProgramBinds;
}

void gl_GLESInternalInvalidateProgramBinding()
{
	NativeProgramBindingKnown = false;
	NativeBoundProgram = 0;
}

bool gl_GLES_CollectCapabilities()
{
	FGLESContextInfo context = {};
	#ifdef __ANDROID__
	if (!gl_GLES_InstallDirectContext(3, 2, &context))
		I_FatalError("Zandronum GLES 3.2 context is unavailable.");
	#else
	if (!gl_GLES_HasContext())
	{
		gl_GLES_Report("capabilities", "desktop context was not loaded before renderer initialization");
		return false;
	}
	context = gl_GLES_GetContextInfo();
	if (context.isGLES || context.majorVersion < 3 ||
		(context.majorVersion == 3 && context.minorVersion < 3))
	{
		gl_GLES_Report("capabilities", "desktop GLES backend requires an OpenGL 3.3 core context");
		return false;
	}
	#endif
	Capabilities.majorVersion = context.majorVersion;
	Capabilities.minorVersion = context.minorVersion;
	Capabilities.vendor = context.vendor;
	Capabilities.renderer = context.renderer;
	Capabilities.version = context.version;
	Capabilities.shadingLanguageVersion = context.shadingLanguageVersion;
	Capabilities.hasVertexBuffers = true;
	Capabilities.hasVertexArrays = true;
	Capabilities.hasUniformBuffers = true;
	Capabilities.hasFramebuffers = true;
	Capabilities.hasDepthStencil = context.hasDepthStencil;
	Capabilities.maxTextureSize = context.maxTextureSize;
	Capabilities.maxTextureUnits = context.maxTextureUnits;
	Capabilities.maxVertexUniformVectors = context.maxVertexUniformVectors;
	Capabilities.maxFragmentUniformVectors = context.maxFragmentUniformVectors;
	Capabilities.extensionCount = context.extensionCount;
	if (Capabilities.maxTextureSize <= 0 || Capabilities.maxTextureUnits <= 0 ||
		Capabilities.maxVertexUniformVectors <= 0 || Capabilities.maxFragmentUniformVectors <= 0 ||
		Capabilities.extensionCount < 0)
		I_FatalError("Zandronum GLES capability enumeration returned invalid limits.");
	Capabilities.hasAnisotropicFiltering = context.hasAnisotropicFiltering;
	Capabilities.hasAstcCompression = gl_GLES_HasExtension("GL_KHR_texture_compression_astc_ldr");
	Capabilities.hasEtc2Compression = context.isGLES;
	Capabilities.hasDebugLabels = context.hasDebugLabels;
	Capabilities.hasMultiview = context.hasMultiview;
	CapabilitiesReady = true;
	return true;
}

bool gl_GLES_InitializeBootstrap(int width, int height)
{
	return InitializeResources(width, height, false);
}

void gl_GLES_BeginScene(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, float fieldOfView, float aspect, float fovRatio)
{
	if (!gl_GLES_CanUseResources()) return;
	Profile = {};
	Profile.active = gl_gles_profile || developer;
	Profile.cameraTargetCreates = PendingCameraTargetCreates;
	Profile.cameraCopies = PendingCameraCopies;
	PendingCameraTargetCreates = 0;
	PendingCameraCopies = 0;
	gl_GLESInternalInvalidateStateCache();
	if (developer || Profile.active)
	{
		NativeFrameStart = std::chrono::steady_clock::now();
		Profile.frameStart = NativeFrameStart;
	}
	if (developer && !SceneInputLogged)
	{
		SceneInputLogged = true;
		DPrintf("Zandronum GLES scene input: %d segs, %d subsectors, %d vertices.\n", numsegs, numsubsectors, numvertexes);
	}
	Resources.nativeViewArea = {};
	if (screen != NULL)
	{
		Resources.nativeViewArea = gl_GLESInternalComputeViewArea(screen->GetWidth(),
			screen->GetHeight(), screen->GetTrueHeight(), screenblocks,
			viewwindowx, viewwindowy, viewwidth, viewheight);
		Resources.viewContract.viewportX = viewwindowx;
		Resources.viewContract.viewportY = viewwindowy;
		Resources.viewContract.viewportWidth = viewwidth;
		Resources.viewContract.viewportHeight = viewheight;
	}
	if (developer && Resources.frame == 0 && Resources.nativeViewArea.valid)
	{
		const FGLESViewArea &area = Resources.nativeViewArea;
		DPrintf("Zandronum GLES view: screenblocks=%d view=(%d,%d %dx%d) viewport=(%d,%d %dx%d) scissor=(%d,%d %dx%d).\n",
			static_cast<int>(screenblocks), area.x, area.y, area.width, area.height,
			area.renderX, area.renderY, area.renderWidth, area.renderHeight,
			area.scissorX, area.scissorY, area.scissorWidth, area.scissorHeight);
	}
	BuildViewProjection(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch, cameraRoll, fieldOfView, aspect, fovRatio);
	Resources.sceneVertices.clear();
	Resources.sceneIndices.clear();
	Resources.sceneShortIndices.clear();
	Resources.sceneIndexType = GL_UNSIGNED_INT;
	Resources.sceneIndexStride = sizeof(GLuint);
	gl_GLESInternalSceneClearLights();
	Resources.sceneIndexCount = 0;
	Resources.sceneReady = false;
	Resources.sceneViewSnapshots.clear();
	Resources.sceneBatches.clear();
	Resources.portalTargets.clear();
	NativePortalCaptureStack.clear();
	gl_GLESInternalPortalBeginFrame();
	NativeProgramBindingKnown = false;
	ResetNativeSkyRecord(Resources.outerSky, -1, 1u);
	Resources.sceneWallCount = 0;
	Resources.sceneFlatCount = 0;
	Resources.sceneSpriteCount = 0;
	Resources.sceneBatchSubmissions = 0;
	Resources.sceneOrderHash = SceneOrderHashOffset;
	Resources.sceneOpaqueBatchMerges = 0;
	Resources.sceneProgramBinds = 0;
	Resources.sceneProgramBindSkips = 0;
	Resources.sceneTextureBinds = 0;
	Resources.sceneTextureBindSkips = 0;
	Resources.sceneActiveTextureSets = 0;
	Resources.sceneActiveTextureSkips = 0;
	Resources.sceneSamplerBinds = 0;
	Resources.sceneSamplerBindSkips = 0;
	Resources.sceneOpaqueStateSets = 0;
	Resources.sceneOpaqueStateSkips = 0;
	Resources.sceneLightUniformUploads = 0;
	Resources.sceneLightUniformUploadSkips = 0;
	Resources.sceneGlowUniformUploads = 0;
	Resources.sceneGlowUniformUploadSkips = 0;
	Resources.skyMaterial = NULL;
	Resources.skyXOffset = 0.0f;
	Resources.skyYOffset = 0.0f;
	Resources.skyLayerMaterial = NULL;
	Resources.skyLayerXOffset = 0.0f;
	Resources.skyLayerYOffset = 0.0f;
	Resources.sky2 = false;
	Resources.skyMirrored = false;
	Resources.skyLayerMirrored = false;
	Resources.skyUpperCapColor = 0;
	Resources.skyLowerCapColor = 0;
	Resources.skyFogColor = 0;
	Resources.skyFogEnabled = false;
	Resources.clipPlaneEnabled = false;
	memset(Resources.clipPlane, 0, sizeof(Resources.clipPlane));
}

void gl_GLES_ClearScene()
{
	if (!gl_GLES_CanUseResources()) return;
	NativeWipeOverlayCollecting = false;
	FlatCollectionDeferred = false;
	Resources.sceneVertices.clear();
	Resources.sceneIndices.clear();
	Resources.sceneShortIndices.clear();
	Resources.sceneIndexType = GL_UNSIGNED_INT;
	Resources.sceneIndexStride = sizeof(GLuint);
	Resources.sceneViewSnapshots.clear();
	Resources.sceneBatches.clear();
	gl_GLESInternalSceneClearLights();
	Resources.portalTargets.clear();
	NativePortalCaptureStack.clear();
	gl_GLESInternalPortalBeginFrame();
	ResetNativeSkyRecord(Resources.outerSky, -1, 1u);
	Resources.sceneIndexCount = 0;
	Resources.sceneReady = false;
	NativeProgramBindingKnown = false;
	Resources.sceneWallCount = 0;
	Resources.sceneFlatCount = 0;
	Resources.sceneSpriteCount = 0;
	Resources.sceneBatchSubmissions = 0;
	Resources.sceneOrderHash = SceneOrderHashOffset;
	Resources.sceneOpaqueBatchMerges = 0;
	Resources.sceneProgramBinds = 0;
	Resources.sceneProgramBindSkips = 0;
	Resources.sceneTextureBinds = 0;
	Resources.sceneTextureBindSkips = 0;
	Resources.sceneActiveTextureSets = 0;
	Resources.sceneActiveTextureSkips = 0;
	Resources.sceneSamplerBinds = 0;
	Resources.sceneSamplerBindSkips = 0;
	Resources.sceneOpaqueStateSets = 0;
	Resources.sceneOpaqueStateSkips = 0;
	Resources.sceneLightUniformUploads = 0;
	Resources.sceneLightUniformUploadSkips = 0;
	Resources.sceneGlowUniformUploads = 0;
	Resources.sceneGlowUniformUploadSkips = 0;
	Resources.skyMaterial = NULL;
	Resources.skyXOffset = 0.0f;
	Resources.skyYOffset = 0.0f;
	Resources.skyLayerMaterial = NULL;
	Resources.skyLayerXOffset = 0.0f;
	Resources.skyLayerYOffset = 0.0f;
	Resources.sky2 = false;
	Resources.skyMirrored = false;
	Resources.skyLayerMirrored = false;
	Resources.skyUpperCapColor = 0;
	Resources.skyLowerCapColor = 0;
	Resources.skyFogColor = 0;
	Resources.skyFogEnabled = false;
	Resources.clipPlaneEnabled = false;
	memset(Resources.clipPlane, 0, sizeof(Resources.clipPlane));
}

void gl_GLES_SetFlatCollectionDeferred(bool deferred)
{
	FlatCollectionDeferred = deferred;
}

bool gl_GLES_IsFlatCollectionDeferred()
{
	return FlatCollectionDeferred;
}

void gl_GLES_SetSky(FMaterial *material, float xOffset, float yOffset, bool mirrored,
	bool sky2, PalEntry fadeColor)
{
	if (!gl_GLES_CanUseResources() || material == NULL || Resources.skyMaterial != NULL) return;
	Resources.skyMaterial = material;
	Resources.skyXOffset = xOffset;
	Resources.skyYOffset = yOffset;
	Resources.skyMirrored = mirrored;
	Resources.sky2 = sky2;
	Resources.skyUpperCapColor = material->tex->GetSkyCapColor(false);
	Resources.skyLowerCapColor = material->tex->GetSkyCapColor(true);
	if (gl_fixedcolormap)
	{
		const int colormap = gl_fixedcolormap < CM_FIRSTSPECIALCOLORMAP + SpecialColormaps.Size() ?
			gl_fixedcolormap : CM_DEFAULT;
		if (colormap != CM_DEFAULT)
		{
			ModifyPalette(&Resources.skyUpperCapColor, &Resources.skyUpperCapColor, colormap, 1);
			ModifyPalette(&Resources.skyLowerCapColor, &Resources.skyLowerCapColor, colormap, 1);
		}
		float red, green, blue;
		gl_GetLightColor(255, 0, NULL, &red, &green, &blue);
		Resources.skyUpperCapColor.r = static_cast<unsigned char>(Resources.skyUpperCapColor.r * red);
		Resources.skyUpperCapColor.g = static_cast<unsigned char>(Resources.skyUpperCapColor.g * green);
		Resources.skyUpperCapColor.b = static_cast<unsigned char>(Resources.skyUpperCapColor.b * blue);
		Resources.skyLowerCapColor.r = static_cast<unsigned char>(Resources.skyLowerCapColor.r * red);
		Resources.skyLowerCapColor.g = static_cast<unsigned char>(Resources.skyLowerCapColor.g * green);
		Resources.skyLowerCapColor.b = static_cast<unsigned char>(Resources.skyLowerCapColor.b * blue);
	}
	Resources.skyFogColor = fadeColor;
	Resources.skyFogEnabled = !gl_fixedcolormap && skyfog > 0 &&
		(fadeColor.r != 0 || fadeColor.g != 0 || fadeColor.b != 0);
	if (developer && !SkyLogged)
	{
		SkyLogged = true;
		DPrintf("Zandronum GLES basic sky material enabled (offset %.2f, %.2f).\n", xOffset, yOffset);
	}
}

void gl_GLES_SetSkyLayer(FMaterial *material, float xOffset, float yOffset, bool mirrored)
{
	if (!gl_GLES_CanUseResources() || material == NULL || Resources.skyLayerMaterial != NULL) return;
	Resources.skyLayerMaterial = material;
	Resources.skyLayerXOffset = xOffset;
	Resources.skyLayerYOffset = yOffset;
	Resources.skyLayerMirrored = mirrored;
}

void gl_GLES_AddSkyMask(const float *positions)
{
	if (!gl_GLES_CanUseResources() || positions == NULL) return;
	FNativeSkyRecord *sky = ActiveNativeSkyRecord();
	if (sky == NULL) return;
	if (!CanAppendSceneGeometry(4, 6, "sky mask"))
	{
		++sky->rejected;
		return;
	}
	++sky->submitted;
	for (int i = 0; i < 4; ++i)
	{
		const float x = positions[i * 3 + 0];
		const float y = positions[i * 3 + 1];
		const float z = positions[i * 3 + 2];
		const float clip[4] =
		{
			Resources.viewProjection[0] * x + Resources.viewProjection[4] * y +
				Resources.viewProjection[8] * z + Resources.viewProjection[12],
			Resources.viewProjection[1] * x + Resources.viewProjection[5] * y +
				Resources.viewProjection[9] * z + Resources.viewProjection[13],
			Resources.viewProjection[2] * x + Resources.viewProjection[6] * y +
				Resources.viewProjection[10] * z + Resources.viewProjection[14],
			Resources.viewProjection[3] * x + Resources.viewProjection[7] * y +
				Resources.viewProjection[11] * z + Resources.viewProjection[15]
		};
		for (int axis = 0; axis < 4; ++axis)
		{
			sky->clipMin[axis] = std::min(sky->clipMin[axis], clip[axis]);
			sky->clipMax[axis] = std::max(sky->clipMax[axis], clip[axis]);
		}
	}
	const unsigned int firstVertex = static_cast<unsigned int>(Resources.sceneVertices.size());
	for (int i = 0; i < 4; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = positions[i * 3 + 0];
		vertex.y = positions[i * 3 + 1];
		vertex.z = positions[i * 3 + 2];
		vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
		if (AddSceneVertex(vertex) == 0xffffffffu)
		{
			++sky->rejected;
			return;
		}
	}
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	Resources.sceneIndices.push_back(firstVertex);
	Resources.sceneIndices.push_back(firstVertex + 1);
	Resources.sceneIndices.push_back(firstVertex + 2);
	Resources.sceneIndices.push_back(firstVertex);
	Resources.sceneIndices.push_back(firstVertex + 2);
	Resources.sceneIndices.push_back(firstVertex + 3);
	FSceneBatch batch = {};
	CaptureBatchView(batch);
	batch.firstIndex = firstIndex;
	batch.indexCount = static_cast<GLsizei>(Resources.sceneIndices.size()) - firstIndex;
	batch.skyMask = true;
	RecordSceneBatchSubmission(batch);
	PushNativeSceneBatch(batch);
	sky->maskBatches.push_back(Resources.sceneBatches.size() - 1);
	if (!sky->viewValid)
	{
		const FSceneViewSnapshot &view = BatchView(batch);
		memcpy(sky->viewProjection, view.viewProjection, sizeof(sky->viewProjection));
		memcpy(sky->cameraPosition, view.cameraPosition, sizeof(sky->cameraPosition));
		sky->viewValid = true;
	}
	// The source portal adds caps when its owning wall list has multiple lines.
	// One sky record is built per owner, so the second wall is sufficient here.
	if (sky->portalId < 0 && sky->maskBatches.size() >= 2)
	{
		sky->capEligible = true;
	}
}

void gl_GLES_AddWall(const float *positions, const float *texcoords,
	const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	const float *lightData, const unsigned int *lightCounts, unsigned int brightmap, int brightmapDesaturation,
	const float *topGlowColor, const float *bottomGlowColor, const float *glowDistances,
	const FShaderLightParameters *lighting, float alphaCutoff, bool customBlend,
	int sourceBlend, int destinationBlend)
{
	if (!gl_GLES_CanUseResources() || positions == NULL) return;
	FScopedProfileTimer timer(Profile.wallCollectionMilliseconds);
	++Resources.sceneWallCount;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	FSceneVertex vertices[4];
	const bool sphereMap = (materialFlags & GLES_MATERIAL_SPHERE_MAP) != 0;
	float sphereNormal[3] = {};
	if (sphereMap)
	{
		const float edgeX = positions[6] - positions[3];
		const float edgeZ = positions[8] - positions[5];
		const float length = sqrtf(edgeX * edgeX + edgeZ * edgeZ);
		if (length > 0.0001f)
		{
			const float normal[3] = { edgeZ / length, 0.0f, -edgeX / length };
			const float *view = Resources.viewContract.viewMatrix;
			for (int row = 0; row < 3; ++row)
				sphereNormal[row] = view[row] * normal[0] + view[4 + row] * normal[1] + view[8 + row] * normal[2];
			const float viewLength = sqrtf(sphereNormal[0] * sphereNormal[0] + sphereNormal[1] * sphereNormal[1] + sphereNormal[2] * sphereNormal[2]);
			if (viewLength > 0.0001f)
			{
				sphereNormal[0] /= viewLength;
				sphereNormal[1] /= viewLength;
				sphereNormal[2] /= viewLength;
			}
		}
	}
	for (int i = 0; i < 4; ++i)
	{
		vertices[i].x = positions[i * 3 + 0];
		vertices[i].y = positions[i * 3 + 1];
		vertices[i].z = positions[i * 3 + 2];
		vertices[i].nx = vertices[i].ny = vertices[i].nz = 0.0f;
		vertices[i].u = texcoords != NULL ? texcoords[i * 2 + 0] : 0.0f;
		vertices[i].v = texcoords != NULL ? texcoords[i * 2 + 1] : 0.0f;
		if (sphereMap)
		{
			const float *view = Resources.viewContract.viewMatrix;
			const float eye[3] =
			{
				view[0] * vertices[i].x + view[4] * vertices[i].y + view[8] * vertices[i].z + view[12],
				view[1] * vertices[i].x + view[5] * vertices[i].y + view[9] * vertices[i].z + view[13],
				view[2] * vertices[i].x + view[6] * vertices[i].y + view[10] * vertices[i].z + view[14]
			};
			const float eyeLength = sqrtf(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
			if (eyeLength > 0.0001f)
			{
				const float dot = (eye[0] * sphereNormal[0] + eye[1] * sphereNormal[1] + eye[2] * sphereNormal[2]) / eyeLength;
				const float reflection[3] =
				{
					eye[0] / eyeLength - 2.0f * dot * sphereNormal[0],
					eye[1] / eyeLength - 2.0f * dot * sphereNormal[1],
					eye[2] / eyeLength - 2.0f * dot * sphereNormal[2]
				};
				const float denominator = 2.0f * sqrtf(reflection[0] * reflection[0] + reflection[1] * reflection[1] +
					(reflection[2] + 1.0f) * (reflection[2] + 1.0f));
				if (denominator > 0.0001f)
				{
					vertices[i].u = reflection[0] / denominator + 0.5f;
					vertices[i].v = reflection[1] / denominator + 0.5f;
				}
			}
		}
		vertices[i].r = rgb[0];
		vertices[i].g = rgb[1];
		vertices[i].b = rgb[2];
		vertices[i].a = ClampUnit(alpha);
		vertices[i].glowTopDistance = glowDistances != NULL ? glowDistances[i * 2 + 0] : 0.0f;
		vertices[i].glowBottomDistance = glowDistances != NULL ? glowDistances[i * 2 + 1] : 0.0f;
	}
	AddSceneQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, masked, fog, alpha < 0.999f, repeat,
		blendMode, fogColor, fogDensity, materialFlags, lightData, lightCounts, brightmap,
		brightmapDesaturation, topGlowColor, bottomGlowColor, customBlend, static_cast<GLenum>(sourceBlend),
		static_cast<GLenum>(destinationBlend), alphaCutoff, false, lighting);
}

void gl_GLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	const float *lightData, const unsigned int *lightCounts, unsigned int brightmap, int brightmapDesaturation,
	const FShaderLightParameters *lighting)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || vertexCount < 3) return;
	FScopedProfileTimer timer(Profile.flatCollectionMilliseconds);
	++Resources.sceneFlatCount;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	if (!CanAppendSceneGeometry(vertexCount, vertexCount >= 3 ? (vertexCount - 2) * 3 : 0, "flat")) return;
	for (unsigned int i = 0; i < vertexCount; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = positions[i * 3 + 0];
		vertex.y = positions[i * 3 + 1];
		vertex.z = positions[i * 3 + 2];
		vertex.nx = vertex.ny = vertex.nz = 0.0f;
		vertex.u = texcoords != NULL ? texcoords[i * 2 + 0] : 0.0f;
		vertex.v = texcoords != NULL ? texcoords[i * 2 + 1] : 0.0f;
		vertex.r = rgb[0];
		vertex.g = rgb[1];
		vertex.b = rgb[2];
		vertex.a = ClampUnit(alpha);
		Resources.sceneVertices.push_back(vertex);
		if (i >= 2)
		{
			FScopedProfileTimer indexTimer(Profile.indexEmissionMilliseconds);
			Resources.sceneIndices.push_back(first);
			Resources.sceneIndices.push_back(first + i - 1);
			Resources.sceneIndices.push_back(first + i);
		}
	}
	const GLsizei indexCount = static_cast<GLsizei>(Resources.sceneIndices.size()) - firstIndex;
	if (indexCount > 0)
	{
		float centerX = 0.0f;
		float centerY = 0.0f;
		float centerZ = 0.0f;
		for (unsigned int vertex = 0; vertex < vertexCount; ++vertex)
		{
			centerX += positions[vertex * 3 + 0];
			centerY += positions[vertex * 3 + 1];
			centerZ += positions[vertex * 3 + 2];
		}
		const float inverseCount = 1.0f / static_cast<float>(vertexCount);
		centerX *= inverseCount;
		centerY *= inverseCount;
		centerZ *= inverseCount;
		const float dx = centerX - Resources.cameraX;
		const float dy = centerY - Resources.cameraY;
		const float dz = centerZ - Resources.cameraZ;
		FSceneBatch batch = {};
		CaptureBatchView(batch);
	if (lighting != NULL) batch.lighting = *lighting;
		batch.firstIndex = firstIndex;
		batch.indexCount = indexCount;
		batch.texture = texture;
		batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
		batch.brightmap = brightmap;
		batch.brightmapDesaturation = brightmapDesaturation;
		batch.masked = masked;
		batch.fog = fog;
		batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
		batch.repeat = repeat;
		const unsigned int textureFlags = gl_GLESInternalGetMaterialFlags(texture);
		batch.cameraTexture = (textureFlags & GLES_TEXTURE_FLAG_FRAMEBUFFER) != 0;
		batch.palette = (textureFlags & GLES_TEXTURE_FLAG_PALETTE) != 0;
		batch.flat = true;
		batch.materialFlags = materialFlags;
		batch.blendMode = blendMode;
		CopyNativeLightData(batch, lightData, lightCounts);
		if (batch.lightCount > 0 && vertexCount >= 3)
		{
			FSceneVertex &firstVertex = Resources.sceneVertices[first];
			FSceneVertex &secondVertex = Resources.sceneVertices[first + 1];
			FSceneVertex &thirdVertex = Resources.sceneVertices[first + 2];
			SetLightPlaneNormal(batch, firstVertex, secondVertex, thirdVertex);
		}
		batch.sortDepth = dx * dx + dy * dy + dz * dz;
		if (fogColor != NULL)
		{
			batch.fogColor[0] = ClampUnit(fogColor[0]);
			batch.fogColor[1] = ClampUnit(fogColor[1]);
			batch.fogColor[2] = ClampUnit(fogColor[2]);
		}
		batch.fogDensity = std::max(0.0f, fogDensity);
		AppendOpaqueBatch(batch);
	}
}

void gl_GLES_AddFloodPlane(const float *wallPositions, const float *planePositions,
	const float *texcoords, const float *color, unsigned int texture, bool fog,
	const float *fogColor, float fogDensity, const FShaderLightParameters *lighting)
{
	if (!gl_GLES_CanUseResources() || wallPositions == NULL || planePositions == NULL ||
		texcoords == NULL || !CanAppendSceneGeometry(8, 12, "flood plane"))
		return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int wallVertex = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei wallIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	for (int i = 0; i < 4; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = wallPositions[i * 3 + 0];
		vertex.y = wallPositions[i * 3 + 1];
		vertex.z = wallPositions[i * 3 + 2];
		vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
		Resources.sceneVertices.push_back(vertex);
	}
	Resources.sceneIndices.push_back(wallVertex + 0);
	Resources.sceneIndices.push_back(wallVertex + 1);
	Resources.sceneIndices.push_back(wallVertex + 2);
	Resources.sceneIndices.push_back(wallVertex + 2);
	Resources.sceneIndices.push_back(wallVertex + 3);
	Resources.sceneIndices.push_back(wallVertex + 0);

	const unsigned int planeVertex = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei planeIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	for (int i = 0; i < 4; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = planePositions[i * 3 + 0];
		vertex.y = planePositions[i * 3 + 1];
		vertex.z = planePositions[i * 3 + 2];
		vertex.u = texcoords[i * 2 + 0];
		vertex.v = texcoords[i * 2 + 1];
		vertex.r = rgb[0];
		vertex.g = rgb[1];
		vertex.b = rgb[2];
		vertex.a = 1.0f;
		Resources.sceneVertices.push_back(vertex);
	}
	Resources.sceneIndices.push_back(planeVertex + 0);
	Resources.sceneIndices.push_back(planeVertex + 1);
	Resources.sceneIndices.push_back(planeVertex + 2);
	Resources.sceneIndices.push_back(planeVertex + 2);
	Resources.sceneIndices.push_back(planeVertex + 3);
	Resources.sceneIndices.push_back(planeVertex + 0);

	const float centerX = (planePositions[0] + planePositions[3] + planePositions[6] + planePositions[9]) * 0.25f;
	const float centerY = (planePositions[1] + planePositions[4] + planePositions[7] + planePositions[10]) * 0.25f;
	const float centerZ = (planePositions[2] + planePositions[5] + planePositions[8] + planePositions[11]) * 0.25f;
	const float dx = centerX - Resources.cameraX;
	const float dy = centerY - Resources.cameraY;
	const float dz = centerZ - Resources.cameraZ;
	FSceneBatch batch = {};
	CaptureBatchView(batch);
	batch.firstIndex = planeIndex;
	batch.indexCount = 6;
	batch.texture = texture;
	batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
	batch.fog = fog;
	batch.repeat = true;
	batch.palette = IsPaletteTexture(texture);
	batch.flood = true;
	if (lighting != NULL) batch.lighting = *lighting;
	batch.floodWallFirstIndex = wallIndex;
	batch.sortDepth = dx * dx + dy * dy + dz * dz;
	if (fogColor != NULL)
	{
		batch.fogColor[0] = ClampUnit(fogColor[0]);
		batch.fogColor[1] = ClampUnit(fogColor[1]);
		batch.fogColor[2] = ClampUnit(fogColor[2]);
	}
	batch.fogDensity = std::max(0.0f, fogDensity);
	RecordSceneBatchSubmission(batch);
	PushNativeSceneBatch(batch);
}

void gl_GLES_AddHUDPrimitive(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EGLESBlendMode blendMode, unsigned int materialFlags, EGLESPrimitiveMode primitiveMode)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || vertexCount == 0) return;
	const bool triangleFan = primitiveMode == GLES_PRIMITIVE_TRIANGLE_FAN;
	if ((triangleFan && vertexCount < 3) ||
		(primitiveMode == GLES_PRIMITIVE_LINES && (vertexCount % 2) != 0)) return;
	const unsigned int requestedIndices = triangleFan ? (vertexCount - 2) * 3 : vertexCount;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	if (!CanAppendSceneGeometry(vertexCount, requestedIndices, "HUD primitive")) return;
	for (unsigned int i = 0; i < vertexCount; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = positions[i * 3 + 0];
		vertex.y = positions[i * 3 + 1];
		vertex.z = positions[i * 3 + 2];
		vertex.nx = vertex.ny = vertex.nz = 0.0f;
		vertex.u = texcoords != NULL ? texcoords[i * 2 + 0] : 0.0f;
		vertex.v = texcoords != NULL ? texcoords[i * 2 + 1] : 0.0f;
		vertex.r = rgb[0];
		vertex.g = rgb[1];
		vertex.b = rgb[2];
		vertex.a = ClampUnit(alpha);
		Resources.sceneVertices.push_back(vertex);
		if (!triangleFan) Resources.sceneIndices.push_back(first + i);
		else if (i >= 2)
		{
			Resources.sceneIndices.push_back(first);
			Resources.sceneIndices.push_back(first + i - 1);
			Resources.sceneIndices.push_back(first + i);
		}
	}
	const GLsizei indexCount = static_cast<GLsizei>(Resources.sceneIndices.size()) - firstIndex;
	if (indexCount <= 0) return;
	FSceneBatch batch = {};
	CaptureBatchView(batch);
	batch.firstIndex = firstIndex;
	batch.indexCount = indexCount;
	batch.primitiveMode = triangleFan ? GL_TRIANGLES :
		primitiveMode == GLES_PRIMITIVE_LINES ? GL_LINES : GL_POINTS;
	batch.texture = texture;
	batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
	batch.masked = masked;
	batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
	batch.repeat = repeat;
	batch.palette = IsPaletteTexture(texture);
	batch.hud = true;
	batch.blendMode = blendMode;
	batch.materialFlags = materialFlags;
	batch.sortDepth = 0.0f;
	RecordSceneBatchSubmission(batch);
	PushNativeSceneBatch(batch);
}

void gl_GLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	unsigned int brightmap, int brightmapDesaturation, bool customBlend,
	int sourceBlend, int destinationBlend, float alphaCutoff,
	const FShaderLightParameters *lighting)
{
	const bool resourcesReady = gl_GLES_CanUseResources();
	if (resourcesReady) ++Resources.sceneSpriteCount;
	if (!resourcesReady || positions == NULL) return;
	FScopedProfileTimer timer(Profile.spriteCollectionMilliseconds);
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	FSceneVertex vertices[4];
	for (int i = 0; i < 4; ++i)
	{
		vertices[i].x = positions[i * 3 + 0];
		vertices[i].y = positions[i * 3 + 1];
		vertices[i].z = positions[i * 3 + 2];
		vertices[i].nx = vertices[i].ny = vertices[i].nz = 0.0f;
		vertices[i].u = texcoords != NULL ? texcoords[i * 2 + 0] : 0.0f;
		vertices[i].v = texcoords != NULL ? texcoords[i * 2 + 1] : 0.0f;
		vertices[i].r = rgb[0];
		vertices[i].g = rgb[1];
		vertices[i].b = rgb[2];
		vertices[i].a = ClampUnit(alpha);
		vertices[i].glowTopDistance = vertices[i].glowBottomDistance = 0.0f;
	}
	const bool fogLayer = fog && fogColor != NULL &&
		(fogColor[0] != 0.0f || fogColor[1] != 0.0f || fogColor[2] != 0.0f) &&
		(blendMode == GLES_BLEND_SUBTRACT || blendMode == GLES_BLEND_REVERSE_SUBTRACT);
	AddSceneQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture,
		fogLayer ? false : masked, fog && !fogLayer,
		alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE, false, blendMode, fogColor, fogDensity, materialFlags, NULL, NULL,
		brightmap, brightmapDesaturation, NULL, NULL, customBlend,
		static_cast<GLenum>(sourceBlend), static_cast<GLenum>(destinationBlend),
		fogLayer ? 0.0f : alphaCutoff, false, lighting);
	if (fogLayer)
	{
		AddSceneQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, false, true,
			true, false, GLES_BLEND_ALPHA, fogColor, fogDensity, GLES_MATERIAL_SPRITE_FOG_LAYER,
			NULL, NULL, 0, 0, NULL, NULL, true, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, 0.0f, false);
	}
}

void gl_GLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	const float *normals, unsigned int brightmap, int brightmapDesaturation, bool cullBackFaces,
	bool customBlend, int sourceBlend, int destinationBlend,
	const FShaderLightParameters *lighting, float alphaCutoff)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || indices == NULL ||
		vertexCount == 0 || indexCount < 3 || (indexCount % 3) != 0) return;
	FScopedProfileTimer timer(Profile.modelCollectionMilliseconds);
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	if (!CanAppendSceneGeometry(vertexCount, indexCount, "model surface")) return;
	for (unsigned int index = 0; index < indexCount; ++index)
		if (indices[index] >= vertexCount) return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	for (unsigned int vertex = 0; vertex < vertexCount; ++vertex)
	{
		FSceneVertex value = {};
		value.x = positions[vertex * 3 + 0];
		value.y = positions[vertex * 3 + 1];
		value.z = positions[vertex * 3 + 2];
		value.nx = normals != NULL ? normals[vertex * 3 + 0] : 0.0f;
		value.ny = normals != NULL ? normals[vertex * 3 + 1] : 0.0f;
		value.nz = normals != NULL ? normals[vertex * 3 + 2] : 0.0f;
		value.u = texcoords != NULL ? texcoords[vertex * 2 + 0] : 0.0f;
		value.v = texcoords != NULL ? texcoords[vertex * 2 + 1] : 0.0f;
		value.r = rgb[0];
		value.g = rgb[1];
		value.b = rgb[2];
		value.a = ClampUnit(alpha);
		Resources.sceneVertices.push_back(value);
	}
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	{
		FScopedProfileTimer indexTimer(Profile.indexEmissionMilliseconds);
		for (unsigned int index = 0; index < indexCount; ++index)
			Resources.sceneIndices.push_back(first + indices[index]);
	}
	float centerX = 0.0f;
	float centerY = 0.0f;
	float centerZ = 0.0f;
	for (unsigned int vertex = 0; vertex < vertexCount; ++vertex)
	{
		centerX += positions[vertex * 3 + 0];
		centerY += positions[vertex * 3 + 1];
		centerZ += positions[vertex * 3 + 2];
	}
	const float inverseCount = 1.0f / static_cast<float>(vertexCount);
	centerX *= inverseCount;
	centerY *= inverseCount;
	centerZ *= inverseCount;
	const float dx = centerX - Resources.cameraX;
	const float dy = centerY - Resources.cameraY;
	const float dz = centerZ - Resources.cameraZ;
	FSceneBatch batch = {};
	CaptureBatchView(batch);
	if (lighting != NULL) batch.lighting = *lighting;
	batch.firstIndex = firstIndex;
	batch.indexCount = static_cast<GLsizei>(indexCount);
	batch.texture = texture;
	batch.materialEffect = gl_GLESInternalGetMaterialEffect(texture);
	batch.brightmap = brightmap;
	batch.brightmapDesaturation = brightmapDesaturation;
	batch.masked = masked;
	batch.fog = fog;
	batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
	batch.repeat = true;
	const unsigned int textureFlags = gl_GLESInternalGetMaterialFlags(texture);
	batch.cameraTexture = (textureFlags & GLES_TEXTURE_FLAG_FRAMEBUFFER) != 0;
	batch.palette = (textureFlags & GLES_TEXTURE_FLAG_PALETTE) != 0;
	batch.model = true;
	batch.cullBackFaces = cullBackFaces;
	batch.materialFlags = materialFlags;
	batch.blendMode = blendMode;
	batch.customBlend = customBlend;
	batch.sourceBlend = static_cast<GLenum>(sourceBlend);
	batch.destinationBlend = static_cast<GLenum>(destinationBlend);
	batch.alphaCutoff = ClampUnit(alphaCutoff);
	// Custom materials test alpha after Process applies the actor opacity.
	if ((materialFlags & GLES_MATERIAL_FUZZ) == 0 &&
		batch.materialEffect.shaderIndex >= FIRST_USER_SHADER)
		batch.alphaCutoff *= ClampUnit(alpha);
	batch.sortDepth = dx * dx + dy * dy + dz * dz;
	if (fogColor != NULL)
	{
		batch.fogColor[0] = ClampUnit(fogColor[0]);
		batch.fogColor[1] = ClampUnit(fogColor[1]);
		batch.fogColor[2] = ClampUnit(fogColor[2]);
	}
	batch.fogDensity = std::max(0.0f, fogDensity);
	RecordSceneBatchSubmission(batch);
	PushNativeSceneBatch(batch);
}

void gl_GLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EGLESBlendMode blendMode, unsigned int materialFlags, bool customBlend,
	int sourceBlend, int destinationBlend, float alphaCutoff, unsigned int brightmap, int brightmapDesaturation)
{
	if (!gl_GLES_CanUseResources() || positions == NULL) return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	FSceneVertex vertices[4];
	for (int i = 0; i < 4; ++i)
	{
		vertices[i].x = positions[i * 3 + 0];
		vertices[i].y = positions[i * 3 + 1];
		vertices[i].z = positions[i * 3 + 2];
		vertices[i].nx = vertices[i].ny = vertices[i].nz = 0.0f;
		vertices[i].u = texcoords != NULL ? texcoords[i * 2 + 0] : 0.0f;
		vertices[i].v = texcoords != NULL ? texcoords[i * 2 + 1] : 0.0f;
		vertices[i].r = rgb[0];
		vertices[i].g = rgb[1];
		vertices[i].b = rgb[2];
		vertices[i].a = ClampUnit(alpha);
	}
	AddHUDQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, masked, blendMode, materialFlags,
		customBlend, static_cast<GLenum>(sourceBlend), static_cast<GLenum>(destinationBlend), alphaCutoff, brightmap, brightmapDesaturation);
}

void gl_GLES_AddScreenQuad(const float *color, float alpha,
	EGLESBlendMode blendMode)
{
	static const float positions[12] =
	{
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f
	};
	static const float texcoords[8] =
	{
		0.0f, 0.0f,
		0.0f, 0.0f,
		0.0f, 0.0f,
		0.0f, 0.0f
	};
	gl_GLES_AddHUDQuad(positions, texcoords, color, alpha, false, 0, blendMode, 0);
}

void gl_GLES_EndScene()
{
	if (!gl_GLES_CanUseResources()) return;
	if (Profile.active)
	{
		const auto now = std::chrono::steady_clock::now();
		Profile.collectionMilliseconds = std::chrono::duration<double, std::milli>(
			now - Profile.frameStart).count();
	}
	if (Resources.skyMaterial != NULL && Resources.outerSky.capEligible)
		AddSkyMaskCaps(Resources.outerSky);
	if (developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
	{
		unsigned int skyMaskCount = static_cast<unsigned int>(Resources.outerSky.maskBatches.size());
		unsigned int floodCount = 0;
		unsigned int flatCount = 0;
		unsigned int translucentCount = 0;
		unsigned int hudCount = 0;
		unsigned int portalCount = 0;
		unsigned int maskedCount = 0;
		for (size_t i = 0; i < Resources.sceneBatches.size(); ++i)
		{
			const FSceneBatch &batch = Resources.sceneBatches[i];
			if (batch.flood) ++floodCount;
			if (batch.flat) ++flatCount;
			if (batch.translucent) ++translucentCount;
			if (batch.hud) ++hudCount;
			if (batch.portalId >= 0) ++portalCount;
			if (batch.masked) ++maskedCount;
		}
		DPrintf("Zandronum GLES scene: %u vertices, %u indices, %u batches, %u opaque commands coalesced.\n",
			static_cast<unsigned int>(Resources.sceneVertices.size()),
			static_cast<unsigned int>(Resources.sceneIndices.size()),
			static_cast<unsigned int>(Resources.sceneBatches.size()), Resources.sceneOpaqueBatchMerges);
		DPrintf("Zandronum GLES scene submissions: %u walls, %u flats, %u sprites.\n",
			Resources.sceneWallCount, Resources.sceneFlatCount, Resources.sceneSpriteCount);
		DPrintf("Zandronum GLES scene masks: %u sky, %u flood, %u flat.\n",
			skyMaskCount, floodCount, flatCount);
		DPrintf("Zandronum GLES scene kinds: %u translucent, %u HUD, %u portal, %u masked.\n",
			translucentCount, hudCount, portalCount, maskedCount);
	}
	gl_GLESInternalInvalidateStateCache();
}

struct FNativeTextureBindingCache
{
	GLuint textures[3];
	GLuint samplers[3];
	bool known[3];
	unsigned int activeUnit;
	bool activeUnitKnown;
};

struct FNativeRenderStateCache
{
	bool blendEnabledKnown;
	bool blendEnabled;
	bool blendFunctionKnown;
	GLenum sourceBlend;
	GLenum destinationBlend;
	bool blendEquationKnown;
	GLenum blendEquation;
	bool depthKnown;
	bool depthEnabled;
	GLenum depthFunction;
	bool depthWrite;
	bool cullKnown;
	bool cullEnabled;
	GLenum frontFace;
	GLuint staticUniformProgram;
	bool staticUniformsKnown;
};

static void ApplyNativeBlendState(FNativeRenderStateCache &cache, bool enabled,
	GLenum sourceBlend, GLenum destinationBlend, GLenum equation)
{
	bool changed = false;
	if (!cache.blendEnabledKnown || cache.blendEnabled != enabled)
	{
		if (enabled) glEnable(GL_BLEND);
		else glDisable(GL_BLEND);
		cache.blendEnabled = enabled;
		cache.blendEnabledKnown = true;
		changed = true;
	}
	if (enabled && (!cache.blendFunctionKnown || cache.sourceBlend != sourceBlend ||
		cache.destinationBlend != destinationBlend))
	{
		glBlendFunc(sourceBlend, destinationBlend);
		cache.sourceBlend = sourceBlend;
		cache.destinationBlend = destinationBlend;
		cache.blendFunctionKnown = true;
		changed = true;
	}
	if (!cache.blendEquationKnown || cache.blendEquation != equation)
	{
		glBlendEquation(equation);
		cache.blendEquation = equation;
		cache.blendEquationKnown = true;
		changed = true;
	}
	gl_GLES_RecordProfilePortalState(!changed);
}

static void ApplyNativeDepthState(FNativeRenderStateCache &cache, bool enabled,
	GLenum depthFunction, bool depthWrite)
{
	const bool changed = !cache.depthKnown || cache.depthEnabled != enabled ||
		(enabled && cache.depthFunction != depthFunction) || cache.depthWrite != depthWrite;
	if (changed)
	{
		if (!cache.depthKnown || cache.depthEnabled != enabled)
		{
			if (enabled) glEnable(GL_DEPTH_TEST);
			else glDisable(GL_DEPTH_TEST);
		}
		if (enabled && (!cache.depthKnown || cache.depthFunction != depthFunction))
			glDepthFunc(depthFunction);
		if (!cache.depthKnown || cache.depthWrite != depthWrite)
			glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);
		cache.depthEnabled = enabled;
		cache.depthFunction = depthFunction;
		cache.depthWrite = depthWrite;
		cache.depthKnown = true;
	}
	gl_GLES_RecordProfilePortalState(!changed);
}

static void ApplyNativeCullState(FNativeRenderStateCache &cache, bool enabled,
	GLenum frontFace)
{
	const bool changed = !cache.cullKnown || cache.cullEnabled != enabled ||
		(enabled && cache.frontFace != frontFace);
	if (changed)
	{
		if (!cache.cullKnown || cache.cullEnabled != enabled)
		{
			if (enabled) glEnable(GL_CULL_FACE);
			else glDisable(GL_CULL_FACE);
		}
		if (enabled && (!cache.cullKnown || cache.frontFace != frontFace))
		{
			glFrontFace(frontFace);
			cache.frontFace = frontFace;
		}
		cache.cullEnabled = enabled;
		cache.cullKnown = true;
	}
	gl_GLES_RecordProfilePortalState(!changed);
}

static void BindNativePortalTexture(FNativeTextureBindingCache &cache, unsigned int unit,
	GLuint texture, GLuint sampler)
{
	if (!cache.activeUnitKnown || cache.activeUnit != unit)
	{
		glActiveTexture(GL_TEXTURE0 + unit);
		cache.activeUnit = unit;
		cache.activeUnitKnown = true;
		if (Profile.active) ++Profile.portalActiveTextureSets;
	}
	else if (Profile.active)
	{
		++Profile.portalActiveTextureSkips;
	}
	if (!cache.known[unit] || cache.textures[unit] != texture)
		glBindTexture(GL_TEXTURE_2D, texture);
	if (!cache.known[unit] || cache.samplers[unit] != sampler)
		glBindSampler(unit, sampler);
	cache.textures[unit] = texture;
	cache.samplers[unit] = sampler;
	cache.known[unit] = true;
}

static void RecordNativePortalUniform(bool skipped)
{
	if (!Profile.active) return;
	if (skipped) ++Profile.portalUniformSkips;
	else ++Profile.portalUniformSets;
}

static void DrawNativePortalBatch(const FSceneBatch &batch, GLuint dynamicLightTexture,
	GLuint stencilBit, FNativeTextureBindingCache &textureCache,
	FNativeRenderStateCache &stateCache)
{
	if (batch.firstIndex < 0 || batch.indexCount <= 0 ||
		static_cast<size_t>(batch.firstIndex) + static_cast<size_t>(batch.indexCount) > Resources.sceneIndices.size())
		return;
	const FSceneViewSnapshot &view = BatchView(batch);
	if (batch.flood && (batch.floodWallFirstIndex < 0 ||
		static_cast<size_t>(batch.floodWallFirstIndex) + 6 > Resources.sceneIndices.size()))
		return;
	if (stencilBit != 0)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	}
	const bool depthEnabled = !batch.hud;
	const GLenum depthFunction = batch.model || batch.sourceOrdered || batch.translucent ||
		IsNativeDecal(batch) || (batch.materialFlags & GLES_MATERIAL_SPHERE_MAP) != 0 ? GL_LEQUAL : GL_LESS;
	const bool cullEnabled = batch.model && batch.cullBackFaces;
	if (batch.translucent)
	{
		GLenum equation = GL_FUNC_ADD;
		if (batch.blendMode == GLES_BLEND_SUBTRACT) equation = GL_FUNC_SUBTRACT;
		else if (batch.blendMode == GLES_BLEND_REVERSE_SUBTRACT) equation = GL_FUNC_REVERSE_SUBTRACT;
		const bool additive = batch.blendMode == GLES_BLEND_ADD ||
			batch.blendMode == GLES_BLEND_SUBTRACT || batch.blendMode == GLES_BLEND_REVERSE_SUBTRACT;
		GLenum sourceBlend = GL_SRC_ALPHA;
		GLenum destinationBlend = additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA;
		if (batch.customBlend)
		{
			sourceBlend = batch.sourceBlend;
			destinationBlend = batch.destinationBlend;
		}
		else if (batch.blendMode == GLES_BLEND_FUZZ)
		{
			sourceBlend = (batch.materialFlags >> GLES_MATERIAL_FUZZ_SHIFT) & 7 ? GL_SRC_ALPHA : GL_DST_COLOR;
			destinationBlend = GL_ONE_MINUS_SRC_ALPHA;
		}
		else if (batch.blendMode == GLES_BLEND_MULTIPLY)
		{
			sourceBlend = GL_DST_COLOR;
			destinationBlend = GL_ZERO;
		}
		ApplyNativeBlendState(stateCache, true, sourceBlend, destinationBlend, equation);
	}
	else
	{
		ApplyNativeBlendState(stateCache, false, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_FUNC_ADD);
	}
	ApplyNativeDepthState(stateCache, depthEnabled, depthFunction,
		batch.sourceOrdered || batch.translucent || batch.hud || batch.flood || IsNativeDecal(batch) ? false : true);
	ApplyNativeCullState(stateCache, cullEnabled, GL_CW);
	GLuint program = batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
		(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram));
	const char *programName = batch.fog ? (batch.masked ? "gles/portal-fog-masked" : "gles/portal-fog") :
		(batch.masked ? "gles/portal-masked" : (batch.palette ? "gles/portal-palette" : "gles/portal-opaque"));
	const GLuint materialProgram = ResolveNativeMaterialProgram(program, batch);
	if (materialProgram != program) { program = materialProgram; programName = NULL; }
	BindNativeProgram(program, programName);
	SetNativeLightingUniforms(program, batch);
	SetNativeMaterialUniforms(program, batch);
	SetNativeGlowUniforms(program, batch);
	const bool setStaticUniforms = !stateCache.staticUniformsKnown ||
		stateCache.staticUniformProgram != program;
	RecordNativePortalUniform(!setStaticUniforms);
	if (setStaticUniforms)
	{
		const FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
		if (uniforms.skyDepth >= 0) glUniform1i(uniforms.skyDepth, 0);
		if (uniforms.skyFog >= 0) glUniform1i(uniforms.skyFog, 0);
	}
	const FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
	const GLint viewProjection = uniforms.viewProjection;
	const GLint textureUniform = uniforms.textureUniform;
	const GLint brightmapUniform = uniforms.brightmapUniform;
	const GLint useBrightmap = uniforms.useBrightmap;
	const GLint brightmapDesaturation = uniforms.brightmapDesaturation;
	const GLint useTexture = uniforms.useTexture;
	const GLint model = uniforms.model;
	const GLint textureTransform = uniforms.textureTransform;
	const GLint cameraPosition = uniforms.cameraPosition;
	const GLint objectColor = uniforms.objectColor;
	const GLint materialFlags = uniforms.materialFlags;
	const GLint fuzzTime = uniforms.fuzzTime;
	const GLint alphaCutoff = uniforms.alphaCutoff;
	const GLint fogColor = uniforms.fogColor;
	const GLint fogDensity = uniforms.fogDensity;
	const GLint lightDataSampler = uniforms.lightDataSampler;
	const GLint lightDataOffset = uniforms.lightDataOffset;
	const GLint lightCounts = uniforms.lightCounts;
	const GLint lightPlaneNormal = uniforms.lightPlaneNormal;
	const GLint projectedLights = uniforms.projectedLights;
	const GLint dynamicLightSampler = uniforms.dynamicLightSampler;
	const GLint clipPlaneUniform = uniforms.clipPlaneUniform;
	const GLint clipPlaneEnabledUniform = uniforms.clipPlaneEnabledUniform;
	static const GLfloat identity[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	if (viewProjection >= 0) glUniformMatrix4fv(viewProjection, 1, GL_FALSE,
		batch.hud ? identity : view.viewProjection);
	if (setStaticUniforms && model >= 0) glUniformMatrix4fv(model, 1, GL_FALSE, identity);
	if (setStaticUniforms && textureTransform >= 0) glUniform4f(textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (cameraPosition >= 0) glUniform3f(cameraPosition, batch.hud ? 0.0f : view.cameraPosition[0],
		batch.hud ? 0.0f : view.cameraPosition[1], batch.hud ? 0.0f : view.cameraPosition[2]);
	if (setStaticUniforms && objectColor >= 0) glUniform4f(objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
	if (materialFlags >= 0) glUniform1i(materialFlags, static_cast<GLint>(batch.materialFlags |
		(gl_fogmode == 2 ? GLES_MATERIAL_RADIAL_FOG : 0)));
	if (setStaticUniforms && fuzzTime >= 0) glUniform1f(fuzzTime, gl_frameMS / 1000.0f);
	if (clipPlaneUniform >= 0) glUniform4fv(clipPlaneUniform, 1, view.clipPlane);
	if (clipPlaneEnabledUniform >= 0) glUniform1i(clipPlaneEnabledUniform, view.clipPlaneEnabled ? 1 : 0);
	if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ? 0.0f : batch.alphaCutoff);
	if (fogColor >= 0) glUniform4f(fogColor, batch.fogColor[0], batch.fogColor[1], batch.fogColor[2], 1.0f);
	if (fogDensity >= 0) glUniform1f(fogDensity, batch.fogDensity / 64000.0f);
	const unsigned int lightCount = batch.lightCount;
	if (lightDataSampler >= 0) glUniform1i(lightDataSampler, 3);
	if (lightDataOffset >= 0) glUniform1i(lightDataOffset, static_cast<GLint>(batch.lightOffset));
	if (lightCounts >= 0) glUniform3i(lightCounts, static_cast<GLint>(batch.lightNormalCount),
		static_cast<GLint>(batch.lightSubtractiveCount), static_cast<GLint>(lightCount));
	if (lightPlaneNormal >= 0) glUniform3fv(lightPlaneNormal, 1, batch.lightPlaneNormal);
	const bool projected = dynamicLightTexture != 0 && lightCount > 0 &&
		(batch.lightPlaneNormal[0] != 0.0f || batch.lightPlaneNormal[1] != 0.0f || batch.lightPlaneNormal[2] != 0.0f);
	if (projectedLights >= 0) glUniform1i(projectedLights, projected ? 1 : 0);
	if (setStaticUniforms && dynamicLightSampler >= 0) glUniform1i(dynamicLightSampler, 2);
	if (setStaticUniforms && textureUniform >= 0) glUniform1i(textureUniform, 0);
	if (setStaticUniforms && brightmapUniform >= 0) glUniform1i(brightmapUniform, 1);
	if (useBrightmap >= 0) glUniform1i(useBrightmap, batch.brightmap != 0 ? 1 : 0);
	if (brightmapDesaturation >= 0) glUniform1i(brightmapDesaturation, batch.brightmapDesaturation);
	if (useTexture >= 0) glUniform1i(useTexture, batch.texture != 0 ? 1 : 0);
	BindNativePortalTexture(textureCache, 0,
		batch.texture != 0 ? batch.texture : Resources.checkerTexture,
		NativeSamplerForBatch(batch));
	BindNativePortalTexture(textureCache, 1,
		batch.brightmap != 0 ? batch.brightmap : Resources.checkerTexture,
		NativeSamplerForBatch(batch));
	BindNativePortalTexture(textureCache, 2,
		projected ? dynamicLightTexture : Resources.checkerTexture,
		Resources.sceneSamplers[3]);
	BindNativePortalTexture(textureCache, 0, textureCache.textures[0], textureCache.samplers[0]);
	SetNativeDecalDepthBias(batch, true);
	ProfileDrawElements(batch.primitiveMode, batch.indexCount, NativeSceneIndexType(),
		NativeSceneIndexOffset(batch.firstIndex));
	stateCache.staticUniformProgram = program;
	stateCache.staticUniformsKnown = true;
	SetNativeDecalDepthBias(batch, false);
}

static void DrawNativeWipeOverlay(const FGLESTargetDescriptor &target)
{
	if (!HasNativeWipeOverlay() || target.renderWidth <= 0 || target.renderHeight <= 0)
		return;
	gl_GLESInternalResetState(target.renderWidth, target.renderHeight);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	SetNativeHUDViewport(target.renderWidth, target.renderHeight);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(Resources.sceneVertexArray);
	FNativeTextureBindingCache textureCache = {};
	FNativeRenderStateCache stateCache = {};
	for (size_t i = 0; i < Resources.sceneBatches.size(); ++i)
	{
		const FSceneBatch &batch = Resources.sceneBatches[i];
		if (batch.wipeOverlay) DrawNativePortalBatch(batch, 0, 0, textureCache, stateCache);
	}
	glBindVertexArray(0);
	glDisable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glDepthMask(GL_TRUE);
	gl_GLESInternalResetState(target.renderWidth, target.renderHeight);
	if (gl_GLES_CheckErrors("wipe overlay") != GL_NO_ERROR)
		I_FatalError("Zandronum GLES wipe overlay failed.");
}

static bool DrawNativePortalMask(const FSceneBatch &batch, GLuint stencilBit, bool writeStencil,
	bool configureStencil = true, GLuint stencilRef = 0, GLuint stencilCompareMask = 0)
{
	if (batch.firstIndex < 0 || batch.indexCount <= 0 ||
		static_cast<size_t>(batch.firstIndex) + static_cast<size_t>(batch.indexCount) > Resources.sceneIndices.size())
		return false;
	const FSceneViewSnapshot &view = BatchView(batch);
	BindNativeProgram(Resources.sceneProgram, "gles/portal-mask");
	static const GLfloat identity[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	if (Resources.sceneViewProjection >= 0)
		glUniformMatrix4fv(Resources.sceneViewProjection, 1, GL_FALSE, view.viewProjection);
	if (Resources.sceneModel >= 0) glUniformMatrix4fv(Resources.sceneModel, 1, GL_FALSE, identity);
	if (Resources.sceneTextureTransform >= 0) glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (Resources.sceneCameraPosition >= 0)
		glUniform3f(Resources.sceneCameraPosition, view.cameraPosition[0], view.cameraPosition[1], view.cameraPosition[2]);
	if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
	if (Resources.sceneSkyDepth >= 0) glUniform1i(Resources.sceneSkyDepth, 0);
	if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 0);
	if (Resources.sceneMaterialFlags >= 0) glUniform1i(Resources.sceneMaterialFlags, 0);
	if (Resources.sceneTextureUniform >= 0) glUniform1i(Resources.sceneTextureUniform, 0);
	if (Resources.sceneBrightmapUniform >= 0) glUniform1i(Resources.sceneBrightmapUniform, 1);
	if (Resources.sceneUseBrightmap >= 0) glUniform1i(Resources.sceneUseBrightmap, 0);
	if (Resources.sceneBrightmapDesaturation >= 0) glUniform1i(Resources.sceneBrightmapDesaturation, 0);
	if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
	if (Resources.sceneLightCounts >= 0) glUniform3i(Resources.sceneLightCounts, 0, 0, 0);
	if (Resources.sceneProjectedLights >= 0) glUniform1i(Resources.sceneProjectedLights, 0);
	if (Resources.sceneClipPlane >= 0) glUniform4f(Resources.sceneClipPlane, 0.0f, 0.0f, 0.0f, 0.0f);
	if (Resources.sceneClipPlaneEnabled >= 0) glUniform1i(Resources.sceneClipPlaneEnabled, 0);
	if (writeStencil && configureStencil)
	{
		glStencilMask(stencilBit);
		if (stencilCompareMask != 0)
			glStencilFunc(GL_EQUAL, stencilRef, stencilCompareMask);
		else
			glStencilFunc(GL_ALWAYS, stencilBit, stencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	}
	ProfileDrawElements(batch.primitiveMode, batch.indexCount, NativeSceneIndexType(),
		NativeSceneIndexOffset(batch.firstIndex));
	return true;
}

static bool DrawNativeSkyboxLayer(FMaterial *material, float xOffset, bool sky2, bool fliptop, const float *viewProjection)
{
	if (material == NULL || material->tex == NULL || !material->tex->gl_info.bSkybox) return false;
	FSkyBox *skybox = static_cast<FSkyBox *>(material->tex);
	if (skybox->faces[0] == NULL) return false;
		gl_GLESInternalPortalUploadSkyboxGeometry(xOffset, sky2, fliptop,
			Resources.cameraX, Resources.cameraY, Resources.cameraZ);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
	const bool threeFace = skybox->faces[5] == NULL;
	bool drawn = false;
	for (int face = 0; face < 6; ++face)
	{
		const int sourceFace = face < 4 ? (threeFace ? 0 : face) :
			(threeFace ? face - 3 : face);
		if (skybox->faces[sourceFace] == NULL) continue;
		FMaterial *faceMaterial = FMaterial::ValidateTexture(skybox->faces[sourceFace]);
		if (faceMaterial == NULL) continue;
		const GLuint texture = faceMaterial->BindNative(CM_DEFAULT, 0, false);
		if (texture == 0) continue;
		FNativeDrawUniforms *skyUniforms = &BindNativeSkyMaterial(texture, viewProjection);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		// Skybox faces use the source renderer's clamped edge sampling.
		glBindSampler(0, Resources.sceneSamplers[3]);
		if (skyUniforms->textureTransform >= 0)
		{
			if (threeFace && face < 4)
				glUniform4f(skyUniforms->textureTransform, 0.25f, 1.0f, face * 0.25f, 0.0f);
			else
				glUniform4f(skyUniforms->textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
		}
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
		if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		const FGLESSkyPrimitiveRange range = gl_GLESInternalPortalSkyboxFace(face);
		ProfileDrawElements(GL_TRIANGLES, range.indexCount, GL_UNSIGNED_SHORT,
			reinterpret_cast<const void *>(range.firstIndex * sizeof(GLushort)));
		drawn = true;
	}
	return drawn;
}

static void DrawNativePortalSky(const FNativePortalTarget &target, GLuint stencilRef, GLuint stencilMask)
{
	if (!target.sky.viewValid || target.skyMaterial == NULL || target.skyMaterial->tex == NULL)
		return;
	const float savedCameraX = Resources.cameraX;
	const float savedCameraY = Resources.cameraY;
	const float savedCameraZ = Resources.cameraZ;
	Resources.cameraX = target.sky.cameraPosition[0];
	Resources.cameraY = target.sky.cameraPosition[1];
	Resources.cameraZ = target.sky.cameraPosition[2];
	// Portal mask depth protects the target from later world batches; the sky
	// itself is selected solely by stencil, as in the desktop non-depth path.
	glDisable(GL_DEPTH_TEST);
	// The sky cap and the alpha-faded first strip overlap. Like Zandronum's
	// non-depth portal path, neither may write depth or the cap hides the fade.
	glDepthMask(GL_FALSE);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0x00);
	glStencilFunc(GL_EQUAL, stencilRef, stencilMask);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	FNativeDrawUniforms *skyUniforms = &BindNativeSkyMaterial(0, target.sky.viewProjection);
	glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
	if (target.skyMaterial->tex->gl_info.bSkybox)
	{
		if (DrawNativeSkyboxLayer(target.skyMaterial, target.skyXOffset, target.sky2,
			static_cast<FSkyBox *>(target.skyMaterial->tex)->fliptop, target.sky.viewProjection))
		{
			// The skybox uses its own VAO; reflected scene batches use the scene stream.
			glBindVertexArray(Resources.sceneVertexArray);
			Resources.cameraX = savedCameraX;
			Resources.cameraY = savedCameraY;
			Resources.cameraZ = savedCameraZ;
			return;
		}
	}
	auto drawSkyLayer = [&](FMaterial *material, float xOffset, float yOffset, bool mirrored, bool drawCaps) -> bool
	{
		if (material == NULL || material->tex == NULL) return false;
		const bool caps = drawCaps;
		const GLuint texture = material->BindNative(CM_DEFAULT, 0, true);
		if (texture == 0) return false;
		skyUniforms = &BindNativeSkyMaterial(0, target.sky.viewProjection);
		gl_GLESInternalPortalUploadSkyGeometry(material, xOffset, yOffset, mirrored,
			Resources.cameraX, Resources.cameraY, Resources.cameraZ);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		glBindSampler(0, Resources.worldSamplers[0]);
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, caps ? 0 : 1);
		if (caps)
		{
			if (skyUniforms->objectColor >= 0)
				glUniform4f(skyUniforms->objectColor, target.skyUpperCapColor.r / 255.0f,
					target.skyUpperCapColor.g / 255.0f, target.skyUpperCapColor.b / 255.0f, 1.0f);
			ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
		}
		skyUniforms = &BindNativeSkyMaterial(texture, target.sky.viewProjection);
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
		if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
		if (caps)
		{
			skyUniforms = &BindNativeSkyMaterial(0, target.sky.viewProjection);
			if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 0);
			if (skyUniforms->objectColor >= 0)
				glUniform4f(skyUniforms->objectColor, target.skyLowerCapColor.r / 255.0f,
					target.skyLowerCapColor.g / 255.0f, target.skyLowerCapColor.b / 255.0f, 1.0f);
			ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
		}
		skyUniforms = &BindNativeSkyMaterial(texture, target.sky.viewProjection);
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
		if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
		return true;
	};
	const bool firstLayerDrawn = drawSkyLayer(target.skyMaterial, target.skyXOffset, target.skyYOffset,
		target.skyMirrored, true);
	if (firstLayerDrawn && target.skyLayerMaterial != NULL && target.skyLayerMaterial != target.skyMaterial)
		drawSkyLayer(target.skyLayerMaterial, target.skyLayerXOffset, target.skyLayerYOffset,
			target.skyLayerMirrored, false);
	if (firstLayerDrawn && target.skyFogEnabled && skyfog > 0)
	{
		gl_GLESInternalPortalUploadSkyGeometry(NULL, 0.0f, 0.0f, false,
			Resources.cameraX, Resources.cameraY, Resources.cameraZ);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
		skyUniforms = &BindNativeSkyMaterial(0, target.sky.viewProjection, true);
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 0);
		if (skyUniforms->objectColor >= 0)
			glUniform4f(skyUniforms->objectColor, target.skyFogColor.r / 255.0f,
				target.skyFogColor.g / 255.0f, target.skyFogColor.b / 255.0f, skyfog / 255.0f);
		glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
		ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
			reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
		ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
			reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
	}
	if (skyUniforms->skyFog >= 0) glUniform1i(skyUniforms->skyFog, 0);
	if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
	if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(Resources.sceneVertexArray);
	Resources.cameraX = savedCameraX;
	Resources.cameraY = savedCameraY;
	Resources.cameraZ = savedCameraZ;
}

static bool IsNativePortalDescendant(size_t targetIndex, unsigned int ancestorId)
{
	if (targetIndex >= Resources.portalTargets.size()) return false;
	int parentId = Resources.portalTargets[targetIndex].parentId;
	while (parentId >= 0 && static_cast<size_t>(parentId) < Resources.portalTargets.size())
	{
		if (static_cast<unsigned int>(parentId) == ancestorId) return true;
		parentId = Resources.portalTargets[parentId].parentId;
	}
	return false;
}

static GLuint GetNativePortalParentStencilBit(const FNativePortalTarget &target)
{
	if (target.parentId < 0 || static_cast<size_t>(target.parentId) >= Resources.portalTargets.size())
		return 0;
	return NativePortalStencilBit(Resources.portalTargets[target.parentId].stencilSlot);
}

static void BindNativePortalSurface(FGLESTargetDescriptor *surface)
{
	if (surface == nullptr) return;
	NativeActiveTarget = surface;
	gl_GLES_BindRenderTarget(surface);
	SetNativeFullViewport(surface->renderWidth, surface->renderHeight);
	if (!NativeOffscreenRender && Resources.nativeViewArea.valid)
		gl_GLESInternalSetSceneViewport(Resources.nativeViewArea);
}

static bool CompositeNativePortalFallback(const FNativePortalTarget &target,
	FGLESTargetDescriptor *parentSurface)
{
	if (parentSurface == nullptr) return false;
	FGLESTargetDescriptor *fallback = target.fallbackTargetReady ?
		gl_GLESInternalPortalGetFallbackTarget(target.fallbackTargetIndex) : nullptr;
	const GLuint sourceTexture = fallback != nullptr ? fallback->colorAttachment : 0;
	const bool solidColor = sourceTexture == 0;
	const GLuint parentStencilBit = GetNativePortalParentStencilBit(target);
	std::vector<FGLESPortalMask> masks;
	masks.reserve(target.maskBatches.size());
	for (size_t maskIndex = 0; maskIndex < target.maskBatches.size(); ++maskIndex)
	{
		const size_t batchIndex = target.maskBatches[maskIndex];
		if (batchIndex >= Resources.sceneBatches.size()) continue;
		const FSceneBatch &mask = Resources.sceneBatches[batchIndex];
		const FSceneViewSnapshot &view = BatchView(mask);
		masks.push_back({ mask.indexCount,
			static_cast<size_t>(mask.firstIndex) * NativeSceneIndexStride(), view.viewProjection });
	}
	FGLESPortalCompositeList composite = {};
	composite.sourceTexture = sourceTexture;
	composite.sceneVertexArray = Resources.sceneVertexArray;
	composite.sceneSampler = Resources.sceneSamplers[3];
	composite.indexType = NativeSceneIndexType();
	composite.masks = masks.data();
	composite.maskCount = masks.size();
	composite.targetWidth = parentSurface->renderWidth;
	composite.targetHeight = parentSurface->renderHeight;
	composite.parentStencilBit = parentStencilBit;
	composite.solidColor = solidColor;
	return gl_GLESInternalPortalCompositeMasks(composite);
}

struct FNativePortalScope
{
	unsigned int targetId;
	FGLESTargetDescriptor *targetSurface;
	FGLESTargetDescriptor *parentSurface;
};

static unsigned int DrawNativePortalContents(const FNativePortalTarget &target,
	GLuint dynamicLightTexture, bool translucent)
{
	std::vector<FGLESSceneOrderRecord> &records = Resources.portalOrderRecords;
	records.clear();
	const size_t end = std::min(target.endBatch, Resources.sceneBatches.size());
	for (size_t index = target.firstBatch; index < end; ++index)
	{
		const FSceneBatch &batch = Resources.sceneBatches[index];
		const bool late = batch.sourceOrdered || batch.translucent || IsNativeDecal(batch);
		FGLESSceneOrderRecord record = {};
		record.batchIndex = index;
		record.included = batch.portalId == static_cast<int>(target.id) &&
			!batch.portalMask && !batch.skyMask && !batch.hud && late == translucent;
		record.sourceOrdered = batch.sourceOrdered;
		record.flood = batch.flood;
		record.flat = batch.flat;
		record.decal = IsNativeDecal(batch) && (batch.materialFlags & GLES_MATERIAL_MIRROR_DECAL) == 0;
		record.translucent = batch.translucent || IsNativeDecal(batch);
		record.sortDepth = batch.sortDepth;
		records.push_back(record);
	}
	const auto start = Profile.active ? std::chrono::steady_clock::now() :
		std::chrono::steady_clock::time_point();
	const FGLESSceneDrawOrderView order = gl_GLESInternalSceneSortBatches(
		records.data(), records.size(), GLES_SCENE_ORDER_PORTAL);
	if (Profile.active) Profile.orderingMilliseconds += ProfileMilliseconds(start);
	const GLuint stencilBit = NativePortalStencilBit(target.stencilSlot);
	glEnable(GL_STENCIL_TEST);
	glStencilMask(0x00);
	glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	FNativeTextureBindingCache textureCache = {};
	FNativeRenderStateCache stateCache = {};
	for (size_t index = 0; index < order.count; ++index)
		DrawNativePortalBatch(Resources.sceneBatches[order.indices[index]], dynamicLightTexture,
			stencilBit, textureCache, stateCache);
	return static_cast<unsigned int>(order.count);
}

static unsigned int FinishNativePortalScope(const FNativePortalScope &scope, GLuint dynamicLightTexture)
{
	if (scope.targetId >= Resources.portalTargets.size()) return 0;
	const FNativePortalTarget &target = Resources.portalTargets[scope.targetId];
	BindNativePortalSurface(scope.targetSurface);
	const unsigned int drawn = DrawNativePortalContents(target, dynamicLightTexture, true);
	if (target.framebufferFallback)
	{
		BindNativePortalSurface(scope.parentSurface);
		if (!CompositeNativePortalFallback(target, scope.parentSurface))
			gl_GLES_Report("portal", "isolated capture could not be composited into its parent aperture");
	}
	// Restore the enclosing view's aperture depth before its translucent pass.
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(target.framebufferFallback ? GL_LEQUAL : GL_ALWAYS);
	glDepthMask(GL_TRUE);
	glDepthRangef(0.0f, 1.0f);
	glDisable(GL_BLEND);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	const GLuint stencilBit = NativePortalStencilBit(target.stencilSlot);
	const GLuint parentBit = GetNativePortalParentStencilBit(target);
	if (!target.framebufferFallback)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilMask(stencilBit | target.sky.stencilBit);
		glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
	}
	else if (parentBit != 0)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, parentBit, parentBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	}
	else glDisable(GL_STENCIL_TEST);
	for (size_t index = 0; index < target.maskBatches.size(); ++index)
	{
		const size_t batchIndex = target.maskBatches[index];
		if (batchIndex < Resources.sceneBatches.size())
			DrawNativePortalMask(Resources.sceneBatches[batchIndex], stencilBit, false);
	}
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glStencilMask(0x00);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	return drawn;
}

static void DrawNativePortalTargets(GLuint dynamicLightTexture)
{
	if (Resources.portalTargets.empty()) return;
	unsigned int drawnTargets = 0;
	unsigned int drawnBatches = 0;
	unsigned int fallbackTargets = 0;
	FGLESTargetDescriptor *savedActiveTarget = NativeActiveTarget;
	FGLESTargetDescriptor *rootSurface = savedActiveTarget != nullptr ?
		savedActiveTarget : &Resources.sceneTarget;
	std::vector<FNativePortalScope> portalScopes;
	NativeActiveTarget = rootSurface;
	glBindVertexArray(Resources.sceneVertexArray);
	for (size_t targetIndex = 0; targetIndex < Resources.portalTargets.size(); ++targetIndex)
	{
		while (!portalScopes.empty() &&
			!IsNativePortalDescendant(targetIndex, portalScopes.back().targetId))
		{
			const FNativePortalScope scope = portalScopes.back();
			portalScopes.pop_back();
			drawnBatches += FinishNativePortalScope(scope, dynamicLightTexture);
		}
		int ancestorId = Resources.portalTargets[targetIndex].parentId;
		bool failedFallbackAncestor = false;
		while (ancestorId >= 0 && static_cast<size_t>(ancestorId) < Resources.portalTargets.size())
		{
			const FNativePortalTarget &ancestor = Resources.portalTargets[ancestorId];
			if (ancestor.framebufferFallback && !ancestor.fallbackTargetReady)
			{
				failedFallbackAncestor = true;
				break;
			}
			ancestorId = ancestor.parentId;
		}
		if (failedFallbackAncestor) continue;
		FNativePortalTarget &target = Resources.portalTargets[targetIndex];
		if (target.maskBatches.empty()) continue;
		if (target.parentId >= 0 && static_cast<size_t>(target.parentId) >= Resources.portalTargets.size())
		{
			gl_GLES_Report("portal", "native portal target has an invalid parent and was skipped");
			continue;
		}
		FGLESTargetDescriptor *parentSurface = portalScopes.empty() ?
			(NativeActiveTarget != nullptr ? NativeActiveTarget : rootSurface) :
			portalScopes.back().targetSurface;
		FGLESTargetDescriptor *targetSurface = parentSurface;
		const GLuint stencilBit = NativePortalStencilBit(target.stencilSlot);
		const GLuint skyStencilBit = target.sky.stencilBit;
		const GLuint parentStencilBit = GetNativePortalParentStencilBit(target);
		if (target.framebufferFallback)
		{
			++fallbackTargets;
			targetSurface = gl_GLESInternalPortalGetFallbackTarget(target.fallbackTargetIndex);
			if (!target.fallbackTargetReady || targetSurface == nullptr || targetSurface->framebuffer == 0)
			{
				if (!CompositeNativePortalFallback(target, parentSurface))
					gl_GLES_Report("portal", "unavailable isolated target could not draw its black aperture fallback");
				++drawnTargets;
				continue;
			}
			BindNativePortalSurface(targetSurface);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glDepthMask(GL_TRUE);
			glStencilMask(0xff);
			glClearColor(0.025f, 0.045f, 0.075f, 1.0f);
			glClearDepthf(1.0f);
			glClearStencil(static_cast<GLint>(stencilBit));
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			SetNativeFullViewport(targetSurface->renderWidth, targetSurface->renderHeight);
			if (!NativeOffscreenRender && Resources.nativeViewArea.valid)
				gl_GLESInternalSetSceneViewport(Resources.nativeViewArea);
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LEQUAL);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glEnable(GL_STENCIL_TEST);
			glStencilMask(0x00);
			glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		}
		portalScopes.push_back({ target.id, targetSurface, parentSurface });
		const size_t targetEndBatch = std::min(target.endBatch, Resources.sceneBatches.size());
		const unsigned int targetSkyMaskCount = static_cast<unsigned int>(target.sky.maskBatches.size());
		if (developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		{
			const unsigned int skyMasks = targetSkyMaskCount;
			unsigned int opaqueWalls = 0;
			unsigned int opaqueFlats = 0;
			unsigned int floods = 0;
			unsigned int translucent = 0;
			unsigned int sprites = 0;
			for (size_t batchIndex = target.firstBatch; batchIndex < targetEndBatch; ++batchIndex)
			{
				const FSceneBatch &batch = Resources.sceneBatches[batchIndex];
				if (batch.portalId != static_cast<int>(target.id)) continue;
				if (batch.flood) ++floods;
				else if (batch.model) ++sprites;
				else if (batch.translucent) ++translucent;
				else if (batch.flat) ++opaqueFlats;
				else ++opaqueWalls;
			}
			DPrintf("Zandronum GLES portal target %u (parent %d): mask=%u skyMask=%u walls=%u flats=%u "
				"floods=%u translucent=%u models=%u batches=[%u,%u).\\n",
				target.id, target.parentId, static_cast<unsigned int>(target.maskBatches.size()), skyMasks,
				opaqueWalls, opaqueFlats, floods, translucent, sprites,
				static_cast<unsigned int>(target.firstBatch), static_cast<unsigned int>(targetEndBatch));
		}
		if (!target.framebufferFallback)
		{
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LEQUAL);
			glDepthMask(GL_FALSE);
			glDisable(GL_BLEND);
			glEnable(GL_STENCIL_TEST);
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glStencilMask(stencilBit);
			glStencilFunc(GL_ALWAYS, stencilBit, stencilBit);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			for (size_t maskIndex = 0; maskIndex < target.maskBatches.size(); ++maskIndex)
			{
				const size_t batchIndex = target.maskBatches[maskIndex];
				if (batchIndex < Resources.sceneBatches.size())
					DrawNativePortalMask(Resources.sceneBatches[batchIndex], stencilBit, true, true,
						stencilBit | parentStencilBit, parentStencilBit);
			}
			glStencilMask(0x00);
			glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			glDepthFunc(GL_ALWAYS);
			glDepthMask(GL_TRUE);
			glDepthRangef(1.0f, 1.0f);
			for (size_t maskIndex = 0; maskIndex < target.maskBatches.size(); ++maskIndex)
			{
				const size_t batchIndex = target.maskBatches[maskIndex];
				if (batchIndex < Resources.sceneBatches.size())
					DrawNativePortalMask(Resources.sceneBatches[batchIndex], stencilBit, false);
			}
			glDepthRangef(0.0f, 1.0f);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glDepthFunc(GL_LEQUAL);
			glDepthMask(GL_TRUE);
		}
		if (targetSkyMaskCount > 0)
		{
			// Sky walls own a second stencil bit inside the portal surface. This
			// keeps the reflected dome out of solid room ceilings and walls.
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LEQUAL);
			glDepthMask(GL_TRUE);
			glDisable(GL_BLEND);
			glEnable(GL_STENCIL_TEST);
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			glStencilMask(skyStencilBit);
			glStencilFunc(GL_EQUAL, stencilBit | skyStencilBit, stencilBit);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
			for (size_t maskIndex = 0; maskIndex < target.sky.maskBatches.size(); ++maskIndex)
			{
				const size_t batchIndex = target.sky.maskBatches[maskIndex];
				if (batchIndex < Resources.sceneBatches.size())
					DrawNativePortalMask(Resources.sceneBatches[batchIndex], skyStencilBit, true, false);
			}
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glStencilMask(0x00);
			glStencilFunc(GL_EQUAL, stencilBit | skyStencilBit, stencilBit | skyStencilBit);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		}
		target.sky.stencilRef = targetSkyMaskCount > 0 ? stencilBit | skyStencilBit : stencilBit;
		target.sky.stencilMask = targetSkyMaskCount > 0 ? stencilBit | skyStencilBit : stencilBit;
		DrawNativePortalSky(target, target.sky.stencilRef, target.sky.stencilMask);
		if (target.clearScreen)
		{
			if (target.framebufferFallback)
			{
				glDisable(GL_SCISSOR_TEST);
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
				glClear(GL_COLOR_BUFFER_BIT);
			}
			else
			{
				const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
				if (!gl_GLESInternalFillStencil(targetSurface->renderWidth, targetSurface->renderHeight,
					stencilBit, stencilBit, clearColor))
					gl_GLES_Report("portal", "native recursion termination fill failed");
			}
			++drawnTargets;
			continue;
		}
		drawnBatches += DrawNativePortalContents(target, dynamicLightTexture, false);
		++drawnTargets;

	}
	while (!portalScopes.empty())
	{
		const FNativePortalScope scope = portalScopes.back();
		portalScopes.pop_back();
		drawnBatches += FinishNativePortalScope(scope, dynamicLightTexture);
	}
	glBindVertexArray(0);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xff);
	glStencilFunc(GL_ALWAYS, 0, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	NativeActiveTarget = savedActiveTarget;
	if (developer && drawnTargets > 0 &&
		(Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES portal targets: %u, batches: %u, isolated FBOs: %u.\n",
			drawnTargets, drawnBatches, fallbackTargets);
}

unsigned int gl_GLES_BeginPortalCapture()
{
	if (!gl_GLES_CanUseResources())
		return ~0u;
	const unsigned int stencilSlot = FindNativePortalStencilSlot();
	FNativePortalTarget target = {};
	target.id = static_cast<unsigned int>(Resources.portalTargets.size());
	target.framebufferFallback = stencilSlot == ~0u;
	target.stencilSlot = target.framebufferFallback ? 0u : stencilSlot;
	target.fallbackTargetIndex = -1;
	if (target.framebufferFallback)
	{
		target.fallbackTargetReady = gl_GLESInternalPortalAcquireFallbackTarget(
			Resources.sceneTarget.renderWidth, Resources.sceneTarget.renderHeight,
			&target.fallbackTargetIndex);
	}
	target.parentId = NativePortalCaptureStack.empty() ? -1 :
		static_cast<int>(NativePortalCaptureStack.back());
	target.firstBatch = Resources.sceneBatches.size();
	target.endBatch = target.firstBatch;
	ResetNativeSkyRecord(target.sky, static_cast<int>(target.id), NativePortalSkyStencilBit(target.stencilSlot));
	memcpy(target.savedViewProjection, Resources.viewProjection, sizeof(target.savedViewProjection));
	memcpy(target.savedCameraPosition, &Resources.cameraX, sizeof(target.savedCameraPosition));
	target.savedCameraYaw = Resources.cameraYaw;
	target.savedCameraPitch = Resources.cameraPitch;
	target.savedCameraFieldOfView = Resources.cameraFieldOfView;
	target.savedCameraAspect = Resources.cameraAspect;
	target.savedCameraFovRatio = Resources.cameraFovRatio;
	target.savedSkyMaterial = Resources.skyMaterial;
	target.savedSkyXOffset = Resources.skyXOffset;
	target.savedSkyYOffset = Resources.skyYOffset;
	target.savedSkyLayerMaterial = Resources.skyLayerMaterial;
		target.savedSkyLayerXOffset = Resources.skyLayerXOffset;
		target.savedSkyLayerYOffset = Resources.skyLayerYOffset;
		target.savedSkyMirrored = Resources.skyMirrored;
		target.savedSkyLayerMirrored = Resources.skyLayerMirrored;
		target.savedSky2 = Resources.sky2;
	target.savedSkyUpperCapColor = Resources.skyUpperCapColor;
	target.savedSkyLowerCapColor = Resources.skyLowerCapColor;
	target.savedSkyFogColor = Resources.skyFogColor;
	target.savedSkyFogEnabled = Resources.skyFogEnabled;
	target.savedClipPlaneEnabled = Resources.clipPlaneEnabled;
	target.savedViewSerial = Resources.viewSerial;
	target.savedClipSerial = Resources.clipSerial;
	memcpy(target.savedClipPlane, Resources.clipPlane, sizeof(target.savedClipPlane));
	Resources.portalTargets.push_back(target);
	NativePortalCaptureStack.push_back(target.id);
	if (developer && Resources.frame < 2 && target.id < 16)
		DPrintf("Zandronum GLES begin portal capture %u parent %d stencil=%u fallback=%d.\n",
			target.id, target.parentId, target.stencilSlot, target.framebufferFallback ? 1 : 0);
	return target.id;
}

bool gl_GLES_ClearPortalCapture()
{
	if (!gl_GLES_CanUseResources() || NativePortalCaptureStack.empty()) return false;
	const unsigned int portalId = NativePortalCaptureStack.back();
	if (portalId >= Resources.portalTargets.size()) return false;
	Resources.portalTargets[portalId].clearScreen = true;
	return true;
}

void gl_GLES_AddPortalMask(unsigned int portalId, const float *positions)
{
	if (!gl_GLES_CanUseResources() || positions == NULL ||
		portalId >= Resources.portalTargets.size()) return;
	if (!CanAppendSceneGeometry(4, 6, "portal mask")) return;
	const unsigned int firstVertex = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	for (int i = 0; i < 4; ++i)
	{
		FSceneVertex vertex = {};
		vertex.x = positions[i * 3 + 0];
		vertex.y = positions[i * 3 + 1];
		vertex.z = positions[i * 3 + 2];
		vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
		Resources.sceneVertices.push_back(vertex);
	}
	Resources.sceneIndices.push_back(firstVertex + 0);
	Resources.sceneIndices.push_back(firstVertex + 1);
	Resources.sceneIndices.push_back(firstVertex + 2);
	Resources.sceneIndices.push_back(firstVertex + 2);
	Resources.sceneIndices.push_back(firstVertex + 3);
	Resources.sceneIndices.push_back(firstVertex + 0);
	FSceneBatch batch = {};
	CaptureBatchView(batch);
	batch.firstIndex = firstIndex;
	batch.indexCount = 6;
	batch.portalId = static_cast<int>(portalId);
	batch.portalMask = true;
	RecordSceneBatchSubmission(batch);
	PushNativeSceneBatch(batch);
	FNativePortalTarget &target = Resources.portalTargets[portalId];
	target.maskBatches.push_back(Resources.sceneBatches.size() - 1);
	// All masks precede the target scene batches in the shared stream.
	target.firstBatch = Resources.sceneBatches.size();
}

void gl_GLES_SetPortalView(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, bool mirrored, bool planeMirrored)
{
	if (!gl_GLES_CanUseResources() || NativePortalCaptureStack.empty()) return;
	const unsigned int portalId = NativePortalCaptureStack.back();
	if (portalId < Resources.portalTargets.size())
	{
		FNativeSkyRecord &sky = Resources.portalTargets[portalId].sky;
		// Native portal classes use the same view handoff. A reflected line
		// mirror has no caps; other captured owners may cap only multi-line masks.
		const FNativePortalTarget &target = Resources.portalTargets[portalId];
		sky.capEligible = (!mirrored || planeMirrored) && target.maskBatches.size() > 1;
	}
	BuildViewProjection(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch, cameraRoll,
		Resources.cameraFieldOfView, Resources.cameraAspect,
		Resources.cameraFovRatio,
		mirrored, planeMirrored);
}

void gl_GLES_SetPortalClipPlane(float a, float b, float c, float d)
{
	if (!gl_GLES_CanUseResources() || NativePortalCaptureStack.empty()) return;
	Resources.clipPlane[0] = a;
	Resources.clipPlane[1] = b;
	Resources.clipPlane[2] = c;
	Resources.clipPlane[3] = d;
	Resources.clipPlaneEnabled = true;
	++Resources.clipSerial;
}

void gl_GLES_EndPortalCapture(unsigned int portalId)
{
	if (!gl_GLES_CanUseResources() || NativePortalCaptureStack.empty() ||
		NativePortalCaptureStack.back() != portalId || portalId >= Resources.portalTargets.size()) return;
	FNativePortalTarget &target = Resources.portalTargets[portalId];
	target.skyMaterial = Resources.skyMaterial;
	target.skyXOffset = Resources.skyXOffset;
	target.skyYOffset = Resources.skyYOffset;
	target.skyLayerMaterial = Resources.skyLayerMaterial;
	target.skyLayerXOffset = Resources.skyLayerXOffset;
	target.skyLayerYOffset = Resources.skyLayerYOffset;
	target.skyMirrored = Resources.skyMirrored;
	target.skyLayerMirrored = Resources.skyLayerMirrored;
	target.sky2 = Resources.sky2;
	target.skyUpperCapColor = Resources.skyUpperCapColor;
	target.skyLowerCapColor = Resources.skyLowerCapColor;
	target.skyFogColor = Resources.skyFogColor;
	target.skyFogEnabled = Resources.skyFogEnabled;
	if (target.skyMaterial != NULL && target.sky.capEligible)
	{
		AddSkyMaskCaps(target.sky);
	}
	target.endBatch = Resources.sceneBatches.size();
	if (!target.sky.viewValid)
	{
		for (size_t batchIndex = target.firstBatch; batchIndex < target.endBatch; ++batchIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[batchIndex];
			if (batch.portalId == static_cast<int>(portalId) && !batch.portalMask)
			{
				const FSceneViewSnapshot &view = BatchView(batch);
				memcpy(target.sky.viewProjection, view.viewProjection, sizeof(target.sky.viewProjection));
				memcpy(target.sky.cameraPosition, view.cameraPosition, sizeof(target.sky.cameraPosition));
				target.sky.viewValid = true;
				break;
			}
		}
	}
	NativePortalCaptureStack.pop_back();
	if (developer && Resources.frame < 2 && target.id < 16)
		DPrintf("Zandronum GLES end portal capture %u batches=[%u,%u) masks=%u.\n",
			target.id, static_cast<unsigned int>(target.firstBatch),
			static_cast<unsigned int>(target.endBatch),
			static_cast<unsigned int>(target.maskBatches.size()));
	memcpy(Resources.viewProjection, target.savedViewProjection, sizeof(Resources.viewProjection));
	memcpy(&Resources.cameraX, target.savedCameraPosition, sizeof(target.savedCameraPosition));
	Resources.cameraYaw = target.savedCameraYaw;
	Resources.cameraPitch = target.savedCameraPitch;
	Resources.cameraFieldOfView = target.savedCameraFieldOfView;
	Resources.cameraAspect = target.savedCameraAspect;
	Resources.cameraFovRatio = target.savedCameraFovRatio;
	Resources.skyMaterial = target.savedSkyMaterial;
	Resources.skyXOffset = target.savedSkyXOffset;
	Resources.skyYOffset = target.savedSkyYOffset;
	Resources.skyLayerMaterial = target.savedSkyLayerMaterial;
	Resources.skyLayerXOffset = target.savedSkyLayerXOffset;
	Resources.skyLayerYOffset = target.savedSkyLayerYOffset;
	Resources.skyMirrored = target.savedSkyMirrored;
	Resources.skyLayerMirrored = target.savedSkyLayerMirrored;
	Resources.sky2 = target.savedSky2;
	Resources.skyUpperCapColor = target.savedSkyUpperCapColor;
	Resources.skyLowerCapColor = target.savedSkyLowerCapColor;
	Resources.skyFogColor = target.savedSkyFogColor;
	Resources.skyFogEnabled = target.savedSkyFogEnabled;
	Resources.clipPlaneEnabled = target.savedClipPlaneEnabled;
	Resources.viewSerial = target.savedViewSerial;
	Resources.clipSerial = target.savedClipSerial;
	memcpy(Resources.clipPlane, target.savedClipPlane, sizeof(Resources.clipPlane));
}

unsigned int gl_GLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires, bool noFilter, bool noCompression)
{
	if (!gl_GLES_CanUseResources() || key == NULL || pixels == NULL || width <= 0 || height <= 0)
		return 0;
	FScopedProfileTimer timer(Profile.materialResolutionMilliseconds);
	ConfigureNativeSamplers();
	return gl_GLESInternalBindMaterial(true, key, pixels, width, height, repeat,
		colormap, translation, allowhires, colormap != CM_DEFAULT || translation != 0, noFilter, noCompression);
}

unsigned int gl_GLES_EnsureMaterialTexture(const void *key, int width, int height,
	bool repeat, int colormap, int translation, bool allowhires)
{
	if (!gl_GLES_CanUseResources() || key == NULL || width <= 0 || height <= 0)
		return 0;
	FScopedProfileTimer timer(Profile.materialResolutionMilliseconds);
	const GLuint texture = gl_GLESInternalFindMaterialTexture(key, colormap, translation,
		repeat, allowhires, width, height);
	if (texture != 0) return texture;
	std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
	ConfigureNativeSamplers();
	return gl_GLESInternalBindMaterial(true, key, pixels.data(), width, height, repeat,
		colormap, translation, allowhires, colormap != CM_DEFAULT || translation != 0, false, true);
}

void gl_GLES_MarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires)
{
	gl_GLESInternalMarkMaterialFramebufferContent(key, colormap, translation, repeat, allowhires);
}

void gl_GLES_ClearMaterialCache()
{
	gl_GLESInternalClearMaterials(Resources.ready);
	Resources.skyMaterial = NULL;
	Resources.skyXOffset = 0.0f;
	Resources.skyYOffset = 0.0f;
	Resources.skyLayerMaterial = NULL;
	Resources.skyLayerXOffset = 0.0f;
	Resources.skyLayerYOffset = 0.0f;
	Resources.sky2 = false;
	Resources.skyMirrored = false;
	Resources.skyLayerMirrored = false;
	Resources.skyUpperCapColor = 0;
	Resources.skyLowerCapColor = 0;
	Resources.skyFogColor = 0;
	Resources.skyFogEnabled = false;
}

void gl_GLES_OnContextLost()
{
	const bool interruptedWipe = gl_GLESInternalWipeIsActive();
	gl_GLES_UnregisterShaderPrograms();
	InvalidateResources();
	gl_GLES_InvalidateTextures();
	gl_GLES_InvalidateFlatBuffers();
	gl_GLES_ShutdownContext(true);
	CapabilitiesReady = false;
	memset(&Capabilities, 0, sizeof(Capabilities));
	if (developer)
		DPrintf("Zandronum GLES context lost%s; native resource names invalidated.\n",
			interruptedWipe ? " during a wipe" : "");
}

bool gl_GLES_OnContextRestored(int width, int height)
{
	if (!gl_GLES_CollectCapabilities()) return false;
	const bool restored = InitializeResources(width, height, true);
	if (restored) gl_GLES_RegisterShaderPrograms();
	return restored;
}

void gl_GLES_SetSourceOrder(bool enabled)
{
	NativeSourceOrder = enabled;
}

void gl_GLES_RenderBootstrap(int width, int height)
{
	if (!Resources.ready)
	{
		if (!BootstrapPauseLogged)
		{
			BootstrapPauseLogged = true;
		if (developer)
			DPrintf("GLES rendering paused while the host surface is unavailable.\n");
		}
		return;
	}
	BootstrapPauseLogged = false;
	if (!NativeOffscreenRender &&
		(Resources.sceneTarget.renderWidth != width || Resources.sceneTarget.renderHeight != height))
	{
		gl_GLESInternalWipeDestroy();
		if (!BuildFramebuffer(width, height))
		I_FatalError("Zandronum GLES render target could not follow surface size %dx%d.", width, height);
	}
	FGLESTargetDescriptor *activeTarget = NativeActiveTarget != NULL ?
		NativeActiveTarget : &Resources.sceneTarget;
	if (activeTarget->framebuffer == 0 || activeTarget->renderWidth != width ||
		activeTarget->renderHeight != height)
	{
		I_FatalError("Zandronum GLES active render target has invalid size %dx%d.", width, height);
	}
	gl_GLES_BindRenderTarget(activeTarget);
	if (gl_GLES_GetContextInfo().hasDepthClamp) glEnable(GL_DEPTH_CLAMP);
	SetNativeFullViewport(activeTarget->renderWidth, activeTarget->renderHeight);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	// Doom wall winding is not consistently front-facing across two-sided sectors.
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	const bool letterboxed = !NativeOffscreenRender && screen != NULL &&
		screen->GetTrueHeight() > screen->GetHeight();
	glClearColor(letterboxed ? 0.0f : 0.025f, letterboxed ? 0.0f : 0.045f,
		letterboxed ? 0.0f : 0.075f, 1.0f);
	glClearDepthf(1.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	UploadSceneGeometry();
	if (!NativeOffscreenRender && Resources.nativeViewArea.valid)
		gl_GLESInternalSetSceneViewport(Resources.nativeViewArea);
	const bool renderSky = !gl_no_skyclear;
	bool skyMaskReady = !renderSky;
	bool skyMaskPresent = false;
	bool skyDrawn = !renderSky;
	bool skyMaskHadGeometry = false;
	bool skyMaskUsedGeometry = false;
	auto drawSkyMask = [&]()
	{
		if (skyMaskReady) return;
		FNativeSkyRecord &sky = Resources.outerSky;
		const bool hasMask = !sky.maskBatches.empty();
		skyMaskHadGeometry = hasMask;
		if (!hasMask)
		{
			skyMaskReady = true;
			skyMaskPresent = false;
			return;
		}
		skyMaskPresent = true;
		skyMaskUsedGeometry = true;
		glBindVertexArray(Resources.sceneVertexArray);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		// The sky wall mask is a depth owner, just as DrawPortalStencil is on
		// desktop. Later opaque batches behind this opening must fail depth.
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		glEnable(GL_STENCIL_TEST);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glStencilMask(sky.stencilBit);
		glStencilFunc(GL_ALWAYS, sky.stencilBit, sky.stencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		for (size_t maskIndex = 0; maskIndex < sky.maskBatches.size(); ++maskIndex)
		{
			const size_t batchIndex = sky.maskBatches[maskIndex];
			if (batchIndex < Resources.sceneBatches.size())
				DrawNativePortalMask(Resources.sceneBatches[batchIndex], sky.stencilBit, true);
		}
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, sky.stencilRef, sky.stencilMask);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		skyMaskReady = true;
	};
	auto drawSky = [&]()
	{
		if (skyDrawn || !skyMaskReady) return;
		const FNativeSkyRecord &sky = Resources.outerSky;
		if (developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		{
			int upperStripCount = 0;
			int lowerStripCount = 0;
			for (int row = 0; row < 4; ++row)
			{
				if (gl_GLESInternalPortalSkyUpperStrip(row).indexCount > 0) ++upperStripCount;
				if (gl_GLESInternalPortalSkyLowerStrip(row).indexCount > 0) ++lowerStripCount;
			}
			const char *stencilRoute = skyMaskUsedGeometry ? "geometry" : "none";
			const float clipMinX = sky.submitted > 0 ? sky.clipMin[0] : 0.0f;
			const float clipMaxX = sky.submitted > 0 ? sky.clipMax[0] : 0.0f;
			const float clipMinY = sky.submitted > 0 ? sky.clipMin[1] : 0.0f;
			const float clipMaxY = sky.submitted > 0 ? sky.clipMax[1] : 0.0f;
			const float clipMinZ = sky.submitted > 0 ? sky.clipMin[2] : 0.0f;
			const float clipMaxZ = sky.submitted > 0 ? sky.clipMax[2] : 0.0f;
			const float clipMinW = sky.submitted > 0 ? sky.clipMin[3] : 0.0f;
			const float clipMaxW = sky.submitted > 0 ? sky.clipMax[3] : 0.0f;
			DPrintf("Zandronum GLES sky: pitch %.2f, yaw %.2f, fov %.2f, aspect %.3f, hasMask=%d, "
				"skyMaskPresent=%d, upperCap=%d, lowerCap=%d, upperStrips=%d, "
				"lowerStrips=%d, stencil=%s, masks submitted=%u clipped=%u rejected=%u, "
				"clip x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] w[%.2f,%.2f].\n",
				Resources.cameraPitch * 180.0f / 3.14159265359f,
				Resources.cameraYaw * 180.0f / 3.14159265359f, Resources.cameraFieldOfView,
				Resources.cameraAspect, skyMaskHadGeometry ? 1 : 0, skyMaskPresent ? 1 : 0,
				gl_GLESInternalPortalSkyUpperCap().indexCount,
				gl_GLESInternalPortalSkyLowerCap().indexCount, upperStripCount, lowerStripCount, stencilRoute,
				sky.submitted, sky.clipped, sky.rejected,
				clipMinX, clipMaxX, clipMinY, clipMaxY, clipMinZ, clipMaxZ, clipMinW, clipMaxW);
		}
		if (!skyMaskPresent || Resources.skyMaterial == NULL)
		{
			glDisable(GL_STENCIL_TEST);
			glStencilMask(0xff);
			glDepthMask(GL_TRUE);
			glDepthFunc(GL_LESS);
			return;
		}
		// The mask owns the portal depth. The dome is selected by stencil only,
		// otherwise that depth would reject the far sky geometry.
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, sky.stencilRef, sky.stencilMask);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glEnable(GL_STENCIL_TEST);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_CULL_FACE);
		FNativeDrawUniforms *skyUniforms = &BindNativeSkyMaterial(0, Resources.viewProjection);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
		auto drawSkyLayer = [&](FMaterial *material, float xOffset, float yOffset, bool drawCaps) -> bool
		{
			if (material == NULL || material->tex == NULL) return false;
			const bool caps = drawCaps;
			if (material->tex->gl_info.bSkybox)
			{
				FSkyBox *skybox = static_cast<FSkyBox *>(material->tex);
				return DrawNativeSkyboxLayer(material, xOffset, Resources.sky2, skybox->fliptop, Resources.viewProjection);
			}
			const GLuint skyTexture = material->BindNative(CM_DEFAULT, 0, true);
			if (skyTexture == 0) return false;
			skyUniforms = &BindNativeSkyMaterial(0, Resources.viewProjection);
			gl_GLESInternalPortalUploadSkyGeometry(material, xOffset, yOffset,
				caps ? Resources.skyMirrored : Resources.skyLayerMirrored,
				Resources.cameraX, Resources.cameraY, Resources.cameraZ);
			glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, skyTexture);
			glBindSampler(0, Resources.worldSamplers[0]);
			if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, caps ? 0 : 1);
			if (caps)
			{
				if (skyUniforms->objectColor >= 0)
					glUniform4f(skyUniforms->objectColor, Resources.skyUpperCapColor.r / 255.0f,
						Resources.skyUpperCapColor.g / 255.0f, Resources.skyUpperCapColor.b / 255.0f, 1.0f);
				ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
			}
			skyUniforms = &BindNativeSkyMaterial(skyTexture, Resources.viewProjection);
			if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
			if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
			if (caps)
			{
				skyUniforms = &BindNativeSkyMaterial(0, Resources.viewProjection);
				if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 0);
				if (skyUniforms->objectColor >= 0)
					glUniform4f(skyUniforms->objectColor, Resources.skyLowerCapColor.r / 255.0f,
						Resources.skyLowerCapColor.g / 255.0f, Resources.skyLowerCapColor.b / 255.0f, 1.0f);
				ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
			}
			skyUniforms = &BindNativeSkyMaterial(skyTexture, Resources.viewProjection);
			if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
			if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
			return true;
		};
		const bool firstLayerDrawn = drawSkyLayer(Resources.skyMaterial, Resources.skyXOffset, Resources.skyYOffset, true);
		const bool firstLayerIsSkybox = Resources.skyMaterial != NULL &&
			Resources.skyMaterial->tex != NULL && Resources.skyMaterial->tex->gl_info.bSkybox;
		if (firstLayerDrawn && !firstLayerIsSkybox && Resources.skyLayerMaterial != NULL &&
			Resources.skyLayerMaterial != Resources.skyMaterial)
			drawSkyLayer(Resources.skyLayerMaterial, Resources.skyLayerXOffset, Resources.skyLayerYOffset, false);
		if (firstLayerDrawn && !firstLayerIsSkybox && Resources.skyFogEnabled && skyfog > 0)
		{
			gl_GLESInternalPortalUploadSkyGeometry(NULL, 0.0f, 0.0f, false,
				Resources.cameraX, Resources.cameraY, Resources.cameraZ);
			glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
			skyUniforms = &BindNativeSkyMaterial(0, Resources.viewProjection, true);
			if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 0);
			if (skyUniforms->objectColor >= 0)
				glUniform4f(skyUniforms->objectColor, Resources.skyFogColor.r / 255.0f,
					Resources.skyFogColor.g / 255.0f, Resources.skyFogColor.b / 255.0f, skyfog / 255.0f);
			glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
			ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
			ProfileDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				ProfileDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
			if (skyUniforms->skyFog >= 0) glUniform1i(skyUniforms->skyFog, 0);
		}
		skyUniforms = &BindNativeSkyMaterial(0, Resources.viewProjection);
		if (skyUniforms->useTexture >= 0) glUniform1i(skyUniforms->useTexture, 1);
		if (skyUniforms->objectColor >= 0) glUniform4f(skyUniforms->objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		glBindVertexArray(0);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glStencilMask(0xff);
		glDisable(GL_BLEND);
		glDisable(GL_STENCIL_TEST);
		skyDrawn = firstLayerDrawn;
	};
	const bool renderScene = Resources.sceneReady;
	GLuint dynamicLightTexture = 0;
	if (!gl_dynlight_shader && gl_lights && GLRenderer != NULL && GLRenderer->gllight != NULL)
	{
		FMaterial *lightMaterial = FMaterial::ValidateTexture(GLRenderer->gllight);
		if (lightMaterial != NULL) dynamicLightTexture = lightMaterial->BindNative(CM_DEFAULT, 0, false);
	}
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	if (renderScene && renderSky && Resources.skyMaterial != NULL)
	{
		// The desktop sky portal is composed before opaque world geometry.
		const auto portalSkyStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		drawSkyMask();
		drawSky();
		if (Profile.active)
			Profile.portalSkyMilliseconds += ProfileMilliseconds(portalSkyStart);
	}
	if (renderScene)
	{
		glBindVertexArray(Resources.sceneVertexArray);
		std::vector<FGLESSceneOrderRecord> &orderRecords = Resources.sceneOrderRecords;
		orderRecords.resize(Resources.sceneBatches.size());
		for (size_t i = 0; i < orderRecords.size(); ++i)
		{
			const FSceneBatch &batch = Resources.sceneBatches[i];
			orderRecords[i].batchIndex = i;
			orderRecords[i].included = true;
			orderRecords[i].sourceOrdered = batch.sourceOrdered;
			orderRecords[i].hud = batch.hud;
			orderRecords[i].flood = batch.flood;
			orderRecords[i].flat = batch.flat;
			orderRecords[i].decal = IsNativeDecal(batch) && (batch.materialFlags & GLES_MATERIAL_MIRROR_DECAL) == 0;
			orderRecords[i].translucent = batch.translucent || IsNativeDecal(batch);
			orderRecords[i].sortDepth = batch.sortDepth;
		}
		const auto orderingStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		const FGLESSceneDrawOrderView drawOrder = gl_GLESInternalSceneSortBatches(
			orderRecords.data(), orderRecords.size(), GLES_SCENE_ORDER_VIEW);
		if (Profile.active)
			Profile.orderingMilliseconds += ProfileMilliseconds(orderingStart);
		GLuint boundTextures[3] = { 0, 0, 0 };
		GLuint boundSamplers[3] = { 0, 0, 0 };
		bool textureKnown[3] = { false, false, false };
		unsigned int activeTextureUnit = 0;
		bool activeTextureKnown = false;
		auto bindOpaqueTexture = [&](unsigned int unit, GLuint texture, GLuint sampler)
		{
			if (!activeTextureKnown || activeTextureUnit != unit)
			{
				glActiveTexture(GL_TEXTURE0 + unit);
				activeTextureUnit = unit;
				activeTextureKnown = true;
				++Resources.sceneActiveTextureSets;
			}
			else ++Resources.sceneActiveTextureSkips;
			if (!textureKnown[unit] || boundTextures[unit] != texture)
			{
				glBindTexture(GL_TEXTURE_2D, texture);
				boundTextures[unit] = texture;
				++Resources.sceneTextureBinds;
			}
			else ++Resources.sceneTextureBindSkips;
			if (!textureKnown[unit] || boundSamplers[unit] != sampler)
			{
				glBindSampler(unit, sampler);
				boundSamplers[unit] = sampler;
				++Resources.sceneSamplerBinds;
			}
			else ++Resources.sceneSamplerBindSkips;
			textureKnown[unit] = true;
		};
		bool commonOpaqueStateReady = false;
		auto applyCommonOpaqueState = [&]()
		{
			if (commonOpaqueStateReady)
			{
				++Resources.sceneOpaqueStateSkips;
				return;
			}
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glDisable(GL_CULL_FACE);
			glDisable(GL_STENCIL_TEST);
			glDisable(GL_BLEND);
			glDepthMask(GL_TRUE);
			glBlendEquation(GL_FUNC_ADD);
			commonOpaqueStateReady = true;
			++Resources.sceneOpaqueStateSets;
		};
		bool lightUniformStateKnown = false;
		GLuint lastLightUniformProgram = 0;
		size_t lastLightUniformOffset = 0;
		unsigned int lastLightUniformNormalCount = 0;
		unsigned int lastLightUniformSubtractiveCount = 0;
		unsigned int lastLightUniformCount = 0;
		GLint lastLightDataSampler = -1;
		GLint lastLightDataOffset = -1;
		GLint lastLightCounts = -1;
		auto uploadOpaqueLights = [&](const FSceneBatch &batch, GLuint program,
			GLint positionRadius, GLint color, GLint counts,
			unsigned int normalCount, unsigned int subtractiveCount,
			unsigned int count)
		{
			const bool sameSelection = lightUniformStateKnown && lastLightUniformProgram == program &&
				lastLightUniformOffset == batch.lightOffset && lastLightUniformNormalCount == normalCount &&
				lastLightUniformSubtractiveCount == subtractiveCount && lastLightUniformCount == count &&
				lastLightDataSampler == positionRadius && lastLightDataOffset == color && lastLightCounts == counts;
			if (sameSelection)
			{
				++Resources.sceneLightUniformUploadSkips;
				return;
			}
			if (positionRadius >= 0) glUniform1i(positionRadius, 3);
			if (color >= 0) glUniform1i(color, static_cast<GLint>(batch.lightOffset));
			if (counts >= 0) glUniform3i(counts, static_cast<GLint>(normalCount),
				static_cast<GLint>(subtractiveCount), static_cast<GLint>(count));
			lightUniformStateKnown = true;
			lastLightUniformProgram = program;
			lastLightUniformOffset = batch.lightOffset;
			lastLightUniformNormalCount = normalCount;
			lastLightUniformSubtractiveCount = subtractiveCount;
			lastLightUniformCount = count;
			lastLightDataSampler = positionRadius;
			lastLightDataOffset = color;
			lastLightCounts = counts;
			++Resources.sceneLightUniformUploads;
		};
		GLuint opaqueStaticUniformProgram = 0;
		bool opaqueStaticUniformsKnown = false;
		const auto worldStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		for (size_t orderIndex = 0; orderIndex < drawOrder.count; ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder.indices[orderIndex]];
			const FSceneViewSnapshot &view = BatchView(batch);
			// Opaque world geometry establishes depth before portal targets and
			// translucent/HUD batches are composited.
			if (batch.skyMask || batch.portalMask || batch.portalId >= 0 || batch.sourceOrdered || batch.translucent || batch.hud ||
				IsNativeDecal(batch))
				continue;
			if (batch.firstIndex < 0 || batch.indexCount <= 0 ||
				static_cast<size_t>(batch.firstIndex) + static_cast<size_t>(batch.indexCount) > Resources.sceneIndices.size())
			{
				continue;
			}
			if (batch.flood && (batch.floodWallFirstIndex < 0 ||
				static_cast<size_t>(batch.floodWallFirstIndex) + 6 > Resources.sceneIndices.size()))
			{
				continue;
			}
			if (!skyMaskReady && batch.flood)
			{
				drawSkyMask();
				drawSky();
				glBindVertexArray(Resources.sceneVertexArray);
			}
			if (batch.flood)
			{
				commonOpaqueStateReady = false;
				opaqueStaticUniformsKnown = false;
				// Keep flood masks on a dedicated stencil bit so sky masks remain intact.
				glEnable(GL_STENCIL_TEST);
				glStencilMask(0x02);
				glStencilFunc(GL_ALWAYS, 0x02, 0x02);
				glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(GL_TRUE);
				BindNativeProgram(Resources.sceneProgram, "gles/flood-mask");
				if (Resources.sceneSkyDepth >= 0) glUniform1i(Resources.sceneSkyDepth, 0);
				static const GLfloat identity[16] =
				{
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 1.0f
				};
				if (Resources.sceneViewProjection >= 0)
					glUniformMatrix4fv(Resources.sceneViewProjection, 1, GL_FALSE, Resources.viewProjection);
				if (Resources.sceneModel >= 0) glUniformMatrix4fv(Resources.sceneModel, 1, GL_FALSE, identity);
				if (Resources.sceneTextureTransform >= 0) glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
				if (Resources.sceneCameraPosition >= 0)
					glUniform3f(Resources.sceneCameraPosition, Resources.cameraX, Resources.cameraY, Resources.cameraZ);
				if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
				if (Resources.sceneMaterialFlags >= 0) glUniform1i(Resources.sceneMaterialFlags, 0);
				if (Resources.sceneTextureUniform >= 0) glUniform1i(Resources.sceneTextureUniform, 0);
				if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
				glBindSampler(0, Resources.sceneSamplers[3]);
				textureKnown[0] = false;
		ProfileDrawElements(GL_TRIANGLES, 6, NativeSceneIndexType(),
			NativeSceneIndexOffset(batch.floodWallFirstIndex));
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glStencilMask(0x00);
				// The flood plane may only fill its projected gap. Bit 0 is the
				// already-established sky portal mask, which must remain visible.
				glStencilFunc(GL_EQUAL, 0x02, 0x03);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(GL_FALSE);
			}
			else if (batch.hud)
			{
				glDisable(GL_DEPTH_TEST);
				glDisable(GL_CULL_FACE);
				glDepthMask(GL_FALSE);
			}
			else if (batch.model)
			{
				commonOpaqueStateReady = false;
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glDisable(GL_CULL_FACE);
				if (batch.cullBackFaces)
				{
					glEnable(GL_CULL_FACE);
					glFrontFace(GL_CW);
				}
			}
			else
			{
				applyCommonOpaqueState();
			}
			if (!commonOpaqueStateReady) glDisable(GL_STENCIL_TEST);
			if (batch.translucent)
			{
				glEnable(GL_BLEND);
				glDepthMask(GL_FALSE);
				GLenum equation = GL_FUNC_ADD;
				switch (batch.blendMode)
				{
				case GLES_BLEND_SUBTRACT:
					equation = GL_FUNC_SUBTRACT;
					break;
				case GLES_BLEND_REVERSE_SUBTRACT:
					equation = GL_FUNC_REVERSE_SUBTRACT;
					break;
				default:
					break;
				}
				glBlendEquation(equation);
				const bool additiveBlend = batch.blendMode == GLES_BLEND_ADD ||
					batch.blendMode == GLES_BLEND_SUBTRACT ||
					batch.blendMode == GLES_BLEND_REVERSE_SUBTRACT;
				if (batch.customBlend)
					glBlendFunc(batch.sourceBlend, batch.destinationBlend);
				else if (batch.blendMode == GLES_BLEND_MULTIPLY)
					glBlendFunc(GL_DST_COLOR, GL_ZERO);
				else if (batch.blendMode == GLES_BLEND_FUZZ)
					glBlendFunc((batch.materialFlags >> GLES_MATERIAL_FUZZ_SHIFT) & 7 ? GL_SRC_ALPHA : GL_DST_COLOR,
						GL_ONE_MINUS_SRC_ALPHA);
				else
					glBlendFunc(GL_SRC_ALPHA, additiveBlend ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
			}
			else
			{
				if (!commonOpaqueStateReady)
				{
					glDisable(GL_BLEND);
					glDepthMask((batch.flood || IsNativeDecal(batch)) ? GL_FALSE : GL_TRUE);
					glBlendEquation(GL_FUNC_ADD);
				}
			}
			const bool simpleOpaque = CanUseNativeSimpleProgram(batch);
			GLuint program = simpleOpaque ? Resources.simpleProgram :
				(batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
				(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram)));
			const char *programName = simpleOpaque ? "gles/simple-opaque" :
				(batch.fog ? (batch.masked ? "gles/fog-masked" : "gles/fog") :
				(batch.masked ? "gles/masked" : (batch.palette ? "gles/palette" : "gles/opaque")));
			const GLuint materialProgram = ResolveNativeMaterialProgram(program, batch);
			if (materialProgram != program) { program = materialProgram; programName = NULL; }
			BindNativeProgram(program, programName);
			if (Profile.active && simpleOpaque) ++Profile.simpleShaderDraws;
			SetNativeLightingUniforms(program, batch);
			SetNativeMaterialUniforms(program, batch);
			SetNativeGlowUniforms(program, batch);
			const bool setStaticUniforms = !opaqueStaticUniformsKnown ||
				opaqueStaticUniformProgram != program;
			if (setStaticUniforms)
			{
				const FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
				if (uniforms.skyDepth >= 0) glUniform1i(uniforms.skyDepth, 0);
				if (uniforms.skyFog >= 0) glUniform1i(uniforms.skyFog, 0);
			}
			const FNativeDrawUniforms &uniforms = GetNativeDrawUniforms(program);
			const GLint viewProjection = uniforms.viewProjection;
			const GLint textureUniform = uniforms.textureUniform;
			const GLint brightmapUniform = uniforms.brightmapUniform;
			const GLint useBrightmap = uniforms.useBrightmap;
			const GLint brightmapDesaturation = uniforms.brightmapDesaturation;
			const GLint useTexture = uniforms.useTexture;
			const GLint model = uniforms.model;
			const GLint textureTransform = uniforms.textureTransform;
			const GLint cameraPosition = uniforms.cameraPosition;
			const GLint objectColor = uniforms.objectColor;
			const GLint materialFlags = uniforms.materialFlags;
			const GLint fuzzTime = uniforms.fuzzTime;
			const GLint alphaCutoff = uniforms.alphaCutoff;
			const GLint fogColor = uniforms.fogColor;
			const GLint fogDensity = uniforms.fogDensity;
			const GLint lightDataSampler = uniforms.lightDataSampler;
			const GLint lightDataOffset = uniforms.lightDataOffset;
			const GLint lightCounts = uniforms.lightCounts;
			const GLint lightPlaneNormal = uniforms.lightPlaneNormal;
			const GLint projectedLights = uniforms.projectedLights;
			const GLint dynamicLightSampler = uniforms.dynamicLightSampler;
			const GLint clipPlaneUniform = uniforms.clipPlaneUniform;
			const GLint clipPlaneEnabledUniform = uniforms.clipPlaneEnabledUniform;
			if (fogColor >= 0) glUniform4f(fogColor, batch.fogColor[0], batch.fogColor[1], batch.fogColor[2], 1.0f);
			if (fogDensity >= 0) glUniform1f(fogDensity, batch.fogDensity / 64000.0f);
			static const GLfloat identity[16] =
			{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			};
			if (viewProjection >= 0)
				glUniformMatrix4fv(viewProjection, 1, GL_FALSE, batch.hud ? identity : view.viewProjection);
			if (setStaticUniforms && model >= 0) glUniformMatrix4fv(model, 1, GL_FALSE, identity);
			if (setStaticUniforms && textureTransform >= 0) glUniform4f(textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
			if (cameraPosition >= 0)
				glUniform3f(cameraPosition, batch.hud ? 0.0f : view.cameraPosition[0],
					batch.hud ? 0.0f : view.cameraPosition[1], batch.hud ? 0.0f : view.cameraPosition[2]);
			if (setStaticUniforms && objectColor >= 0) glUniform4f(objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			if (materialFlags >= 0) glUniform1i(materialFlags, static_cast<GLint>(batch.materialFlags |
		(gl_fogmode == 2 ? GLES_MATERIAL_RADIAL_FOG : 0)));
			if (setStaticUniforms && fuzzTime >= 0) glUniform1f(fuzzTime, gl_frameMS / 1000.0f);
			if (clipPlaneUniform >= 0) glUniform4fv(clipPlaneUniform, 1, view.clipPlane);
			if (clipPlaneEnabledUniform >= 0) glUniform1i(clipPlaneEnabledUniform, view.clipPlaneEnabled ? 1 : 0);
			if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ? 0.0f : batch.alphaCutoff);
			const unsigned int lightCount = batch.lightCount;
			uploadOpaqueLights(batch, program, lightDataSampler, lightDataOffset, lightCounts,
				batch.lightNormalCount, batch.lightSubtractiveCount, lightCount);
			if (lightPlaneNormal >= 0)
				glUniform3fv(lightPlaneNormal, 1, batch.lightPlaneNormal);
			const bool useProjectedLights = dynamicLightTexture != 0 && lightCount > 0 &&
				(batch.lightPlaneNormal[0] != 0.0f || batch.lightPlaneNormal[1] != 0.0f || batch.lightPlaneNormal[2] != 0.0f);
			if (projectedLights >= 0) glUniform1i(projectedLights, useProjectedLights ? 1 : 0);
			if (setStaticUniforms && dynamicLightSampler >= 0) glUniform1i(dynamicLightSampler, 2);
			if (setStaticUniforms && textureUniform >= 0) glUniform1i(textureUniform, 0);
			if (setStaticUniforms && brightmapUniform >= 0) glUniform1i(brightmapUniform, 1);
			if (useBrightmap >= 0) glUniform1i(useBrightmap, batch.brightmap != 0 ? 1 : 0);
			if (brightmapDesaturation >= 0)
				glUniform1i(brightmapDesaturation, batch.brightmapDesaturation);
			if (useTexture >= 0) glUniform1i(useTexture, batch.texture != 0 ? 1 : 0);
			bindOpaqueTexture(0, batch.texture != 0 ? batch.texture : Resources.checkerTexture,
				NativeSamplerForBatch(batch));
			bindOpaqueTexture(1, batch.brightmap != 0 ? batch.brightmap : Resources.checkerTexture,
				NativeSamplerForBatch(batch));
			bindOpaqueTexture(2, useProjectedLights ? dynamicLightTexture : Resources.checkerTexture,
				Resources.sceneSamplers[3]);
			if (!activeTextureKnown || activeTextureUnit != 0)
			{
				glActiveTexture(GL_TEXTURE0);
				activeTextureUnit = 0;
				activeTextureKnown = true;
				++Resources.sceneActiveTextureSets;
			}
			else ++Resources.sceneActiveTextureSkips;
			SetNativeDecalDepthBias(batch, true);
			ProfileDrawElements(batch.primitiveMode, batch.indexCount, NativeSceneIndexType(),
				NativeSceneIndexOffset(batch.firstIndex));
			SetNativeDecalDepthBias(batch, false);
			if (batch.flood)
			{
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glStencilMask(0x02);
				glStencilFunc(GL_EQUAL, 0x02, 0x02);
				glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(GL_TRUE);
				ProfileDrawElements(GL_TRIANGLES, 6, NativeSceneIndexType(),
					NativeSceneIndexOffset(batch.floodWallFirstIndex));
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glStencilMask(0xff);
				glStencilFunc(GL_ALWAYS, 0, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				glDepthFunc(GL_LESS);
				glDisable(GL_STENCIL_TEST);
			}
			opaqueStaticUniformProgram = program;
			opaqueStaticUniformsKnown = true;
		}
		if (Profile.active)
			Profile.worldMilliseconds += ProfileMilliseconds(worldStart);
		drawSkyMask();
		drawSky();
		const auto portalSkyStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		DrawNativePortalTargets(dynamicLightTexture);
		if (Profile.active)
			Profile.portalSkyMilliseconds += ProfileMilliseconds(portalSkyStart);
		// Portal targets must be complete before the outer HUD is drawn.  Reuse
		// the same batch path so blending and shader selection stay consistent.
		glDisable(GL_STENCIL_TEST);
		glStencilMask(0xff);
		glDepthMask(GL_FALSE);
		glBindVertexArray(Resources.sceneVertexArray);
	bool hudViewportReady = NativeOffscreenRender;
	FNativeTextureBindingCache overlayTextureCache = {};
	FNativeRenderStateCache overlayStateCache = {};
		const auto overlayStart = Profile.active ? std::chrono::steady_clock::now() :
			std::chrono::steady_clock::time_point();
		for (size_t orderIndex = 0; orderIndex < drawOrder.count; ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder.indices[orderIndex]];
			if (batch.wipeOverlay || batch.skyMask || batch.portalMask || batch.portalId >= 0 ||
				(!batch.sourceOrdered && !batch.translucent && !batch.hud && !IsNativeDecal(batch)))
				continue;
			if (batch.hud && !hudViewportReady)
			{
				SetNativeHUDViewport(activeTarget->renderWidth, activeTarget->renderHeight);
				hudViewportReady = true;
			}
			DrawNativePortalBatch(batch, dynamicLightTexture, 0, overlayTextureCache, overlayStateCache);
		}
		if (Profile.active)
			Profile.overlayMilliseconds += ProfileMilliseconds(overlayStart);
		glBindVertexArray(0);
		glDisable(GL_BLEND);
		glDisable(GL_CULL_FACE);
		glBlendEquation(GL_FUNC_ADD);
		glDepthMask(GL_TRUE);
	}
	else
	{
		BindNativeProgram(Resources.sceneProgram, "gles/opaque");
		if (Resources.sceneSkyDepth >= 0) glUniform1i(Resources.sceneSkyDepth, 0);
		static const GLfloat identity[16] =
		{
			1.0f, 0.0f, 0.0f, 0.0f,
			0.0f, 1.0f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.0f, 0.0f, 0.0f, 1.0f
		};
		if (Resources.sceneModel >= 0) glUniformMatrix4fv(Resources.sceneModel, 1, GL_FALSE, identity);
		if (Resources.sceneTextureTransform >= 0) glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		if (Resources.sceneTextureUniform >= 0) glUniform1i(Resources.sceneTextureUniform, 0);
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
		glBindSampler(0, Resources.worldSamplers[0]);
		glBindVertexArray(Resources.vertexArray);
		ProfileDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(0));
		ProfileDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(6 * sizeof(GLushort)));
	}
	if (gl_GLES_IsProfileEnabled())
		CheckError(renderScene ? "world scene draw" : "bootstrap draw");
	if (renderScene && developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES scene programs: %u binds, %u redundant binds skipped.\n",
			Resources.sceneProgramBinds, Resources.sceneProgramBindSkips);
	if (renderScene && developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES opaque bindings: %u texture binds, %u texture binds skipped, %u sampler binds, %u sampler binds skipped.\n",
			Resources.sceneTextureBinds, Resources.sceneTextureBindSkips,
			Resources.sceneSamplerBinds, Resources.sceneSamplerBindSkips);
	if (renderScene && developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES opaque state: %u sets, %u redundant sets skipped.\n",
			Resources.sceneOpaqueStateSets, Resources.sceneOpaqueStateSkips);
	if (renderScene && developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES opaque lights: %u uniform uploads, %u redundant uploads skipped.\n",
			Resources.sceneLightUniformUploads, Resources.sceneLightUniformUploadSkips);
	if (renderScene && developer && (Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES scene glow: %u uniform uploads, %u redundant uploads skipped.\n",
			Resources.sceneGlowUniformUploads, Resources.sceneGlowUniformUploadSkips);
	const auto resolveStart = Profile.active ? std::chrono::steady_clock::now() :
		std::chrono::steady_clock::time_point();
	if (!gl_GLES_ResolveRenderTarget(activeTarget))
		I_FatalError("Zandronum GLES scene resolve failed.");
	if (Profile.active)
		Profile.resolveMilliseconds += ProfileMilliseconds(resolveStart);
	if (!NativeOffscreenRender && !gl_GLESInternalWipeCaptureEndFrame(Resources.sceneTarget))
		I_FatalError("Zandronum GLES wipe end capture failed.");
	if (NativeOffscreenRender)
	{
		gl_GLESInternalResetState(width, height);
		return;
	}

	FGLESPresentConfig present = {};
	if (!gl_GLES_GetHostTarget(&present.presentationTarget))
		I_FatalError("Zandronum GLES host did not supply a presentation target.");
	present.target = Resources.sceneTarget;
	present.wipe = gl_GLESInternalWipeGetBindings();
	present.sceneSampler = Resources.sceneSamplers[3];
	present.stateWidth = present.presentationTarget.renderWidth;
	present.stateHeight = present.presentationTarget.renderHeight;
	present.gamma = static_cast<float>(Gamma);
	present.brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
	present.contrast = clamp<float>(vid_contrast, 0.1f, 3.0f);
	if (HasNativeWipeOverlay() && gl_GLESInternalWipeIsActive())
	{
		if (!gl_GLES_CreateRenderTarget(&Resources.wipeOverlayTarget,
			Resources.sceneTarget.renderWidth, Resources.sceneTarget.renderHeight, 1))
			I_FatalError("Zandronum GLES wipe overlay target allocation failed.");
		FGLESPresentConfig composite = present;
		composite.presentationTarget = Resources.wipeOverlayTarget;
		composite.stateWidth = composite.presentationTarget.renderWidth;
		composite.stateHeight = composite.presentationTarget.renderHeight;
		composite.deferColorCorrection = true;
		if (!gl_GLESInternalPresent(composite))
			I_FatalError("Zandronum GLES wipe composition failed.");
		DrawNativeWipeOverlay(Resources.wipeOverlayTarget);
		present.target = Resources.wipeOverlayTarget;
		present.wipe = {};
	}
	else gl_GLES_DestroyRenderTarget(&Resources.wipeOverlayTarget);
	if (!gl_GLESInternalPresent(present))
		I_FatalError("Zandronum GLES frame failed.");
	gl_GLESInternalPublishFrameContract(Resources.frame,
		static_cast<double>(Resources.frame) / 35.0, present.presentationTarget,
		Resources.viewContract, Resources.nativeViewArea.valid);
	++Resources.frame;
	if (developer && (Resources.frame == 1 || (Resources.frame % 120) == 0))
		DPrintf("Zandronum GLES frame %u at %dx%d.\n", Resources.frame, width, height);
}

void gl_GLES_RecordHostPresentation(double waitMilliseconds, double presentMilliseconds,
	bool presented, int configuredLimit, int capFPS, int displayLimit, int effectiveLimit)
{
	const bool developerSample = developer && NativeFrameStart.time_since_epoch().count() != 0 &&
		(Resources.frame == 1 || (Resources.frame % 120) == 0);
	const bool profileSample = ProfileSampleDue();
	if ((!developerSample && !profileSample) || NativeFrameStart.time_since_epoch().count() == 0)
		return;
	const std::chrono::duration<double, std::milli> totalElapsed =
		std::chrono::steady_clock::now() - NativeFrameStart;
	const double totalMilliseconds = totalElapsed.count();
	const double renderMilliseconds = MAX(0.0, totalMilliseconds - waitMilliseconds - presentMilliseconds);
	if (developerSample)
	{
		char message[320];
		snprintf(message, sizeof(message),
			"cpu=%.3f ms wait=%.3f ms present=%.3f ms total=%.3f ms limit=%d cap=%d "
			"display=%d effective=%d (%s)", renderMilliseconds, waitMilliseconds,
			presentMilliseconds, totalMilliseconds, configuredLimit, capFPS, displayLimit,
			effectiveLimit, presented ? "ok" : "failed");
		gl_GLES_Report("frame timing", message);
	}
	if (profileSample)
	{
		char message[2048];
		snprintf(message, sizeof(message),
			"frame=%u collect=%.3f order=%.3f upload=%.3f world=%.3f portal_sky=%.3f "
			"overlay=%.3f resolve=%.3f present=%.3f cpu=%.3f total=%.3f ms; "
			"collect_parts=wall:%.3f flat:%.3f sprite:%.3f model:%.3f material:%.3f "
			"lights:%.3f batches:%.3f indices:%.3f; "
			"draws=%u tris=%u vertices=%u indices=%u submitted=%u batches=%u simple_draws=%u merges=%u "
			"walls=%u flats=%u sprites=%u order_hash=%016llx "
			"portal_targets=%u camera=%u/%u realloc=%u bytes=%zu/%zu index_type=%u resolves=%u readbacks=%u wipes=%u; "
			"binds=%u/%u tex=%u/%u active_tex=%u/%u samp=%u/%u sky_upload=%u/%u "
			"state=%u/%u bridge=%u/%u portal_state=%u/%u portal_tex=%u/%u portal_uniform=%u/%u "
			"lights=%u/%u light_select=%u/%u glow=%u/%u "
			"material=%u/%u uploads=%u; presented=%s",
			Resources.frame, Profile.collectionMilliseconds, Profile.orderingMilliseconds,
			Profile.uploadMilliseconds, Profile.worldMilliseconds, Profile.portalSkyMilliseconds,
			Profile.overlayMilliseconds, Profile.resolveMilliseconds, presentMilliseconds,
			renderMilliseconds, totalMilliseconds,
			Profile.wallCollectionMilliseconds, Profile.flatCollectionMilliseconds,
			Profile.spriteCollectionMilliseconds, Profile.modelCollectionMilliseconds,
			Profile.materialResolutionMilliseconds, Profile.lightSelectionMilliseconds,
			Profile.batchRecordMilliseconds, Profile.indexEmissionMilliseconds,
			Profile.drawCalls, Profile.triangles,
			static_cast<unsigned int>(Resources.sceneVertices.size()),
			static_cast<unsigned int>(Resources.sceneIndices.size()),
			Resources.sceneBatchSubmissions, static_cast<unsigned int>(Resources.sceneBatches.size()),
			Profile.simpleShaderDraws, Resources.sceneOpaqueBatchMerges,
			Resources.sceneWallCount, Resources.sceneFlatCount, Resources.sceneSpriteCount,
			static_cast<unsigned long long>(Resources.sceneOrderHash),
			static_cast<unsigned int>(Resources.portalTargets.size()), Profile.cameraTargetCreates,
			Profile.cameraCopies, Profile.bufferReallocations,
			Profile.vertexBytes, Profile.indexBytes,
			NativeSceneIndexType() == GL_UNSIGNED_SHORT ? 16u : 32u,
			Profile.resolveCount, Profile.readbackCount, Profile.wipeCaptureCount,
			Resources.sceneProgramBinds, Resources.sceneProgramBindSkips,
			Resources.sceneTextureBinds, Resources.sceneTextureBindSkips,
			Resources.sceneActiveTextureSets, Resources.sceneActiveTextureSkips,
			Resources.sceneSamplerBinds, Resources.sceneSamplerBindSkips,
			Profile.portalSkyUploadBuilds, Profile.portalSkyUploadSkips,
			Resources.sceneOpaqueStateSets, Resources.sceneOpaqueStateSkips,
			Profile.stateCalls, Profile.stateSkips,
			Profile.portalStateSets, Profile.portalStateSkips,
			Profile.portalActiveTextureSets, Profile.portalActiveTextureSkips,
			Profile.portalUniformSets, Profile.portalUniformSkips,
			Resources.sceneLightUniformUploads, Resources.sceneLightUniformUploadSkips,
			Profile.lightSelectionRequests, Profile.lightSelectionReuses,
			Resources.sceneGlowUniformUploads, Resources.sceneGlowUniformUploadSkips,
			Profile.materialHits, Profile.materialMisses, Profile.materialUploads,
			presented ? "ok" : "failed");
		gl_GLES_Report("profile", message);
	}
}

void gl_GLES_RecordProfileDraw(bool triangleStrip, int indexCount)
{
	if (!Profile.active || indexCount <= 0) return;
	++Profile.drawCalls;
	if (triangleStrip)
		Profile.triangles += indexCount > 2 ? static_cast<unsigned int>(indexCount - 2) : 0;
	else
		Profile.triangles += static_cast<unsigned int>(indexCount / 3);
}

void gl_GLES_RecordProfileState(bool skipped)
{
	if (!Profile.active) return;
	if (skipped) ++Profile.stateSkips;
	else ++Profile.stateCalls;
}

void gl_GLES_RecordProfilePortalState(bool skipped)
{
	if (!Profile.active) return;
	if (skipped) ++Profile.portalStateSkips;
	else ++Profile.portalStateSets;
}

void gl_GLES_RecordProfilePortalSkyUpload(bool skipped)
{
	if (!Profile.active) return;
	if (skipped) ++Profile.portalSkyUploadSkips;
	else ++Profile.portalSkyUploadBuilds;
}

void gl_GLES_RecordProfileMaterial(bool hit, bool upload)
{
	if (!Profile.active) return;
	if (hit) ++Profile.materialHits;
	else ++Profile.materialMisses;
	if (upload) ++Profile.materialUploads;
}

void gl_GLES_RecordProfileLightSelection(bool reused)
{
	if (!Profile.active) return;
	++Profile.lightSelectionRequests;
	if (reused) ++Profile.lightSelectionReuses;
}

void gl_GLES_RecordProfileResolve()
{
	if (Profile.active) ++Profile.resolveCount;
}

void gl_GLES_RecordProfileReadback()
{
	if (Profile.active) ++Profile.readbackCount;
}

void gl_GLES_RecordProfileWipeCapture()
{
	if (Profile.active) ++Profile.wipeCaptureCount;
}

static bool RenderNativeOffscreenTarget(int width, int height)
{
	if (!gl_GLES_CanUseResources() || width <= 0 || height <= 0)
		return false;
	if (Resources.cameraTarget.framebuffer == 0 ||
		Resources.cameraTarget.renderWidth != width || Resources.cameraTarget.renderHeight != height)
	{
		if (!gl_GLES_CreateRenderTarget(&Resources.cameraTarget, width, height, 1))
			return false;
		++PendingCameraTargetCreates;
	}
	gl_GLES_EndScene();
	NativeActiveTarget = &Resources.cameraTarget;
	NativeOffscreenRender = true;
	gl_GLES_RenderBootstrap(width, height);
	NativeOffscreenRender = false;
	NativeActiveTarget = NULL;
	return true;
}

bool gl_GLES_EndSceneToTexture(unsigned int targetTexture, int width, int height)
{
	if (!gl_GLES_CanUseResources() || targetTexture == 0 || width <= 0 || height <= 0)
		return false;

	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousTexture = 0;
	GLint previousTexture0 = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	const bool copied = RenderNativeOffscreenTarget(width, height) &&
		gl_GLESInternalCopyTargetToTexture(Resources.cameraTarget,
		static_cast<GLuint>(targetTexture));
	if (copied) ++PendingCameraCopies;
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	return copied;
}

bool gl_GLES_ReadScreenshot(unsigned char *rgba, int width, int height, int trueHeight)
{
	if (!gl_GLES_CanUseResources() || trueHeight < height || height <= 0)
		return false;
	const int targetHeight = Resources.sceneTarget.renderHeight;
	const int sourceBottom = static_cast<int>(
		static_cast<int64_t>((trueHeight - height) / 2) * targetHeight / trueHeight);
	const int sourceHeight = static_cast<int>(static_cast<int64_t>(height) * targetHeight / trueHeight);
	return gl_GLESInternalReadTarget(Resources.sceneTarget, rgba, width, height,
		sourceBottom, sourceHeight);
}

bool gl_GLES_WriteSavePic(FILE *file, int width, int height)
{
	if (!gl_GLES_CanUseResources() || file == NULL || width <= 0 || height <= 0)
		return false;
	GLint previousDrawFramebuffer = 0;
	GLint previousReadFramebuffer = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
	const bool written = RenderNativeOffscreenTarget(width, height) &&
		gl_GLESInternalWriteSavePic(file, Resources.cameraTarget, width, height);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	return written;
}

bool gl_GLES_WipeStart(int type)
{
	return gl_GLES_CanUseResources() &&
		gl_GLESInternalWipeStart(type, Resources.sceneTarget);
}

void gl_GLES_WipeEnd()
{
	gl_GLESInternalWipeEnd();
}

bool gl_GLES_WipeDo(int ticks)
{
	return gl_GLESInternalWipeDo(ticks);
}

void gl_GLES_WipeCleanup()
{
	gl_GLESInternalWipeCleanup();
}

bool gl_GLES_IsWipeInProgress()
{
	return gl_GLESInternalWipeIsActive();
}

void gl_GLES_BeginWipeOverlay()
{
	NativeWipeOverlayCollecting = gl_GLESInternalWipeIsActive();
}

void gl_GLES_EndWipeOverlay()
{
	NativeWipeOverlayCollecting = false;
}

bool gl_GLES_IsActive()
{
	return NativeBackendEnabled;
}

bool gl_GLES_CanUseResources()
{
	return NativeBackendEnabled && Resources.ready;
}

const FGLESNativeCapabilities &gl_GLES_GetCapabilities()
{
	return Capabilities;
}

void gl_GLES_PrintStartupLog()
{
	if (!CapabilitiesReady || !gl_GLES_IsProfileEnabled()) return;
	Printf("GL_VENDOR: %s\n", Capabilities.vendor);
	Printf("GL_RENDERER: %s\n", Capabilities.renderer);
	Printf("GL_VERSION: %s\n", Capabilities.version);
	Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", Capabilities.shadingLanguageVersion);
	const FGLESContextInfo &context = gl_GLES_GetContextInfo();
	Printf("GLES-compatible capabilities: %s %d.%d, max texture %d, texture units %d, fragment uniforms %d, extensions %d.\n",
		context.isGLES ? "OpenGL ES" : "desktop OpenGL",
		Capabilities.majorVersion, Capabilities.minorVersion, Capabilities.maxTextureSize,
		Capabilities.maxTextureUnits, Capabilities.maxFragmentUniformVectors, Capabilities.extensionCount);
	Printf("GLES core: buffers=%s, arrays=%s, uniform-buffers=%s, framebuffers=%s, depth-stencil=%s.\n",
		Capabilities.hasVertexBuffers ? "yes" : "no",
		Capabilities.hasVertexArrays ? "yes" : "no",
		Capabilities.hasUniformBuffers ? "yes" : "no",
		Capabilities.hasFramebuffers ? "yes" : "no",
		Capabilities.hasDepthStencil ? "yes" : "no");
	Printf("GLES optional: ETC2=%s, anisotropy=%s, ASTC=%s, debug=%s, multiview=%s.\n",
		Capabilities.hasEtc2Compression ? "yes" : "no",
		Capabilities.hasAnisotropicFiltering ? "yes" : "no",
		Capabilities.hasAstcCompression ? "yes" : "no",
		Capabilities.hasDebugLabels ? "yes" : "no",
		Capabilities.hasMultiview ? "yes" : "no");
	if (developer)
	{
		DPrintf("GLES extension list follows (%d entries):\n", Capabilities.extensionCount);
		for (GLint index = 0; index < Capabilities.extensionCount; ++index)
			DPrintf("  %s\n", glGetStringi(GL_EXTENSIONS, index));
	}
}

void gl_GLES_ResetState(int width, int height)
{
	NativeProgramBindingKnown = false;
	gl_GLESInternalResetState(width, height);
}

bool gl_GLES_ApplyBlendState(int srcBlend, int dstBlend, int blendEquation)
{
	return gl_GLESInternalApplyBlendState(gl_GLES_CanUseResources(), srcBlend, dstBlend, blendEquation);
}

void gl_GLES_GenerateMipmap()
{
	glGenerateMipmap(GL_TEXTURE_2D);
}

bool gl_GLES_CreateFlatBufferObjects(unsigned int *vbo, unsigned int *vao, unsigned int *ebo)
{
	if (!gl_GLES_CanUseResources() || vbo == NULL || vao == NULL || ebo == NULL) return false;
	glGenBuffers(1, vbo);
	glGenVertexArrays(1, vao);
	glGenBuffers(1, ebo);
	return CheckError("flat buffer object creation") == GL_NO_ERROR;
}

bool gl_GLES_UploadFlatBuffer(unsigned int vbo, unsigned int vao, unsigned int ebo,
	const void *vertices, int vertexCount, int vertexStride)
{
	if (!gl_GLES_CanUseResources() || vbo == 0 || vao == 0 || ebo == 0 || vertices == NULL ||
		vertexCount <= 0 || vertexCount > 65535 || vertexStride < 20)
		return false;
	GLushort *indices = new GLushort[vertexCount];
	for (int i = 0; i < vertexCount; ++i) indices[i] = static_cast<GLushort>(i);
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, vertexCount * vertexStride, vertices, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, vertexCount * sizeof(GLushort), indices, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertexStride, reinterpret_cast<const void *>(0));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vertexStride, reinterpret_cast<const void *>(16));
	glBindVertexArray(0);
	delete [] indices;
	return CheckError("flat buffer upload") == GL_NO_ERROR;
}

bool gl_GLES_UpdateFlatBuffer(unsigned int vbo, int offset, int size, const void *vertices)
{
	if (!gl_GLES_CanUseResources() || vbo == 0 || offset < 0 || size <= 0 || vertices == NULL) return false;
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferSubData(GL_ARRAY_BUFFER, offset, size, vertices);
	return CheckError("flat buffer update") == GL_NO_ERROR;
}

void gl_GLES_DestroyFlatBufferObjects(unsigned int vbo, unsigned int vao, unsigned int ebo)
{
	if (vbo != 0) glDeleteBuffers(1, &vbo);
	if (ebo != 0) glDeleteBuffers(1, &ebo);
	if (vao != 0) glDeleteVertexArrays(1, &vao);
}

void gl_GLES_BindFlatBuffer(unsigned int vao, unsigned int vbo)
{
	if (!gl_GLES_CanUseResources()) return;
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
}

#endif
