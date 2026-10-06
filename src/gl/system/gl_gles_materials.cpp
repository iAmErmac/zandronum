#include "gl/system/gl_gles_internal.h"

#if defined(__ANDROID__) || defined(ZANDRONUM_GLES_BACKEND)

#if defined(__ANDROID__)
#include <GLES3/gl32.h>
#endif
#include <string.h>
#include <stdint.h>
#include <functional>
#include <unordered_map>
#include <vector>

#include "gl/renderer/gl_renderer.h"
#include "gl/renderer/gl_colormap.h"
#include "gl/system/gl_cvars.h"
#include "gl/system/gl_gles_dispatch.h"

extern TexFilter_s TexFilter[];

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
		uint64_t revision;
		GLenum internalFormat;
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
	std::unordered_map<const void *, uint64_t> MaterialRevisions;
	std::unordered_map<GLuint, unsigned int> MaterialFlagsByTexture;
	std::unordered_map<GLuint, FGLESMaterialEffect> MaterialEffectsByTexture;
	constexpr size_t MaterialFlagCacheSize = 256;
	struct FMaterialFlagCacheEntry
	{
		GLuint texture;
		unsigned int flags;
	};
	FMaterialFlagCacheEntry MaterialFlagCache[MaterialFlagCacheSize] = {};

	size_t MaterialFlagCacheSlot(GLuint texture)
	{
		return (static_cast<size_t>(texture) * 2654435761u) & (MaterialFlagCacheSize - 1);
	}

	void InvalidateMaterialFlagCache(GLuint texture)
	{
		if (texture == 0) return;
		FMaterialFlagCacheEntry &entry = MaterialFlagCache[MaterialFlagCacheSlot(texture)];
		if (entry.texture == texture) entry.texture = 0;
	}

	void ClearMaterialFlagCache()
	{
		memset(MaterialFlagCache, 0, sizeof(MaterialFlagCache));
	}

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

	uint64_t MaterialRevision(const void *key)
	{
		const auto found = MaterialRevisions.find(key);
		return found != MaterialRevisions.end() ? found->second : 0;
	}

	void RemoveMaterialTextureIndex(GLuint texture)
	{
		if (texture != 0)
		{
			MaterialFlagsByTexture.erase(texture);
			MaterialEffectsByTexture.erase(texture);
			InvalidateMaterialFlagCache(texture);
		}
	}
}

GLuint gl_GLESInternalFindMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires, int width, int height)
{
	const FGLESMaterialTexture *entry = FindMaterialEntry(
		MakeMaterialKey(key, colormap, translation, repeat, allowhires));
	if (entry != nullptr && entry->texture != 0 && entry->width == width && entry->height == height)
	{
		gl_GLES_RecordProfileMaterial(true, false);
		return entry->texture;
	}
	return 0;
}

GLuint gl_GLESInternalFindStaticMaterialTexture(const void *key, int colormap, int translation,
	bool repeat, bool allowhires)
{
	const FGLESMaterialTexture *entry = FindMaterialEntry(
		MakeMaterialKey(key, colormap, translation, repeat, allowhires));
	if (entry != nullptr && entry->texture != 0 && !entry->framebufferContent &&
		entry->revision == MaterialRevision(key))
	{
		gl_GLES_RecordProfileMaterial(true, false);
		return entry->texture;
	}
	return 0;
}

void gl_GLESInternalInvalidateMaterial(const void *key)
{
	++MaterialRevisions[key];
}

unsigned int gl_GLES_GetTextureFormat(bool fullPrecision)
{
	if (fullPrecision) return GL_RGBA8;
	if (gl_texture_format == 1) return GL_RGB5_A1;
	// GLES has no RGBA2; RGBA4 preserves its minimum component precision.
	if (gl_texture_format == 2 || gl_texture_format == 3) return GL_RGBA4;
	return GL_RGBA8;
}

GLuint gl_GLESInternalBindMaterial(bool resourcesAvailable, const void *key,
	const unsigned char *pixels, int width, int height, bool repeat, int colormap,
	int translation, bool allowhires, bool palette, bool noFilter, bool noCompression)
{
	if (!resourcesAvailable || key == NULL || pixels == NULL || width <= 0 || height <= 0)
		return 0;
	const size_t pixelBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
	const GLenum internalFormat = gl_GLES_GetTextureFormat(noCompression || colormap == CM_SHADE);
	const FGLESMaterialKey materialKey = MakeMaterialKey(key, colormap, translation, repeat, allowhires);
	FGLESMaterialTexture *entry = FindMaterialEntry(materialKey);
	if (entry != NULL)
	{
		entry->revision = MaterialRevision(key);
		if (entry->framebufferContent && entry->texture != 0 && entry->width == width && entry->height == height)
		{
			gl_GLES_RecordProfileMaterial(true, false);
			return entry->texture;
		}
		if (entry->texture != 0 && entry->width == width && entry->height == height && entry->internalFormat == internalFormat &&
			entry->pixels.size() == pixelBytes && memcmp(entry->pixels.data(), pixels, pixelBytes) == 0)
		{
			gl_GLES_RecordProfileMaterial(true, false);
			return entry->texture;
		}
	}
	gl_GLES_RecordProfileMaterial(false, true);
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
	entry->revision = MaterialRevision(key);
	if (entry->width != width || entry->height != height || entry->internalFormat != internalFormat)
	{
		if (entry->texture != 0)
		{
			RemoveMaterialTextureIndex(entry->texture);
			glDeleteTextures(1, &entry->texture);
		}
		entry->texture = 0;
		entry->width = width;
		entry->height = height;
		entry->internalFormat = internalFormat;
		entry->framebufferContent = false;
		entry->pixels.assign(pixels, pixels + pixelBytes);
	}
	else if (entry->texture != 0)
	{
		entry->framebufferContent = false;
		MaterialFlagsByTexture[entry->texture] &= ~GLES_TEXTURE_FLAG_FRAMEBUFFER;
		InvalidateMaterialFlagCache(entry->texture);
		entry->pixels.assign(pixels, pixels + pixelBytes);
		glBindTexture(GL_TEXTURE_2D, entry->texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		glGenerateMipmap(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		if (gl_GLES_CheckErrors("material texture update") != GL_NO_ERROR)
		{
			RemoveMaterialTextureIndex(entry->texture);
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
	int filterIndex = gl_texture_filter;
	if (filterIndex < 0 || filterIndex > 5) filterIndex = 0;
	const TexFilter_s &filter = TexFilter[filterIndex];
	const bool useMipmaps = repeat;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		useMipmaps ? filter.minfilter : filter.magfilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter.magfilter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		entry->pixels.data());
	if (useMipmaps) glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	MaterialFlagsByTexture[entry->texture] = (entry->palette ? GLES_TEXTURE_FLAG_PALETTE : 0) |
		(noFilter ? GLES_TEXTURE_FLAG_NOFILTER : 0);
	if (gl_GLES_CheckErrors("material texture upload") != GL_NO_ERROR)
	{
		RemoveMaterialTextureIndex(entry->texture);
		glDeleteTextures(1, &entry->texture);
		entry->texture = 0;
	}
	return entry->texture;
}

unsigned int gl_GLESInternalGetMaterialFlags(GLuint texture)
{
	if (texture == 0) return 0;
	FMaterialFlagCacheEntry &cached = MaterialFlagCache[MaterialFlagCacheSlot(texture)];
	if (cached.texture == texture) return cached.flags;
	const auto found = MaterialFlagsByTexture.find(texture);
	const unsigned int flags = found != MaterialFlagsByTexture.end() ? found->second : 0;
	cached.texture = texture;
	cached.flags = flags;
	return flags;
}

void gl_GLESInternalSetMaterialEffect(GLuint texture, int shaderIndex, float speed, int colormap)
{
	if (texture != 0 && (shaderIndex != 0 || colormap != CM_DEFAULT))
		MaterialEffectsByTexture[texture] = { shaderIndex, speed, colormap };
	else
		MaterialEffectsByTexture.erase(texture);
}

FGLESMaterialEffect gl_GLESInternalGetMaterialEffect(GLuint texture)
{
	const auto found = MaterialEffectsByTexture.find(texture);
	return found != MaterialEffectsByTexture.end() ? found->second : FGLESMaterialEffect{};
}

void gl_GLESInternalMarkMaterialFramebufferContent(const void *key, int colormap,
	int translation, bool repeat, bool allowhires)
{
	if (key == NULL) return;
	FGLESMaterialTexture *entry = FindMaterialEntry(
		MakeMaterialKey(key, colormap, translation, repeat, allowhires));
	if (entry != NULL)
	{
		entry->framebufferContent = true;
		if (entry->texture != 0)
		{
			MaterialFlagsByTexture[entry->texture] |= GLES_TEXTURE_FLAG_FRAMEBUFFER;
			InvalidateMaterialFlagCache(entry->texture);
		}
	}
}

void gl_GLESInternalDeleteMaterialTextures()
{
	for (size_t i = 0; i < MaterialTextures.size(); ++i)
	{
		if (MaterialTextures[i].texture != 0)
		{
			RemoveMaterialTextureIndex(MaterialTextures[i].texture);
			glDeleteTextures(1, &MaterialTextures[i].texture);
		}
		MaterialTextures[i].texture = 0;
	}
}

void gl_GLESInternalInvalidateMaterials()
{
	for (size_t i = 0; i < MaterialTextures.size(); ++i)
		MaterialTextures[i].texture = 0;
	MaterialFlagsByTexture.clear();
	MaterialEffectsByTexture.clear();
	ClearMaterialFlagCache();
}

void gl_GLESInternalClearMaterials(bool contextAvailable)
{
	if (contextAvailable) gl_GLESInternalDeleteMaterialTextures();
	MaterialTextures.clear();
	MaterialTextureIndices.clear();
	MaterialRevisions.clear();
	MaterialFlagsByTexture.clear();
	MaterialEffectsByTexture.clear();
	ClearMaterialFlagCache();
}

#endif
