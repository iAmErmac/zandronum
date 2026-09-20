#include "gl/system/gl_gles_internal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif
#include <string.h>
#include <functional>
#include <unordered_map>
#include <vector>

#include "gl/system/gl_gles_dispatch.h"

namespace
{
	struct FGLESMaterialTexture
	{
		const void *key;
		int colormap;
		int translation;
		int width;
		int height;
		bool repeat;
		bool allowhires;
		bool palette;
		bool framebufferContent;
		GLuint texture;
		std::vector<unsigned char> pixels;
	};

	struct FGLESMaterialKey
	{
		const void *key;
		int colormap;
		int translation;
		bool repeat;
		bool allowhires;

		bool operator==(const FGLESMaterialKey &other) const
		{
			return key == other.key && colormap == other.colormap &&
				translation == other.translation && repeat == other.repeat &&
				allowhires == other.allowhires;
		}
	};

	struct FGLESMaterialKeyHash
	{
		size_t operator()(const FGLESMaterialKey &value) const
		{
			size_t hash = std::hash<const void *>()(value.key);
			hash ^= static_cast<size_t>(value.colormap) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
			hash ^= static_cast<size_t>(value.translation) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
			hash ^= static_cast<size_t>(value.repeat) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
			return hash ^ (static_cast<size_t>(value.allowhires) + 0x9e3779b9 + (hash << 6) + (hash >> 2));
		}
	};

	std::vector<FGLESMaterialTexture> MaterialTextures;
	std::unordered_map<FGLESMaterialKey, size_t, FGLESMaterialKeyHash> MaterialTextureIndices;

	FGLESMaterialKey MakeMaterialKey(const void *key, int colormap, int translation,
		bool repeat, bool allowhires)
	{
		return { key, colormap, translation, repeat, allowhires };
	}

	FGLESMaterialTexture *FindMaterialEntry(const FGLESMaterialKey &key)
	{
		const auto found = MaterialTextureIndices.find(key);
		if (found == MaterialTextureIndices.end() || found->second >= MaterialTextures.size())
			return nullptr;
		return &MaterialTextures[found->second];
	}
}

GLuint gl_GLESInternalFindMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires, int width, int height)
{
	const FGLESMaterialTexture *entry = FindMaterialEntry(
		MakeMaterialKey(key, colormap, translation, repeat, allowhires));
	if (entry != nullptr && entry->texture != 0 && entry->width == width && entry->height == height)
		return entry->texture;
	return 0;
}

GLuint gl_GLESInternalBindMaterial(bool resourcesAvailable, const void *key,
	const unsigned char *pixels, int width, int height, bool repeat, int colormap,
	int translation, bool allowhires, bool palette)
{
	if (!resourcesAvailable || key == NULL || pixels == NULL || width <= 0 || height <= 0)
		return 0;
	const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
	const FGLESMaterialKey materialKey = MakeMaterialKey(key, colormap, translation, repeat, allowhires);
	FGLESMaterialTexture *entry = FindMaterialEntry(materialKey);
	if (entry != NULL)
	{
		if (entry->framebufferContent && entry->texture != 0 && entry->width == width && entry->height == height)
			return entry->texture;
		if (entry->texture != 0 && entry->width == width && entry->height == height &&
			entry->pixels.size() == pixelBytes && memcmp(entry->pixels.data(), pixels, pixelBytes) == 0)
			return entry->texture;
	}
	if (entry == NULL)
	{
		FGLESMaterialTexture value = {};
		value.key = key;
		value.colormap = colormap;
		value.translation = translation;
		value.allowhires = allowhires;
		value.width = width;
		value.height = height;
		value.repeat = repeat;
		value.palette = palette;
		MaterialTextures.push_back(value);
		MaterialTextureIndices.emplace(materialKey, MaterialTextures.size() - 1);
		entry = &MaterialTextures.back();
	}
	if (entry->width != width || entry->height != height)
	{
		if (entry->texture != 0) glDeleteTextures(1, &entry->texture);
		entry->texture = 0;
		entry->width = width;
		entry->height = height;
		entry->framebufferContent = false;
		entry->pixels.assign(pixels, pixels + pixelBytes);
	}
	else if (entry->texture != 0)
	{
		entry->framebufferContent = false;
		entry->pixels.assign(pixels, pixels + pixelBytes);
		glBindTexture(GL_TEXTURE_2D, entry->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (gl_GLES_CheckErrors("material texture update") != GL_NO_ERROR)
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
		entry->pixels.data());
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	if (gl_GLES_CheckErrors("material texture upload") != GL_NO_ERROR)
	{
		glDeleteTextures(1, &entry->texture);
		entry->texture = 0;
	}
	return entry->texture;
}

bool gl_GLESInternalIsPaletteTexture(GLuint texture)
{
	if (texture == 0) return false;
	for (size_t i = 0; i < MaterialTextures.size(); ++i)
		if (MaterialTextures[i].texture == texture) return MaterialTextures[i].palette;
	return false;
}

void gl_GLESInternalMarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires)
{
	if (key == NULL) return;
	FGLESMaterialTexture *entry = FindMaterialEntry(
		MakeMaterialKey(key, colormap, translation, repeat, allowhires));
	if (entry != NULL) entry->framebufferContent = true;
}

void gl_GLESInternalDeleteMaterialTextures()
{
	for (size_t i = 0; i < MaterialTextures.size(); ++i)
	{
		if (MaterialTextures[i].texture != 0) glDeleteTextures(1, &MaterialTextures[i].texture);
		MaterialTextures[i].texture = 0;
	}
}

void gl_GLESInternalInvalidateMaterials()
{
	for (size_t i = 0; i < MaterialTextures.size(); ++i)
		MaterialTextures[i].texture = 0;
}

void gl_GLESInternalClearMaterials(bool contextAvailable)
{
	if (contextAvailable) gl_GLESInternalDeleteMaterialTextures();
	MaterialTextures.clear();
	MaterialTextureIndices.clear();
}

#endif
