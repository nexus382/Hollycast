// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "metal_common.h"
#include "rend/TexCache.h"

// Each upload replaces the Metal resource, so command buffers already using the
// previous texture retain an immutable snapshot until they complete.
class MetalTexture final : public BaseTextureCacheData
{
public:
	MetalTexture(TSP tsp = {}, TCW tcw = {}, int area = 0)
		: BaseTextureCacheData(tsp, tcw, area) {}
	MetalTexture(MetalTexture&& other)
		: BaseTextureCacheData(std::move(other)), image(other.image) { other.image = nil; }

	void UploadToGPU(int width, int height, const u8 *data, bool mipmapped,
			bool mipmapsIncluded = false) override;
	bool uploadCustomTexture(const PreparedCustomTexture& texture, bool mipmapped) override;
	bool useGpuPreloadedTexture(const GpuPreloadedTexture::Ptr& texture) override;
	static GpuPreloadedTexture::Ptr createGpuPreloadedTexture(const PreparedCustomTexture& texture);
	bool Force32BitTexture(TextureType type) const override { return type != TextureType::_8; }
	std::string GetId() override;
	id<MTLTexture> texture() const { return image; }
	void setTexture(id<MTLTexture> texture) { image = texture; }
	static CustomTextureCapabilities getCustomTextureCapabilities();

private:
	id<MTLTexture> image = nil;
};

class MetalTextureCache final : public BaseTextureCache<MetalTexture>
{
public:
	MetalTextureCache() { MetalTexture::SetDirectXColorOrder(false); }
	MetalTexture *GetTexture(TSP tsp, TCW tcw, int area);
	void Cleanup() { CollectCleanup(); }
};
