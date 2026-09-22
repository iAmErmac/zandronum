#include "gl/system/gl_gles_portal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif

#include <algorithm>
#include <deque>
#include <math.h>
#include <stdio.h>
#include <vector>

#include "gl/data/gl_data.h"
#include "gl/renderer/gl_renderer.h"
#include "gl/system/gl_gles_context.h"
#include "gl/system/gl_cvars.h"
#include "gl/system/gl_gles_dispatch.h"
#include "gl/system/gl_gles_renderer.h"
#include "gl/system/gl_gles_shader.h"
#include "gl/textures/gl_material.h"
#include "gl/textures/gl_skyboxtexture.h"

EXTERN_CVAR(Float, skyoffset)
EXTERN_CVAR(Int, gl_sky_detail)

namespace
{
	struct FSkyVertex
	{
		GLfloat x, y, z;
		GLfloat nx, ny, nz;
		GLfloat u, v;
		GLfloat r, g, b, a;
		GLfloat glowTopDistance, glowBottomDistance;
	};

	struct FSkyGeometry
	{
		GLuint vertexArray;
		GLuint vertexBuffer;
		GLuint indexBuffer;
		FGLESSkyPrimitiveRange upperCap;
		FGLESSkyPrimitiveRange upperStrips[4];
		FGLESSkyPrimitiveRange lowerCap;
		FGLESSkyPrimitiveRange lowerStrips[4];
		FGLESSkyPrimitiveRange skyboxFaces[6];
	};

	struct FPortalResources
	{
		GLuint compositeProgram;
		GLint viewProjection;
		GLint sourceTexture;
		GLint targetSize;
		GLint forceFarDepth;
		GLint solidColor;
		// Parent surfaces remain referenced while nested captures append targets.
		std::deque<FGLESTargetDescriptor> fallbackTargets;
		size_t fallbackTargetsInUse;
		bool allocationFailureReported;
		FSkyGeometry sky;
		std::vector<FSkyVertex> skyVertices;
		std::vector<GLushort> skyIndices;
		size_t skyVertexBufferCapacity;
		size_t skyIndexBufferCapacity;
		bool skyUploadKnown;
		FMaterial *skyUploadMaterial;
		float skyUploadXOffset;
		float skyUploadYOffset;
		float skyUploadCameraX;
		float skyUploadCameraY;
		float skyUploadCameraZ;
		float skyUploadGlobalOffset;
		int skyUploadDetail;
		bool skyUploadMirrored;
		GLsizei skyUploadIndexCount;
	};

	FPortalResources Portal = {};

	const char *PortalVertexSource =
		"#version 320 es\n"
		"layout(location = 0) in vec3 a_position;\n"
		"uniform mat4 u_view_projection;\n"
		"uniform bool u_force_far_depth;\n"
		"void main() { vec4 position = u_view_projection * vec4(a_position, 1.0); if (u_force_far_depth) position.z = position.w; gl_Position = position; }\n";

	const char *PortalFragmentSource =
		"#version 320 es\n"
		"precision highp float;\n"
		"layout(location = 0) out vec4 frag_color;\n"
		"uniform sampler2D u_capture;\n"
		"uniform vec4 u_target_size;\n"
		"uniform bool u_solid_color;\n"
		"void main() { frag_color = u_solid_color ? vec4(0.0, 0.0, 0.0, 1.0) : texture(u_capture, gl_FragCoord.xy / u_target_size.xy); }\n";

	void DestroySkyGeometry()
	{
		if (Portal.sky.vertexBuffer != 0) glDeleteBuffers(1, &Portal.sky.vertexBuffer);
		if (Portal.sky.indexBuffer != 0) glDeleteBuffers(1, &Portal.sky.indexBuffer);
		if (Portal.sky.vertexArray != 0) glDeleteVertexArrays(1, &Portal.sky.vertexArray);
		Portal.sky = {};
		Portal.skyVertexBufferCapacity = 0;
		Portal.skyIndexBufferCapacity = 0;
		Portal.skyUploadKnown = false;
	}

	size_t GrowBufferCapacity(size_t capacity, size_t required)
	{
		if (capacity == 0) capacity = 4096;
		while (capacity < required) capacity *= 2;
		return capacity;
	}

	bool InitializeSkyGeometry()
	{
		if (Portal.sky.vertexArray != 0 && Portal.sky.vertexBuffer != 0 && Portal.sky.indexBuffer != 0)
			return true;
		glGenVertexArrays(1, &Portal.sky.vertexArray);
		glGenBuffers(1, &Portal.sky.vertexBuffer);
		glGenBuffers(1, &Portal.sky.indexBuffer);
		if (Portal.sky.vertexArray == 0 || Portal.sky.vertexBuffer == 0 || Portal.sky.indexBuffer == 0)
		{
			DestroySkyGeometry();
			gl_GLES_Report("portal", "sky geometry buffers could not be allocated");
			return false;
		}
		glBindVertexArray(Portal.sky.vertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Portal.sky.vertexBuffer);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Portal.sky.indexBuffer);
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(FSkyVertex),
			reinterpret_cast<const void *>(offsetof(FSkyVertex, x)));
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(FSkyVertex),
			reinterpret_cast<const void *>(offsetof(FSkyVertex, u)));
		glEnableVertexAttribArray(2);
		glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(FSkyVertex),
			reinterpret_cast<const void *>(offsetof(FSkyVertex, r)));
		glEnableVertexAttribArray(3);
		glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(FSkyVertex),
			reinterpret_cast<const void *>(offsetof(FSkyVertex, nx)));
		glEnableVertexAttribArray(4);
		glVertexAttribPointer(4, 2, GL_FLOAT, GL_FALSE, sizeof(FSkyVertex),
			reinterpret_cast<const void *>(offsetof(FSkyVertex, glowTopDistance)));
		glBindVertexArray(0);
		return gl_GLES_CheckErrors("sky geometry setup") == GL_NO_ERROR;
	}

	void ResetUniforms()
	{
		Portal.viewProjection = -1;
		Portal.sourceTexture = -1;
		Portal.targetSize = -1;
		Portal.forceFarDepth = -1;
		Portal.solidColor = -1;
	}

	bool EnsureCompositeProgram()
	{
		if (Portal.compositeProgram != 0)
			return true;
		char log[1024] = {};
		Portal.compositeProgram = gl_GLES_LinkProgram(PortalVertexSource, PortalFragmentSource,
			"portal FBO composite", log, sizeof(log));
		if (Portal.compositeProgram == 0)
		{
			gl_GLES_Report("portal", log);
			return false;
		}
		Portal.viewProjection = glGetUniformLocation(Portal.compositeProgram, "u_view_projection");
		Portal.sourceTexture = glGetUniformLocation(Portal.compositeProgram, "u_capture");
		Portal.targetSize = glGetUniformLocation(Portal.compositeProgram, "u_target_size");
		Portal.forceFarDepth = glGetUniformLocation(Portal.compositeProgram, "u_force_far_depth");
		Portal.solidColor = glGetUniformLocation(Portal.compositeProgram, "u_solid_color");
		return true;
	}

	GLsizei BuildSkyGeometry(FMaterial *material, float xOffset, float yOffset, bool mirrored,
		float cameraX, float cameraY, float cameraZ)
	{
		const float radius = 10000.0f;
		const int rows = 4;
		const int columns = 4 * std::max(gl_sky_detail > 0 ? gl_sky_detail : 1, 1);
		std::vector<FSkyVertex> &vertices = Portal.skyVertices;
		std::vector<GLushort> &indices = Portal.skyIndices;
		vertices.clear();
		indices.clear();
		vertices.reserve((columns + rows * (columns + 1)) * 2);
		indices.reserve((columns * 3 + rows * (columns + 1) * 2) * 2);
		Portal.sky.upperCap = {};
		Portal.sky.lowerCap = {};
		for (int row = 0; row < rows; ++row)
		{
			Portal.sky.upperStrips[row] = {};
			Portal.sky.lowerStrips[row] = {};
		}
		const int textureWidth = material != nullptr ?
			std::max(1, material->TextureWidth(GLUSE_TEXTURE)) : 256;
		const int textureHeight = material != nullptr ?
			std::max(1, material->TextureHeight(GLUSE_TEXTURE)) : 128;
		float timesRepeat = static_cast<float>(static_cast<short>(4.0f * (256.0f / textureWidth)));
		if (timesRepeat == 0.0f) timesRepeat = 1.0f;
		const float textureVOffset = yOffset / static_cast<float>(textureHeight);
		float verticalScale = 1.0f;
		float verticalOffset = 0.0f;
		float textureVScale = 1.0f;
		if (material != nullptr && textureHeight < 128)
		{
			verticalOffset = -1250.0f;
			verticalScale = 128.0f / 230.0f;
			textureVScale = static_cast<float>(128 / textureHeight);
		}
		else if (material != nullptr && textureHeight < 200)
		{
			verticalOffset = -1250.0f;
			verticalScale = textureHeight / 230.0f;
		}
		else if (material != nullptr && textureHeight <= 240)
		{
			verticalOffset = (200.0f - textureHeight + material->tex->SkyOffset + skyoffset) * 57.0f;
			verticalScale = 1.0f + ((textureHeight - 200.0f) / 200.0f) * 1.17f;
		}
		else if (material != nullptr)
		{
			verticalOffset = (-40.0f + material->tex->SkyOffset + skyoffset) * 57.0f;
			verticalScale = 1.2f * 1.17f;
			textureVScale = 240.0f / textureHeight;
		}
		const float skyCenter[3] = { cameraX, cameraZ + verticalOffset, cameraY };
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
			FSkyVertex vertex = {};
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
			FGLESSkyPrimitiveRange &cap = lower ? Portal.sky.lowerCap : Portal.sky.upperCap;
			FGLESSkyPrimitiveRange *strips = lower ? Portal.sky.lowerStrips : Portal.sky.upperStrips;
			const int capRow = material == nullptr ? 0 : 1;
			const float capSide = 1.04719755f * (rows - capRow) / rows;
			const float capHeight = radius * sinf(capSide) * verticalScale;
			FSkyVertex capCenter = {};
			capCenter.x = skyCenter[0];
			capCenter.y = skyCenter[1] + (lower ? -capHeight : capHeight) + 300.0f - 1.0f;
			capCenter.z = skyCenter[2];
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
		if (Portal.sky.vertexArray == 0 || Portal.sky.vertexBuffer == 0 || Portal.sky.indexBuffer == 0)
			return 0;
		glBindVertexArray(Portal.sky.vertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Portal.sky.vertexBuffer);
		const size_t vertexBytes = vertices.size() * sizeof(FSkyVertex);
		if (vertexBytes > Portal.skyVertexBufferCapacity)
		{
			Portal.skyVertexBufferCapacity = GrowBufferCapacity(Portal.skyVertexBufferCapacity, vertexBytes);
			glBufferData(GL_ARRAY_BUFFER, Portal.skyVertexBufferCapacity, nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ARRAY_BUFFER, 0, vertexBytes, vertices.data());
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Portal.sky.indexBuffer);
		const size_t indexBytes = indices.size() * sizeof(GLushort);
		if (indexBytes > Portal.skyIndexBufferCapacity)
		{
			Portal.skyIndexBufferCapacity = GrowBufferCapacity(Portal.skyIndexBufferCapacity, indexBytes);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, Portal.skyIndexBufferCapacity, nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexBytes, indices.data());
		glBindVertexArray(0);
		if (gl_GLES_CheckErrors("sky geometry upload") != GL_NO_ERROR)
		{
			Portal.skyVertexBufferCapacity = 0;
			Portal.skyIndexBufferCapacity = 0;
			return 0;
		}
		return static_cast<GLsizei>(indices.size());
	}

	void RotateSkyboxPoint(float x, float y, float z, float angle, bool sky2,
		float &outX, float &outY, float &outZ)
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

	void BuildSkyboxGeometry(float xOffset, bool sky2, bool fliptop,
		float cameraX, float cameraY, float cameraZ)
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
		std::vector<FSkyVertex> &vertices = Portal.skyVertices;
		std::vector<GLushort> &indices = Portal.skyIndices;
		vertices.clear();
		indices.clear();
		vertices.reserve(24);
		indices.reserve(36);
		const float centerX = cameraX;
		const float centerY = cameraZ;
		const float centerZ = cameraY;
		const float rotation = -180.0f + xOffset;
		for (int face = 0; face < 6; ++face)
		{
			Portal.sky.skyboxFaces[face] = {};
			const GLushort firstVertex = static_cast<GLushort>(vertices.size());
			for (int vertexIndex = 0; vertexIndex < 4; ++vertexIndex)
			{
				const float *position = face < 4 ? sidePositions[face][vertexIndex] :
					(face == 4 ? topPositions[fliptop ? 1 : 0][vertexIndex] : bottomPositions[vertexIndex]);
				float rotatedX, rotatedY, rotatedZ;
				RotateSkyboxPoint(position[0], position[1], position[2], rotation, sky2,
					rotatedX, rotatedY, rotatedZ);
				FSkyVertex vertex = {};
				vertex.x = centerX + rotatedX;
				vertex.y = centerY + rotatedY;
				vertex.z = centerZ + rotatedZ;
				vertex.u = vertexIndex == 1 || vertexIndex == 2 ? 1.0f : 0.0f;
				vertex.v = vertexIndex >= 2 ? 1.0f : 0.0f;
				vertex.r = vertex.g = vertex.b = vertex.a = 1.0f;
				vertices.push_back(vertex);
			}
			Portal.sky.skyboxFaces[face].firstIndex = static_cast<GLsizei>(indices.size());
			indices.push_back(firstVertex + 0);
			indices.push_back(firstVertex + 1);
			indices.push_back(firstVertex + 2);
			indices.push_back(firstVertex + 2);
			indices.push_back(firstVertex + 3);
			indices.push_back(firstVertex + 0);
			Portal.sky.skyboxFaces[face].indexCount = 6;
		}
		if (Portal.sky.vertexArray == 0 || Portal.sky.vertexBuffer == 0 || Portal.sky.indexBuffer == 0)
			return;
		glBindVertexArray(Portal.sky.vertexArray);
		glBindBuffer(GL_ARRAY_BUFFER, Portal.sky.vertexBuffer);
		const size_t vertexBytes = vertices.size() * sizeof(FSkyVertex);
		if (vertexBytes > Portal.skyVertexBufferCapacity)
		{
			Portal.skyVertexBufferCapacity = GrowBufferCapacity(Portal.skyVertexBufferCapacity, vertexBytes);
			glBufferData(GL_ARRAY_BUFFER, Portal.skyVertexBufferCapacity, nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ARRAY_BUFFER, 0, vertexBytes, vertices.data());
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, Portal.sky.indexBuffer);
		const size_t indexBytes = indices.size() * sizeof(GLushort);
		if (indexBytes > Portal.skyIndexBufferCapacity)
		{
			Portal.skyIndexBufferCapacity = GrowBufferCapacity(Portal.skyIndexBufferCapacity, indexBytes);
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, Portal.skyIndexBufferCapacity, nullptr, GL_DYNAMIC_DRAW);
		}
		glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexBytes, indices.data());
		glBindVertexArray(0);
		if (gl_GLES_CheckErrors("skybox geometry upload") != GL_NO_ERROR)
		{
			Portal.skyVertexBufferCapacity = 0;
			Portal.skyIndexBufferCapacity = 0;
		}
	}
}

GLsizei gl_GLESInternalPortalUploadSkyGeometry(FMaterial *material, float xOffset,
	float yOffset, bool mirrored, float cameraX, float cameraY, float cameraZ)
{
	const bool sameInput = Portal.skyUploadKnown && Portal.skyUploadMaterial == material &&
		Portal.skyUploadXOffset == xOffset && Portal.skyUploadYOffset == yOffset &&
		Portal.skyUploadCameraX == cameraX && Portal.skyUploadCameraY == cameraY &&
		Portal.skyUploadCameraZ == cameraZ && Portal.skyUploadGlobalOffset == skyoffset &&
		Portal.skyUploadDetail == gl_sky_detail && Portal.skyUploadMirrored == mirrored;
	if (sameInput)
	{
		gl_GLES_RecordProfilePortalSkyUpload(true);
		return Portal.skyUploadIndexCount;
	}
	const GLsizei indexCount = BuildSkyGeometry(material, xOffset, yOffset, mirrored,
		cameraX, cameraY, cameraZ);
	if (indexCount <= 0)
	{
		Portal.skyUploadKnown = false;
		return indexCount;
	}
	Portal.skyUploadKnown = true;
	Portal.skyUploadMaterial = material;
	Portal.skyUploadXOffset = xOffset;
	Portal.skyUploadYOffset = yOffset;
	Portal.skyUploadCameraX = cameraX;
	Portal.skyUploadCameraY = cameraY;
	Portal.skyUploadCameraZ = cameraZ;
	Portal.skyUploadGlobalOffset = skyoffset;
	Portal.skyUploadDetail = gl_sky_detail;
	Portal.skyUploadMirrored = mirrored;
	Portal.skyUploadIndexCount = indexCount;
	gl_GLES_RecordProfilePortalSkyUpload(false);
	return indexCount;
}

void gl_GLESInternalPortalUploadSkyboxGeometry(float xOffset, bool sky2, bool fliptop,
	float cameraX, float cameraY, float cameraZ)
{
	Portal.skyUploadKnown = false;
	BuildSkyboxGeometry(xOffset, sky2, fliptop, cameraX, cameraY, cameraZ);
}

GLuint gl_GLESInternalPortalSkyVertexArray()
{
	return Portal.sky.vertexArray;
}

FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyUpperCap()
{
	return Portal.sky.upperCap;
}

FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyLowerCap()
{
	return Portal.sky.lowerCap;
}

FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyUpperStrip(int row)
{
	return row >= 0 && row < 4 ? Portal.sky.upperStrips[row] : FGLESSkyPrimitiveRange{};
}

FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyLowerStrip(int row)
{
	return row >= 0 && row < 4 ? Portal.sky.lowerStrips[row] : FGLESSkyPrimitiveRange{};
}

FGLESSkyPrimitiveRange gl_GLESInternalPortalSkyboxFace(int face)
{
	return face >= 0 && face < 6 ? Portal.sky.skyboxFaces[face] : FGLESSkyPrimitiveRange{};
}

void gl_GLESInternalPortalBeginFrame()
{
	Portal.fallbackTargetsInUse = 0;
}

bool gl_GLESInternalPortalAcquireFallbackTarget(int width, int height, int *index)
{
	if (index == nullptr || width <= 0 || height <= 0 || !gl_GLES_HasContext())
		return false;
	const size_t targetIndex = Portal.fallbackTargetsInUse++;
	if (targetIndex >= Portal.fallbackTargets.size())
		Portal.fallbackTargets.resize(targetIndex + 1);
	FGLESTargetDescriptor &target = Portal.fallbackTargets[targetIndex];
	if (target.framebuffer == 0 || target.renderWidth != width || target.renderHeight != height)
	{
		if (!gl_GLES_CreateRenderTarget(&target, width, height, 1))
		{
			if (!Portal.allocationFailureReported)
			{
				Portal.allocationFailureReported = true;
				gl_GLES_Report("portal", "dedicated capture target allocation failed; this capture will use a black aperture fallback");
			}
			*index = static_cast<int>(targetIndex);
			return false;
		}
	}
	*index = static_cast<int>(targetIndex);
	return true;
}

FGLESTargetDescriptor *gl_GLESInternalPortalGetFallbackTarget(int index)
{
	if (index < 0 || static_cast<size_t>(index) >= Portal.fallbackTargets.size())
		return nullptr;
	return &Portal.fallbackTargets[static_cast<size_t>(index)];
}

bool gl_GLESInternalPortalInitialize()
{
	return EnsureCompositeProgram() && InitializeSkyGeometry();
}

void gl_GLESInternalPortalDestroy()
{
	DestroySkyGeometry();
	if (Portal.compositeProgram != 0)
		glDeleteProgram(Portal.compositeProgram);
	Portal.compositeProgram = 0;
	ResetUniforms();
	for (FGLESTargetDescriptor &target : Portal.fallbackTargets)
		gl_GLES_DestroyRenderTarget(&target);
	Portal.fallbackTargets.clear();
	Portal.fallbackTargetsInUse = 0;
	Portal.allocationFailureReported = false;
}

void gl_GLESInternalPortalContextLost()
{
	Portal.sky = {};
	Portal.skyVertexBufferCapacity = 0;
	Portal.skyIndexBufferCapacity = 0;
	Portal.skyUploadKnown = false;
	Portal.compositeProgram = 0;
	ResetUniforms();
	for (FGLESTargetDescriptor &target : Portal.fallbackTargets)
		gl_GLES_InvalidateRenderTarget(&target);
	Portal.fallbackTargetsInUse = 0;
}

bool gl_GLESInternalPortalComposite(const FGLESPortalComposite &config)
{
	if (config.sceneVertexArray == 0 || config.indexCount <= 0 || config.viewProjection == nullptr ||
		config.targetWidth <= 0 || config.targetHeight <= 0 ||
		(!config.solidColor && config.sourceTexture == 0) || !EnsureCompositeProgram())
		return false;

	GLint previousProgram = 0;
	GLint previousVertexArray = 0;
	GLint previousActiveTexture = GL_TEXTURE0;
	GLint previousTexture = 0;
	GLint previousSampler = 0;
	GLint previousDepthFunction = GL_LESS;
	GLint previousStencilFunction = GL_ALWAYS;
	GLint previousStencilReference = 0;
	GLint previousStencilValueMask = 0xffffffffu;
	GLint previousStencilWriteMask = 0xffffffffu;
	GLint previousStencilFail = GL_KEEP;
	GLint previousStencilDepthFail = GL_KEEP;
	GLint previousStencilDepthPass = GL_KEEP;
	GLboolean previousDepthWrite = GL_TRUE;
	GLboolean previousColorWrite[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
	glGetIntegerv(GL_SAMPLER_BINDING, &previousSampler);
	glGetIntegerv(GL_DEPTH_FUNC, &previousDepthFunction);
	glGetIntegerv(GL_STENCIL_FUNC, &previousStencilFunction);
	glGetIntegerv(GL_STENCIL_REF, &previousStencilReference);
	glGetIntegerv(GL_STENCIL_VALUE_MASK, &previousStencilValueMask);
	glGetIntegerv(GL_STENCIL_WRITEMASK, &previousStencilWriteMask);
	glGetIntegerv(GL_STENCIL_FAIL, &previousStencilFail);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL, &previousStencilDepthFail);
	glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS, &previousStencilDepthPass);
	glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthWrite);
	glGetBooleanv(GL_COLOR_WRITEMASK, previousColorWrite);
	const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean stencilWasEnabled = glIsEnabled(GL_STENCIL_TEST);
	const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
	const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
	const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (config.parentStencilBit != 0)
	{
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x00);
		glStencilFunc(GL_EQUAL, config.parentStencilBit, config.parentStencilBit);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	}
	else
		glDisable(GL_STENCIL_TEST);
	gl_GLES_UseProgram(Portal.compositeProgram);
	if (Portal.viewProjection >= 0)
		glUniformMatrix4fv(Portal.viewProjection, 1, GL_FALSE, config.viewProjection);
	if (Portal.sourceTexture >= 0)
		glUniform1i(Portal.sourceTexture, 0);
	if (Portal.targetSize >= 0)
		glUniform4f(Portal.targetSize, static_cast<float>(config.targetWidth),
			static_cast<float>(config.targetHeight), 0.0f, 0.0f);
	if (Portal.forceFarDepth >= 0)
		glUniform1i(Portal.forceFarDepth, GL_FALSE);
	if (Portal.solidColor >= 0)
		glUniform1i(Portal.solidColor, config.solidColor ? GL_TRUE : GL_FALSE);
	glBindTexture(GL_TEXTURE_2D, config.sourceTexture);
	glBindSampler(0, config.sceneSampler);
	glBindVertexArray(config.sceneVertexArray);
	gl_GLES_RecordProfileDraw(false, config.indexCount);
	const GLenum indexType = config.indexType != 0 ? config.indexType : GL_UNSIGNED_INT;
	glDrawElements(GL_TRIANGLES, config.indexCount, indexType,
		reinterpret_cast<const void *>(config.indexOffsetBytes));

	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthFunc(GL_ALWAYS);
	glDepthMask(GL_TRUE);
	if (Portal.forceFarDepth >= 0)
		glUniform1i(Portal.forceFarDepth, GL_TRUE);
	gl_GLES_RecordProfileDraw(false, config.indexCount);
	glDrawElements(GL_TRIANGLES, config.indexCount, indexType,
		reinterpret_cast<const void *>(config.indexOffsetBytes));

	glBindVertexArray(static_cast<GLuint>(previousVertexArray));
	gl_GLES_UseProgram(static_cast<GLuint>(previousProgram));
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
	glBindSampler(0, static_cast<GLuint>(previousSampler));
	glActiveTexture(static_cast<GLenum>(previousActiveTexture));
	glColorMask(previousColorWrite[0], previousColorWrite[1], previousColorWrite[2], previousColorWrite[3]);
	glStencilMask(static_cast<GLuint>(previousStencilWriteMask));
	glStencilFunc(static_cast<GLenum>(previousStencilFunction), previousStencilReference,
	static_cast<GLuint>(previousStencilValueMask));
	glStencilOp(static_cast<GLenum>(previousStencilFail), static_cast<GLenum>(previousStencilDepthFail),
	static_cast<GLenum>(previousStencilDepthPass));
	glDepthFunc(static_cast<GLenum>(previousDepthFunction));
	glDepthMask(previousDepthWrite);
	if (depthWasEnabled) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (stencilWasEnabled) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
	if (blendWasEnabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
	if (cullWasEnabled) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	if (scissorWasEnabled) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
	return gl_GLES_CheckErrors("portal FBO composite") == GL_NO_ERROR;
}

bool gl_GLESInternalPortalCompositeMasks(const FGLESPortalCompositeList &config)
{
	if (config.masks == nullptr || config.maskCount == 0)
		return false;
	for (size_t maskIndex = 0; maskIndex < config.maskCount; ++maskIndex)
	{
		const FGLESPortalMask &mask = config.masks[maskIndex];
		FGLESPortalComposite composite = {};
		composite.sourceTexture = config.sourceTexture;
		composite.sceneVertexArray = config.sceneVertexArray;
		composite.sceneSampler = config.sceneSampler;
		composite.indexCount = mask.indexCount;
		composite.indexType = config.indexType;
		composite.indexOffsetBytes = mask.indexOffsetBytes;
		composite.viewProjection = mask.viewProjection;
		composite.targetWidth = config.targetWidth;
		composite.targetHeight = config.targetHeight;
		composite.parentStencilBit = config.parentStencilBit;
		composite.solidColor = config.solidColor;
		if (!gl_GLESInternalPortalComposite(composite))
			return false;
	}
	return true;
}

#endif
