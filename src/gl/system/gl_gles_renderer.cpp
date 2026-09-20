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
#include <string.h>
#include <vector>

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

	struct FSceneBatch
	{
		GLsizei firstIndex;
		GLsizei indexCount;
		GLuint texture;
		GLuint brightmap;
		int brightmapDesaturation;
		bool masked;
		bool fog;
		bool translucent;
		bool repeat;
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
		float alphaCutoff;
		float sortDepth;
		float fogColor[3];
		float fogDensity;
		float glowTopColor[4];
		float glowBottomColor[4];
		size_t lightOffset;
		unsigned int lightCount;
		unsigned int lightNormalCount;
		unsigned int lightSubtractiveCount;
		float lightPlaneNormal[3];
		// Keep the camera that produced this batch beside its geometry. Nested
		// portal collection must not replace the outer scene's view uniforms.
		float viewProjection[16];
		float cameraPosition[3];
		bool clipPlaneEnabled;
		float clipPlane[4];
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
		GLuint checkerSampler;
		GLuint sceneSampler;
		int textureFilter;
		FGLESTargetDescriptor sceneTarget;
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
		GLint sceneFuzzTime;
		GLint sceneLightPositionRadius;
		GLint sceneLightColor;
		GLint sceneLightCounts;
		GLint sceneLightPlaneNormal;
		GLint sceneProjectedLights;
		GLint sceneDynamicLightTexture;
		GLint sceneClipPlane;
		GLint sceneClipPlaneEnabled;
		GLint maskedTextureUniform;
		GLint maskedBrightmapUniform;
		GLint maskedUseBrightmap;
		GLint maskedBrightmapDesaturation;
		GLint maskedViewProjection;
		GLint maskedUseTexture;
		GLint maskedModel;
		GLint maskedTextureTransform;
		GLint maskedCameraPosition;
		GLint maskedObjectColor;
		GLint maskedAlphaCutoff;
		GLint maskedMaterialFlags;
		GLint maskedFuzzTime;
		GLint maskedLightPositionRadius;
		GLint maskedLightColor;
		GLint maskedLightCounts;
		GLint maskedLightPlaneNormal;
		GLint maskedProjectedLights;
		GLint maskedDynamicLightTexture;
		GLint maskedClipPlane;
		GLint maskedClipPlaneEnabled;
		GLint paletteTextureUniform;
		GLint paletteBrightmapUniform;
		GLint paletteUseBrightmap;
		GLint paletteBrightmapDesaturation;
		GLint paletteViewProjection;
		GLint paletteUseTexture;
		GLint paletteModel;
		GLint paletteTextureTransform;
		GLint paletteCameraPosition;
		GLint paletteObjectColor;
		GLint paletteMaterialFlags;
		GLint paletteFuzzTime;
		GLint paletteLightPositionRadius;
		GLint paletteLightColor;
		GLint paletteLightCounts;
		GLint paletteLightPlaneNormal;
		GLint paletteProjectedLights;
		GLint paletteDynamicLightTexture;
		GLint paletteClipPlane;
		GLint paletteClipPlaneEnabled;
		GLint fogViewProjection;
		GLint fogTextureUniform;
		GLint fogBrightmapUniform;
		GLint fogUseBrightmap;
		GLint fogBrightmapDesaturation;
		GLint fogUseTexture;
		GLint fogModel;
		GLint fogTextureTransform;
		GLint fogCameraPosition;
		GLint fogObjectColor;
		GLint fogMaterialFlags;
		GLint fogFuzzTime;
		GLint fogLightPositionRadius;
		GLint fogLightColor;
		GLint fogLightCounts;
		GLint fogLightPlaneNormal;
		GLint fogProjectedLights;
		GLint fogDynamicLightTexture;
		GLint fogClipPlane;
		GLint fogClipPlaneEnabled;
		GLint fogMaskedViewProjection;
		GLint fogMaskedTextureUniform;
		GLint fogMaskedBrightmapUniform;
		GLint fogMaskedUseBrightmap;
		GLint fogMaskedBrightmapDesaturation;
		GLint fogMaskedUseTexture;
		GLint fogMaskedModel;
		GLint fogMaskedTextureTransform;
		GLint fogMaskedCameraPosition;
		GLint fogMaskedObjectColor;
		GLint fogMaskedAlphaCutoff;
		GLint fogMaskedMaterialFlags;
		GLint fogMaskedFuzzTime;
		GLint fogMaskedColor;
		GLint fogMaskedDensity;
		GLint fogMaskedLightPositionRadius;
		GLint fogMaskedLightColor;
		GLint fogMaskedLightCounts;
		GLint fogMaskedLightPlaneNormal;
		GLint fogMaskedProjectedLights;
		GLint fogMaskedDynamicLightTexture;
		GLint fogMaskedClipPlane;
		GLint fogMaskedClipPlaneEnabled;
		GLint fogColor;
		GLint fogDensity;
		unsigned int frame;
		GLsizei sceneIndexCount;
		std::vector<FSceneVertex> sceneVertices;
		// GLES 3.2 core indices keep large model surfaces in the shared stream.
		std::vector<GLuint> sceneIndices;
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
		unsigned int sceneOpaqueBatchMerges;
		unsigned int sceneProgramBinds;
		unsigned int sceneProgramBindSkips;
		unsigned int sceneTextureBinds;
		unsigned int sceneTextureBindSkips;
		unsigned int sceneSamplerBinds;
		unsigned int sceneSamplerBindSkips;
		unsigned int sceneOpaqueStateSets;
		unsigned int sceneOpaqueStateSkips;
		unsigned int sceneLightUniformUploads;
		unsigned int sceneLightUniformUploadSkips;
		unsigned int sceneGlowUniformUploads;
		unsigned int sceneGlowUniformUploadSkips;
		float viewProjection[16];
		float cameraX;
		float cameraY;
		float cameraZ;
		float cameraYaw;
		float cameraPitch;
		float cameraFieldOfView;
		float cameraAspect;
		float cameraFovRatio;
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
	static bool NativePortalCapacityLogged = false;
	static bool FlatCollectionDeferred = false;
	static FGLESTargetDescriptor *NativeActiveTarget = NULL;
	static bool NativeOffscreenRender = false;
	static bool NativeWipeOverlayCollecting = false;
	static GLuint NativeBoundProgram = 0;
	static bool NativeProgramBindingKnown = false;
	static GLuint NativeGlowPrograms[5] = {};
	static GLint NativeGlowTopLocations[5] = { -1, -1, -1, -1, -1 };
	static GLint NativeGlowBottomLocations[5] = { -1, -1, -1, -1, -1 };
	static GLfloat NativeGlowTopValues[5][4] = {};
	static GLfloat NativeGlowBottomValues[5][4] = {};
	static bool NativeGlowValuesKnown[5] = {};

	static void SetNativeFullViewport(int width, int height)
	{
		glViewport(0, 0, width, height);
		glScissor(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
	}

	static void ResetNativeGlowLocations()
	{
		memset(NativeGlowPrograms, 0, sizeof(NativeGlowPrograms));
		for (int i = 0; i < 5; ++i)
		{
			NativeGlowTopLocations[i] = -1;
			NativeGlowBottomLocations[i] = -1;
			NativeGlowValuesKnown[i] = false;
		}
	}
	static void SetNativeGlowUniforms(GLuint program, const FSceneBatch &batch)
	{
		int slot = -1;
		const GLuint knownPrograms[5] =
		{
			Resources.sceneProgram, Resources.maskedProgram, Resources.paletteProgram,
			Resources.fogProgram, Resources.fogMaskedProgram
		};
		for (int i = 0; i < 5; ++i)
			if (knownPrograms[i] == program) { slot = i; break; }
		if (slot < 0) return;
		if (NativeGlowPrograms[slot] != program)
		{
			NativeGlowPrograms[slot] = program;
			NativeGlowTopLocations[slot] = glGetUniformLocation(program, "u_glow_top_color");
			NativeGlowBottomLocations[slot] = glGetUniformLocation(program, "u_glow_bottom_color");
			NativeGlowValuesKnown[slot] = false;
		}
		const bool unchanged = NativeGlowValuesKnown[slot] &&
			memcmp(NativeGlowTopValues[slot], batch.glowTopColor, sizeof(batch.glowTopColor)) == 0 &&
			memcmp(NativeGlowBottomValues[slot], batch.glowBottomColor, sizeof(batch.glowBottomColor)) == 0;
		if (unchanged)
		{
			++Resources.sceneGlowUniformUploadSkips;
			return;
		}
		if (NativeGlowTopLocations[slot] >= 0)
			glUniform4fv(NativeGlowTopLocations[slot], 1, batch.glowTopColor);
		if (NativeGlowBottomLocations[slot] >= 0)
			glUniform4fv(NativeGlowBottomLocations[slot], 1, batch.glowBottomColor);
		memcpy(NativeGlowTopValues[slot], batch.glowTopColor, sizeof(batch.glowTopColor));
		memcpy(NativeGlowBottomValues[slot], batch.glowBottomColor, sizeof(batch.glowBottomColor));
		NativeGlowValuesKnown[slot] = true;
		++Resources.sceneGlowUniformUploads;
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

	// The native scene is the normal path. The debug switch keeps the indexed
	// test surface available when checking context and shader setup.
	CVAR(Bool, gl_gles_test_pattern, false, CVAR_DEBUGONLY)
	CVAR(Bool, gl_gles_shader_test_failure, false, CVAR_DEBUGONLY)

	static bool IsPaletteTexture(GLuint texture)
	{
		return gl_GLESInternalIsPaletteTexture(texture);
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
		GLuint program = gl_GLES_LinkProgram(vertexSource, fragmentSource, label, log, sizeof(log));
		if (program == 0)
		{
			DPrintf("Zandronum GLES %s defines:\n%s\nvertex source:\n%s\nfragment source:\n%s\n",
				label, defines, vertexSource, fragmentSource);
			I_FatalError("Zandronum GLES %s program failed: %s", label, log);
		}
		return program;
	}

	static void DeleteResources(bool clearTextureCache)
	{
		gl_GLESInternalSceneInvalidateBuffers();
		if (Resources.sceneProgram != 0) glDeleteProgram(Resources.sceneProgram);
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
		if (Resources.checkerSampler != 0) glDeleteSamplers(1, &Resources.checkerSampler);
		if (Resources.sceneSampler != 0) glDeleteSamplers(1, &Resources.sceneSampler);
		gl_GLES_DestroyRenderTarget(&Resources.sceneTarget);
		gl_GLES_DestroyRenderTarget(&Resources.cameraTarget);
		Resources.sceneProgram = 0;
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
		Resources.checkerSampler = 0;
		Resources.sceneSampler = 0;
		Resources.textureFilter = -1;
		Resources.sceneTarget = {};
		Resources.cameraTarget = {};
		Resources.viewContract = {};
		NativeActiveTarget = NULL;
		NativeOffscreenRender = false;
		ResetNativeGlowLocations();
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
		Resources.sceneLightPositionRadius = -1;
		Resources.sceneLightColor = -1;
		Resources.sceneLightCounts = -1;
		Resources.sceneLightPlaneNormal = -1;
		Resources.sceneProjectedLights = -1;
		Resources.sceneDynamicLightTexture = -1;
		Resources.sceneClipPlane = -1;
		Resources.sceneClipPlaneEnabled = -1;
		Resources.maskedTextureUniform = -1;
		Resources.maskedBrightmapUniform = -1;
		Resources.maskedUseBrightmap = -1;
		Resources.maskedBrightmapDesaturation = -1;
		Resources.maskedViewProjection = -1;
		Resources.maskedUseTexture = -1;
		Resources.maskedModel = -1;
		Resources.maskedTextureTransform = -1;
		Resources.maskedCameraPosition = -1;
		Resources.maskedObjectColor = -1;
		Resources.maskedAlphaCutoff = -1;
		Resources.maskedMaterialFlags = -1;
		Resources.maskedLightPositionRadius = -1;
		Resources.maskedLightColor = -1;
		Resources.maskedLightCounts = -1;
		Resources.maskedLightPlaneNormal = -1;
		Resources.maskedProjectedLights = -1;
		Resources.maskedDynamicLightTexture = -1;
		Resources.maskedClipPlane = -1;
		Resources.maskedClipPlaneEnabled = -1;
		Resources.paletteTextureUniform = -1;
		Resources.paletteBrightmapUniform = -1;
		Resources.paletteUseBrightmap = -1;
		Resources.paletteBrightmapDesaturation = -1;
		Resources.paletteViewProjection = -1;
		Resources.paletteUseTexture = -1;
		Resources.paletteModel = -1;
		Resources.paletteTextureTransform = -1;
		Resources.paletteCameraPosition = -1;
		Resources.paletteObjectColor = -1;
		Resources.paletteMaterialFlags = -1;
		Resources.paletteLightPositionRadius = -1;
		Resources.paletteLightColor = -1;
		Resources.paletteLightCounts = -1;
		Resources.paletteLightPlaneNormal = -1;
		Resources.paletteProjectedLights = -1;
		Resources.paletteDynamicLightTexture = -1;
		Resources.paletteClipPlane = -1;
		Resources.paletteClipPlaneEnabled = -1;
		Resources.fogViewProjection = -1;
		Resources.fogTextureUniform = -1;
		Resources.fogBrightmapUniform = -1;
		Resources.fogUseBrightmap = -1;
		Resources.fogBrightmapDesaturation = -1;
		Resources.fogUseTexture = -1;
		Resources.fogModel = -1;
		Resources.fogTextureTransform = -1;
		Resources.fogCameraPosition = -1;
		Resources.fogObjectColor = -1;
		Resources.fogMaterialFlags = -1;
		Resources.fogLightPositionRadius = -1;
		Resources.fogLightColor = -1;
		Resources.fogLightCounts = -1;
		Resources.fogLightPlaneNormal = -1;
		Resources.fogProjectedLights = -1;
		Resources.fogDynamicLightTexture = -1;
		Resources.fogClipPlane = -1;
		Resources.fogClipPlaneEnabled = -1;
		Resources.fogMaskedViewProjection = -1;
		Resources.fogMaskedTextureUniform = -1;
		Resources.fogMaskedBrightmapUniform = -1;
		Resources.fogMaskedUseBrightmap = -1;
		Resources.fogMaskedBrightmapDesaturation = -1;
		Resources.fogMaskedUseTexture = -1;
		Resources.fogMaskedModel = -1;
		Resources.fogMaskedTextureTransform = -1;
		Resources.fogMaskedCameraPosition = -1;
		Resources.fogMaskedObjectColor = -1;
		Resources.fogMaskedAlphaCutoff = -1;
		Resources.fogMaskedMaterialFlags = -1;
		Resources.fogMaskedColor = -1;
		Resources.fogMaskedDensity = -1;
		Resources.fogMaskedLightPositionRadius = -1;
		Resources.fogMaskedLightColor = -1;
		Resources.fogMaskedLightCounts = -1;
		Resources.fogMaskedLightPlaneNormal = -1;
		Resources.fogMaskedProjectedLights = -1;
		Resources.fogMaskedDynamicLightTexture = -1;
		Resources.fogMaskedClipPlane = -1;
		Resources.fogMaskedClipPlaneEnabled = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
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
		gl_GLESInternalSceneInvalidateBuffers();
		Resources.sceneProgram = 0;
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
		gl_GLES_InvalidateRenderTarget(&Resources.cameraTarget);
		Resources.viewContract = {};
		NativeActiveTarget = NULL;
		NativeOffscreenRender = false;
		ResetNativeGlowLocations();
		gl_GLESInternalWipeContextLost();
		Resources.checkerSampler = 0;
		Resources.sceneSampler = 0;
		Resources.textureFilter = -1;
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
		Resources.sceneLightPositionRadius = -1;
		Resources.sceneLightColor = -1;
		Resources.sceneLightCounts = -1;
		Resources.sceneLightPlaneNormal = -1;
		Resources.sceneProjectedLights = -1;
		Resources.sceneDynamicLightTexture = -1;
		Resources.sceneClipPlane = -1;
		Resources.sceneClipPlaneEnabled = -1;
		Resources.maskedTextureUniform = -1;
		Resources.maskedBrightmapUniform = -1;
		Resources.maskedUseBrightmap = -1;
		Resources.maskedBrightmapDesaturation = -1;
		Resources.maskedViewProjection = -1;
		Resources.maskedUseTexture = -1;
		Resources.maskedModel = -1;
		Resources.maskedTextureTransform = -1;
		Resources.maskedCameraPosition = -1;
		Resources.maskedObjectColor = -1;
		Resources.maskedAlphaCutoff = -1;
		Resources.maskedMaterialFlags = -1;
		Resources.maskedLightPositionRadius = -1;
		Resources.maskedLightColor = -1;
		Resources.maskedLightCounts = -1;
		Resources.maskedLightPlaneNormal = -1;
		Resources.maskedProjectedLights = -1;
		Resources.maskedDynamicLightTexture = -1;
		Resources.maskedClipPlane = -1;
		Resources.maskedClipPlaneEnabled = -1;
		Resources.paletteTextureUniform = -1;
		Resources.paletteBrightmapUniform = -1;
		Resources.paletteUseBrightmap = -1;
		Resources.paletteBrightmapDesaturation = -1;
		Resources.paletteViewProjection = -1;
		Resources.paletteUseTexture = -1;
		Resources.paletteModel = -1;
		Resources.paletteTextureTransform = -1;
		Resources.paletteCameraPosition = -1;
		Resources.paletteObjectColor = -1;
		Resources.paletteMaterialFlags = -1;
		Resources.paletteLightPositionRadius = -1;
		Resources.paletteLightColor = -1;
		Resources.paletteLightCounts = -1;
		Resources.paletteLightPlaneNormal = -1;
		Resources.paletteProjectedLights = -1;
		Resources.paletteDynamicLightTexture = -1;
		Resources.paletteClipPlane = -1;
		Resources.paletteClipPlaneEnabled = -1;
		Resources.fogViewProjection = -1;
		Resources.fogTextureUniform = -1;
		Resources.fogBrightmapUniform = -1;
		Resources.fogUseBrightmap = -1;
		Resources.fogBrightmapDesaturation = -1;
		Resources.fogUseTexture = -1;
		Resources.fogModel = -1;
		Resources.fogTextureTransform = -1;
		Resources.fogCameraPosition = -1;
		Resources.fogObjectColor = -1;
		Resources.fogMaterialFlags = -1;
		Resources.fogLightPositionRadius = -1;
		Resources.fogLightColor = -1;
		Resources.fogLightCounts = -1;
		Resources.fogLightPlaneNormal = -1;
		Resources.fogProjectedLights = -1;
		Resources.fogDynamicLightTexture = -1;
		Resources.fogClipPlane = -1;
		Resources.fogClipPlaneEnabled = -1;
		Resources.fogMaskedViewProjection = -1;
		Resources.fogMaskedTextureUniform = -1;
		Resources.fogMaskedBrightmapUniform = -1;
		Resources.fogMaskedUseBrightmap = -1;
		Resources.fogMaskedBrightmapDesaturation = -1;
		Resources.fogMaskedUseTexture = -1;
		Resources.fogMaskedModel = -1;
		Resources.fogMaskedTextureTransform = -1;
		Resources.fogMaskedCameraPosition = -1;
		Resources.fogMaskedObjectColor = -1;
		Resources.fogMaskedAlphaCutoff = -1;
		Resources.fogMaskedMaterialFlags = -1;
		Resources.fogMaskedColor = -1;
		Resources.fogMaskedDensity = -1;
		Resources.fogMaskedLightPositionRadius = -1;
		Resources.fogMaskedLightColor = -1;
		Resources.fogMaskedLightCounts = -1;
		Resources.fogMaskedLightPlaneNormal = -1;
		Resources.fogMaskedProjectedLights = -1;
		Resources.fogMaskedDynamicLightTexture = -1;
		Resources.fogMaskedClipPlane = -1;
		Resources.fogMaskedClipPlaneEnabled = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
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

		glGenSamplers(1, &Resources.checkerSampler);
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glGenSamplers(1, &Resources.sceneSampler);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		CheckError("sampler setup");
	}

	static void ConfigureNativeSamplers()
	{
		if (Resources.checkerSampler == 0 || Resources.sceneSampler == 0) return;
		int filter = gl_texture_filter;
		if (filter < 0 || filter >= 6) filter = 0;
		if (Resources.textureFilter == filter) return;
		const TexFilter_s &settings = TexFilter[filter];
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_MIN_FILTER, settings.minfilter);
		glSamplerParameteri(Resources.checkerSampler, GL_TEXTURE_MAG_FILTER, settings.magfilter);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_MIN_FILTER, settings.minfilter);
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_MAG_FILTER, settings.magfilter);
		Resources.textureFilter = filter;
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
		const float radians = fieldOfView * 3.14159265359f / 180.0f;
		const float verticalRadians = 2.0f * atanf(tanf(radians * 0.5f) /
			std::max(fovRatio, 0.01f));
		const float focal = 1.0f / tanf(verticalRadians * 0.5f);
		const float nearPlane = 5.0f;
		const float farPlane = 65536.0f;
		const float invRange = 1.0f / (nearPlane - farPlane);
		float projection[16] =
		{
			focal / (aspect > 0.01f ? aspect : 1.0f), 0.0f, 0.0f, 0.0f,
			0.0f, focal, 0.0f, 0.0f,
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
		if (lightData == NULL || lightCounts == NULL)
		{
			batch.lightOffset = 0;
			batch.lightCount = 0;
			batch.lightNormalCount = 0;
			batch.lightSubtractiveCount = 0;
			return;
		}
		FGLESSceneLightSelection selection = {};
		gl_GLESInternalSceneAppendLights(lightData, lightCounts, &selection);
		batch.lightOffset = selection.streamOffset;
		batch.lightCount = selection.lightCount;
		batch.lightNormalCount = selection.normalCount;
		batch.lightSubtractiveCount = selection.subtractiveCount;
	}

	static void GetNativeLightUpload(const FSceneBatch &batch, unsigned int firstLight,
		unsigned int maxLightCount,
		const GLfloat *&positions, const GLfloat *&colors, unsigned int &normalCount,
		unsigned int &subtractiveCount, unsigned int &lightCount)
	{
		FGLESSceneLightSelection selection = {};
		selection.streamOffset = batch.lightOffset;
		selection.lightCount = batch.lightCount;
		selection.normalCount = batch.lightNormalCount;
		selection.subtractiveCount = batch.lightSubtractiveCount;
		FGLESSceneLightUpload upload = {};
		gl_GLESInternalSceneGetLightUpload(selection, firstLight, maxLightCount, &upload);
		positions = upload.positions;
		colors = upload.colors;
		normalCount = upload.normalCount;
		subtractiveCount = upload.subtractiveCount;
		lightCount = upload.lightCount;
	}

	static void DrawNativeLightOverflow(const FSceneBatch &batch, GLuint dynamicLightTexture,
		GLuint program, GLint lightPositionRadius, GLint lightColor, GLint lightCounts,
		GLint projectedLights, GLsizei indexCount, size_t firstIndex)
	{
		FGLESSceneLightPass pass = {};
		pass.selection.streamOffset = batch.lightOffset;
		pass.selection.lightCount = batch.lightCount;
		pass.selection.normalCount = batch.lightNormalCount;
		pass.selection.subtractiveCount = batch.lightSubtractiveCount;
		memcpy(pass.planeNormal, batch.lightPlaneNormal, sizeof(pass.planeNormal));
		pass.dynamicLightTexture = dynamicLightTexture;
		pass.checkerTexture = Resources.checkerTexture;
		pass.program = program;
		pass.positionUniform = lightPositionRadius;
		pass.colorUniform = lightColor;
		pass.countsUniform = lightCounts;
		pass.projectedUniform = projectedLights;
		pass.indexCount = indexCount;
		pass.firstIndex = firstIndex;
		pass.translucent = batch.translucent;
		pass.blendMode = static_cast<int>(batch.blendMode);
		gl_GLESInternalSceneDrawLightOverflow(pass);
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

	static void CaptureBatchView(FSceneBatch &batch)
	{
		memcpy(batch.viewProjection, Resources.viewProjection, sizeof(batch.viewProjection));
		batch.cameraPosition[0] = Resources.cameraX;
		batch.cameraPosition[1] = Resources.cameraY;
		batch.cameraPosition[2] = Resources.cameraZ;
		batch.clipPlaneEnabled = Resources.clipPlaneEnabled;
		memcpy(batch.clipPlane, Resources.clipPlane, sizeof(batch.clipPlane));
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
		// Keep every order-sensitive path as its own command. Consecutive opaque
		// world quads sharing the exact selected-light payload are safe to submit together.
		return !previous.translucent && !batch.translucent &&
			previous.flat == batch.flat && !previous.hud && !batch.hud &&
			!previous.wipeOverlay && !batch.wipeOverlay && !previous.model && !batch.model &&
			!previous.cullBackFaces && !batch.cullBackFaces && !previous.skyMask && !batch.skyMask &&
			!previous.flood && !batch.flood && !previous.portalMask && !batch.portalMask &&
			previous.portalId < 0 && batch.portalId < 0 &&
			previous.firstIndex + previous.indexCount == batch.firstIndex &&
			previous.texture == batch.texture && previous.brightmap == batch.brightmap &&
			previous.brightmapDesaturation == batch.brightmapDesaturation &&
			previous.masked == batch.masked && previous.fog == batch.fog &&
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
			previous.clipPlaneEnabled == batch.clipPlaneEnabled &&
			memcmp(previous.fogColor, batch.fogColor, sizeof(batch.fogColor)) == 0 &&
			previous.fogDensity == batch.fogDensity &&
			memcmp(previous.glowTopColor, batch.glowTopColor, sizeof(batch.glowTopColor)) == 0 &&
			memcmp(previous.glowBottomColor, batch.glowBottomColor, sizeof(batch.glowBottomColor)) == 0 &&
			memcmp(previous.viewProjection, batch.viewProjection, sizeof(batch.viewProjection)) == 0 &&
			memcmp(previous.cameraPosition, batch.cameraPosition, sizeof(batch.cameraPosition)) == 0 &&
			memcmp(previous.clipPlane, batch.clipPlane, sizeof(batch.clipPlane)) == 0;
	}

	static void AppendOpaqueBatch(FSceneBatch &batch)
	{
		if (!Resources.sceneBatches.empty() && CanMergeOpaqueBatches(Resources.sceneBatches.back(), batch))
		{
			Resources.sceneBatches.back().indexCount += batch.indexCount;
			++Resources.sceneOpaqueBatchMerges;
			return;
		}
		Resources.sceneBatches.push_back(batch);
	}

	static void AddSceneQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked, bool fog, bool translucent, bool repeat,
		EGLESBlendMode blendMode, const float *fogColor, float fogDensity, unsigned int materialFlags,
		const float *lightData, const unsigned int *lightCounts, GLuint brightmap = 0,
		int brightmapDesaturation = 0, const float *topGlowColor = NULL,
		const float *bottomGlowColor = NULL, bool customBlend = false,
		GLenum sourceBlend = GL_SRC_ALPHA, GLenum destinationBlend = GL_ONE_MINUS_SRC_ALPHA,
	float alphaCutoff = 0.5f, bool hud = false)
	{
		if (!CanAppendSceneGeometry(4, 6, "quad")) return;
		const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
		Resources.sceneVertices.push_back(a);
		Resources.sceneVertices.push_back(b);
		Resources.sceneVertices.push_back(c);
		Resources.sceneVertices.push_back(d);
		const unsigned int second = first + 1;
		const unsigned int third = first + 2;
		const unsigned int fourth = first + 3;
		const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
		Resources.sceneIndices.push_back(first);
		Resources.sceneIndices.push_back(second);
		Resources.sceneIndices.push_back(third);
		Resources.sceneIndices.push_back(third);
		Resources.sceneIndices.push_back(fourth);
		Resources.sceneIndices.push_back(first);
		const float centerX = (a.x + b.x + c.x + d.x) * 0.25f;
		const float centerY = (a.y + b.y + c.y + d.y) * 0.25f;
		const float centerZ = (a.z + b.z + c.z + d.z) * 0.25f;
		const float dx = centerX - Resources.cameraX;
		const float dy = centerY - Resources.cameraY;
		const float dz = centerZ - Resources.cameraZ;
		FSceneBatch batch = {};
		CaptureBatchView(batch);
		batch.firstIndex = firstIndex;
		batch.indexCount = 6;
		batch.texture = texture;
		batch.brightmap = brightmap;
		batch.brightmapDesaturation = brightmapDesaturation;
		batch.masked = masked;
		batch.fog = fog;
		batch.translucent = translucent || blendMode != GLES_BLEND_OPAQUE;
		batch.repeat = repeat;
		batch.palette = IsPaletteTexture(texture);
		batch.materialFlags = materialFlags;
		batch.blendMode = blendMode;
		batch.customBlend = customBlend;
		batch.sourceBlend = sourceBlend;
		batch.destinationBlend = destinationBlend;
		batch.alphaCutoff = alphaCutoff;
		batch.hud = hud;
		CopyNativeLightData(batch, lightData, lightCounts);
		if (batch.lightCount > 0)
			SetLightPlaneNormal(batch, a, b, c);
		batch.sortDepth = dx * dx + dy * dy + dz * dz;
		if (fogColor != NULL)
		{
			batch.fogColor[0] = ClampUnit(fogColor[0]);
			batch.fogColor[1] = ClampUnit(fogColor[1]);
			batch.fogColor[2] = ClampUnit(fogColor[2]);
		}
		batch.fogDensity = std::max(0.0f, fogDensity);
		if (topGlowColor != NULL) memcpy(batch.glowTopColor, topGlowColor, sizeof(batch.glowTopColor));
		if (bottomGlowColor != NULL) memcpy(batch.glowBottomColor, bottomGlowColor, sizeof(batch.glowBottomColor));
		AppendOpaqueBatch(batch);
	}

	static void AddHUDQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked,
		EGLESBlendMode blendMode, unsigned int materialFlags, bool customBlend = false,
		GLenum sourceBlend = GL_SRC_ALPHA, GLenum destinationBlend = GL_ONE_MINUS_SRC_ALPHA,
		float alphaCutoff = 0.5f)
	{
		const size_t batchCount = Resources.sceneBatches.size();
		const bool translucent = masked || blendMode != GLES_BLEND_OPAQUE || a.a < 0.999f ||
			b.a < 0.999f || c.a < 0.999f || d.a < 0.999f;
		AddSceneQuad(a, b, c, d, texture, masked, false, translucent, false, blendMode, NULL, 0.0f, materialFlags,
			NULL, NULL, 0, 0, NULL, NULL, customBlend, sourceBlend, destinationBlend, alphaCutoff, true);
		if (Resources.sceneBatches.size() <= batchCount) return;
		FSceneBatch &batch = Resources.sceneBatches.back();
		if (!batch.wipeOverlay || Resources.sceneBatches.size() < 2) return;

		FSceneBatch &previous = Resources.sceneBatches[Resources.sceneBatches.size() - 2];
		const bool compatible = previous.hud && previous.wipeOverlay &&
			previous.firstIndex + previous.indexCount == batch.firstIndex &&
			previous.texture == batch.texture && previous.brightmap == batch.brightmap &&
			previous.brightmapDesaturation == batch.brightmapDesaturation &&
			previous.masked == batch.masked && previous.fog == batch.fog &&
			previous.translucent == batch.translucent && previous.repeat == batch.repeat &&
			previous.palette == batch.palette && previous.model == batch.model &&
			previous.cullBackFaces == batch.cullBackFaces &&
			previous.materialFlags == batch.materialFlags &&
			previous.customBlend == batch.customBlend && previous.sourceBlend == batch.sourceBlend &&
			previous.destinationBlend == batch.destinationBlend && previous.alphaCutoff == batch.alphaCutoff &&
			previous.blendMode == batch.blendMode && previous.portalId == batch.portalId &&
			previous.clipPlaneEnabled == batch.clipPlaneEnabled &&
			memcmp(previous.viewProjection, batch.viewProjection, sizeof(batch.viewProjection)) == 0 &&
			memcmp(previous.cameraPosition, batch.cameraPosition, sizeof(batch.cameraPosition)) == 0 &&
			memcmp(previous.clipPlane, batch.clipPlane, sizeof(batch.clipPlane)) == 0;
		if (compatible)
		{
			previous.indexCount += batch.indexCount;
			Resources.sceneBatches.pop_back();
		}
	}

	static void SetNativeDecalDepthBias(const FSceneBatch &batch, bool enabled)
	{
		if ((batch.materialFlags & GLES_MATERIAL_DECAL) == 0) return;
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

	static void UploadSceneGeometry()
	{
		Resources.sceneIndexCount = static_cast<GLsizei>(Resources.sceneIndices.size());
		Resources.sceneReady = Resources.sceneIndexCount > 0;
		if (!Resources.sceneReady) return;
		gl_GLESInternalSceneUploadGeometry(Resources.sceneVertexArray,
			Resources.sceneVertexBuffer, Resources.sceneIndexBuffer,
			&Resources.sceneVertices[0], Resources.sceneVertices.size() * sizeof(FSceneVertex),
			Resources.sceneIndices.data(), Resources.sceneIndices.size());
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
			Resources.sceneBatches.push_back(batch);
			sky.maskBatches.push_back(Resources.sceneBatches.size() - 1);
		}
		sky.capsAdded = true;
	}

	static bool BuildResources(int width, int height)
	{
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
			"void main() { vec4 world_position = u_model * vec4(a_position, 1.0); gl_Position = u_view_projection * world_position; if (u_sky_depth) gl_Position.z = clamp(gl_Position.z, -gl_Position.w, gl_Position.w); v_world_position = world_position.xyz; v_uv = a_uv * u_texture_transform.xy + u_texture_transform.zw; v_color = vec4(a_color.rgb * u_object_color.rgb, (u_sky_fog ? 1.0 : a_color.a) * u_object_color.a); v_camera_distance = distance(world_position.xyz, u_camera_position); v_glow_distance = a_secondary; }\n";
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
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[32];\n"
			"uniform vec4 u_light_color[32];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform int u_light_only_mode;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += u_glow_top_color.rgb * (1.0 - v_glow_distance.x / u_glow_top_color.a); if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += u_glow_bottom_color.rgb * (1.0 - v_glow_distance.y / u_glow_bottom_color.a); return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 32; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } if (u_light_only_mode == 1) lighting = regular; else if (u_light_only_mode == 2) lighting = subtractive; else if (u_light_only_mode == 3) lighting = additive; else lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_light_only_mode != 0) { frag_color = vec4(lighting, 0.0); return; } if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a = texel.a * v_color.a; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; color.rgb = apply_glow(color.rgb); frag_color = color; }\n";
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
			"uniform float u_alpha_cutoff;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[32];\n"
			"uniform vec4 u_light_color[32];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform int u_light_only_mode;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += u_glow_top_color.rgb * (1.0 - v_glow_distance.x / u_glow_top_color.a); if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += u_glow_bottom_color.rgb * (1.0 - v_glow_distance.y / u_glow_bottom_color.a); return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 32; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } if (u_light_only_mode == 1) lighting = regular; else if (u_light_only_mode == 2) lighting = subtractive; else if (u_light_only_mode == 3) lighting = additive; else lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; if ((u_material_flags & 1) != 0 ? texel.a <= 0.0 : texel.a < u_alpha_cutoff) discard; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_light_only_mode != 0) { frag_color = vec4(lighting, 0.0); return; } if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a = texel.a * v_color.a; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; color.rgb = apply_glow(color.rgb); frag_color = color; }\n";
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
			"uniform vec4 u_fog_color;\n"
			"uniform float u_fog_density;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[32];\n"
			"uniform vec4 u_light_color[32];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform int u_light_only_mode;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += u_glow_top_color.rgb * (1.0 - v_glow_distance.x / u_glow_top_color.a); if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += u_glow_bottom_color.rgb * (1.0 - v_glow_distance.y / u_glow_bottom_color.a); return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 32; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } if (u_light_only_mode == 1) lighting = regular; else if (u_light_only_mode == 2) lighting = subtractive; else if (u_light_only_mode == 3) lighting = additive; else lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_light_only_mode != 0) { frag_color = vec4(lighting, 0.0); return; } if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a = texel.a * v_color.a; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; float fog = clamp(exp(-u_fog_density * v_camera_distance), 0.0, 1.0); if ((u_material_flags & 256) != 0) { frag_color = vec4(u_fog_color.rgb, 1.0 - fog); return; } color.rgb = apply_glow(mix(u_fog_color.rgb, color.rgb, fog)); frag_color = color; }\n";
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
			"uniform vec4 u_fog_color;\n"
			"uniform float u_fog_density;\n"
			"uniform float u_alpha_cutoff;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[32];\n"
			"uniform vec4 u_light_color[32];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform int u_light_only_mode;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += u_glow_top_color.rgb * (1.0 - v_glow_distance.x / u_glow_top_color.a); if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += u_glow_bottom_color.rgb * (1.0 - v_glow_distance.y / u_glow_bottom_color.a); return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 32; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } if (u_light_only_mode == 1) lighting = regular; else if (u_light_only_mode == 2) lighting = subtractive; else if (u_light_only_mode == 3) lighting = additive; else lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; if ((u_material_flags & 1) != 0 ? texel.a <= 0.0 : texel.a < u_alpha_cutoff) discard; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_light_only_mode != 0) { frag_color = vec4(lighting, 0.0); return; } if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a = texel.a * v_color.a; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; float fog = clamp(exp(-u_fog_density * v_camera_distance), 0.0, 1.0); color.rgb = apply_glow(mix(u_fog_color.rgb, color.rgb, fog)); frag_color = color; }\n";
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
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[32];\n"
			"uniform vec4 u_light_color[32];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform int u_light_only_mode;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"uniform vec4 u_glow_top_color;\n"
			"uniform vec4 u_glow_bottom_color;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"vec3 apply_glow(vec3 color) { if (u_glow_top_color.a > 0.0 && v_glow_distance.x < u_glow_top_color.a) color += u_glow_top_color.rgb * (1.0 - v_glow_distance.x / u_glow_top_color.a); if (u_glow_bottom_color.a > 0.0 && v_glow_distance.y < u_glow_bottom_color.a) color += u_glow_bottom_color.rgb * (1.0 - v_glow_distance.y / u_glow_bottom_color.a); return min(color, vec3(1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 32; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } if (u_light_only_mode == 1) lighting = regular; else if (u_light_only_mode == 2) lighting = subtractive; else if (u_light_only_mode == 3) lighting = additive; else lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel.rgb = clamp(texel.rgb, vec3(0.0), vec3(1.0)); vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_light_only_mode != 0) { frag_color = vec4(lighting, 0.0); return; } if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a = texel.a * v_color.a; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; color.rgb = apply_glow(color.rgb); frag_color = color; }\n";
		Resources.sceneProgram = LinkProgram(sceneVertexSource, sceneFragmentSource, "opaque scene");
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
		Resources.sceneFuzzTime = glGetUniformLocation(Resources.sceneProgram, "u_fuzz_time");
		Resources.sceneLightPositionRadius = glGetUniformLocation(Resources.sceneProgram, "u_light_position_radius[0]");
		Resources.sceneLightColor = glGetUniformLocation(Resources.sceneProgram, "u_light_color[0]");
		Resources.sceneLightCounts = glGetUniformLocation(Resources.sceneProgram, "u_light_counts");
		Resources.sceneLightPlaneNormal = glGetUniformLocation(Resources.sceneProgram, "u_light_plane_normal");
		Resources.sceneProjectedLights = glGetUniformLocation(Resources.sceneProgram, "u_projected_lights");
		Resources.sceneDynamicLightTexture = glGetUniformLocation(Resources.sceneProgram, "u_dynamic_light_texture");
		Resources.sceneClipPlane = glGetUniformLocation(Resources.sceneProgram, "u_clip_plane");
		Resources.sceneClipPlaneEnabled = glGetUniformLocation(Resources.sceneProgram, "u_clip_plane_enabled");
		Resources.maskedTextureUniform = glGetUniformLocation(Resources.maskedProgram, "u_texture");
		Resources.maskedBrightmapUniform = glGetUniformLocation(Resources.maskedProgram, "u_brightmap");
		Resources.maskedUseBrightmap = glGetUniformLocation(Resources.maskedProgram, "u_use_brightmap");
		Resources.maskedBrightmapDesaturation = glGetUniformLocation(Resources.maskedProgram, "u_brightmap_desaturation");
		Resources.maskedViewProjection = glGetUniformLocation(Resources.maskedProgram, "u_view_projection");
		Resources.maskedUseTexture = glGetUniformLocation(Resources.maskedProgram, "u_use_texture");
		Resources.maskedModel = glGetUniformLocation(Resources.maskedProgram, "u_model");
		Resources.maskedTextureTransform = glGetUniformLocation(Resources.maskedProgram, "u_texture_transform");
		Resources.maskedCameraPosition = glGetUniformLocation(Resources.maskedProgram, "u_camera_position");
		Resources.maskedObjectColor = glGetUniformLocation(Resources.maskedProgram, "u_object_color");
		Resources.maskedAlphaCutoff = glGetUniformLocation(Resources.maskedProgram, "u_alpha_cutoff");
		Resources.maskedMaterialFlags = glGetUniformLocation(Resources.maskedProgram, "u_material_flags");
		Resources.maskedFuzzTime = glGetUniformLocation(Resources.maskedProgram, "u_fuzz_time");
		Resources.maskedLightPositionRadius = glGetUniformLocation(Resources.maskedProgram, "u_light_position_radius[0]");
		Resources.maskedLightColor = glGetUniformLocation(Resources.maskedProgram, "u_light_color[0]");
		Resources.maskedLightCounts = glGetUniformLocation(Resources.maskedProgram, "u_light_counts");
		Resources.maskedLightPlaneNormal = glGetUniformLocation(Resources.maskedProgram, "u_light_plane_normal");
		Resources.maskedProjectedLights = glGetUniformLocation(Resources.maskedProgram, "u_projected_lights");
		Resources.maskedDynamicLightTexture = glGetUniformLocation(Resources.maskedProgram, "u_dynamic_light_texture");
		Resources.maskedClipPlane = glGetUniformLocation(Resources.maskedProgram, "u_clip_plane");
		Resources.maskedClipPlaneEnabled = glGetUniformLocation(Resources.maskedProgram, "u_clip_plane_enabled");
		Resources.paletteTextureUniform = glGetUniformLocation(Resources.paletteProgram, "u_texture");
		Resources.paletteBrightmapUniform = glGetUniformLocation(Resources.paletteProgram, "u_brightmap");
		Resources.paletteUseBrightmap = glGetUniformLocation(Resources.paletteProgram, "u_use_brightmap");
		Resources.paletteBrightmapDesaturation = glGetUniformLocation(Resources.paletteProgram, "u_brightmap_desaturation");
		Resources.paletteViewProjection = glGetUniformLocation(Resources.paletteProgram, "u_view_projection");
		Resources.paletteUseTexture = glGetUniformLocation(Resources.paletteProgram, "u_use_texture");
		Resources.paletteModel = glGetUniformLocation(Resources.paletteProgram, "u_model");
		Resources.paletteTextureTransform = glGetUniformLocation(Resources.paletteProgram, "u_texture_transform");
		Resources.paletteCameraPosition = glGetUniformLocation(Resources.paletteProgram, "u_camera_position");
		Resources.paletteObjectColor = glGetUniformLocation(Resources.paletteProgram, "u_object_color");
		Resources.paletteMaterialFlags = glGetUniformLocation(Resources.paletteProgram, "u_material_flags");
		Resources.paletteFuzzTime = glGetUniformLocation(Resources.paletteProgram, "u_fuzz_time");
		Resources.paletteLightPositionRadius = glGetUniformLocation(Resources.paletteProgram, "u_light_position_radius[0]");
		Resources.paletteLightColor = glGetUniformLocation(Resources.paletteProgram, "u_light_color[0]");
		Resources.paletteLightCounts = glGetUniformLocation(Resources.paletteProgram, "u_light_counts");
		Resources.paletteLightPlaneNormal = glGetUniformLocation(Resources.paletteProgram, "u_light_plane_normal");
		Resources.paletteProjectedLights = glGetUniformLocation(Resources.paletteProgram, "u_projected_lights");
		Resources.paletteDynamicLightTexture = glGetUniformLocation(Resources.paletteProgram, "u_dynamic_light_texture");
		Resources.paletteClipPlane = glGetUniformLocation(Resources.paletteProgram, "u_clip_plane");
		Resources.paletteClipPlaneEnabled = glGetUniformLocation(Resources.paletteProgram, "u_clip_plane_enabled");
		Resources.fogViewProjection = glGetUniformLocation(Resources.fogProgram, "u_view_projection");
		Resources.fogTextureUniform = glGetUniformLocation(Resources.fogProgram, "u_texture");
		Resources.fogBrightmapUniform = glGetUniformLocation(Resources.fogProgram, "u_brightmap");
		Resources.fogUseBrightmap = glGetUniformLocation(Resources.fogProgram, "u_use_brightmap");
		Resources.fogBrightmapDesaturation = glGetUniformLocation(Resources.fogProgram, "u_brightmap_desaturation");
		Resources.fogUseTexture = glGetUniformLocation(Resources.fogProgram, "u_use_texture");
		Resources.fogModel = glGetUniformLocation(Resources.fogProgram, "u_model");
		Resources.fogTextureTransform = glGetUniformLocation(Resources.fogProgram, "u_texture_transform");
		Resources.fogCameraPosition = glGetUniformLocation(Resources.fogProgram, "u_camera_position");
		Resources.fogObjectColor = glGetUniformLocation(Resources.fogProgram, "u_object_color");
		Resources.fogMaterialFlags = glGetUniformLocation(Resources.fogProgram, "u_material_flags");
		Resources.fogFuzzTime = glGetUniformLocation(Resources.fogProgram, "u_fuzz_time");
		Resources.fogColor = glGetUniformLocation(Resources.fogProgram, "u_fog_color");
		Resources.fogDensity = glGetUniformLocation(Resources.fogProgram, "u_fog_density");
		Resources.fogLightPositionRadius = glGetUniformLocation(Resources.fogProgram, "u_light_position_radius[0]");
		Resources.fogLightColor = glGetUniformLocation(Resources.fogProgram, "u_light_color[0]");
		Resources.fogLightCounts = glGetUniformLocation(Resources.fogProgram, "u_light_counts");
		Resources.fogLightPlaneNormal = glGetUniformLocation(Resources.fogProgram, "u_light_plane_normal");
		Resources.fogProjectedLights = glGetUniformLocation(Resources.fogProgram, "u_projected_lights");
		Resources.fogDynamicLightTexture = glGetUniformLocation(Resources.fogProgram, "u_dynamic_light_texture");
		Resources.fogClipPlane = glGetUniformLocation(Resources.fogProgram, "u_clip_plane");
		Resources.fogClipPlaneEnabled = glGetUniformLocation(Resources.fogProgram, "u_clip_plane_enabled");
		Resources.fogMaskedViewProjection = glGetUniformLocation(Resources.fogMaskedProgram, "u_view_projection");
		Resources.fogMaskedTextureUniform = glGetUniformLocation(Resources.fogMaskedProgram, "u_texture");
		Resources.fogMaskedBrightmapUniform = glGetUniformLocation(Resources.fogMaskedProgram, "u_brightmap");
		Resources.fogMaskedUseBrightmap = glGetUniformLocation(Resources.fogMaskedProgram, "u_use_brightmap");
		Resources.fogMaskedBrightmapDesaturation = glGetUniformLocation(Resources.fogMaskedProgram, "u_brightmap_desaturation");
		Resources.fogMaskedUseTexture = glGetUniformLocation(Resources.fogMaskedProgram, "u_use_texture");
		Resources.fogMaskedModel = glGetUniformLocation(Resources.fogMaskedProgram, "u_model");
		Resources.fogMaskedTextureTransform = glGetUniformLocation(Resources.fogMaskedProgram, "u_texture_transform");
		Resources.fogMaskedCameraPosition = glGetUniformLocation(Resources.fogMaskedProgram, "u_camera_position");
		Resources.fogMaskedObjectColor = glGetUniformLocation(Resources.fogMaskedProgram, "u_object_color");
		Resources.fogMaskedAlphaCutoff = glGetUniformLocation(Resources.fogMaskedProgram, "u_alpha_cutoff");
		Resources.fogMaskedMaterialFlags = glGetUniformLocation(Resources.fogMaskedProgram, "u_material_flags");
		Resources.fogMaskedFuzzTime = glGetUniformLocation(Resources.fogMaskedProgram, "u_fuzz_time");
		Resources.fogMaskedColor = glGetUniformLocation(Resources.fogMaskedProgram, "u_fog_color");
		Resources.fogMaskedDensity = glGetUniformLocation(Resources.fogMaskedProgram, "u_fog_density");
		Resources.fogMaskedLightPositionRadius = glGetUniformLocation(Resources.fogMaskedProgram, "u_light_position_radius[0]");
		Resources.fogMaskedLightColor = glGetUniformLocation(Resources.fogMaskedProgram, "u_light_color[0]");
		Resources.fogMaskedLightCounts = glGetUniformLocation(Resources.fogMaskedProgram, "u_light_counts");
		Resources.fogMaskedLightPlaneNormal = glGetUniformLocation(Resources.fogMaskedProgram, "u_light_plane_normal");
		Resources.fogMaskedProjectedLights = glGetUniformLocation(Resources.fogMaskedProgram, "u_projected_lights");
		Resources.fogMaskedDynamicLightTexture = glGetUniformLocation(Resources.fogMaskedProgram, "u_dynamic_light_texture");
		Resources.fogMaskedClipPlane = glGetUniformLocation(Resources.fogMaskedProgram, "u_clip_plane");
		Resources.fogMaskedClipPlaneEnabled = glGetUniformLocation(Resources.fogMaskedProgram, "u_clip_plane_enabled");

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
	if (strcmp(name, "gles/masked") == 0) return Resources.maskedProgram;
	if (strcmp(name, "gles/fog") == 0) return Resources.fogProgram;
	if (strcmp(name, "gles/fog-masked") == 0) return Resources.fogMaskedProgram;
	if (strcmp(name, "gles/palette") == 0) return Resources.paletteProgram;
	if (strcmp(name, "gles/present") == 0) return gl_GLESInternalPresentGetProgram();
	return 0;
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
	Capabilities.hasBufferMapping = false;
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
	if (developer)
		NativeFrameStart = std::chrono::steady_clock::now();
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
	gl_GLESInternalSceneClearLights();
	Resources.sceneIndexCount = 0;
	Resources.sceneReady = false;
	Resources.sceneBatches.clear();
	Resources.portalTargets.clear();
	NativePortalCaptureStack.clear();
	gl_GLESInternalPortalBeginFrame();
	NativeProgramBindingKnown = false;
	ResetNativeSkyRecord(Resources.outerSky, -1, 1u);
	Resources.sceneWallCount = 0;
	Resources.sceneFlatCount = 0;
	Resources.sceneSpriteCount = 0;
	Resources.sceneOpaqueBatchMerges = 0;
	Resources.sceneProgramBinds = 0;
	Resources.sceneProgramBindSkips = 0;
	Resources.sceneTextureBinds = 0;
	Resources.sceneTextureBindSkips = 0;
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
	Resources.sceneOpaqueBatchMerges = 0;
	Resources.sceneProgramBinds = 0;
	Resources.sceneProgramBindSkips = 0;
	Resources.sceneTextureBinds = 0;
	Resources.sceneTextureBindSkips = 0;
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
	Resources.sceneBatches.push_back(batch);
	sky->maskBatches.push_back(Resources.sceneBatches.size() - 1);
	if (!sky->viewValid)
	{
		memcpy(sky->viewProjection, batch.viewProjection, sizeof(sky->viewProjection));
		memcpy(sky->cameraPosition, batch.cameraPosition, sizeof(sky->cameraPosition));
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
	const float *topGlowColor, const float *bottomGlowColor, const float *glowDistances)
{
	if (!gl_GLES_CanUseResources() || positions == NULL) return;
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
		brightmapDesaturation, topGlowColor, bottomGlowColor);
}

void gl_GLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	const float *lightData, const unsigned int *lightCounts, unsigned int brightmap, int brightmapDesaturation)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || vertexCount < 3) return;
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
		batch.firstIndex = firstIndex;
		batch.indexCount = indexCount;
		batch.texture = texture;
		batch.brightmap = brightmap;
		batch.brightmapDesaturation = brightmapDesaturation;
		batch.masked = masked;
		batch.fog = fog;
		batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
		batch.repeat = repeat;
		batch.palette = IsPaletteTexture(texture);
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
	const float *fogColor, float fogDensity)
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
	batch.fog = fog;
	batch.repeat = true;
	batch.palette = IsPaletteTexture(texture);
	batch.flood = true;
	batch.floodWallFirstIndex = wallIndex;
	batch.sortDepth = dx * dx + dy * dy + dz * dz;
	if (fogColor != NULL)
	{
		batch.fogColor[0] = ClampUnit(fogColor[0]);
		batch.fogColor[1] = ClampUnit(fogColor[1]);
		batch.fogColor[2] = ClampUnit(fogColor[2]);
	}
	batch.fogDensity = std::max(0.0f, fogDensity);
	Resources.sceneBatches.push_back(batch);
}

void gl_GLES_AddHUDPolygon(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EGLESBlendMode blendMode, unsigned int materialFlags)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || vertexCount < 3) return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	if (!CanAppendSceneGeometry(vertexCount, vertexCount >= 3 ? (vertexCount - 2) * 3 : 0, "HUD polygon")) return;
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
	batch.texture = texture;
	batch.masked = masked;
	batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
	batch.repeat = repeat;
	batch.palette = IsPaletteTexture(texture);
	batch.hud = true;
	batch.blendMode = blendMode;
	batch.materialFlags = materialFlags;
	batch.sortDepth = 0.0f;
	Resources.sceneBatches.push_back(batch);
}

void gl_GLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	unsigned int brightmap, int brightmapDesaturation, bool customBlend,
	int sourceBlend, int destinationBlend, float alphaCutoff)
{
	if (gl_GLES_CanUseResources()) ++Resources.sceneSpriteCount;
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
		vertices[i].glowTopDistance = vertices[i].glowBottomDistance = 0.0f;
	}
	AddSceneQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, masked, fog,
		alpha < 0.999f, false, blendMode, fogColor, fogDensity, materialFlags, NULL, NULL,
		brightmap, brightmapDesaturation, NULL, NULL, customBlend,
		static_cast<GLenum>(sourceBlend), static_cast<GLenum>(destinationBlend), alphaCutoff);
}

void gl_GLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EGLESBlendMode blendMode, unsigned int materialFlags,
	const float *normals, unsigned int brightmap, int brightmapDesaturation, bool cullBackFaces,
	bool customBlend, int sourceBlend, int destinationBlend)
{
	if (!gl_GLES_CanUseResources() || positions == NULL || indices == NULL ||
		vertexCount == 0 || indexCount < 3 || (indexCount % 3) != 0) return;
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
	for (unsigned int index = 0; index < indexCount; ++index)
		Resources.sceneIndices.push_back(first + indices[index]);
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
	batch.firstIndex = firstIndex;
	batch.indexCount = static_cast<GLsizei>(indexCount);
	batch.texture = texture;
	batch.brightmap = brightmap;
	batch.brightmapDesaturation = brightmapDesaturation;
	batch.masked = masked;
	batch.fog = fog;
	batch.translucent = alpha < 0.999f || blendMode != GLES_BLEND_OPAQUE;
	batch.repeat = false;
	batch.palette = IsPaletteTexture(texture);
	batch.model = true;
	batch.cullBackFaces = cullBackFaces;
	batch.materialFlags = materialFlags;
	batch.blendMode = blendMode;
	batch.customBlend = customBlend;
	batch.sourceBlend = static_cast<GLenum>(sourceBlend);
	batch.destinationBlend = static_cast<GLenum>(destinationBlend);
	batch.alphaCutoff = 0.5f;
	batch.sortDepth = dx * dx + dy * dy + dz * dz;
	if (fogColor != NULL)
	{
		batch.fogColor[0] = ClampUnit(fogColor[0]);
		batch.fogColor[1] = ClampUnit(fogColor[1]);
		batch.fogColor[2] = ClampUnit(fogColor[2]);
	}
	batch.fogDensity = std::max(0.0f, fogDensity);
	Resources.sceneBatches.push_back(batch);
}

void gl_GLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EGLESBlendMode blendMode, unsigned int materialFlags, bool customBlend,
	int sourceBlend, int destinationBlend, float alphaCutoff)
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
		customBlend, static_cast<GLenum>(sourceBlend), static_cast<GLenum>(destinationBlend), alphaCutoff);
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
}

static void DrawNativePortalBatch(const FSceneBatch &batch, GLuint dynamicLightTexture,
	GLuint stencilBit)
{
	if (batch.firstIndex < 0 || batch.indexCount <= 0 ||
		static_cast<size_t>(batch.firstIndex) + static_cast<size_t>(batch.indexCount) > Resources.sceneIndices.size())
		return;
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
	if (batch.hud)
	{
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_CULL_FACE);
		glDepthMask(GL_FALSE);
	}
	else if (batch.model)
	{
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
		glEnable(GL_DEPTH_TEST);
		glDepthFunc((batch.materialFlags & GLES_MATERIAL_SPHERE_MAP) != 0 ? GL_LEQUAL : GL_LESS);
		glDisable(GL_CULL_FACE);
	}
	if (batch.translucent)
	{
		glEnable(GL_BLEND);
		glDepthMask(GL_FALSE);
		GLenum equation = GL_FUNC_ADD;
		if (batch.blendMode == GLES_BLEND_SUBTRACT) equation = GL_FUNC_SUBTRACT;
		else if (batch.blendMode == GLES_BLEND_REVERSE_SUBTRACT) equation = GL_FUNC_REVERSE_SUBTRACT;
		glBlendEquation(equation);
		const bool additive = batch.blendMode == GLES_BLEND_ADD ||
			batch.blendMode == GLES_BLEND_SUBTRACT || batch.blendMode == GLES_BLEND_REVERSE_SUBTRACT;
		if (batch.customBlend) glBlendFunc(batch.sourceBlend, batch.destinationBlend);
		else if (batch.blendMode == GLES_BLEND_FUZZ) glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
		else if (batch.blendMode == GLES_BLEND_MULTIPLY) glBlendFunc(GL_DST_COLOR, GL_ZERO);
		else glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		glDisable(GL_BLEND);
		glDepthMask(batch.hud || batch.flood || IsNativeDecal(batch) ? GL_FALSE : GL_TRUE);
		glBlendEquation(GL_FUNC_ADD);
	}
	const GLuint program = batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
		(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram));
	const char *programName = batch.fog ? (batch.masked ? "gles/portal-fog-masked" : "gles/portal-fog") :
		(batch.masked ? "gles/portal-masked" : (batch.palette ? "gles/portal-palette" : "gles/portal-opaque"));
	BindNativeProgram(program, programName);
	SetNativeGlowUniforms(program, batch);
	if (program == Resources.sceneProgram && Resources.sceneSkyDepth >= 0)
		glUniform1i(Resources.sceneSkyDepth, 0);
	GLint viewProjection = Resources.sceneViewProjection;
	GLint textureUniform = Resources.sceneTextureUniform;
	GLint brightmapUniform = Resources.sceneBrightmapUniform;
	GLint useBrightmap = Resources.sceneUseBrightmap;
	GLint brightmapDesaturation = Resources.sceneBrightmapDesaturation;
	GLint useTexture = Resources.sceneUseTexture;
	GLint model = Resources.sceneModel;
	GLint textureTransform = Resources.sceneTextureTransform;
	GLint cameraPosition = Resources.sceneCameraPosition;
	GLint objectColor = Resources.sceneObjectColor;
	GLint materialFlags = Resources.sceneMaterialFlags;
	GLint fuzzTime = Resources.sceneFuzzTime;
	GLint alphaCutoff = -1;
	GLint fogColor = -1;
	GLint fogDensity = -1;
	GLint lightPositionRadius = Resources.sceneLightPositionRadius;
	GLint lightColor = Resources.sceneLightColor;
	GLint lightCounts = Resources.sceneLightCounts;
	GLint lightPlaneNormal = Resources.sceneLightPlaneNormal;
	GLint projectedLights = Resources.sceneProjectedLights;
	GLint dynamicLightSampler = Resources.sceneDynamicLightTexture;
	GLint clipPlaneUniform = Resources.sceneClipPlane;
	GLint clipPlaneEnabledUniform = Resources.sceneClipPlaneEnabled;
	if (batch.fog)
	{
		if (batch.masked)
		{
			viewProjection = Resources.fogMaskedViewProjection;
			textureUniform = Resources.fogMaskedTextureUniform;
			brightmapUniform = Resources.fogMaskedBrightmapUniform;
			useBrightmap = Resources.fogMaskedUseBrightmap;
			brightmapDesaturation = Resources.fogMaskedBrightmapDesaturation;
			useTexture = Resources.fogMaskedUseTexture;
			model = Resources.fogMaskedModel;
			textureTransform = Resources.fogMaskedTextureTransform;
			cameraPosition = Resources.fogMaskedCameraPosition;
			objectColor = Resources.fogMaskedObjectColor;
			materialFlags = Resources.fogMaskedMaterialFlags;
			fuzzTime = Resources.fogMaskedFuzzTime;
			alphaCutoff = Resources.fogMaskedAlphaCutoff;
			fogColor = Resources.fogMaskedColor;
			fogDensity = Resources.fogMaskedDensity;
			lightPositionRadius = Resources.fogMaskedLightPositionRadius;
			lightColor = Resources.fogMaskedLightColor;
			lightCounts = Resources.fogMaskedLightCounts;
			lightPlaneNormal = Resources.fogMaskedLightPlaneNormal;
			projectedLights = Resources.fogMaskedProjectedLights;
			dynamicLightSampler = Resources.fogMaskedDynamicLightTexture;
			clipPlaneUniform = Resources.fogMaskedClipPlane;
			clipPlaneEnabledUniform = Resources.fogMaskedClipPlaneEnabled;
		}
		else
		{
			viewProjection = Resources.fogViewProjection;
			textureUniform = Resources.fogTextureUniform;
			brightmapUniform = Resources.fogBrightmapUniform;
			useBrightmap = Resources.fogUseBrightmap;
			brightmapDesaturation = Resources.fogBrightmapDesaturation;
			useTexture = Resources.fogUseTexture;
			model = Resources.fogModel;
			textureTransform = Resources.fogTextureTransform;
			cameraPosition = Resources.fogCameraPosition;
			objectColor = Resources.fogObjectColor;
			materialFlags = Resources.fogMaterialFlags;
			fuzzTime = Resources.fogFuzzTime;
			fogColor = Resources.fogColor;
			fogDensity = Resources.fogDensity;
			lightPositionRadius = Resources.fogLightPositionRadius;
			lightColor = Resources.fogLightColor;
			lightCounts = Resources.fogLightCounts;
			lightPlaneNormal = Resources.fogLightPlaneNormal;
			projectedLights = Resources.fogProjectedLights;
			dynamicLightSampler = Resources.fogDynamicLightTexture;
			clipPlaneUniform = Resources.fogClipPlane;
			clipPlaneEnabledUniform = Resources.fogClipPlaneEnabled;
		}
	}
	else if (batch.masked)
	{
		viewProjection = Resources.maskedViewProjection;
		textureUniform = Resources.maskedTextureUniform;
		brightmapUniform = Resources.maskedBrightmapUniform;
		useBrightmap = Resources.maskedUseBrightmap;
		brightmapDesaturation = Resources.maskedBrightmapDesaturation;
		useTexture = Resources.maskedUseTexture;
		model = Resources.maskedModel;
		textureTransform = Resources.maskedTextureTransform;
		cameraPosition = Resources.maskedCameraPosition;
		objectColor = Resources.maskedObjectColor;
		materialFlags = Resources.maskedMaterialFlags;
		fuzzTime = Resources.maskedFuzzTime;
		alphaCutoff = Resources.maskedAlphaCutoff;
		lightPositionRadius = Resources.maskedLightPositionRadius;
		lightColor = Resources.maskedLightColor;
		lightCounts = Resources.maskedLightCounts;
		lightPlaneNormal = Resources.maskedLightPlaneNormal;
		projectedLights = Resources.maskedProjectedLights;
		dynamicLightSampler = Resources.maskedDynamicLightTexture;
		clipPlaneUniform = Resources.maskedClipPlane;
		clipPlaneEnabledUniform = Resources.maskedClipPlaneEnabled;
	}
	else if (batch.palette)
	{
		viewProjection = Resources.paletteViewProjection;
		textureUniform = Resources.paletteTextureUniform;
		brightmapUniform = Resources.paletteBrightmapUniform;
		useBrightmap = Resources.paletteUseBrightmap;
		brightmapDesaturation = Resources.paletteBrightmapDesaturation;
		useTexture = Resources.paletteUseTexture;
		model = Resources.paletteModel;
		textureTransform = Resources.paletteTextureTransform;
		cameraPosition = Resources.paletteCameraPosition;
		objectColor = Resources.paletteObjectColor;
		materialFlags = Resources.paletteMaterialFlags;
		fuzzTime = Resources.paletteFuzzTime;
		lightPositionRadius = Resources.paletteLightPositionRadius;
		lightColor = Resources.paletteLightColor;
		lightCounts = Resources.paletteLightCounts;
		lightPlaneNormal = Resources.paletteLightPlaneNormal;
		projectedLights = Resources.paletteProjectedLights;
		dynamicLightSampler = Resources.paletteDynamicLightTexture;
		clipPlaneUniform = Resources.paletteClipPlane;
		clipPlaneEnabledUniform = Resources.paletteClipPlaneEnabled;
	}
	static const GLfloat identity[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	if (viewProjection >= 0) glUniformMatrix4fv(viewProjection, 1, GL_FALSE,
		batch.hud ? identity : batch.viewProjection);
	if (model >= 0) glUniformMatrix4fv(model, 1, GL_FALSE, identity);
	if (textureTransform >= 0) glUniform4f(textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (cameraPosition >= 0) glUniform3f(cameraPosition, batch.hud ? 0.0f : batch.cameraPosition[0],
		batch.hud ? 0.0f : batch.cameraPosition[1], batch.hud ? 0.0f : batch.cameraPosition[2]);
	if (objectColor >= 0) glUniform4f(objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
	if (materialFlags >= 0) glUniform1i(materialFlags, static_cast<GLint>(batch.materialFlags));
	if (fuzzTime >= 0) glUniform1f(fuzzTime, Resources.frame / 35.0f);
	if (clipPlaneUniform >= 0) glUniform4fv(clipPlaneUniform, 1, batch.clipPlane);
	if (clipPlaneEnabledUniform >= 0) glUniform1i(clipPlaneEnabledUniform, batch.clipPlaneEnabled ? 1 : 0);
	if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ||
		(IsNativeDecal(batch) && (batch.materialFlags & GLES_MATERIAL_RED_IS_ALPHA) != 0) ? 0.0f :
		(batch.alphaCutoff > 0.0f ? batch.alphaCutoff : 0.5f));
	if (fogColor >= 0) glUniform4f(fogColor, batch.fogColor[0], batch.fogColor[1], batch.fogColor[2], 1.0f);
	if (fogDensity >= 0) glUniform1f(fogDensity, batch.fogDensity / 64000.0f);
	const GLfloat *lightPositions = NULL;
	const GLfloat *lightColors = NULL;
	unsigned int lightNormalCount = 0;
	unsigned int lightSubtractiveCount = 0;
	unsigned int lightCount = 0;
	GetNativeLightUpload(batch, 0, GLES_MAX_LIGHTS, lightPositions, lightColors, lightNormalCount,
		lightSubtractiveCount, lightCount);
	if (lightPositionRadius >= 0 && lightCount > 0)
		glUniform4fv(lightPositionRadius, lightCount, lightPositions);
	if (lightColor >= 0 && lightCount > 0)
		glUniform4fv(lightColor, lightCount, lightColors);
	if (lightCounts >= 0) glUniform3i(lightCounts, static_cast<GLint>(lightNormalCount),
		static_cast<GLint>(lightSubtractiveCount), static_cast<GLint>(lightCount));
	if (lightPlaneNormal >= 0) glUniform3fv(lightPlaneNormal, 1, batch.lightPlaneNormal);
	const bool projected = dynamicLightTexture != 0 && lightCount > 0 &&
		(batch.lightPlaneNormal[0] != 0.0f || batch.lightPlaneNormal[1] != 0.0f || batch.lightPlaneNormal[2] != 0.0f);
	if (projectedLights >= 0) glUniform1i(projectedLights, projected ? 1 : 0);
	if (dynamicLightSampler >= 0) glUniform1i(dynamicLightSampler, 2);
	if (textureUniform >= 0) glUniform1i(textureUniform, 0);
	if (brightmapUniform >= 0) glUniform1i(brightmapUniform, 1);
	if (useBrightmap >= 0) glUniform1i(useBrightmap, batch.brightmap != 0 ? 1 : 0);
	if (brightmapDesaturation >= 0) glUniform1i(brightmapDesaturation, batch.brightmapDesaturation);
	if (useTexture >= 0) glUniform1i(useTexture, batch.texture != 0 ? 1 : 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, batch.texture != 0 ? batch.texture : Resources.checkerTexture);
	glBindSampler(0, batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, batch.brightmap != 0 ? batch.brightmap : Resources.checkerTexture);
	glBindSampler(1, batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, projected ? dynamicLightTexture : Resources.checkerTexture);
	glBindSampler(2, Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE0);
	SetNativeDecalDepthBias(batch, true);
	glDrawElements(GL_TRIANGLES, batch.indexCount, GL_UNSIGNED_INT,
		reinterpret_cast<const void *>(batch.firstIndex * sizeof(GLuint)));
	DrawNativeLightOverflow(batch, dynamicLightTexture, program, lightPositionRadius,
		lightColor, lightCounts, projectedLights, batch.indexCount, batch.firstIndex);
	SetNativeDecalDepthBias(batch, false);
}

static void DrawNativeWipeOverlay(const FGLESTargetDescriptor &target)
{
	if (!HasNativeWipeOverlay() || target.renderWidth <= 0 || target.renderHeight <= 0)
		return;
	gl_GLESInternalResetState(target.renderWidth, target.renderHeight);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	SetNativeFullViewport(target.renderWidth, target.renderHeight);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(Resources.sceneVertexArray);
	for (size_t i = 0; i < Resources.sceneBatches.size(); ++i)
	{
		const FSceneBatch &batch = Resources.sceneBatches[i];
		if (batch.wipeOverlay) DrawNativePortalBatch(batch, 0, 0);
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
	BindNativeProgram(Resources.sceneProgram, "gles/portal-mask");
	static const GLfloat identity[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	if (Resources.sceneViewProjection >= 0)
		glUniformMatrix4fv(Resources.sceneViewProjection, 1, GL_FALSE, batch.viewProjection);
	if (Resources.sceneModel >= 0) glUniformMatrix4fv(Resources.sceneModel, 1, GL_FALSE, identity);
	if (Resources.sceneTextureTransform >= 0) glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (Resources.sceneCameraPosition >= 0)
		glUniform3f(Resources.sceneCameraPosition, batch.cameraPosition[0], batch.cameraPosition[1], batch.cameraPosition[2]);
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
	glDrawElements(GL_TRIANGLES, batch.indexCount, GL_UNSIGNED_INT,
		reinterpret_cast<const void *>(batch.firstIndex * sizeof(GLuint)));
	return true;
}

static bool DrawNativeSkyboxLayer(FMaterial *material, float xOffset, bool sky2, bool fliptop)
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
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		// Skybox faces use the source renderer's clamped edge sampling.
		glBindSampler(0, Resources.sceneSampler);
		if (Resources.sceneTextureTransform >= 0)
		{
			if (threeFace && face < 4)
				glUniform4f(Resources.sceneTextureTransform, 0.25f, 1.0f, face * 0.25f, 0.0f);
			else
				glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		const FGLESSkyPrimitiveRange range = gl_GLESInternalPortalSkyboxFace(face);
		glDrawElements(GL_TRIANGLES, range.indexCount, GL_UNSIGNED_SHORT,
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
	BindNativeProgram(Resources.sceneProgram, "gles/portal-sky");
	static const GLfloat identity[16] =
	{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 0.0f, 1.0f
	};
	if (Resources.sceneViewProjection >= 0)
		glUniformMatrix4fv(Resources.sceneViewProjection, 1, GL_FALSE, target.sky.viewProjection);
	if (Resources.sceneModel >= 0) glUniformMatrix4fv(Resources.sceneModel, 1, GL_FALSE, identity);
	if (Resources.sceneTextureTransform >= 0) glUniform4f(Resources.sceneTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (Resources.sceneCameraPosition >= 0)
		glUniform3f(Resources.sceneCameraPosition, target.sky.cameraPosition[0], target.sky.cameraPosition[1], target.sky.cameraPosition[2]);
	if (Resources.sceneSkyDepth >= 0) glUniform1i(Resources.sceneSkyDepth, 1);
	if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 0);
	if (Resources.sceneMaterialFlags >= 0) glUniform1i(Resources.sceneMaterialFlags, 0);
	if (Resources.sceneLightCounts >= 0) glUniform3i(Resources.sceneLightCounts, 0, 0, 0);
	if (Resources.sceneProjectedLights >= 0) glUniform1i(Resources.sceneProjectedLights, 0);
	if (Resources.sceneClipPlane >= 0) glUniform4f(Resources.sceneClipPlane, 0.0f, 0.0f, 0.0f, 0.0f);
	if (Resources.sceneClipPlaneEnabled >= 0) glUniform1i(Resources.sceneClipPlaneEnabled, 0);
	if (Resources.sceneBrightmapUniform >= 0) glUniform1i(Resources.sceneBrightmapUniform, 1);
	if (Resources.sceneUseBrightmap >= 0) glUniform1i(Resources.sceneUseBrightmap, 0);
	if (Resources.sceneBrightmapDesaturation >= 0) glUniform1i(Resources.sceneBrightmapDesaturation, 0);
	if (Resources.sceneTextureUniform >= 0) glUniform1i(Resources.sceneTextureUniform, 0);
	glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
	if (target.sky.capEligible && target.skyMaterial->tex->gl_info.bSkybox)
	{
		if (DrawNativeSkyboxLayer(target.skyMaterial, target.skyXOffset, target.sky2,
			static_cast<FSkyBox *>(target.skyMaterial->tex)->fliptop))
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
		const bool caps = drawCaps && target.sky.capEligible;
		const GLuint texture = material->BindNative(CM_DEFAULT, 0, true);
		if (texture == 0) return false;
		gl_GLESInternalPortalUploadSkyGeometry(material, xOffset, yOffset, mirrored,
			Resources.cameraX, Resources.cameraY, Resources.cameraZ);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texture);
		glBindSampler(0, Resources.checkerSampler);
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, caps ? 0 : 1);
		if (caps)
		{
			if (Resources.sceneObjectColor >= 0)
				glUniform4f(Resources.sceneObjectColor, target.skyUpperCapColor.r / 255.0f,
					target.skyUpperCapColor.g / 255.0f, target.skyUpperCapColor.b / 255.0f, 1.0f);
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
		if (caps)
		{
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
			if (Resources.sceneObjectColor >= 0)
				glUniform4f(Resources.sceneObjectColor, target.skyLowerCapColor.r / 255.0f,
					target.skyLowerCapColor.g / 255.0f, target.skyLowerCapColor.b / 255.0f, 1.0f);
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
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
		if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 1);
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
		if (Resources.sceneObjectColor >= 0)
			glUniform4f(Resources.sceneObjectColor, target.skyFogColor.r / 255.0f,
				target.skyFogColor.g / 255.0f, target.skyFogColor.b / 255.0f, skyfog / 255.0f);
		glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
		if (target.sky.capEligible)
			glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
		if (target.sky.capEligible)
			glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
	}
	if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 0);
	if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
	if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
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
		masks.push_back({ mask.indexCount,
			static_cast<size_t>(mask.firstIndex) * sizeof(GLuint), mask.viewProjection });
	}
	FGLESPortalCompositeList composite = {};
	composite.sourceTexture = sourceTexture;
	composite.sceneVertexArray = Resources.sceneVertexArray;
	composite.sceneSampler = Resources.sceneSampler;
	composite.masks = masks.data();
	composite.maskCount = masks.size();
	composite.targetWidth = parentSurface->renderWidth;
	composite.targetHeight = parentSurface->renderHeight;
	composite.parentStencilBit = parentStencilBit;
	composite.solidColor = solidColor;
	return gl_GLESInternalPortalCompositeMasks(composite);
}

struct FNativePortalFallbackScope
{
	unsigned int targetId;
	FGLESTargetDescriptor *targetSurface;
	FGLESTargetDescriptor *parentSurface;
};

static void DrawNativePortalTargets(GLuint dynamicLightTexture)
{
	if (Resources.portalTargets.empty()) return;
	unsigned int drawnTargets = 0;
	unsigned int drawnBatches = 0;
	unsigned int fallbackTargets = 0;
	FGLESTargetDescriptor *savedActiveTarget = NativeActiveTarget;
	FGLESTargetDescriptor *rootSurface = savedActiveTarget != nullptr ?
		savedActiveTarget : &Resources.sceneTarget;
	std::vector<FNativePortalFallbackScope> fallbackScopes;
	NativeActiveTarget = rootSurface;
	glBindVertexArray(Resources.sceneVertexArray);
	for (size_t targetIndex = 0; targetIndex < Resources.portalTargets.size(); ++targetIndex)
	{
		while (!fallbackScopes.empty() &&
			!IsNativePortalDescendant(targetIndex, fallbackScopes.back().targetId))
		{
			const FNativePortalFallbackScope scope = fallbackScopes.back();
			fallbackScopes.pop_back();
			BindNativePortalSurface(scope.parentSurface);
			if (scope.targetId < Resources.portalTargets.size() &&
				!CompositeNativePortalFallback(Resources.portalTargets[scope.targetId], scope.parentSurface))
				gl_GLES_Report("portal", "isolated capture could not be composited into its parent aperture");
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
		FGLESTargetDescriptor *parentSurface = fallbackScopes.empty() ?
			(NativeActiveTarget != nullptr ? NativeActiveTarget : rootSurface) :
			fallbackScopes.back().targetSurface;
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
			fallbackScopes.push_back({ target.id, targetSurface, parentSurface });
		}
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
						parentStencilBit, parentStencilBit);
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
		std::vector<FGLESSceneOrderRecord> &orderRecords = Resources.portalOrderRecords;
		orderRecords.clear();
		for (size_t batchIndex = target.firstBatch; batchIndex < targetEndBatch; ++batchIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[batchIndex];
			FGLESSceneOrderRecord record = {};
			record.batchIndex = batchIndex;
			record.included = batch.portalId == static_cast<int>(target.id) &&
				!batch.portalMask && !batch.skyMask && !batch.hud;
			record.hud = batch.hud;
			record.flood = batch.flood;
			record.flat = batch.flat;
			record.translucent = batch.translucent || IsNativeDecal(batch);
			record.sortDepth = batch.sortDepth;
			orderRecords.push_back(record);
		}
		const FGLESSceneDrawOrderView drawOrder = gl_GLESInternalSceneSortBatches(
			orderRecords.data(), orderRecords.size(), GLES_SCENE_ORDER_PORTAL);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
		for (size_t orderIndex = 0; orderIndex < drawOrder.count; ++orderIndex)
		{
			DrawNativePortalBatch(Resources.sceneBatches[drawOrder.indices[orderIndex]], dynamicLightTexture, stencilBit);
			++drawnBatches;
		}
		++drawnTargets;
		static bool fallbackPixelLogWritten = false;
		if (developer && target.framebufferFallback && targetSurface != nullptr &&
			!fallbackPixelLogWritten)
		{
			GLint previousRead = 0;
			glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, targetSurface->resolveFramebuffer);
			GLubyte center[4] = {};
			glReadPixels(targetSurface->renderWidth / 2, targetSurface->renderHeight / 2,
				1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
			const GLenum readError = glGetError();
			DPrintf("Zandronum GLES portal fallback target=%u center=%u,%u,%u,%u read=%s.\n",
				target.id, center[0], center[1], center[2], center[3],
				readError == GL_NO_ERROR ? "ok" : "failed");
			glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
			fallbackPixelLogWritten = true;
		}
	}
	while (!fallbackScopes.empty())
	{
		const FNativePortalFallbackScope scope = fallbackScopes.back();
		fallbackScopes.pop_back();
		BindNativePortalSurface(scope.parentSurface);
		if (scope.targetId < Resources.portalTargets.size() &&
			!CompositeNativePortalFallback(Resources.portalTargets[scope.targetId], scope.parentSurface))
			gl_GLES_Report("portal", "isolated capture could not be composited into its parent aperture");
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
	Resources.sceneBatches.push_back(batch);
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
				memcpy(target.sky.viewProjection, batch.viewProjection, sizeof(target.sky.viewProjection));
				memcpy(target.sky.cameraPosition, batch.cameraPosition, sizeof(target.sky.cameraPosition));
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
	memcpy(Resources.clipPlane, target.savedClipPlane, sizeof(Resources.clipPlane));
}

unsigned int gl_GLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires)
{
	if (!gl_GLES_CanUseResources() || key == NULL || pixels == NULL || width <= 0 || height <= 0)
		return 0;
	ConfigureNativeSamplers();
	return gl_GLESInternalBindMaterial(true, key, pixels, width, height, repeat,
		colormap, translation, allowhires, colormap != CM_DEFAULT || translation != 0);
}

unsigned int gl_GLES_EnsureMaterialTexture(const void *key, int width, int height,
	bool repeat, int colormap, int translation, bool allowhires)
{
	if (!gl_GLES_CanUseResources() || key == NULL || width <= 0 || height <= 0)
		return 0;
	const GLuint texture = gl_GLESInternalFindMaterialTexture(key, colormap, translation,
		repeat, allowhires, width, height);
	if (texture != 0) return texture;
	std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
	return gl_GLES_BindMaterial(key, pixels.data(), width, height, repeat,
		colormap, translation, allowhires);
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
	gl_GLES_UnregisterShaderPrograms();
	InvalidateResources();
	gl_GLES_InvalidateTextures();
	gl_GLES_InvalidateFlatBuffers();
	gl_GLES_ShutdownContext(true);
	CapabilitiesReady = false;
	memset(&Capabilities, 0, sizeof(Capabilities));
	Printf("Zandronum GLES context lost; native resource names invalidated.\n");
}

bool gl_GLES_OnContextRestored(int width, int height)
{
	if (!gl_GLES_CollectCapabilities()) return false;
	const bool restored = InitializeResources(width, height, true);
	if (restored) gl_GLES_RegisterShaderPrograms();
	return restored;
}

void gl_GLES_RenderBootstrap(int width, int height)
{
	if (!Resources.ready)
	{
		if (!BootstrapPauseLogged)
		{
			BootstrapPauseLogged = true;
		Printf("GLES rendering paused while the host surface is unavailable.\n");
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
	SetNativeFullViewport(activeTarget->renderWidth, activeTarget->renderHeight);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(GL_TRUE);
	glDepthFunc(GL_LESS);
	// Doom wall winding is not consistently front-facing across two-sided sectors.
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glClearColor(0.025f, 0.045f, 0.075f, 1.0f);
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
			BindNativeProgram(Resources.sceneProgram, "gles/opaque");
		if (Resources.sceneSkyDepth >= 0) glUniform1i(Resources.sceneSkyDepth, 1);
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
		if (Resources.sceneLightCounts >= 0) glUniform3i(Resources.sceneLightCounts, 0, 0, 0);
		if (Resources.sceneMaterialFlags >= 0) glUniform1i(Resources.sceneMaterialFlags, 0);
		if (Resources.sceneTextureUniform >= 0) glUniform1i(Resources.sceneTextureUniform, 0);
		glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
		auto drawSkyLayer = [&](FMaterial *material, float xOffset, float yOffset, bool drawCaps) -> bool
		{
			if (material == NULL || material->tex == NULL) return false;
			const bool caps = drawCaps && sky.capEligible;
			if (caps && material->tex->gl_info.bSkybox)
			{
				FSkyBox *skybox = static_cast<FSkyBox *>(material->tex);
				return DrawNativeSkyboxLayer(material, xOffset, Resources.sky2, skybox->fliptop);
			}
			const GLuint skyTexture = material->BindNative(CM_DEFAULT, 0, true);
			if (skyTexture == 0) return false;
			gl_GLESInternalPortalUploadSkyGeometry(material, xOffset, yOffset,
				caps ? Resources.skyMirrored : Resources.skyLayerMirrored,
				Resources.cameraX, Resources.cameraY, Resources.cameraZ);
			glBindVertexArray(gl_GLESInternalPortalSkyVertexArray());
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, skyTexture);
			glBindSampler(0, Resources.checkerSampler);
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, caps ? 0 : 1);
			if (caps)
			{
				if (Resources.sceneObjectColor >= 0)
					glUniform4f(Resources.sceneObjectColor, Resources.skyUpperCapColor.r / 255.0f,
						Resources.skyUpperCapColor.g / 255.0f, Resources.skyUpperCapColor.b / 255.0f, 1.0f);
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
			}
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
			if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
			if (caps)
			{
				if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
				if (Resources.sceneObjectColor >= 0)
					glUniform4f(Resources.sceneObjectColor, Resources.skyLowerCapColor.r / 255.0f,
						Resources.skyLowerCapColor.g / 255.0f, Resources.skyLowerCapColor.b / 255.0f, 1.0f);
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
			}
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
			if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
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
			if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 1);
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
			if (Resources.sceneObjectColor >= 0)
				glUniform4f(Resources.sceneObjectColor, Resources.skyFogColor.r / 255.0f,
					Resources.skyFogColor.g / 255.0f, Resources.skyFogColor.b / 255.0f, skyfog / 255.0f);
			glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyUpperCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperCap().firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyUpperStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyUpperStrip(row).firstIndex * sizeof(GLushort)));
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, gl_GLESInternalPortalSkyLowerCap().indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerCap().firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, gl_GLESInternalPortalSkyLowerStrip(row).indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(gl_GLESInternalPortalSkyLowerStrip(row).firstIndex * sizeof(GLushort)));
			if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 0);
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		glBindVertexArray(0);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glStencilMask(0xff);
		glDisable(GL_BLEND);
		glDisable(GL_STENCIL_TEST);
		skyDrawn = firstLayerDrawn;
	};
	const bool renderScene = Resources.sceneReady && !gl_gles_test_pattern;
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
		drawSkyMask();
		drawSky();
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
			orderRecords[i].hud = batch.hud;
			orderRecords[i].flood = batch.flood;
			orderRecords[i].flat = batch.flat;
			orderRecords[i].translucent = batch.translucent || IsNativeDecal(batch);
			orderRecords[i].sortDepth = batch.sortDepth;
		}
		const FGLESSceneDrawOrderView drawOrder = gl_GLESInternalSceneSortBatches(
			orderRecords.data(), orderRecords.size(), GLES_SCENE_ORDER_VIEW);
		GLuint boundTextures[3] = { 0, 0, 0 };
		GLuint boundSamplers[3] = { 0, 0, 0 };
		bool textureKnown[3] = { false, false, false };
		auto bindOpaqueTexture = [&](unsigned int unit, GLuint texture, GLuint sampler)
		{
			glActiveTexture(GL_TEXTURE0 + unit);
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
		GLint lastLightPositionRadius = -1;
		GLint lastLightColor = -1;
		GLint lastLightCounts = -1;
		auto uploadOpaqueLights = [&](const FSceneBatch &batch, GLuint program,
			GLint positionRadius, GLint color, GLint counts, const GLfloat *positions,
			const GLfloat *colors, unsigned int normalCount, unsigned int subtractiveCount,
			unsigned int count)
		{
			const bool sameSelection = lightUniformStateKnown && lastLightUniformProgram == program &&
				lastLightUniformOffset == batch.lightOffset && lastLightUniformNormalCount == normalCount &&
				lastLightUniformSubtractiveCount == subtractiveCount && lastLightUniformCount == count &&
				lastLightPositionRadius == positionRadius && lastLightColor == color && lastLightCounts == counts;
			if (sameSelection)
			{
				++Resources.sceneLightUniformUploadSkips;
				return;
			}
			if (positionRadius >= 0 && count > 0) glUniform4fv(positionRadius, count, positions);
			if (color >= 0 && count > 0) glUniform4fv(color, count, colors);
			if (counts >= 0) glUniform3i(counts, static_cast<GLint>(normalCount),
				static_cast<GLint>(subtractiveCount), static_cast<GLint>(count));
			lightUniformStateKnown = true;
			lastLightUniformProgram = program;
			lastLightUniformOffset = batch.lightOffset;
			lastLightUniformNormalCount = normalCount;
			lastLightUniformSubtractiveCount = subtractiveCount;
			lastLightUniformCount = count;
			lastLightPositionRadius = positionRadius;
			lastLightColor = color;
			lastLightCounts = counts;
			++Resources.sceneLightUniformUploads;
		};
		GLuint opaqueStaticUniformProgram = 0;
		bool opaqueStaticUniformsKnown = false;
		for (size_t orderIndex = 0; orderIndex < drawOrder.count; ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder.indices[orderIndex]];
			// Opaque world geometry establishes depth before portal targets and
			// translucent/HUD batches are composited.
			if (batch.skyMask || batch.portalMask || batch.portalId >= 0 || batch.translucent || batch.hud ||
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
				glBindSampler(0, Resources.sceneSampler);
				textureKnown[0] = false;
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
					reinterpret_cast<const void *>(batch.floodWallFirstIndex * sizeof(GLuint)));
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
					glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
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
			const GLuint program = batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
				(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram));
			const char *programName = batch.fog ? (batch.masked ? "gles/fog-masked" : "gles/fog") :
				(batch.masked ? "gles/masked" : (batch.palette ? "gles/palette" : "gles/opaque"));
			BindNativeProgram(program, programName);
			SetNativeGlowUniforms(program, batch);
			const bool setStaticUniforms = !opaqueStaticUniformsKnown ||
				opaqueStaticUniformProgram != program;
			if (setStaticUniforms && program == Resources.sceneProgram && Resources.sceneSkyDepth >= 0)
				glUniform1i(Resources.sceneSkyDepth, 0);
			GLint viewProjection = Resources.sceneViewProjection;
			GLint textureUniform = Resources.sceneTextureUniform;
			GLint brightmapUniform = Resources.sceneBrightmapUniform;
			GLint useBrightmap = Resources.sceneUseBrightmap;
			GLint brightmapDesaturation = Resources.sceneBrightmapDesaturation;
			GLint useTexture = Resources.sceneUseTexture;
			GLint model = Resources.sceneModel;
			GLint textureTransform = Resources.sceneTextureTransform;
			GLint cameraPosition = Resources.sceneCameraPosition;
			GLint objectColor = Resources.sceneObjectColor;
			GLint materialFlags = Resources.sceneMaterialFlags;
			GLint fuzzTime = Resources.sceneFuzzTime;
			GLint alphaCutoff = -1;
			GLint fogColor = -1;
			GLint fogDensity = -1;
			GLint lightPositionRadius = Resources.sceneLightPositionRadius;
			GLint lightColor = Resources.sceneLightColor;
			GLint lightCounts = Resources.sceneLightCounts;
			GLint lightPlaneNormal = Resources.sceneLightPlaneNormal;
			GLint projectedLights = Resources.sceneProjectedLights;
			GLint dynamicLightSampler = Resources.sceneDynamicLightTexture;
			GLint clipPlaneUniform = Resources.sceneClipPlane;
			GLint clipPlaneEnabledUniform = Resources.sceneClipPlaneEnabled;
			if (batch.fog)
			{
				if (batch.masked)
				{
					viewProjection = Resources.fogMaskedViewProjection;
					textureUniform = Resources.fogMaskedTextureUniform;
					brightmapUniform = Resources.fogMaskedBrightmapUniform;
					useBrightmap = Resources.fogMaskedUseBrightmap;
					brightmapDesaturation = Resources.fogMaskedBrightmapDesaturation;
					useTexture = Resources.fogMaskedUseTexture;
					model = Resources.fogMaskedModel;
					textureTransform = Resources.fogMaskedTextureTransform;
					cameraPosition = Resources.fogMaskedCameraPosition;
					objectColor = Resources.fogMaskedObjectColor;
					materialFlags = Resources.fogMaskedMaterialFlags;
					fuzzTime = Resources.fogMaskedFuzzTime;
					alphaCutoff = Resources.fogMaskedAlphaCutoff;
					fogColor = Resources.fogMaskedColor;
					fogDensity = Resources.fogMaskedDensity;
					lightPositionRadius = Resources.fogMaskedLightPositionRadius;
					lightColor = Resources.fogMaskedLightColor;
					lightCounts = Resources.fogMaskedLightCounts;
					lightPlaneNormal = Resources.fogMaskedLightPlaneNormal;
					projectedLights = Resources.fogMaskedProjectedLights;
					dynamicLightSampler = Resources.fogMaskedDynamicLightTexture;
					clipPlaneUniform = Resources.fogMaskedClipPlane;
					clipPlaneEnabledUniform = Resources.fogMaskedClipPlaneEnabled;
				}
				else
				{
					viewProjection = Resources.fogViewProjection;
					textureUniform = Resources.fogTextureUniform;
					brightmapUniform = Resources.fogBrightmapUniform;
					useBrightmap = Resources.fogUseBrightmap;
					brightmapDesaturation = Resources.fogBrightmapDesaturation;
					useTexture = Resources.fogUseTexture;
					model = Resources.fogModel;
					textureTransform = Resources.fogTextureTransform;
					cameraPosition = Resources.fogCameraPosition;
					objectColor = Resources.fogObjectColor;
					materialFlags = Resources.fogMaterialFlags;
					fuzzTime = Resources.fogFuzzTime;
					fogColor = Resources.fogColor;
					fogDensity = Resources.fogDensity;
					lightPositionRadius = Resources.fogLightPositionRadius;
					lightColor = Resources.fogLightColor;
					lightCounts = Resources.fogLightCounts;
					lightPlaneNormal = Resources.fogLightPlaneNormal;
					projectedLights = Resources.fogProjectedLights;
					dynamicLightSampler = Resources.fogDynamicLightTexture;
					clipPlaneUniform = Resources.fogClipPlane;
					clipPlaneEnabledUniform = Resources.fogClipPlaneEnabled;
				}
				if (fogColor >= 0)
					glUniform4f(fogColor, batch.fogColor[0], batch.fogColor[1], batch.fogColor[2], 1.0f);
				if (fogDensity >= 0)
					glUniform1f(fogDensity, batch.fogDensity / 64000.0f);
			}
			else if (batch.masked)
			{
				viewProjection = Resources.maskedViewProjection;
				textureUniform = Resources.maskedTextureUniform;
				brightmapUniform = Resources.maskedBrightmapUniform;
				useBrightmap = Resources.maskedUseBrightmap;
				brightmapDesaturation = Resources.maskedBrightmapDesaturation;
				useTexture = Resources.maskedUseTexture;
				model = Resources.maskedModel;
				textureTransform = Resources.maskedTextureTransform;
				cameraPosition = Resources.maskedCameraPosition;
				objectColor = Resources.maskedObjectColor;
				materialFlags = Resources.maskedMaterialFlags;
				fuzzTime = Resources.maskedFuzzTime;
				alphaCutoff = Resources.maskedAlphaCutoff;
				lightPositionRadius = Resources.maskedLightPositionRadius;
				lightColor = Resources.maskedLightColor;
				lightCounts = Resources.maskedLightCounts;
				lightPlaneNormal = Resources.maskedLightPlaneNormal;
				projectedLights = Resources.maskedProjectedLights;
				dynamicLightSampler = Resources.maskedDynamicLightTexture;
				clipPlaneUniform = Resources.maskedClipPlane;
				clipPlaneEnabledUniform = Resources.maskedClipPlaneEnabled;
			}
			else if (batch.palette)
			{
				viewProjection = Resources.paletteViewProjection;
				textureUniform = Resources.paletteTextureUniform;
				brightmapUniform = Resources.paletteBrightmapUniform;
				useBrightmap = Resources.paletteUseBrightmap;
				brightmapDesaturation = Resources.paletteBrightmapDesaturation;
				useTexture = Resources.paletteUseTexture;
				model = Resources.paletteModel;
				textureTransform = Resources.paletteTextureTransform;
				cameraPosition = Resources.paletteCameraPosition;
				objectColor = Resources.paletteObjectColor;
				materialFlags = Resources.paletteMaterialFlags;
				fuzzTime = Resources.paletteFuzzTime;
				lightPositionRadius = Resources.paletteLightPositionRadius;
				lightColor = Resources.paletteLightColor;
				lightCounts = Resources.paletteLightCounts;
				lightPlaneNormal = Resources.paletteLightPlaneNormal;
				projectedLights = Resources.paletteProjectedLights;
				dynamicLightSampler = Resources.paletteDynamicLightTexture;
				clipPlaneUniform = Resources.paletteClipPlane;
				clipPlaneEnabledUniform = Resources.paletteClipPlaneEnabled;
			}
			static const GLfloat identity[16] =
			{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			};
			if (viewProjection >= 0)
				glUniformMatrix4fv(viewProjection, 1, GL_FALSE, batch.hud ? identity : batch.viewProjection);
			if (setStaticUniforms && model >= 0) glUniformMatrix4fv(model, 1, GL_FALSE, identity);
			if (setStaticUniforms && textureTransform >= 0) glUniform4f(textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
			if (cameraPosition >= 0)
				glUniform3f(cameraPosition, batch.hud ? 0.0f : batch.cameraPosition[0],
					batch.hud ? 0.0f : batch.cameraPosition[1], batch.hud ? 0.0f : batch.cameraPosition[2]);
			if (setStaticUniforms && objectColor >= 0) glUniform4f(objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			if (materialFlags >= 0) glUniform1i(materialFlags, static_cast<GLint>(batch.materialFlags));
			if (setStaticUniforms && fuzzTime >= 0) glUniform1f(fuzzTime, Resources.frame / 35.0f);
			if (clipPlaneUniform >= 0) glUniform4fv(clipPlaneUniform, 1, batch.clipPlane);
			if (clipPlaneEnabledUniform >= 0) glUniform1i(clipPlaneEnabledUniform, batch.clipPlaneEnabled ? 1 : 0);
			if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ||
				(IsNativeDecal(batch) && (batch.materialFlags & GLES_MATERIAL_RED_IS_ALPHA) != 0) ? 0.0f :
				(batch.alphaCutoff > 0.0f ? batch.alphaCutoff : 0.5f));
			const GLfloat *lightPositions = NULL;
			const GLfloat *lightColors = NULL;
			unsigned int lightNormalCount = 0;
			unsigned int lightSubtractiveCount = 0;
			unsigned int lightCount = 0;
			GetNativeLightUpload(batch, 0, GLES_MAX_LIGHTS, lightPositions, lightColors, lightNormalCount,
				lightSubtractiveCount, lightCount);
			uploadOpaqueLights(batch, program, lightPositionRadius, lightColor, lightCounts,
				lightPositions, lightColors, lightNormalCount, lightSubtractiveCount, lightCount);
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
				batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
			bindOpaqueTexture(1, batch.brightmap != 0 ? batch.brightmap : Resources.checkerTexture,
				batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
			bindOpaqueTexture(2, useProjectedLights ? dynamicLightTexture : Resources.checkerTexture,
				Resources.sceneSampler);
			glActiveTexture(GL_TEXTURE0);
			SetNativeDecalDepthBias(batch, true);
			glDrawElements(GL_TRIANGLES, batch.indexCount, GL_UNSIGNED_INT,
				reinterpret_cast<const void *>(batch.firstIndex * sizeof(GLuint)));
			DrawNativeLightOverflow(batch, dynamicLightTexture, program, lightPositionRadius,
				lightColor, lightCounts, projectedLights, batch.indexCount, batch.firstIndex);
			if (batch.lightCount > GLES_MAX_LIGHTS)
				lightUniformStateKnown = false;
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
				glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT,
					reinterpret_cast<const void *>(batch.floodWallFirstIndex * sizeof(GLuint)));
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glStencilMask(0xff);
				glStencilFunc(GL_ALWAYS, 0, 0xff);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				glDepthFunc(GL_LESS);
				glDisable(GL_STENCIL_TEST);
			}
			opaqueStaticUniformProgram = program;
			opaqueStaticUniformsKnown = batch.lightCount <= GLES_MAX_LIGHTS;
		}
		drawSkyMask();
		drawSky();
		DrawNativePortalTargets(dynamicLightTexture);
		// Portal targets must be complete before the outer HUD is drawn.  Reuse
		// the same batch path so blending and shader selection stay consistent.
		glDisable(GL_STENCIL_TEST);
		glStencilMask(0xff);
		glDepthMask(GL_FALSE);
		glBindVertexArray(Resources.sceneVertexArray);
		bool hudViewportReady = NativeOffscreenRender;
		for (size_t orderIndex = 0; orderIndex < drawOrder.count; ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder.indices[orderIndex]];
			if (batch.wipeOverlay || batch.skyMask || batch.portalMask || batch.portalId >= 0 ||
				(!batch.translucent && !batch.hud && !IsNativeDecal(batch)))
				continue;
			if (batch.hud && !hudViewportReady)
			{
				SetNativeFullViewport(activeTarget->renderWidth, activeTarget->renderHeight);
				hudViewportReady = true;
			}
			DrawNativePortalBatch(batch, dynamicLightTexture, 0);
		}
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
		glBindSampler(0, Resources.checkerSampler);
		glBindVertexArray(Resources.vertexArray);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(0));
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(6 * sizeof(GLushort)));
	}
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
	if (!gl_GLES_ResolveRenderTarget(activeTarget))
		I_FatalError("Zandronum GLES scene resolve failed.");
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
	present.sceneSampler = Resources.sceneSampler;
	present.stateWidth = present.presentationTarget.renderWidth;
	present.stateHeight = present.presentationTarget.renderHeight;
	present.gamma = static_cast<float>(Gamma);
	present.brightness = clamp<float>(vid_brightness, -0.8f, 0.8f);
	present.contrast = clamp<float>(vid_contrast, 0.1f, 3.0f);
	if (!gl_GLESInternalPresent(present))
		I_FatalError("Zandronum GLES frame failed.");
	if (!NativeOffscreenRender && HasNativeWipeOverlay() &&
		gl_GLESInternalWipeIsActive())
		DrawNativeWipeOverlay(present.presentationTarget);
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
	if (!developer || NativeFrameStart.time_since_epoch().count() == 0 || Resources.frame == 0 ||
		(Resources.frame != 1 && (Resources.frame % 120) != 0))
		return;
	const std::chrono::duration<double, std::milli> totalElapsed =
		std::chrono::steady_clock::now() - NativeFrameStart;
	const double totalMilliseconds = totalElapsed.count();
	const double renderMilliseconds = MAX(0.0, totalMilliseconds - waitMilliseconds - presentMilliseconds);
	char message[320];
	snprintf(message, sizeof(message),
		"cpu=%.3f ms wait=%.3f ms present=%.3f ms total=%.3f ms limit=%d cap=%d "
		"display=%d effective=%d (%s)", renderMilliseconds, waitMilliseconds,
		presentMilliseconds, totalMilliseconds, configuredLimit, capFPS, displayLimit,
		effectiveLimit, presented ? "ok" : "failed");
	gl_GLES_Report("frame timing", message);
}

bool gl_GLES_EndSceneToTexture(unsigned int targetTexture, int width, int height)
{
	if (!gl_GLES_CanUseResources() || targetTexture == 0 || width <= 0 || height <= 0)
		return false;
	if (Resources.cameraTarget.framebuffer == 0 ||
		Resources.cameraTarget.renderWidth != width || Resources.cameraTarget.renderHeight != height)
	{
		if (!gl_GLES_CreateRenderTarget(&Resources.cameraTarget, width, height, 1))
			return false;
	}

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
	gl_GLES_EndScene();
	NativeActiveTarget = &Resources.cameraTarget;
	NativeOffscreenRender = true;
	gl_GLES_RenderBootstrap(width, height);
	NativeOffscreenRender = false;
	NativeActiveTarget = NULL;
	static bool cameraTargetPixelLogWritten = false;
	if (developer && !cameraTargetPixelLogWritten)
	{
		GLint previousRead = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, Resources.cameraTarget.resolveFramebuffer);
		GLubyte center[4] = {};
		glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
		const GLenum readError = glGetError();
		DPrintf("Zandronum GLES camera target batches=%u center=%u,%u,%u,%u read=%s.\n",
			static_cast<unsigned int>(Resources.sceneBatches.size()), center[0], center[1], center[2], center[3],
			readError == GL_NO_ERROR ? "ok" : "failed");
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousRead));
		cameraTargetPixelLogWritten = true;
	}

	const bool copied = gl_GLESInternalCopyTargetToTexture(Resources.cameraTarget,
		static_cast<GLuint>(targetTexture));
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
	glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
	return copied;
}

bool gl_GLES_ReadScreenshot(unsigned char *rgba, int width, int height)
{
	return gl_GLES_CanUseResources() &&
		gl_GLESInternalReadTarget(Resources.sceneTarget, rgba, width, height);
}

bool gl_GLES_WriteSavePic(FILE *file, int width, int height)
{
	return gl_GLES_CanUseResources() &&
		gl_GLESInternalWriteSavePic(file, Resources.sceneTarget, width, height);
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
	if (!CapabilitiesReady) return;
	Printf("GL_VENDOR: %s\n", Capabilities.vendor);
	Printf("GL_RENDERER: %s\n", Capabilities.renderer);
	Printf("GL_VERSION: %s\n", Capabilities.version);
	Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", Capabilities.shadingLanguageVersion);
	const FGLESContextInfo &context = gl_GLES_GetContextInfo();
	Printf("GLES-compatible capabilities: %s %d.%d, max texture %d, texture units %d, fragment uniforms %d, extensions %d.\n",
		context.isGLES ? "OpenGL ES" : "desktop OpenGL",
		Capabilities.majorVersion, Capabilities.minorVersion, Capabilities.maxTextureSize,
		Capabilities.maxTextureUnits, Capabilities.maxFragmentUniformVectors, Capabilities.extensionCount);
	Printf("GLES core: buffers=%s, arrays=%s, uniform-buffers=%s, framebuffers=%s, depth-stencil=%s, buffer-mapping=%s.\n",
		Capabilities.hasVertexBuffers ? "yes" : "no",
		Capabilities.hasVertexArrays ? "yes" : "no",
		Capabilities.hasUniformBuffers ? "yes" : "no",
		Capabilities.hasFramebuffers ? "yes" : "no",
		Capabilities.hasDepthStencil ? "yes" : "no",
		Capabilities.hasBufferMapping ? "yes" : "deferred");
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

bool gl_GLES_ApplyRenderState(int srcBlend, int dstBlend, int alphaFunc,
	float alphaThreshold, bool alphaTest, int blendEquation, bool fogEnabled,
	bool textureEnabled, int textureMode)
{
	return gl_GLESInternalApplyRenderState(gl_GLES_CanUseResources(), srcBlend, dstBlend,
		alphaFunc, alphaThreshold, alphaTest, blendEquation, fogEnabled, textureEnabled, textureMode);
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
