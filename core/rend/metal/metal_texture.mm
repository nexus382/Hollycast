// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_texture.h"
#include "metal_context.h"
#include "metal_texture_upload.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
struct MetalGpuPreloadedTexture final : GpuPreloadedTexture
{
	explicit MetalGpuPreloadedTexture(u8 levels) : GpuPreloadedTexture(levels) {}
	id<MTLTexture> image = nil;
};


}

std::string MetalTexture::GetId()
{
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%p", static_cast<void *>(this));
	return buffer;
}

CustomTextureCapabilities MetalTexture::getCustomTextureCapabilities()
{
	// The transcoder can always produce RGBA8, including on devices without
	// BC or ASTC support. Only advertise formats accepted by uploadCustomTexture.
	return CustomTextureCapabilities::rgbaOnly(CustomTextureBackend::Metal);
}

void MetalTexture::UploadToGPU(int width, int height, const u8 *data, bool mipmapped, bool mipmapsIncluded)
{
	using namespace metal_texture_upload;
	PixelKind kind = PixelKind::Rgba8;
	switch (tex_type) {
	case TextureType::_8: kind = PixelKind::Index8; break;
	case TextureType::_565: kind = PixelKind::Rgb565; break;
	case TextureType::_5551: kind = PixelKind::Rgba5551; break;
	case TextureType::_4444: kind = PixelKind::Rgba4444; break;
	default: break;
	}
	id<MTLTexture> replacement = decoded(MetalContext::Instance()->GetDevice(),
			MetalContext::Instance()->GetQueue(), width, height, data, kind, mipmapped, mipmapsIncluded);
	if (!replacement) return;
	image = replacement;
	gpuPreloadedTexture.reset();
	usingGpuPreloadedTexture = false;
}

bool MetalTexture::uploadCustomTexture(const PreparedCustomTexture& prepared, bool mipmapped)
{
	validatePreparedCustomTexture(prepared);
	id<MTLTexture> replacement = metal_texture_upload::custom(MetalContext::Instance()->GetDevice(),
			MetalContext::Instance()->GetQueue(), prepared, mipmapped);
	if (!replacement) return false;
	image = replacement;
	gpuPreloadedTexture.reset();
	usingGpuPreloadedTexture = false;
	return true;
}

GpuPreloadedTexture::Ptr MetalTexture::createGpuPreloadedTexture(const PreparedCustomTexture& prepared)
{
	MetalTexture uploaded;
	if (!uploaded.uploadCustomTexture(prepared, true))
		return nullptr;
	auto result = std::make_shared<MetalGpuPreloadedTexture>(
			static_cast<u8>(prepared.generateMipmaps
					? mipmapLevelCount(prepared.width, prepared.height) : prepared.levels.size()));
	result->image = uploaded.image;
	return result;
}

bool MetalTexture::useGpuPreloadedTexture(const GpuPreloadedTexture::Ptr& texture)
{
	auto metal = std::dynamic_pointer_cast<MetalGpuPreloadedTexture>(texture);
	if (!metal || !metal->image)
		return false;
	image = metal->image;
	return true;
}

MetalTexture *MetalTextureCache::GetTexture(TSP tsp, TCW tcw, int area)
{
	MetalTexture *texture = getTextureCacheData(tsp, tcw, area);
	if (texture->NeedsUpdate())
	{
		if (!texture->Update())
			return nullptr;
	}
	else if (texture->IsCustomTextureAvailable())
		texture->CheckCustomTexture();
	return texture;
}
