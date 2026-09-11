#include "gl/system/gl_android.h"
#include "gl/system/gl_gles_targets.h"

#ifdef __ANDROID__

#include <GLES3/gl32.h>
#include <algorithm>
#include <math.h>
#include <stdio.h>
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
#include "f_wipe.h"
#include "m_misc.h"
#include "m_png.h"

extern TexFilter_s TexFilter[];
EXTERN_CVAR(Float, skyoffset)
EXTERN_CVAR(Float, Gamma)
EXTERN_CVAR(Float, vid_brightness)
EXTERN_CVAR(Float, vid_contrast)
EXTERN_CVAR(Int, gl_vid_multisample)
EXTERN_CVAR(Bool, gl_no_skyclear)
extern int skyfog;

namespace
{
	static const unsigned int ANDROID_NATIVE_MAX_LIGHTS = 16;

	struct FSkyPrimitiveRange
	{
		GLsizei firstIndex;
		GLsizei indexCount;
	};

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
		bool model;
		bool cullBackFaces;
		bool skyMask;
		bool flood;
		GLsizei floodWallFirstIndex;
		unsigned int materialFlags;
		EAndroidNativeBlendMode blendMode;
		float sortDepth;
		float fogColor[3];
		float fogDensity;
		unsigned int lightCount;
		unsigned int lightNormalCount;
		unsigned int lightSubtractiveCount;
		float lightPlaneNormal[3];
		float lightPositionRadius[ANDROID_NATIVE_MAX_LIGHTS * 4];
		float lightColor[ANDROID_NATIVE_MAX_LIGHTS * 4];
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

	struct FAndroidNativeTexture
	{
		const void *key;
		int colormap;
		int translation;
		int width;
		int height;
		bool repeat;
		bool allowhires;
		bool palette;
		GLuint texture;
		std::vector<unsigned char> pixels;
	};

	struct FAndroidGLESResources
	{
		GLuint sceneProgram;
		GLuint fogProgram;
		GLuint fogMaskedProgram;
		GLuint maskedProgram;
		GLuint paletteProgram;
		GLuint presentProgram;
		GLuint sceneVertexArray;
		GLuint sceneVertexBuffer;
		GLuint sceneIndexBuffer;
		GLuint skyVertexArray;
		GLuint skyVertexBuffer;
		GLuint skyIndexBuffer;
		GLuint vertexArray;
		GLuint vertexBuffer;
		GLuint indexBuffer;
		GLuint checkerTexture;
		GLuint checkerSampler;
		GLuint sceneSampler;
		int textureFilter;
		FGLESTargetDescriptor sceneTarget;
		GLuint wipeStartTexture;
		GLuint wipeEndTexture;
		bool wipeStartReady;
		bool wipeEndReady;
		bool wipeEndCapturePending;
		bool wipeActive;
		int wipeType;
		float wipeProgress;
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
		GLint presentTexture;
		GLint presentTextureTransform;
		GLint presentDepth;
		GLint presentWipeStart;
		GLint presentWipeEnd;
		GLint presentWipeProgress;
		GLint presentWipeType;
		GLint presentWipeActive;
		GLint presentGamma;
		GLint presentBrightness;
		GLint presentContrast;
		GLint fogColor;
		GLint fogDensity;
		unsigned int frame;
		GLsizei sceneIndexCount;
		std::vector<FSceneVertex> sceneVertices;
		// GLES 3.2 core indices keep large model surfaces in the shared stream.
		std::vector<GLuint> sceneIndices;
		std::vector<FSceneBatch> sceneBatches;
		std::vector<FNativePortalTarget> portalTargets;
		FNativeSkyRecord outerSky;
		std::vector<FAndroidNativeTexture> nativeTextures;
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
		FSkyPrimitiveRange skyUpperCap;
		FSkyPrimitiveRange skyUpperStrips[4];
		FSkyPrimitiveRange skyLowerCap;
		FSkyPrimitiveRange skyLowerStrips[4];
		FSkyPrimitiveRange skyboxFaces[6];
		int skyColumns;
		unsigned int sceneSpriteCount;
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
		bool stateWarningLogged;
		bool framebufferWarningLogged;
	};

	static FAndroidGLESInfo Capabilities = {};
	static FAndroidGLESResources Resources = {};
	static bool CapabilitiesReady = false;
	static bool NativeBackendEnabled = false;
	static bool BootstrapPauseLogged = false;
	static bool SceneInputLogged = false;
	static bool SkyLogged = false;
	static bool FlatCollectionDeferred = false;
	static std::vector<unsigned int> NativePortalCaptureStack;
	static const unsigned int AndroidNativeMaxSceneVertices = 1u << 20;
	// Bits 0 and 1 belong to the outer sky and flood masks. Each portal target
	// owns a non-overlapping pair in the remaining eight-bit stencil value.
	static const unsigned int AndroidNativeMaxPortalTargets = 3;
	static GLuint NativePortalStencilBit(unsigned int id)
	{
		return 1u << (2u + id * 2u);
	}
	static GLuint NativePortalSkyStencilBit(unsigned int id)
	{
		return 1u << (3u + id * 2u);
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
	CVAR(Bool, gl_android_test_pattern, false, CVAR_DEBUGONLY)
	CVAR(Bool, gl_android_shader_test_failure, false, CVAR_DEBUGONLY)

	static bool HasExtension(const char *name)
	{
		for (GLint index = 0; index < Capabilities.extensionCount; ++index)
		{
			const char *extension = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, index));
			if (extension != NULL && strcmp(extension, name) == 0) return true;
		}
		return false;
	}

	static bool IsPaletteTexture(GLuint texture)
	{
		if (texture == 0) return false;
		for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
		{
			const FAndroidNativeTexture &entry = Resources.nativeTextures[i];
			if (entry.texture == texture) return entry.palette;
		}
		return false;
	}

	static GLuint BindNativeProgram(GLuint fallback, const char *name)
	{
		return static_cast<GLuint>(gl_AndroidNativeGLES_BindShaderProgram(name, fallback));
	}

	static GLenum CheckError(const char *site)
	{
		GLenum error = glGetError();
		if (error != GL_NO_ERROR)
			Printf("Android GLES error at %s: 0x%04x.\n", site, error);
		return error;
	}

	static float ClampUnit(float value)
	{
		return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
	}

	static GLuint CompileShader(GLenum type, const char *source, const char *label, const char *defines)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, NULL);
		glCompileShader(shader);
		GLint compiled = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (!compiled)
		{
			char log[1024] = {};
			GLsizei length = 0;
			glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
			Printf("Android GLES %s shader defines:\n%s\nsource:\n%s\n", label, defines, source);
			I_FatalError("Android GLES %s shader failed: %s", label, log);
		}
		return shader;
	}

	static GLuint LinkProgram(const char *vertexSource, const char *fragmentSource, const char *label,
		const char *defines = "")
	{
		GLuint vertex = CompileShader(GL_VERTEX_SHADER, vertexSource, label, defines);
		GLuint fragment = CompileShader(GL_FRAGMENT_SHADER, fragmentSource, label, defines);
		GLuint program = glCreateProgram();
		glAttachShader(program, vertex);
		glAttachShader(program, fragment);
		glBindAttribLocation(program, 0, "a_position");
		glBindAttribLocation(program, 1, "a_uv");
		glBindAttribLocation(program, 2, "a_color");
		glBindAttribLocation(program, 3, "a_normal");
		glBindAttribLocation(program, 4, "a_secondary");
		glLinkProgram(program);
		GLint linked = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		if (!linked)
		{
			char log[1024] = {};
			GLsizei length = 0;
			glGetProgramInfoLog(program, sizeof(log) - 1, &length, log);
			Printf("Android GLES %s defines:\n%s\nvertex source:\n%s\nfragment source:\n%s\n",
				label, defines, vertexSource, fragmentSource);
			I_FatalError("Android GLES %s program failed: %s", label, log);
		}
		return program;
	}

	static void DeleteResources(bool clearTextureCache)
	{
		if (Resources.sceneProgram != 0) glDeleteProgram(Resources.sceneProgram);
		if (Resources.fogProgram != 0) glDeleteProgram(Resources.fogProgram);
		if (Resources.fogMaskedProgram != 0) glDeleteProgram(Resources.fogMaskedProgram);
		if (Resources.maskedProgram != 0) glDeleteProgram(Resources.maskedProgram);
		if (Resources.paletteProgram != 0) glDeleteProgram(Resources.paletteProgram);
		if (Resources.presentProgram != 0) glDeleteProgram(Resources.presentProgram);
		if (Resources.sceneVertexBuffer != 0) glDeleteBuffers(1, &Resources.sceneVertexBuffer);
		if (Resources.sceneIndexBuffer != 0) glDeleteBuffers(1, &Resources.sceneIndexBuffer);
		if (Resources.sceneVertexArray != 0) glDeleteVertexArrays(1, &Resources.sceneVertexArray);
		if (Resources.skyVertexBuffer != 0) glDeleteBuffers(1, &Resources.skyVertexBuffer);
		if (Resources.skyIndexBuffer != 0) glDeleteBuffers(1, &Resources.skyIndexBuffer);
		if (Resources.skyVertexArray != 0) glDeleteVertexArrays(1, &Resources.skyVertexArray);
		if (Resources.vertexBuffer != 0) glDeleteBuffers(1, &Resources.vertexBuffer);
		if (Resources.indexBuffer != 0) glDeleteBuffers(1, &Resources.indexBuffer);
		if (Resources.vertexArray != 0) glDeleteVertexArrays(1, &Resources.vertexArray);
		if (Resources.checkerTexture != 0) glDeleteTextures(1, &Resources.checkerTexture);
		if (Resources.wipeStartTexture != 0) glDeleteTextures(1, &Resources.wipeStartTexture);
		if (Resources.wipeEndTexture != 0) glDeleteTextures(1, &Resources.wipeEndTexture);
		for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
			if (Resources.nativeTextures[i].texture != 0) glDeleteTextures(1, &Resources.nativeTextures[i].texture);
		if (Resources.checkerSampler != 0) glDeleteSamplers(1, &Resources.checkerSampler);
		if (Resources.sceneSampler != 0) glDeleteSamplers(1, &Resources.sceneSampler);
		gl_GLES_DestroyRenderTarget(&Resources.sceneTarget);
		Resources.sceneProgram = 0;
		Resources.fogProgram = 0;
		Resources.fogMaskedProgram = 0;
		Resources.maskedProgram = 0;
		Resources.paletteProgram = 0;
		Resources.presentProgram = 0;
		Resources.sceneVertexBuffer = 0;
		Resources.sceneIndexBuffer = 0;
		Resources.sceneVertexArray = 0;
		Resources.skyVertexBuffer = 0;
		Resources.skyIndexBuffer = 0;
		Resources.skyVertexArray = 0;
		Resources.vertexBuffer = 0;
		Resources.indexBuffer = 0;
		Resources.vertexArray = 0;
		Resources.checkerTexture = 0;
		Resources.wipeStartTexture = 0;
		Resources.wipeEndTexture = 0;
		Resources.wipeStartReady = false;
		Resources.wipeEndReady = false;
		Resources.wipeEndCapturePending = false;
		Resources.wipeActive = false;
		Resources.wipeType = wipe_None;
		Resources.wipeProgress = 0.0f;
		Resources.checkerSampler = 0;
		Resources.sceneSampler = 0;
		Resources.textureFilter = -1;
		Resources.sceneTarget = {};
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
		Resources.presentTexture = -1;
		Resources.presentTextureTransform = -1;
		Resources.presentDepth = -1;
		Resources.presentWipeStart = -1;
		Resources.presentWipeEnd = -1;
		Resources.presentWipeProgress = -1;
		Resources.presentWipeType = -1;
		Resources.presentWipeActive = -1;
		Resources.presentGamma = -1;
		Resources.presentBrightness = -1;
		Resources.presentContrast = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
		Resources.sceneBatches.clear();
		Resources.portalTargets.clear();
		NativePortalCaptureStack.clear();
		Resources.skyMaterial = NULL;
		Resources.skyXOffset = 0.0f;
		Resources.skyYOffset = 0.0f;
		Resources.skyLayerMaterial = NULL;
		Resources.skyLayerXOffset = 0.0f;
		Resources.skyLayerYOffset = 0.0f;
		Resources.sky2 = false;
		if (clearTextureCache) Resources.nativeTextures.clear();
		Resources.sceneReady = false;
		memset(Resources.viewProjection, 0, sizeof(Resources.viewProjection));
		Resources.ready = false;
	}

	static void InvalidateResources()
	{
		Resources.sceneProgram = 0;
		Resources.fogProgram = 0;
		Resources.fogMaskedProgram = 0;
		Resources.maskedProgram = 0;
		Resources.paletteProgram = 0;
		Resources.presentProgram = 0;
		Resources.sceneVertexBuffer = 0;
		Resources.sceneIndexBuffer = 0;
		Resources.sceneVertexArray = 0;
		Resources.skyVertexBuffer = 0;
		Resources.skyIndexBuffer = 0;
		Resources.skyVertexArray = 0;
		Resources.vertexBuffer = 0;
		Resources.indexBuffer = 0;
		Resources.vertexArray = 0;
		Resources.checkerTexture = 0;
		Resources.sceneTarget = {};
		Resources.wipeStartTexture = 0;
		Resources.wipeEndTexture = 0;
		Resources.wipeStartReady = false;
		Resources.wipeEndReady = false;
		Resources.wipeEndCapturePending = false;
		Resources.wipeActive = false;
		Resources.wipeType = wipe_None;
		Resources.wipeProgress = 0.0f;
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
		Resources.presentTexture = -1;
		Resources.presentTextureTransform = -1;
		Resources.presentDepth = -1;
		Resources.presentWipeStart = -1;
		Resources.presentWipeEnd = -1;
		Resources.presentWipeProgress = -1;
		Resources.presentWipeType = -1;
		Resources.presentWipeActive = -1;
		Resources.presentGamma = -1;
		Resources.presentBrightness = -1;
		Resources.presentContrast = -1;
		Resources.fogColor = -1;
		Resources.fogDensity = -1;
		Resources.sceneIndexCount = 0;
		Resources.sceneVertices.clear();
		Resources.sceneIndices.clear();
		Resources.sceneBatches.clear();
		Resources.portalTargets.clear();
		NativePortalCaptureStack.clear();
		Resources.skyMaterial = NULL;
		Resources.skyXOffset = 0.0f;
		Resources.skyYOffset = 0.0f;
		Resources.skyLayerMaterial = NULL;
		Resources.skyLayerXOffset = 0.0f;
		Resources.skyLayerYOffset = 0.0f;
		Resources.sky2 = false;
		for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
			Resources.nativeTextures[i].texture = 0;
		Resources.sceneReady = false;
		memset(Resources.viewProjection, 0, sizeof(Resources.viewProjection));
		Resources.ready = false;
		Resources.stateWarningLogged = false;
		Resources.framebufferWarningLogged = false;
	}

	static void ResetState(int width, int height)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glViewport(0, 0, width, height);
		glScissor(0, 0, width, height);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);
		glDisable(GL_STENCIL_TEST);
		glStencilMask(0xffffffffu);
		glStencilFunc(GL_ALWAYS, 0, 0xffffffffu);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glDisable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glDisable(GL_POLYGON_OFFSET_FILL);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBlendEquation(GL_FUNC_ADD);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glActiveTexture(GL_TEXTURE0);
		glBindSampler(0, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindSampler(1, 0);
		glActiveTexture(GL_TEXTURE2);
		glBindSampler(2, 0);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(0);
		glUseProgram(0);
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
		glSamplerParameteri(Resources.sceneSampler, GL_TEXTURE_MIN_FILTER, settings.magfilter);
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
				Printf("Android GLES render target could not be allocated at %dx%d.\n", width, height);
			}
			return false;
		}
		return true;
	}

	static bool BuildWipeTexture(GLuint &texture)
	{
		if (texture != 0) return true;
		const int width = Resources.sceneTarget.renderWidth;
		const int height = Resources.sceneTarget.renderHeight;
		if (width <= 0 || height <= 0) return false;
		GLint previousActiveTexture = GL_TEXTURE0;
		GLint previousTexture = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
			GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		if (CheckError("wipe texture allocation") != GL_NO_ERROR)
		{
			glDeleteTextures(1, &texture);
			texture = 0;
			return false;
		}
		return true;
	}

	static bool CaptureWipeTexture(GLuint texture)
	{
		if (texture == 0 || Resources.sceneTarget.resolveFramebuffer == 0) return false;
		GLint previousFramebuffer = 0;
		GLint previousActiveTexture = GL_TEXTURE0;
		GLint previousTexture = 0;
		glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
		glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
		glBindFramebuffer(GL_FRAMEBUFFER, Resources.sceneTarget.resolveFramebuffer);
		glBindTexture(GL_TEXTURE_2D, texture);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0,
			Resources.sceneTarget.renderWidth, Resources.sceneTarget.renderHeight);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
		glActiveTexture(static_cast<GLenum>(previousActiveTexture));
		glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
		return CheckError("wipe scene capture") == GL_NO_ERROR;
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
	}

	static unsigned int AddSceneVertex(const FSceneVertex &vertex)
	{
		if (Resources.sceneVertices.size() >= AndroidNativeMaxSceneVertices) return 0xffffffffu;
		Resources.sceneVertices.push_back(vertex);
		return static_cast<unsigned int>(Resources.sceneVertices.size() - 1);
	}

	static void CopyNativeLightData(FSceneBatch &batch, const float *lightData,
		const unsigned int *lightCounts)
	{
		if (lightData == NULL || lightCounts == NULL) return;
		// The engine stores two vec4 records per light: position and color.
		batch.lightNormalCount = std::min(lightCounts[0] / 2, ANDROID_NATIVE_MAX_LIGHTS);
		batch.lightSubtractiveCount = std::min(lightCounts[1] / 2, ANDROID_NATIVE_MAX_LIGHTS);
		batch.lightCount = std::min(lightCounts[2] / 2, ANDROID_NATIVE_MAX_LIGHTS);
		if (batch.lightSubtractiveCount > batch.lightCount) batch.lightSubtractiveCount = batch.lightCount;
		if (batch.lightNormalCount > batch.lightSubtractiveCount) batch.lightNormalCount = batch.lightSubtractiveCount;
		for (unsigned int light = 0; light < batch.lightCount; ++light)
		{
			memcpy(batch.lightPositionRadius + light * 4, lightData + light * 8,
				4 * sizeof(float));
			memcpy(batch.lightColor + light * 4, lightData + light * 8 + 4,
				4 * sizeof(float));
		}
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
	}

	static void AddSceneQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked, bool fog, bool translucent, bool repeat,
		EAndroidNativeBlendMode blendMode, const float *fogColor, float fogDensity, unsigned int materialFlags,
		const float *lightData, const unsigned int *lightCounts, GLuint brightmap = 0,
		int brightmapDesaturation = 0)
	{
		const unsigned int first = AddSceneVertex(a);
		const unsigned int second = AddSceneVertex(b);
		const unsigned int third = AddSceneVertex(c);
		const unsigned int fourth = AddSceneVertex(d);
		if (fourth == 0xffffffffu || third == 0xffffffffu || second == 0xffffffffu || first == 0xffffffffu)
			return;
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
		batch.translucent = translucent || blendMode != ANDROID_BLEND_OPAQUE;
		batch.repeat = repeat;
		batch.palette = IsPaletteTexture(texture);
		batch.materialFlags = materialFlags;
		batch.blendMode = blendMode;
		SetLightPlaneNormal(batch, a, b, c);
		CopyNativeLightData(batch, lightData, lightCounts);
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

	static void AddHUDQuad(const FSceneVertex &a, const FSceneVertex &b,
		const FSceneVertex &c, const FSceneVertex &d, GLuint texture, bool masked,
		EAndroidNativeBlendMode blendMode, unsigned int materialFlags)
	{
		const size_t batchCount = Resources.sceneBatches.size();
		const bool translucent = masked || blendMode != ANDROID_BLEND_OPAQUE || a.a < 0.999f ||
			b.a < 0.999f || c.a < 0.999f || d.a < 0.999f;
		AddSceneQuad(a, b, c, d, texture, masked, false, translucent, false, blendMode, NULL, 0.0f, materialFlags,
			NULL, NULL);
		if (Resources.sceneBatches.size() > batchCount) Resources.sceneBatches.back().hud = true;
	}

	static void UploadSceneGeometry()
	{
		Resources.sceneIndexCount = static_cast<GLsizei>(Resources.sceneIndices.size());
		Resources.sceneReady = Resources.sceneIndexCount > 0;
		if (!Resources.sceneReady) return;
		glBindVertexArray(Resources.sceneVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.sceneVertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, Resources.sceneVertices.size() * sizeof(FSceneVertex),
			&Resources.sceneVertices[0], GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.sceneIndexBuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, Resources.sceneIndices.size() * sizeof(GLuint),
			&Resources.sceneIndices[0], GL_DYNAMIC_DRAW);
		glBindVertexArray(0);
		CheckError("scene geometry upload");
	}

	static void AddSkyMaskCaps(FNativeSkyRecord &sky)
	{
		if (sky.capsAdded || !sky.capEligible) return;
		if (Resources.sceneVertices.size() + 8 > AndroidNativeMaxSceneVertices)
		{
			++sky.rejected;
			return;
		}
		const float extent = 32767.0f;
		const float heights[2] = { extent, -extent };
		for (int cap = 0; cap < 2; ++cap)
		{
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

	static GLsizei UploadSkyGeometry(FMaterial *material, float xOffset, float yOffset, bool mirrored)
	{
		// Keep Zandronum's four-row, 60-degree dome and expose each primitive range explicitly.
		const float radius = 10000.0f;
		const int rows = 4;
		const int columns = 4 * std::max(gl_sky_detail > 0 ? gl_sky_detail : 1, 1);
		Resources.skyColumns = columns;
		std::vector<FSceneVertex> vertices;
		std::vector<GLushort> indices;
		vertices.reserve((columns + rows * (columns + 1)) * 2);
		indices.reserve((columns * 3 + rows * (columns + 1) * 2) * 2);
		Resources.skyUpperCap = {};
		Resources.skyLowerCap = {};
		for (int row = 0; row < rows; ++row)
		{
			Resources.skyUpperStrips[row] = {};
			Resources.skyLowerStrips[row] = {};
		}
		const int textureWidth = material != NULL ?
			std::max(1, material->TextureWidth(GLUSE_TEXTURE)) : 256;
		const int textureHeight = material != NULL ?
			std::max(1, material->TextureHeight(GLUSE_TEXTURE)) : 128;
		float timesRepeat = static_cast<float>(static_cast<short>(4.0f * (256.0f / textureWidth)));
		if (timesRepeat == 0.0f) timesRepeat = 1.0f;
		const float textureVOffset = yOffset / static_cast<float>(textureHeight);
		float verticalScale = 1.0f;
		float verticalOffset = 0.0f;
		float textureVScale = 1.0f;
		if (material == NULL)
		{
			// The source sky-fog pass renders the unscaled dome without a material.
		}
		else if (textureHeight < 128)
		{
			verticalOffset = -1250.0f;
			verticalScale = 128.0f / 230.0f;
			// Keep Zandronum's integer small-sky texture scale.
			textureVScale = static_cast<float>(128 / textureHeight);
		}
		else if (textureHeight < 200)
		{
			verticalOffset = -1250.0f;
			verticalScale = textureHeight / 230.0f;
		}
		else if (textureHeight <= 240)
		{
			verticalOffset = (200.0f - textureHeight + material->tex->SkyOffset + skyoffset) * 57.0f;
			verticalScale = 1.0f + ((textureHeight - 200.0f) / 200.0f) * 1.17f;
		}
		else
		{
			verticalOffset = (-40.0f + material->tex->SkyOffset + skyoffset) * 57.0f;
			verticalScale = 1.2f * 1.17f;
			textureVScale = 240.0f / textureHeight;
		}
		const float skyCenter[3] = { Resources.cameraX, Resources.cameraZ + verticalOffset, Resources.cameraY };
		const float rotation = (-180.0f + xOffset) * 3.14159265359f / 180.0f;
		const float rotationCos = cosf(rotation);
		const float rotationSin = sinf(rotation);
		auto addVertex = [&](int row, int column, bool lower)
		{
			const float side = 1.04719755f * (rows - row) / rows;
			const float ringRadius = radius * cosf(side);
			float ringHeight = radius * sinf(side);
			if (lower) ringHeight = -ringHeight;
			ringHeight *= verticalScale;
			// The source translates non-terminal rows after scaling the dome.
			if (row != rows) ringHeight += 300.0f;
			const float angle = 6.28318531f * column / columns;
			const float localX = -ringRadius * cosf(angle);
			const float localZ = ringRadius * sinf(angle);
			const float sourceU = -timesRepeat * column / columns;
			const float u = mirrored ? -sourceU : sourceU;
			const float v = lower ? (1.0f + (rows - row) / static_cast<float>(rows)) :
				(row / static_cast<float>(rows));
			const float rotatedX = rotationCos * localX + rotationSin * localZ;
			const float rotatedZ = -rotationSin * localX + rotationCos * localZ;
			FSceneVertex vertex = {};
			vertex.x = skyCenter[0] + rotatedX;
			vertex.y = skyCenter[1] + ringHeight - 1.0f;
			vertex.z = skyCenter[2] + rotatedZ;
			vertex.u = u;
			vertex.v = v * textureVScale + textureVOffset;
			vertex.r = vertex.g = vertex.b = 1.0f;
			vertex.a = row == 0 ? 0.0f : 1.0f;
			vertices.push_back(vertex);
		};
		auto addHemisphere = [&](bool lower)
		{
			FSkyPrimitiveRange &cap = lower ? Resources.skyLowerCap : Resources.skyUpperCap;
			FSkyPrimitiveRange *strips = lower ? Resources.skyLowerStrips : Resources.skyUpperStrips;
			// Keep the cap independent from the alpha-faded strips. A center and
			// duplicated perimeter make the solid polygon closed at every detail.
			const int capRow = material == NULL ? 0 : 1;
			const float capSide = 1.04719755f * (rows - capRow) / rows;
			const float capHeight = radius * sinf(capSide) * verticalScale;
			FSceneVertex capCenter = {};
			capCenter.x = skyCenter[0];
			capCenter.y = skyCenter[1] + (lower ? -capHeight : capHeight) + 300.0f - 1.0f;
			capCenter.z = skyCenter[2];
			capCenter.u = capCenter.v = 0.0f;
			capCenter.r = capCenter.g = capCenter.b = capCenter.a = 1.0f;
			const GLushort capCenterVertex = static_cast<GLushort>(vertices.size());
			vertices.push_back(capCenter);
			const GLushort firstVertex = static_cast<GLushort>(vertices.size());
			for (int column = 0; column <= columns; ++column)
				addVertex(capRow, column, lower);
			cap.firstIndex = static_cast<GLsizei>(indices.size());
			for (int column = 0; column < columns; ++column)
			{
				indices.push_back(capCenterVertex);
				indices.push_back(static_cast<GLushort>(firstVertex + column));
				indices.push_back(static_cast<GLushort>(firstVertex + column + 1));
			}
			cap.indexCount = static_cast<GLsizei>(indices.size()) - cap.firstIndex;
			for (int row = 0; row < rows; ++row)
			{
				const GLushort rowStart = static_cast<GLushort>(vertices.size());
				for (int column = 0; column <= columns; ++column)
				{
					addVertex(row + (lower ? 1 : 0), column, lower);
					addVertex(row + (lower ? 0 : 1), column, lower);
				}
				strips[row].firstIndex = static_cast<GLsizei>(indices.size());
				for (int column = 0; column < 2 * (columns + 1); ++column)
					indices.push_back(static_cast<GLushort>(rowStart + column));
				strips[row].indexCount = static_cast<GLsizei>(indices.size()) - strips[row].firstIndex;
			}
		};
		addHemisphere(false);
		addHemisphere(true);
		glBindVertexArray(Resources.skyVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.skyVertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(FSceneVertex), &vertices[0], GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.skyIndexBuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLushort), &indices[0], GL_DYNAMIC_DRAW);
		glBindVertexArray(0);
		return static_cast<GLsizei>(indices.size());
	}

	static void RotateSkyboxPoint(float x, float y, float z, float angle,
		bool sky2, float &outX, float &outY, float &outZ)
	{
		const FVector3 &sourceAxis = sky2 ? glset.skyrotatevector2 : glset.skyrotatevector;
		float axisX = sourceAxis.X;
		float axisY = sourceAxis.Z;
		float axisZ = sourceAxis.Y;
		const float axisLength = sqrtf(axisX * axisX + axisY * axisY + axisZ * axisZ);
		if (axisLength > 0.0001f)
		{
			axisX /= axisLength;
			axisY /= axisLength;
			axisZ /= axisLength;
		}
		else
		{
			axisX = 0.0f;
			axisY = 0.0f;
			axisZ = 1.0f;
		}
		const float radians = angle * 3.14159265359f / 180.0f;
		const float sine = sinf(radians);
		const float cosine = cosf(radians);
		const float dot = axisX * x + axisY * y + axisZ * z;
		const float crossX = axisY * z - axisZ * y;
		const float crossY = axisZ * x - axisX * z;
		const float crossZ = axisX * y - axisY * x;
		const float oneMinusCosine = 1.0f - cosine;
		outX = x * cosine + crossX * sine + axisX * dot * oneMinusCosine;
		outY = y * cosine + crossY * sine + axisY * dot * oneMinusCosine;
		outZ = z * cosine + crossZ * sine + axisZ * dot * oneMinusCosine;
	}

	static void UploadSkyboxGeometry(float xOffset, bool sky2, bool fliptop)
	{
		static const float sidePositions[4][4][3] =
		{
			{ { 128.0f, 128.0f, -128.0f }, { -128.0f, 128.0f, -128.0f },
				{ -128.0f, -128.0f, -128.0f }, { 128.0f, -128.0f, -128.0f } },
			{ { -128.0f, 128.0f, -128.0f }, { -128.0f, 128.0f, 128.0f },
				{ -128.0f, -128.0f, 128.0f }, { -128.0f, -128.0f, -128.0f } },
			{ { -128.0f, 128.0f, 128.0f }, { 128.0f, 128.0f, 128.0f },
				{ 128.0f, -128.0f, 128.0f }, { -128.0f, -128.0f, 128.0f } },
			{ { 128.0f, 128.0f, 128.0f }, { 128.0f, 128.0f, -128.0f },
				{ 128.0f, -128.0f, -128.0f }, { 128.0f, -128.0f, 128.0f } }
		};
		static const float topPositions[2][4][3] =
		{
			{ { 128.0f, 128.0f, -128.0f }, { -128.0f, 128.0f, -128.0f },
				{ -128.0f, 128.0f, 128.0f }, { 128.0f, 128.0f, 128.0f } },
			{ { 128.0f, 128.0f, 128.0f }, { -128.0f, 128.0f, 128.0f },
				{ -128.0f, 128.0f, -128.0f }, { 128.0f, 128.0f, -128.0f } }
		};
		static const float bottomPositions[4][3] =
		{
			{ 128.0f, -128.0f, -128.0f }, { -128.0f, -128.0f, -128.0f },
			{ -128.0f, -128.0f, 128.0f }, { 128.0f, -128.0f, 128.0f }
		};
		std::vector<FSceneVertex> vertices;
		std::vector<GLushort> indices;
		vertices.reserve(24);
		indices.reserve(36);
		const float centerX = Resources.cameraX;
		const float centerY = Resources.cameraZ;
		const float centerZ = Resources.cameraY;
		const float rotation = -180.0f + xOffset;
		for (int face = 0; face < 6; ++face)
		{
			Resources.skyboxFaces[face] = {};
			const GLushort firstVertex = static_cast<GLushort>(vertices.size());
			for (int vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
			{
				const float *position = face < 4 ? sidePositions[face][vertexIndex] :
					(face == 4 ? topPositions[fliptop ? 1 : 0][vertexIndex] : bottomPositions[vertexIndex]);
				float rotatedX, rotatedY, rotatedZ;
				RotateSkyboxPoint(position[0], position[1], position[2], rotation, sky2,
					rotatedX, rotatedY, rotatedZ);
				FSceneVertex vertex = {};
				vertex.x = centerX + rotatedX;
				vertex.y = centerY + rotatedY;
				vertex.z = centerZ + rotatedZ;
				vertex.u = vertexIndex == 1 || vertexIndex == 2 ? 1.0f : 0.0f;
				vertex.v = vertexIndex >= 2 ? 1.0f : 0.0f;
				vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
				vertices.push_back(vertex);
			}
			Resources.skyboxFaces[face].firstIndex = static_cast<GLsizei>(indices.size());
			indices.push_back(firstVertex + 0);
			indices.push_back(firstVertex + 1);
			indices.push_back(firstVertex + 2);
			indices.push_back(firstVertex + 2);
			indices.push_back(firstVertex + 3);
			indices.push_back(firstVertex + 0);
			Resources.skyboxFaces[face].indexCount = 6;
		}
		glBindVertexArray(Resources.skyVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.skyVertexBuffer);
		glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(FSceneVertex), &vertices[0], GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.skyIndexBuffer);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(GLushort), &indices[0], GL_DYNAMIC_DRAW);
		glBindVertexArray(0);
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
			"out vec2 v_uv;\n"
			"out vec4 v_color;\n"
			"out vec3 v_world_position;\n"
			"out float v_camera_distance;\n"
			"void main() { vec4 world_position = u_model * vec4(a_position, 1.0); gl_Position = u_view_projection * world_position; if (u_sky_depth) gl_Position.z = clamp(gl_Position.z, -gl_Position.w, gl_Position.w); v_world_position = world_position.xyz; v_uv = a_uv * u_texture_transform.xy + u_texture_transform.zw; v_color = vec4(a_color.rgb * u_object_color.rgb, (u_sky_fog ? 1.0 : a_color.a) * u_object_color.a); v_camera_distance = distance(world_position.xyz, u_camera_position); }\n";
		static const char *sceneFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[16];\n"
			"uniform vec4 u_light_color[16];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 16; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a *= texel.r * v_color.r; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; frag_color = color; }\n";
		static const char *maskedFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform float u_alpha_cutoff;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[16];\n"
			"uniform vec4 u_light_color[16];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 16; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; if (texel.a < u_alpha_cutoff) discard; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a *= texel.r * v_color.r; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; frag_color = color; }\n";
		static const char *fogFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
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
			"uniform vec4 u_light_position_radius[16];\n"
			"uniform vec4 u_light_color[16];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 16; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a *= texel.r * v_color.r; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; float fog = clamp(exp(-u_fog_density * v_camera_distance), 0.0, 1.0); frag_color = vec4(mix(u_fog_color.rgb, color.rgb, fog), color.a); }\n";
		static const char *fogMaskedFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
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
			"uniform vec4 u_light_position_radius[16];\n"
			"uniform vec4 u_light_color[16];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 16; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; if (texel.a < u_alpha_cutoff) discard; vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a *= texel.r * v_color.r; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; float fog = clamp(exp(-u_fog_density * v_camera_distance), 0.0, 1.0); frag_color = vec4(mix(u_fog_color.rgb, color.rgb, fog), color.a); }\n";
		static const char *paletteFragmentSource =
			"#version 320 es\n"
			"precision highp float;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"in vec3 v_world_position;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_brightmap;\n"
			"uniform bool u_use_texture;\n"
			"uniform bool u_use_brightmap;\n"
			"uniform int u_brightmap_desaturation;\n"
			"uniform int u_material_flags;\n"
			"uniform float u_fuzz_time;\n"
			"uniform vec4 u_light_position_radius[16];\n"
			"uniform vec4 u_light_color[16];\n"
			"uniform ivec3 u_light_counts;\n"
			"uniform vec3 u_light_plane_normal;\n"
			"uniform bool u_projected_lights;\n"
			"uniform sampler2D u_dynamic_light_texture;\n"
			"uniform vec4 u_clip_plane;\n"
			"uniform bool u_clip_plane_enabled;\n"
			"vec3 sample_brightmap() { vec3 bright = texture(u_brightmap, v_uv).rgb; float gray = dot(bright, vec3(0.3, 0.56, 0.14)); return mix(bright, vec3(gray), clamp(float(u_brightmap_desaturation) / 31.0, 0.0, 1.0)); }\n"
			"void apply_dynamic_lights(vec3 base, out vec3 lighting, out vec3 additive) { vec3 regular = vec3(0.0); vec3 subtractive = vec3(0.0); additive = vec3(0.0); for (int i = 0; i < 16; ++i) { if (i >= u_light_counts.z) break; vec3 delta = v_world_position - u_light_position_radius[i].xyz; float radius = max(u_light_position_radius[i].w, 0.001); float distanceSquared = dot(delta, delta); float distanceToLight = sqrt(distanceSquared); float amount = clamp(1.0 - distanceToLight / radius, 0.0, 1.0); if (u_projected_lights) { float planeDistance = abs(dot(delta, u_light_plane_normal)); float projectedRadius = max(2.0 * radius - planeDistance, 0.001); float tangentDistance = sqrt(max(distanceSquared - planeDistance * planeDistance, 0.0)); float projectedAmount = clamp(1.0 - planeDistance / radius, 0.0, 1.0); amount = projectedAmount * texture(u_dynamic_light_texture, vec2(0.5 + tangentDistance / projectedRadius, 0.5)).r; } vec3 contribution = u_light_color[i].rgb * amount; if (i < u_light_counts.x) regular += contribution; else if (i < u_light_counts.y) subtractive += contribution; else additive += contribution; } lighting = clamp(base + regular - subtractive, vec3(0.0), vec3(1.4)); }\n"
			"void main() { if (u_clip_plane_enabled && dot(vec4(v_world_position, 1.0), u_clip_plane) < 0.0) discard; vec4 texel = u_use_texture ? texture(u_texture, v_uv) : vec4(1.0); if ((u_material_flags & 32) != 0) texel.rgb = vec3(1.0) - texel.rgb; texel.rgb = clamp(texel.rgb, vec3(0.0), vec3(1.0)); vec3 lighting; vec3 additive; apply_dynamic_lights(v_color.rgb, lighting, additive); if (u_use_brightmap) lighting = min(lighting + sample_brightmap(), vec3(1.0)); vec3 base_texel = (u_material_flags & 64) != 0 ? vec3(1.0) : texel.rgb; vec4 color = vec4(base_texel * lighting, texel.a * v_color.a); if ((u_material_flags & 1) != 0) { color.a *= texel.r * v_color.r; color.rgb = lighting; } if ((u_material_flags & 2) != 0) color.rgb = vec3(1.0) - color.rgb; if ((u_material_flags & 4) != 0) color.rgb *= (1.0 - color.a); if ((u_material_flags & 16) != 0) { color.rgb = v_color.rgb; color.a = texel.a * v_color.a; } if ((u_material_flags & 8) != 0) { vec2 texCoord = floor(v_uv * 128.0) / 128.0; float texX = texCoord.x / 3.0 + 0.66; float texY = 0.34 - texCoord.y / 3.0; float vX = (texX / texY) * 21.0; float vY = (texY / texX) * 13.0; float fuzz = mod(u_fuzz_time * 2.0 + vX + vY, 0.5); color.rgb = vec3(0.0); color.a *= fuzz; } color.rgb += additive; frag_color = color; }\n";
		static const char *presentVertexSource =
			"#version 320 es\n"
			"layout(location = 0) in vec3 a_position;\n"
			"layout(location = 1) in vec2 a_uv;\n"
			"uniform vec4 u_texture_transform;\n"
			"uniform float u_depth;\n"
			"out vec2 v_uv;\n"
			"void main() { gl_Position = vec4(a_position.xy, u_depth, 1.0); v_uv = a_uv * u_texture_transform.xy + u_texture_transform.zw; }\n";
		static const char *presentFragmentSource =
			"#version 320 es\n"
			"precision mediump float;\n"
			"in vec2 v_uv;\n"
			"layout(location = 0) out vec4 frag_color;\n"
			"uniform sampler2D u_texture;\n"
			"uniform sampler2D u_wipe_start;\n"
			"uniform sampler2D u_wipe_end;\n"
			"uniform float u_wipe_progress;\n"
			"uniform int u_wipe_type;\n"
			"uniform bool u_wipe_active;\n"
			"uniform float u_gamma;\n"
			"uniform float u_brightness;\n"
			"uniform float u_contrast;\n"
			"float wipe_noise(vec2 p) { return fract(sin(dot(floor(p), vec2(12.9898, 78.233))) * 43758.5453); }\n"
			"vec4 wipe_color() { vec4 current = texture(u_texture, v_uv); if (!u_wipe_active) return current; vec4 start = texture(u_wipe_start, v_uv); vec4 finish = texture(u_wipe_end, v_uv); float progress = clamp(u_wipe_progress, 0.0, 1.0); if (u_wipe_type == 1) { float column = floor(v_uv.x * 320.0); float delay = (wipe_noise(vec2(column, 0.0)) - 0.5) * 0.30; float reveal = clamp(progress * 1.30 - delay, 0.0, 1.0); return v_uv.y <= reveal ? finish : start; } if (u_wipe_type == 2) { float noise = wipe_noise(v_uv * vec2(320.0, 200.0)); float edge = smoothstep(progress - 0.08, progress + 0.08, noise); return mix(finish, start, edge); } return mix(start, finish, progress); }\n"
			"void main() { vec4 color = wipe_color(); color.rgb = max((color.rgb - 0.5) * u_contrast + 0.5 + u_brightness * 0.5, vec3(0.0)); color.rgb = pow(color.rgb, vec3(1.0 / max(u_gamma, 0.1))); frag_color = color; }\n";

		Resources.sceneProgram = LinkProgram(sceneVertexSource, sceneFragmentSource, "opaque scene");
		Resources.maskedProgram = LinkProgram(sceneVertexSource, maskedFragmentSource, "masked scene");
		Resources.fogProgram = LinkProgram(sceneVertexSource, fogFragmentSource, "fogged scene");
		Resources.fogMaskedProgram = LinkProgram(sceneVertexSource, fogMaskedFragmentSource, "fogged masked scene");
		const char *paletteSource = gl_android_shader_test_failure ?
			"#version 320 es\nthis is an intentional shader test failure\n" : paletteFragmentSource;
		Resources.paletteProgram = LinkProgram(sceneVertexSource, paletteSource, "paletted/translated scene", "MATERIAL_PALETTE");
		Resources.presentProgram = LinkProgram(presentVertexSource, presentFragmentSource, "present");
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
		Resources.presentTexture = glGetUniformLocation(Resources.presentProgram, "u_texture");
		Resources.presentTextureTransform = glGetUniformLocation(Resources.presentProgram, "u_texture_transform");
		Resources.presentDepth = glGetUniformLocation(Resources.presentProgram, "u_depth");
		Resources.presentWipeStart = glGetUniformLocation(Resources.presentProgram, "u_wipe_start");
		Resources.presentWipeEnd = glGetUniformLocation(Resources.presentProgram, "u_wipe_end");
		Resources.presentWipeProgress = glGetUniformLocation(Resources.presentProgram, "u_wipe_progress");
		Resources.presentWipeType = glGetUniformLocation(Resources.presentProgram, "u_wipe_type");
		Resources.presentWipeActive = glGetUniformLocation(Resources.presentProgram, "u_wipe_active");
		Resources.presentGamma = glGetUniformLocation(Resources.presentProgram, "u_gamma");
		Resources.presentBrightness = glGetUniformLocation(Resources.presentProgram, "u_brightness");
		Resources.presentContrast = glGetUniformLocation(Resources.presentProgram, "u_contrast");

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
		glBindVertexArray(0);
		glGenVertexArrays(1, &Resources.skyVertexArray);
		glGenBuffers(1, &Resources.skyVertexBuffer);
		glGenBuffers(1, &Resources.skyIndexBuffer);
		glBindVertexArray(Resources.skyVertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Resources.skyVertexBuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Resources.skyIndexBuffer);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(0));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(6 * sizeof(GLfloat)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(8 * sizeof(GLfloat)));
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(FSceneVertex), reinterpret_cast<const void *>(3 * sizeof(GLfloat)));
		glBindVertexArray(0);
		BuildCheckerTexture();
		ConfigureNativeSamplers();
		if (!BuildFramebuffer(width, height)) return false;
		Resources.viewProjection[0] = 1.0f;
		Resources.viewProjection[5] = 1.0f;
		Resources.viewProjection[10] = 1.0f;
		Resources.viewProjection[15] = 1.0f;
		ResetState(width, height);
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
	if (!BuildResources(width, height))
	{
		DeleteResources(!preserveTextureCache);
		I_FatalError("Android GLES resources could not be created.");
	}
	return true;
}
}

unsigned int gl_AndroidNativeGLES_GetShaderProgram(const char *name)
{
	if (name == NULL || !Resources.ready) return 0;
	if (strcmp(name, "android/opaque") == 0) return Resources.sceneProgram;
	if (strcmp(name, "android/masked") == 0) return Resources.maskedProgram;
	if (strcmp(name, "android/fog") == 0) return Resources.fogProgram;
	if (strcmp(name, "android/fog-masked") == 0) return Resources.fogMaskedProgram;
	if (strcmp(name, "android/palette") == 0) return Resources.paletteProgram;
	if (strcmp(name, "android/present") == 0) return Resources.presentProgram;
	return 0;
}

void gl_AndroidNativeGLES_UseProgram(unsigned int handle)
{
	glUseProgram(static_cast<GLuint>(handle));
}

bool gl_AndroidNativeGLES_CollectCapabilities()
{
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	if (version == NULL || strncmp(version, "OpenGL ES ", 10) != 0)
		I_FatalError("Android GLES context did not report an OpenGL ES version.");

	int major = 0, minor = 0;
	if (sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2 || major < 3 || (major == 3 && minor < 2))
		I_FatalError("Android GLES 3.2 is required, got %s.", version);

	Capabilities.majorVersion = major;
	Capabilities.minorVersion = minor;
	Capabilities.vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
	Capabilities.renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
	Capabilities.version = version;
	Capabilities.shadingLanguageVersion = reinterpret_cast<const char *>(glGetString(GL_SHADING_LANGUAGE_VERSION));
	Capabilities.hasVertexBuffers = true;
	Capabilities.hasVertexArrays = true;
	Capabilities.hasUniformBuffers = true;
	Capabilities.hasFramebuffers = true;
	Capabilities.hasDepthStencil = true;
	Capabilities.hasBufferMapping = false;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &Capabilities.maxTextureSize);
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &Capabilities.maxTextureUnits);
	glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &Capabilities.maxVertexUniformVectors);
	glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &Capabilities.maxFragmentUniformVectors);
	glGetIntegerv(GL_NUM_EXTENSIONS, &Capabilities.extensionCount);
	if (Capabilities.maxTextureSize <= 0 || Capabilities.maxTextureUnits <= 0 ||
		Capabilities.maxVertexUniformVectors <= 0 || Capabilities.maxFragmentUniformVectors <= 0 ||
		Capabilities.extensionCount < 0)
		I_FatalError("Android GLES capability enumeration returned invalid limits.");
	Capabilities.hasAnisotropicFiltering = HasExtension("GL_EXT_texture_filter_anisotropic");
	Capabilities.hasAstcCompression = HasExtension("GL_KHR_texture_compression_astc_ldr");
	Capabilities.hasEtc2Compression = true;
	Capabilities.hasDebugLabels = HasExtension("GL_KHR_debug");
	Capabilities.hasMultiview = HasExtension("GL_OVR_multiview2") || HasExtension("GL_ANDROID_extension_pack_es31a");
	CapabilitiesReady = true;
	return true;
}

bool gl_AndroidNativeGLES_InitializeBootstrap(int width, int height)
{
	return InitializeResources(width, height, false);
}

void gl_AndroidNativeGLES_BeginScene(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, float fieldOfView, float aspect, float fovRatio)
{
	if (!gl_AndroidNativeGLES_CanUseResources()) return;
	if (!SceneInputLogged)
	{
		SceneInputLogged = true;
		DPrintf("Android GLES scene input: %d segs, %d subsectors, %d vertices.\n", numsegs, numsubsectors, numvertexes);
	}
	BuildViewProjection(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch, cameraRoll, fieldOfView, aspect, fovRatio);
	Resources.sceneVertices.clear();
	Resources.sceneIndices.clear();
	Resources.sceneIndexCount = 0;
	Resources.sceneReady = false;
	Resources.sceneBatches.clear();
	Resources.portalTargets.clear();
	NativePortalCaptureStack.clear();
	ResetNativeSkyRecord(Resources.outerSky, -1, 1u);
	Resources.sceneSpriteCount = 0;
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

void gl_AndroidNativeGLES_ClearScene()
{
	if (!gl_AndroidNativeGLES_CanUseResources()) return;
	FlatCollectionDeferred = false;
	Resources.sceneVertices.clear();
	Resources.sceneIndices.clear();
	Resources.sceneBatches.clear();
	Resources.portalTargets.clear();
	NativePortalCaptureStack.clear();
	ResetNativeSkyRecord(Resources.outerSky, -1, 1u);
	Resources.sceneIndexCount = 0;
	Resources.sceneReady = false;
	Resources.sceneSpriteCount = 0;
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

void gl_AndroidNativeGLES_SetFlatCollectionDeferred(bool deferred)
{
	FlatCollectionDeferred = deferred;
}

bool gl_AndroidNativeGLES_IsFlatCollectionDeferred()
{
	return FlatCollectionDeferred;
}

void gl_AndroidNativeGLES_SetSky(FMaterial *material, float xOffset, float yOffset, bool mirrored,
	bool sky2, PalEntry fadeColor)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || material == NULL || Resources.skyMaterial != NULL) return;
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
	if (!SkyLogged)
	{
		SkyLogged = true;
		DPrintf("Android GLES basic sky material enabled (offset %.2f, %.2f).\n", xOffset, yOffset);
	}
}

void gl_AndroidNativeGLES_SetSkyLayer(FMaterial *material, float xOffset, float yOffset, bool mirrored)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || material == NULL || Resources.skyLayerMaterial != NULL) return;
	Resources.skyLayerMaterial = material;
	Resources.skyLayerXOffset = xOffset;
	Resources.skyLayerYOffset = yOffset;
	Resources.skyLayerMirrored = mirrored;
}

void gl_AndroidNativeGLES_AddSkyMask(const float *positions)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL) return;
	FNativeSkyRecord *sky = ActiveNativeSkyRecord();
	if (sky == NULL) return;
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

void gl_AndroidNativeGLES_AddWall(const float *positions, const float *texcoords,
	const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode, unsigned int materialFlags,
	const float *lightData, const unsigned int *lightCounts, unsigned int brightmap, int brightmapDesaturation)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL) return;
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
	AddSceneQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, masked, fog, alpha < 0.999f, repeat,
		blendMode, fogColor, fogDensity, materialFlags, lightData, lightCounts, brightmap,
		brightmapDesaturation);
}

void gl_AndroidNativeGLES_AddFlat(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, unsigned int texture, bool masked, bool fog, bool repeat,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode, unsigned int materialFlags,
	const float *lightData, const unsigned int *lightCounts, unsigned int brightmap, int brightmapDesaturation)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL || vertexCount < 3) return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	if (first + vertexCount > AndroidNativeMaxSceneVertices) return;
	for (unsigned int i = 0; i < vertexCount; ++i)
	{
		FSceneVertex vertex;
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
		batch.translucent = alpha < 0.999f || blendMode != ANDROID_BLEND_OPAQUE;
		batch.repeat = repeat;
		batch.palette = IsPaletteTexture(texture);
		batch.flat = true;
		batch.materialFlags = materialFlags;
		batch.blendMode = blendMode;
		if (vertexCount >= 3)
		{
			FSceneVertex &firstVertex = Resources.sceneVertices[first];
			FSceneVertex &secondVertex = Resources.sceneVertices[first + 1];
			FSceneVertex &thirdVertex = Resources.sceneVertices[first + 2];
			SetLightPlaneNormal(batch, firstVertex, secondVertex, thirdVertex);
		}
		CopyNativeLightData(batch, lightData, lightCounts);
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
}

void gl_AndroidNativeGLES_AddFloodPlane(const float *wallPositions, const float *planePositions,
	const float *texcoords, const float *color, unsigned int texture, bool fog,
	const float *fogColor, float fogDensity)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || wallPositions == NULL || planePositions == NULL ||
		texcoords == NULL || Resources.sceneVertices.size() + 8 > AndroidNativeMaxSceneVertices)
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

void gl_AndroidNativeGLES_AddHUDPolygon(const float *positions, const float *texcoords,
	unsigned int vertexCount, const float *color, float alpha, bool masked,
	unsigned int texture, bool repeat, EAndroidNativeBlendMode blendMode, unsigned int materialFlags)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL || vertexCount < 3) return;
	const float white[3] = { 1.0f, 1.0f, 1.0f };
	const float *rgb = color != NULL ? color : white;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	const GLsizei firstIndex = static_cast<GLsizei>(Resources.sceneIndices.size());
	if (first + vertexCount > AndroidNativeMaxSceneVertices) return;
	for (unsigned int i = 0; i < vertexCount; ++i)
	{
		FSceneVertex vertex;
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
	batch.translucent = alpha < 0.999f || blendMode != ANDROID_BLEND_OPAQUE;
	batch.repeat = repeat;
	batch.palette = IsPaletteTexture(texture);
	batch.hud = true;
	batch.blendMode = blendMode;
	batch.materialFlags = materialFlags;
	batch.sortDepth = 0.0f;
	Resources.sceneBatches.push_back(batch);
}

void gl_AndroidNativeGLES_AddSprite(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode, unsigned int materialFlags,
	unsigned int brightmap, int brightmapDesaturation)
{
	if (gl_AndroidNativeGLES_CanUseResources()) ++Resources.sceneSpriteCount;
	gl_AndroidNativeGLES_AddWall(positions, texcoords, color, alpha, texture, masked, fog, false,
		fogColor, fogDensity, blendMode, materialFlags, NULL, NULL, brightmap, brightmapDesaturation);
}

void gl_AndroidNativeGLES_AddModelSurface(const float *positions, const float *texcoords,
	unsigned int vertexCount, const unsigned int *indices, unsigned int indexCount,
	const float *color, float alpha, bool masked, bool fog, unsigned int texture,
	const float *fogColor, float fogDensity, EAndroidNativeBlendMode blendMode, unsigned int materialFlags,
	const float *normals, unsigned int brightmap, int brightmapDesaturation, bool cullBackFaces)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL || indices == NULL ||
		vertexCount == 0 || indexCount < 3 || (indexCount % 3) != 0) return;
	const unsigned int first = static_cast<unsigned int>(Resources.sceneVertices.size());
	if (first + vertexCount > AndroidNativeMaxSceneVertices) return;
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
	batch.translucent = alpha < 0.999f || blendMode != ANDROID_BLEND_OPAQUE;
	batch.repeat = false;
	batch.palette = IsPaletteTexture(texture);
	batch.model = true;
	batch.cullBackFaces = cullBackFaces;
	batch.materialFlags = materialFlags;
	batch.blendMode = blendMode;
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

void gl_AndroidNativeGLES_AddHUDQuad(const float *positions, const float *texcoords,
	const float *color, float alpha, bool masked, unsigned int texture,
	EAndroidNativeBlendMode blendMode, unsigned int materialFlags)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL) return;
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
	AddHUDQuad(vertices[0], vertices[1], vertices[2], vertices[3], texture, masked, blendMode, materialFlags);
}

void gl_AndroidNativeGLES_AddScreenQuad(const float *color, float alpha,
	EAndroidNativeBlendMode blendMode)
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
	gl_AndroidNativeGLES_AddHUDQuad(positions, texcoords, color, alpha, false, 0, blendMode, 0);
}

void gl_AndroidNativeGLES_EndScene()
{
	if (!gl_AndroidNativeGLES_CanUseResources()) return;
	if (Resources.skyMaterial != NULL && Resources.outerSky.capEligible)
		AddSkyMaskCaps(Resources.outerSky);
	UploadSceneGeometry();
	if (Resources.frame == 0 || (Resources.frame % 120) == 0)
	{
		unsigned int skyMaskCount = static_cast<unsigned int>(Resources.outerSky.maskBatches.size());
		unsigned int floodCount = 0;
		unsigned int flatCount = 0;
		for (size_t i = 0; i < Resources.sceneBatches.size(); ++i)
		{
			const FSceneBatch &batch = Resources.sceneBatches[i];
			if (batch.flood) ++floodCount;
			if (batch.flat) ++flatCount;
		}
		DPrintf("Android GLES scene: %u vertices, %u indices, %u batches.\n",
			static_cast<unsigned int>(Resources.sceneVertices.size()),
			static_cast<unsigned int>(Resources.sceneIndices.size()),
			static_cast<unsigned int>(Resources.sceneBatches.size()));
		DPrintf("Android GLES scene sprites: %u.\n", Resources.sceneSpriteCount);
		DPrintf("Android GLES scene masks: %u sky, %u flood, %u flat.\n",
			skyMaskCount, floodCount, flatCount);
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
		glDepthFunc(GL_LESS);
		glDisable(GL_CULL_FACE);
	}
	if (batch.translucent)
	{
		glEnable(GL_BLEND);
		glDepthMask(GL_FALSE);
		GLenum equation = GL_FUNC_ADD;
		if (batch.blendMode == ANDROID_BLEND_SUBTRACT) equation = GL_FUNC_SUBTRACT;
		else if (batch.blendMode == ANDROID_BLEND_REVERSE_SUBTRACT) equation = GL_FUNC_REVERSE_SUBTRACT;
		glBlendEquation(equation);
		const bool additive = batch.blendMode == ANDROID_BLEND_ADD ||
			batch.blendMode == ANDROID_BLEND_SUBTRACT || batch.blendMode == ANDROID_BLEND_REVERSE_SUBTRACT;
		if (batch.blendMode == ANDROID_BLEND_FUZZ) glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
		else if (batch.blendMode == ANDROID_BLEND_MULTIPLY) glBlendFunc(GL_DST_COLOR, GL_ZERO);
		else glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
	}
	else
	{
		glDisable(GL_BLEND);
		glDepthMask(batch.hud || batch.flood ? GL_FALSE : GL_TRUE);
		glBlendEquation(GL_FUNC_ADD);
	}
	const GLuint program = batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
		(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram));
	const char *programName = batch.fog ? (batch.masked ? "android/portal-fog-masked" : "android/portal-fog") :
		(batch.masked ? "android/portal-masked" : (batch.palette ? "android/portal-palette" : "android/portal-opaque"));
	BindNativeProgram(program, programName);
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
	if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ? 0.0f : 0.5f);
	if (fogColor >= 0) glUniform4f(fogColor, batch.fogColor[0], batch.fogColor[1], batch.fogColor[2], 1.0f);
	if (fogDensity >= 0) glUniform1f(fogDensity, batch.fogDensity / 64000.0f);
	if (lightPositionRadius >= 0) glUniform4fv(lightPositionRadius, batch.lightCount, batch.lightPositionRadius);
	if (lightColor >= 0) glUniform4fv(lightColor, batch.lightCount, batch.lightColor);
	if (lightCounts >= 0) glUniform3i(lightCounts, static_cast<GLint>(batch.lightNormalCount),
		static_cast<GLint>(batch.lightSubtractiveCount), static_cast<GLint>(batch.lightCount));
	if (lightPlaneNormal >= 0) glUniform3fv(lightPlaneNormal, 1, batch.lightPlaneNormal);
	const bool projected = dynamicLightTexture != 0 && batch.lightCount > 0 &&
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
	glDrawElements(GL_TRIANGLES, batch.indexCount, GL_UNSIGNED_INT,
		reinterpret_cast<const void *>(batch.firstIndex * sizeof(GLuint)));
}

static bool DrawNativePortalMask(const FSceneBatch &batch, GLuint stencilBit, bool writeStencil,
	bool configureStencil = true, GLuint stencilRef = 0, GLuint stencilCompareMask = 0)
{
	if (batch.firstIndex < 0 || batch.indexCount <= 0 ||
		static_cast<size_t>(batch.firstIndex) + static_cast<size_t>(batch.indexCount) > Resources.sceneIndices.size())
		return false;
	BindNativeProgram(Resources.sceneProgram, "android/portal-mask");
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
	UploadSkyboxGeometry(xOffset, sky2, fliptop);
	glBindVertexArray(Resources.skyVertexArray);
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
		const FSkyPrimitiveRange &range = Resources.skyboxFaces[face];
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
	BindNativeProgram(Resources.sceneProgram, "android/portal-sky");
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
	glBindVertexArray(Resources.skyVertexArray);
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
		UploadSkyGeometry(material, xOffset, yOffset, mirrored);
		glBindVertexArray(Resources.skyVertexArray);
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
				glDrawElements(GL_TRIANGLES, Resources.skyUpperCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyUpperCap.firstIndex * sizeof(GLushort)));
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, Resources.skyUpperStrips[row].indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyUpperStrips[row].firstIndex * sizeof(GLushort)));
		if (caps)
		{
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
			if (Resources.sceneObjectColor >= 0)
				glUniform4f(Resources.sceneObjectColor, target.skyLowerCapColor.r / 255.0f,
					target.skyLowerCapColor.g / 255.0f, target.skyLowerCapColor.b / 255.0f, 1.0f);
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, Resources.skyLowerCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyLowerCap.firstIndex * sizeof(GLushort)));
		}
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
		if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, Resources.skyLowerStrips[row].indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyLowerStrips[row].firstIndex * sizeof(GLushort)));
		return true;
	};
	const bool firstLayerDrawn = drawSkyLayer(target.skyMaterial, target.skyXOffset, target.skyYOffset,
		target.skyMirrored, true);
	if (firstLayerDrawn && target.skyLayerMaterial != NULL && target.skyLayerMaterial != target.skyMaterial)
		drawSkyLayer(target.skyLayerMaterial, target.skyLayerXOffset, target.skyLayerYOffset,
			target.skyLayerMirrored, false);
	if (firstLayerDrawn && target.skyFogEnabled && skyfog > 0)
	{
		UploadSkyGeometry(NULL, 0.0f, 0.0f, false);
		glBindVertexArray(Resources.skyVertexArray);
		if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 1);
		if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
		if (Resources.sceneObjectColor >= 0)
			glUniform4f(Resources.sceneObjectColor, target.skyFogColor.r / 255.0f,
				target.skyFogColor.g / 255.0f, target.skyFogColor.b / 255.0f, skyfog / 255.0f);
		glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
		if (target.sky.capEligible)
			glDrawElements(GL_TRIANGLES, Resources.skyUpperCap.indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyUpperCap.firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, Resources.skyUpperStrips[row].indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyUpperStrips[row].firstIndex * sizeof(GLushort)));
		if (target.sky.capEligible)
			glDrawElements(GL_TRIANGLES, Resources.skyLowerCap.indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyLowerCap.firstIndex * sizeof(GLushort)));
		for (int row = 0; row < 4; ++row)
			glDrawElements(GL_TRIANGLE_STRIP, Resources.skyLowerStrips[row].indexCount, GL_UNSIGNED_SHORT,
				reinterpret_cast<const void *>(Resources.skyLowerStrips[row].firstIndex * sizeof(GLushort)));
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

static void DrawNativePortalTargets(GLuint dynamicLightTexture)
{
	if (Resources.portalTargets.empty()) return;
	unsigned int drawnTargets = 0;
	unsigned int drawnBatches = 0;
	glBindVertexArray(Resources.sceneVertexArray);
	for (size_t targetIndex = 0; targetIndex < Resources.portalTargets.size(); ++targetIndex)
	{
		FNativePortalTarget &target = Resources.portalTargets[targetIndex];
		if (target.maskBatches.empty() || target.endBatch <= target.firstBatch) continue;
		const GLuint stencilBit = NativePortalStencilBit(target.id);
		const GLuint skyStencilBit = target.sky.stencilBit;
		const GLuint parentStencilBit = target.parentId >= 0 ?
			NativePortalStencilBit(static_cast<unsigned int>(target.parentId)) : 0;
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
			DPrintf("Android GLES portal target %u (parent %d): mask=%u skyMask=%u walls=%u flats=%u "
				"floods=%u translucent=%u models=%u batches=[%u,%u).\\n",
				target.id, target.parentId, static_cast<unsigned int>(target.maskBatches.size()), skyMasks,
				opaqueWalls, opaqueFlats, floods, translucent, sprites,
				static_cast<unsigned int>(target.firstBatch), static_cast<unsigned int>(targetEndBatch));
		}
		// Nested targets can only mark pixels already owned by their parent.
		// Root targets keep the unrestricted mask used by the desktop portal pass.
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
		// Give the captured view a fresh depth range inside its portal surface.
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_TRUE);
		// Match the desktop portal pass: reflected geometry starts at the far
		// plane instead of being depth-tested against the mirror surface.
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
		std::vector<size_t> drawOrder;
		for (size_t batchIndex = target.firstBatch; batchIndex < targetEndBatch; ++batchIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[batchIndex];
			if (batch.portalId == static_cast<int>(target.id) && !batch.portalMask && !batch.skyMask && !batch.hud)
				drawOrder.push_back(batchIndex);
		}
		std::stable_sort(drawOrder.begin(), drawOrder.end(), [](size_t left, size_t right)
		{
			const FSceneBatch &a = Resources.sceneBatches[left];
			const FSceneBatch &b = Resources.sceneBatches[right];
			if (a.hud != b.hud) return !a.hud;
			if (a.hud) return left < right;
			if (a.flood != b.flood) return !a.flood;
			if (a.flat != b.flat) return !a.flat;
			if (a.translucent != b.translucent) return !a.translucent;
			if (a.translucent && a.sortDepth != b.sortDepth) return a.sortDepth > b.sortDepth;
			return left < right;
		});
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, stencilBit, stencilBit);
		for (size_t orderIndex = 0; orderIndex < drawOrder.size(); ++orderIndex)
		{
			DrawNativePortalBatch(Resources.sceneBatches[drawOrder[orderIndex]], dynamicLightTexture, stencilBit);
			++drawnBatches;
		}
		++drawnTargets;
	}
	glBindVertexArray(0);
	glDisable(GL_STENCIL_TEST);
	glStencilMask(0xff);
	glStencilFunc(GL_ALWAYS, 0, 0xff);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glDepthFunc(GL_LESS);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	if (developer && drawnTargets > 0 &&
		(Resources.frame == 0 || (Resources.frame % 120) == 0))
		DPrintf("Android GLES portal targets: %u, batches: %u.\n", drawnTargets, drawnBatches);
}

unsigned int gl_AndroidNativeGLES_BeginPortalCapture()
{
	if (!gl_AndroidNativeGLES_CanUseResources() || NativePortalCaptureStack.size() >= 7 ||
		Resources.portalTargets.size() >= AndroidNativeMaxPortalTargets)
		return ~0u;
	FNativePortalTarget target = {};
	target.id = static_cast<unsigned int>(Resources.portalTargets.size());
	target.parentId = NativePortalCaptureStack.empty() ? -1 :
		static_cast<int>(NativePortalCaptureStack.back());
	target.firstBatch = Resources.sceneBatches.size();
	target.endBatch = target.firstBatch;
	ResetNativeSkyRecord(target.sky, static_cast<int>(target.id), NativePortalSkyStencilBit(target.id));
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
	return target.id;
}

void gl_AndroidNativeGLES_AddPortalMask(unsigned int portalId, const float *positions)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || positions == NULL ||
		portalId >= Resources.portalTargets.size()) return;
	if (Resources.sceneVertices.size() + 4 > AndroidNativeMaxSceneVertices) return;
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

void gl_AndroidNativeGLES_SetPortalView(float cameraX, float cameraY, float cameraZ,
	float cameraYaw, float cameraPitch, float cameraRoll, bool mirrored, bool planeMirrored)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || NativePortalCaptureStack.empty()) return;
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

void gl_AndroidNativeGLES_SetPortalClipPlane(float a, float b, float c, float d)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || NativePortalCaptureStack.empty()) return;
	Resources.clipPlane[0] = a;
	Resources.clipPlane[1] = b;
	Resources.clipPlane[2] = c;
	Resources.clipPlane[3] = d;
	Resources.clipPlaneEnabled = true;
}

void gl_AndroidNativeGLES_EndPortalCapture(unsigned int portalId)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || NativePortalCaptureStack.empty() ||
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

unsigned int gl_AndroidNativeGLES_BindMaterial(const void *key, const unsigned char *pixels,
	int width, int height, bool repeat, int colormap, int translation, bool allowhires)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || key == NULL || pixels == NULL || width <= 0 || height <= 0)
		return 0;
	ConfigureNativeSamplers();
	const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
	for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
	{
		FAndroidNativeTexture &entry = Resources.nativeTextures[i];
		if (entry.key == key && entry.colormap == colormap && entry.translation == translation &&
			entry.repeat == repeat && entry.allowhires == allowhires)
		{
			if (entry.texture != 0 && entry.width == width && entry.height == height &&
				entry.pixels.size() == pixelBytes && memcmp(&entry.pixels[0], pixels, pixelBytes) == 0)
				return entry.texture;
			break;
		}
	}
	FAndroidNativeTexture *entry = NULL;
	for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
	{
		FAndroidNativeTexture &candidate = Resources.nativeTextures[i];
		if (candidate.key == key && candidate.colormap == colormap && candidate.translation == translation &&
			candidate.repeat == repeat && candidate.allowhires == allowhires)
		{
			entry = &candidate;
			break;
		}
	}
	if (entry == NULL)
	{
		FAndroidNativeTexture value = {};
		value.key = key;
		value.colormap = colormap;
		value.translation = translation;
		value.allowhires = allowhires;
		value.width = width;
		value.height = height;
		value.repeat = repeat;
		value.palette = colormap != CM_DEFAULT || translation != 0;
		value.texture = 0;
		Resources.nativeTextures.push_back(value);
		entry = &Resources.nativeTextures.back();
	}
	if (entry->width != width || entry->height != height)
	{
		if (entry->texture != 0) glDeleteTextures(1, &entry->texture);
		entry->texture = 0;
		entry->width = width;
		entry->height = height;
		entry->pixels.assign(pixels, pixels + pixelBytes);
	}
	else if (entry->texture != 0)
	{
		entry->pixels.assign(pixels, pixels + pixelBytes);
		glBindTexture(GL_TEXTURE_2D, entry->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (CheckError("material texture update") != GL_NO_ERROR)
		{
			glDeleteTextures(1, &entry->texture);
			entry->texture = 0;
		}
		else
		{
			return entry->texture;
		}
	}
	else
	{
		entry->pixels.assign(pixels, pixels + pixelBytes);
	}
	glGenTextures(1, &entry->texture);
	glBindTexture(GL_TEXTURE_2D, entry->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		&entry->pixels[0]);
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (CheckError("material texture upload") != GL_NO_ERROR)
	{
		glDeleteTextures(1, &entry->texture);
		entry->texture = 0;
	}
	return entry->texture;
}

void gl_AndroidNativeGLES_ClearMaterialCache()
{
	if (Resources.ready)
	{
		for (size_t i = 0; i < Resources.nativeTextures.size(); ++i)
		{
			if (Resources.nativeTextures[i].texture != 0)
				glDeleteTextures(1, &Resources.nativeTextures[i].texture);
		}
	}
	Resources.nativeTextures.clear();
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

void gl_AndroidNativeGLES_OnContextLost()
{
	gl_AndroidNativeGLES_UnregisterShaderPrograms();
	InvalidateResources();
	gl_AndroidNativeGLES_InvalidateTextures();
	CapabilitiesReady = false;
	memset(&Capabilities, 0, sizeof(Capabilities));
	Printf("Android GLES context lost; native resource names invalidated.\n");
}

bool gl_AndroidNativeGLES_OnContextRestored(int width, int height)
{
	if (!gl_AndroidNativeGLES_CollectCapabilities()) return false;
	const bool restored = InitializeResources(width, height, true);
	if (restored) gl_AndroidNativeGLES_RegisterShaderPrograms();
	return restored;
}

void gl_AndroidNativeGLES_RenderBootstrap(int width, int height)
{
	if (!Resources.ready)
	{
		if (!BootstrapPauseLogged)
		{
			BootstrapPauseLogged = true;
			Printf("Android GLES rendering paused while the surface is unavailable.\n");
		}
		return;
	}
	BootstrapPauseLogged = false;
	if (Resources.sceneTarget.renderWidth != width || Resources.sceneTarget.renderHeight != height)
	{
		if (Resources.wipeStartTexture != 0) glDeleteTextures(1, &Resources.wipeStartTexture);
		if (Resources.wipeEndTexture != 0) glDeleteTextures(1, &Resources.wipeEndTexture);
		Resources.wipeStartTexture = 0;
		Resources.wipeEndTexture = 0;
		Resources.wipeStartReady = false;
		Resources.wipeEndReady = false;
		Resources.wipeEndCapturePending = false;
		Resources.wipeActive = false;
		Resources.wipeType = wipe_None;
		Resources.wipeProgress = 0.0f;
		if (!BuildFramebuffer(width, height))
			I_FatalError("Android GLES render target could not follow surface size %dx%d.", width, height);
	}
	gl_GLES_BindRenderTarget(&Resources.sceneTarget);
	// The legacy view setup leaves its view-window scissor enabled. The native
	// scene target always owns the complete render surface.
	glScissor(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
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
				if (Resources.skyUpperStrips[row].indexCount > 0) ++upperStripCount;
				if (Resources.skyLowerStrips[row].indexCount > 0) ++lowerStripCount;
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
			DPrintf("Android GLES sky: pitch %.2f, yaw %.2f, fov %.2f, aspect %.3f, hasMask=%d, "
				"skyMaskPresent=%d, upperCap=%d, lowerCap=%d, upperStrips=%d, "
				"lowerStrips=%d, stencil=%s, masks submitted=%u clipped=%u rejected=%u, "
				"clip x[%.2f,%.2f] y[%.2f,%.2f] z[%.2f,%.2f] w[%.2f,%.2f].\n",
				Resources.cameraPitch * 180.0f / 3.14159265359f,
				Resources.cameraYaw * 180.0f / 3.14159265359f, Resources.cameraFieldOfView,
				Resources.cameraAspect, skyMaskHadGeometry ? 1 : 0, skyMaskPresent ? 1 : 0,
				Resources.skyUpperCap.indexCount,
				Resources.skyLowerCap.indexCount, upperStripCount, lowerStripCount, stencilRoute,
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
		BindNativeProgram(Resources.sceneProgram, "android/opaque");
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
		glBindVertexArray(Resources.skyVertexArray);
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
			UploadSkyGeometry(material, xOffset, yOffset,
				 caps ? Resources.skyMirrored : Resources.skyLayerMirrored);
			glBindVertexArray(Resources.skyVertexArray);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, skyTexture);
			glBindSampler(0, Resources.checkerSampler);
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, caps ? 0 : 1);
			if (caps)
			{
				if (Resources.sceneObjectColor >= 0)
					glUniform4f(Resources.sceneObjectColor, Resources.skyUpperCapColor.r / 255.0f,
						Resources.skyUpperCapColor.g / 255.0f, Resources.skyUpperCapColor.b / 255.0f, 1.0f);
				glDrawElements(GL_TRIANGLES, Resources.skyUpperCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyUpperCap.firstIndex * sizeof(GLushort)));
			}
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
			if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, Resources.skyUpperStrips[row].indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyUpperStrips[row].firstIndex * sizeof(GLushort)));
			if (caps)
			{
				if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
				if (Resources.sceneObjectColor >= 0)
					glUniform4f(Resources.sceneObjectColor, Resources.skyLowerCapColor.r / 255.0f,
						Resources.skyLowerCapColor.g / 255.0f, Resources.skyLowerCapColor.b / 255.0f, 1.0f);
				glDrawElements(GL_TRIANGLES, Resources.skyLowerCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyLowerCap.firstIndex * sizeof(GLushort)));
			}
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 1);
			if (Resources.sceneObjectColor >= 0) glUniform4f(Resources.sceneObjectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, Resources.skyLowerStrips[row].indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyLowerStrips[row].firstIndex * sizeof(GLushort)));
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
			UploadSkyGeometry(NULL, 0.0f, 0.0f, false);
			glBindVertexArray(Resources.skyVertexArray);
			if (Resources.sceneSkyFog >= 0) glUniform1i(Resources.sceneSkyFog, 1);
			if (Resources.sceneUseTexture >= 0) glUniform1i(Resources.sceneUseTexture, 0);
			if (Resources.sceneObjectColor >= 0)
				glUniform4f(Resources.sceneObjectColor, Resources.skyFogColor.r / 255.0f,
					Resources.skyFogColor.g / 255.0f, Resources.skyFogColor.b / 255.0f, skyfog / 255.0f);
			glBindTexture(GL_TEXTURE_2D, Resources.checkerTexture);
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, Resources.skyUpperCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyUpperCap.firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, Resources.skyUpperStrips[row].indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyUpperStrips[row].firstIndex * sizeof(GLushort)));
			if (Resources.outerSky.capEligible)
				glDrawElements(GL_TRIANGLES, Resources.skyLowerCap.indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyLowerCap.firstIndex * sizeof(GLushort)));
			for (int row = 0; row < 4; ++row)
				glDrawElements(GL_TRIANGLE_STRIP, Resources.skyLowerStrips[row].indexCount, GL_UNSIGNED_SHORT,
					reinterpret_cast<const void *>(Resources.skyLowerStrips[row].firstIndex * sizeof(GLushort)));
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
	const bool renderScene = Resources.sceneReady && !gl_android_test_pattern;
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
		std::vector<size_t> drawOrder(Resources.sceneBatches.size());
		for (size_t i = 0; i < drawOrder.size(); ++i) drawOrder[i] = i;
		std::stable_sort(drawOrder.begin(), drawOrder.end(), [](size_t left, size_t right)
		{
			const FSceneBatch &a = Resources.sceneBatches[left];
			const FSceneBatch &b = Resources.sceneBatches[right];
			if (a.hud != b.hud) return !a.hud;
			if (a.hud) return left < right;
			if (a.flood != b.flood) return !a.flood;
			if (a.flat != b.flat) return !a.flat;
			if (a.translucent != b.translucent) return !a.translucent;
			if (a.translucent && a.sortDepth != b.sortDepth) return a.sortDepth > b.sortDepth;
			return left < right;
		});
		for (size_t orderIndex = 0; orderIndex < drawOrder.size(); ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder[orderIndex]];
			// Opaque world geometry establishes depth before portal targets and
			// translucent/HUD batches are composited.
			if (batch.skyMask || batch.portalMask || batch.portalId >= 0 || batch.translucent || batch.hud)
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
				// Keep flood masks on a dedicated stencil bit so sky masks remain intact.
				glEnable(GL_STENCIL_TEST);
				glStencilMask(0x02);
				glStencilFunc(GL_ALWAYS, 0x02, 0x02);
				glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				glEnable(GL_DEPTH_TEST);
				glDepthFunc(GL_LEQUAL);
				glDepthMask(GL_TRUE);
				BindNativeProgram(Resources.sceneProgram, "android/flood-mask");
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
				glDepthFunc(GL_LESS);
				glDisable(GL_CULL_FACE);
			}
			glDisable(GL_STENCIL_TEST);
			if (batch.translucent)
			{
				glEnable(GL_BLEND);
				glDepthMask(GL_FALSE);
				GLenum equation = GL_FUNC_ADD;
				switch (batch.blendMode)
				{
				case ANDROID_BLEND_SUBTRACT:
					equation = GL_FUNC_SUBTRACT;
					break;
				case ANDROID_BLEND_REVERSE_SUBTRACT:
					equation = GL_FUNC_REVERSE_SUBTRACT;
					break;
				default:
					break;
				}
				glBlendEquation(equation);
				const bool additiveBlend = batch.blendMode == ANDROID_BLEND_ADD ||
					batch.blendMode == ANDROID_BLEND_SUBTRACT ||
					batch.blendMode == ANDROID_BLEND_REVERSE_SUBTRACT;
				if (batch.blendMode == ANDROID_BLEND_MULTIPLY)
					glBlendFunc(GL_DST_COLOR, GL_ZERO);
				else if (batch.blendMode == ANDROID_BLEND_FUZZ)
					glBlendFunc(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA);
				else
					glBlendFunc(GL_SRC_ALPHA, additiveBlend ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
			}
			else
			{
				glDisable(GL_BLEND);
				glDepthMask(batch.flood ? GL_FALSE : GL_TRUE);
				glBlendEquation(GL_FUNC_ADD);
			}
			const GLuint program = batch.fog ? (batch.masked ? Resources.fogMaskedProgram : Resources.fogProgram) :
				(batch.masked ? Resources.maskedProgram : (batch.palette ? Resources.paletteProgram : Resources.sceneProgram));
			const char *programName = batch.fog ? (batch.masked ? "android/fog-masked" : "android/fog") :
				(batch.masked ? "android/masked" : (batch.palette ? "android/palette" : "android/opaque"));
			BindNativeProgram(program, programName);
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
			if (model >= 0) glUniformMatrix4fv(model, 1, GL_FALSE, identity);
			if (textureTransform >= 0) glUniform4f(textureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
			if (cameraPosition >= 0)
				glUniform3f(cameraPosition, batch.hud ? 0.0f : batch.cameraPosition[0],
					batch.hud ? 0.0f : batch.cameraPosition[1], batch.hud ? 0.0f : batch.cameraPosition[2]);
			if (objectColor >= 0) glUniform4f(objectColor, 1.0f, 1.0f, 1.0f, 1.0f);
			if (materialFlags >= 0) glUniform1i(materialFlags, static_cast<GLint>(batch.materialFlags));
			if (fuzzTime >= 0) glUniform1f(fuzzTime, Resources.frame / 35.0f);
			if (clipPlaneUniform >= 0) glUniform4fv(clipPlaneUniform, 1, batch.clipPlane);
			if (clipPlaneEnabledUniform >= 0) glUniform1i(clipPlaneEnabledUniform, batch.clipPlaneEnabled ? 1 : 0);
			if (alphaCutoff >= 0) glUniform1f(alphaCutoff, batch.hud ? 0.0f : 0.5f);
			if (lightPositionRadius >= 0)
				glUniform4fv(lightPositionRadius, batch.lightCount, batch.lightPositionRadius);
			if (lightColor >= 0)
				glUniform4fv(lightColor, batch.lightCount, batch.lightColor);
			if (lightCounts >= 0)
				glUniform3i(lightCounts, static_cast<GLint>(batch.lightNormalCount),
					static_cast<GLint>(batch.lightSubtractiveCount), static_cast<GLint>(batch.lightCount));
			if (lightPlaneNormal >= 0)
				glUniform3fv(lightPlaneNormal, 1, batch.lightPlaneNormal);
			const bool useProjectedLights = dynamicLightTexture != 0 && batch.lightCount > 0 &&
				(batch.lightPlaneNormal[0] != 0.0f || batch.lightPlaneNormal[1] != 0.0f || batch.lightPlaneNormal[2] != 0.0f);
			if (projectedLights >= 0) glUniform1i(projectedLights, useProjectedLights ? 1 : 0);
			if (dynamicLightSampler >= 0) glUniform1i(dynamicLightSampler, 2);
			if (textureUniform >= 0) glUniform1i(textureUniform, 0);
			if (brightmapUniform >= 0) glUniform1i(brightmapUniform, 1);
			if (useBrightmap >= 0) glUniform1i(useBrightmap, batch.brightmap != 0 ? 1 : 0);
			if (brightmapDesaturation >= 0)
				glUniform1i(brightmapDesaturation, batch.brightmapDesaturation);
			if (useTexture >= 0) glUniform1i(useTexture, batch.texture != 0 ? 1 : 0);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, batch.texture != 0 ? batch.texture : Resources.checkerTexture);
			glBindSampler(0, batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, batch.brightmap != 0 ? batch.brightmap : Resources.checkerTexture);
			glBindSampler(1, batch.repeat ? Resources.checkerSampler : Resources.sceneSampler);
			glActiveTexture(GL_TEXTURE2);
			glBindTexture(GL_TEXTURE_2D, useProjectedLights ? dynamicLightTexture : Resources.checkerTexture);
			glBindSampler(2, Resources.sceneSampler);
			glActiveTexture(GL_TEXTURE0);
			glDrawElements(GL_TRIANGLES, batch.indexCount, GL_UNSIGNED_INT,
				reinterpret_cast<const void *>(batch.firstIndex * sizeof(GLuint)));
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
		for (size_t orderIndex = 0; orderIndex < drawOrder.size(); ++orderIndex)
		{
			const FSceneBatch &batch = Resources.sceneBatches[drawOrder[orderIndex]];
			if (batch.skyMask || batch.portalMask || batch.portalId >= 0 ||
				(!batch.translucent && !batch.hud))
				continue;
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
		BindNativeProgram(Resources.sceneProgram, "android/opaque");
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
	if (!gl_GLES_ResolveRenderTarget(&Resources.sceneTarget))
		I_FatalError("Android GLES scene resolve failed.");
	if (Resources.wipeEndCapturePending)
	{
		if (!BuildWipeTexture(Resources.wipeEndTexture) ||
			!CaptureWipeTexture(Resources.wipeEndTexture))
			I_FatalError("Android GLES wipe end capture failed.");
		Resources.wipeEndReady = true;
		Resources.wipeEndCapturePending = false;
	}

	ResetState(width, height);
	glViewport(0, 0, width, height);
	glScissor(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	BindNativeProgram(Resources.presentProgram, "android/present");
	glUniform1i(Resources.presentTexture, 0);
	if (Resources.presentTextureTransform >= 0)
		glUniform4f(Resources.presentTextureTransform, 1.0f, 1.0f, 0.0f, 0.0f);
	if (Resources.presentDepth >= 0) glUniform1f(Resources.presentDepth, 0.0f);
	const bool wipeReady = Resources.wipeActive && Resources.wipeStartReady && Resources.wipeEndReady;
	if (Resources.presentWipeStart >= 0) glUniform1i(Resources.presentWipeStart, 1);
	if (Resources.presentWipeEnd >= 0) glUniform1i(Resources.presentWipeEnd, 2);
	if (Resources.presentWipeProgress >= 0) glUniform1f(Resources.presentWipeProgress, Resources.wipeProgress);
	if (Resources.presentWipeType >= 0) glUniform1i(Resources.presentWipeType, Resources.wipeType);
	if (Resources.presentWipeActive >= 0) glUniform1i(Resources.presentWipeActive, wipeReady ? 1 : 0);
	if (Resources.presentGamma >= 0) glUniform1f(Resources.presentGamma, static_cast<float>(Gamma));
	if (Resources.presentBrightness >= 0)
		glUniform1f(Resources.presentBrightness, clamp<float>(vid_brightness, -0.8f, 0.8f));
	if (Resources.presentContrast >= 0)
		glUniform1f(Resources.presentContrast, clamp<float>(vid_contrast, 0.1f, 3.0f));
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, Resources.sceneTarget.colorAttachment);
	glBindSampler(0, Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, wipeReady ? Resources.wipeStartTexture : Resources.sceneTarget.colorAttachment);
	glBindSampler(1, Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, wipeReady ? Resources.wipeEndTexture : Resources.sceneTarget.colorAttachment);
	glBindSampler(2, Resources.sceneSampler);
	glActiveTexture(GL_TEXTURE0);
	glBindVertexArray(Resources.vertexArray);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, reinterpret_cast<const void *>(0));
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE1);
	glBindSampler(1, 0);
	glActiveTexture(GL_TEXTURE2);
	glBindSampler(2, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindSampler(0, 0);
	glUseProgram(0);
	ResetState(width, height);
	const GLenum error = CheckError("present");
	if (error != GL_NO_ERROR) I_FatalError("Android GLES frame failed.");
	++Resources.frame;
	if (Resources.frame == 1 || (Resources.frame % 120) == 0)
		DPrintf("Android GLES frame %u at %dx%d.\n", Resources.frame, width, height);
}

bool gl_AndroidNativeGLES_WriteSavePic(FILE *file, int width, int height)
{
	if (file == nullptr || width <= 0 || height <= 0 || !gl_AndroidNativeGLES_CanUseResources())
		return false;

	const int sourceWidth = Resources.sceneTarget.renderWidth;
	const int sourceHeight = Resources.sceneTarget.renderHeight;
	if (sourceWidth <= 0 || sourceHeight <= 0)
		return false;

	std::vector<BYTE> rgba(static_cast<size_t>(sourceWidth) * sourceHeight * 4);
	std::vector<BYTE> rgb(static_cast<size_t>(width) * height * 3);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, sourceWidth, sourceHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
	const GLenum readbackError = glGetError();
	if (readbackError != GL_NO_ERROR)
	{
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		return false;
	}

	for (int y = 0; y < height; ++y)
	{
		const int sourceY = (y * sourceHeight) / height;
		for (int x = 0; x < width; ++x)
		{
			const int sourceX = (x * sourceWidth) / width;
			const size_t source = (static_cast<size_t>(sourceY) * sourceWidth + sourceX) * 4;
			const size_t destination = (static_cast<size_t>(y) * width + x) * 3;
			rgb[destination + 0] = rgba[source + 0];
			rgb[destination + 1] = rgba[source + 1];
			rgb[destination + 2] = rgba[source + 2];
		}
	}

	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	return M_CreatePNG(file, rgb.data() + static_cast<size_t>(height - 1) * width * 3,
		nullptr, SS_RGB, width, height, -width * 3);
}

bool gl_AndroidNativeGLES_WipeStart(int type)
{
	if (!gl_AndroidNativeGLES_CanUseResources() ||
		(type != wipe_Melt && type != wipe_Burn && type != wipe_Fade))
		return false;
	if (Resources.wipeStartTexture != 0) glDeleteTextures(1, &Resources.wipeStartTexture);
	if (Resources.wipeEndTexture != 0) glDeleteTextures(1, &Resources.wipeEndTexture);
	Resources.wipeStartTexture = 0;
	Resources.wipeEndTexture = 0;
	Resources.wipeStartReady = false;
	Resources.wipeEndReady = false;
	Resources.wipeEndCapturePending = false;
	Resources.wipeActive = false;
	Resources.wipeType = type;
	Resources.wipeProgress = 0.0f;
	if (!BuildWipeTexture(Resources.wipeStartTexture) ||
		!CaptureWipeTexture(Resources.wipeStartTexture))
	{
		if (Resources.wipeStartTexture != 0) glDeleteTextures(1, &Resources.wipeStartTexture);
		Resources.wipeStartTexture = 0;
		return false;
	}
	Resources.wipeStartReady = true;
	return true;
}

void gl_AndroidNativeGLES_WipeEnd()
{
	if (!gl_AndroidNativeGLES_CanUseResources() || !Resources.wipeStartReady) return;
	Resources.wipeEndCapturePending = true;
	Resources.wipeEndReady = false;
	Resources.wipeProgress = 0.0f;
}

bool gl_AndroidNativeGLES_WipeDo(int ticks)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || !Resources.wipeStartReady) return true;
	Resources.wipeActive = true;
	Resources.wipeProgress = std::min(1.0f, Resources.wipeProgress +
		std::max(ticks, 1) / 32.0f);
	return Resources.wipeProgress >= 1.0f;
}

void gl_AndroidNativeGLES_WipeCleanup()
{
	if (Resources.wipeStartTexture != 0) glDeleteTextures(1, &Resources.wipeStartTexture);
	if (Resources.wipeEndTexture != 0) glDeleteTextures(1, &Resources.wipeEndTexture);
	Resources.wipeStartTexture = 0;
	Resources.wipeEndTexture = 0;
	Resources.wipeStartReady = false;
	Resources.wipeEndReady = false;
	Resources.wipeEndCapturePending = false;
	Resources.wipeActive = false;
	Resources.wipeType = wipe_None;
	Resources.wipeProgress = 0.0f;
}

bool gl_AndroidNativeGLES_IsActive()
{
	return NativeBackendEnabled;
}

bool gl_AndroidNativeGLES_CanUseResources()
{
	return NativeBackendEnabled && Resources.ready;
}

const FAndroidGLESInfo &gl_AndroidNativeGLES_GetCapabilities()
{
	return Capabilities;
}

void gl_AndroidNativeGLES_PrintStartupLog()
{
	if (!CapabilitiesReady) return;
	Printf("GL_VENDOR: %s\n", Capabilities.vendor);
	Printf("GL_RENDERER: %s\n", Capabilities.renderer);
	Printf("GL_VERSION: %s\n", Capabilities.version);
	Printf("GL_SHADING_LANGUAGE_VERSION: %s\n", Capabilities.shadingLanguageVersion);
	Printf("GLES capabilities: ES %d.%d, max texture %d, texture units %d, fragment uniforms %d, extensions %d.\n",
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
	DPrintf("GLES extension list follows (%d entries):\n", Capabilities.extensionCount);
	for (GLint index = 0; index < Capabilities.extensionCount; ++index)
		DPrintf("  %s\n", glGetStringi(GL_EXTENSIONS, index));
}

void gl_AndroidNativeGLES_ResetState(int width, int height)
{
	ResetState(width, height);
}

bool gl_AndroidNativeGLES_ApplyRenderState(int srcBlend, int dstBlend, int alphaFunc,
	float alphaThreshold, bool alphaTest, int blendEquation, bool fogEnabled,
	bool textureEnabled, int textureMode)
{
	(void)alphaFunc;
	(void)alphaThreshold;
	(void)textureMode;
	if (!gl_AndroidNativeGLES_CanUseResources()) return false;
	glBlendFunc(static_cast<GLenum>(srcBlend), static_cast<GLenum>(dstBlend));
	glBlendEquation(static_cast<GLenum>(blendEquation));
	if (alphaTest || fogEnabled || !textureEnabled)
	{
		if (!Resources.stateWarningLogged)
		{
			Resources.stateWarningLogged = true;
			DPrintf("Android GLES state request is consumed by the native shader variants; alpha, fog, and texture flags remain submission semantics.\n");
		}
	}
	return CheckError("FRenderState::Apply") == GL_NO_ERROR;
}

void gl_AndroidNativeGLES_GenerateMipmap()
{
	glGenerateMipmap(GL_TEXTURE_2D);
}

bool gl_AndroidNativeGLES_CreateFlatBufferObjects(unsigned int *vbo, unsigned int *vao, unsigned int *ebo)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || vbo == NULL || vao == NULL || ebo == NULL) return false;
	glGenBuffers(1, vbo);
	glGenVertexArrays(1, vao);
	glGenBuffers(1, ebo);
	return CheckError("flat buffer object creation") == GL_NO_ERROR;
}

bool gl_AndroidNativeGLES_UploadFlatBuffer(unsigned int vbo, unsigned int vao, unsigned int ebo,
	const void *vertices, int vertexCount, int vertexStride)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || vbo == 0 || vao == 0 || ebo == 0 || vertices == NULL ||
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

bool gl_AndroidNativeGLES_UpdateFlatBuffer(unsigned int vbo, int offset, int size, const void *vertices)
{
	if (!gl_AndroidNativeGLES_CanUseResources() || vbo == 0 || offset < 0 || size <= 0 || vertices == NULL) return false;
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferSubData(GL_ARRAY_BUFFER, offset, size, vertices);
	return CheckError("flat buffer update") == GL_NO_ERROR;
}

void gl_AndroidNativeGLES_DestroyFlatBufferObjects(unsigned int vbo, unsigned int vao, unsigned int ebo)
{
	if (vbo != 0) glDeleteBuffers(1, &vbo);
	if (ebo != 0) glDeleteBuffers(1, &ebo);
	if (vao != 0) glDeleteVertexArrays(1, &vao);
}

void gl_AndroidNativeGLES_BindFlatBuffer(unsigned int vao, unsigned int vbo)
{
	if (!gl_AndroidNativeGLES_CanUseResources()) return;
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
}

#endif
