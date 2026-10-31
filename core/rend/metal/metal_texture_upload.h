// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "metal_common.h"
#include "rend/custom_texture/CustomTextureTypes.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Shared by the renderer and the standalone Metal readback test.
namespace metal_texture_upload {
enum class PixelKind { Rgba8, Index8, Rgb565, Rgba5551, Rgba4444 };
inline unsigned levelCount(unsigned width, unsigned height) {
    unsigned count = 1;
    while (width > 1 || height > 1) { width = std::max(1u, width / 2); height = std::max(1u, height / 2); ++count; }
    return count;
}
inline id<MTLTexture> makeTexture(id<MTLDevice> device, unsigned width, unsigned height, MTLPixelFormat format, unsigned levels)
{
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
			width:width height:height mipmapped:levels > 1];
	descriptor.mipmapLevelCount = levels;
	descriptor.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
	if (@available(macOS 10.15, *))
		descriptor.storageMode = device.hasUnifiedMemory ? MTLStorageModeShared : MTLStorageModeManaged;
	else
		descriptor.storageMode = MTLStorageModeManaged;
#else
	descriptor.storageMode = MTLStorageModeShared;
#endif
	return [device newTextureWithDescriptor:descriptor];
}

inline void generateMipmaps(id<MTLCommandQueue> queue, id<MTLTexture> texture)
{
	if (texture.mipmapLevelCount <= 1)
		return;
	id<MTLCommandBuffer> commands = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
	[blit generateMipmapsForTexture:texture];
	[blit endEncoding];
	[commands commit];
	[commands waitUntilCompleted];
}
inline id<MTLTexture> decoded(id<MTLDevice> device, id<MTLCommandQueue> queue,
        int width, int height, const uint8_t *data, PixelKind kind, bool mipmapped, bool mipmapsIncluded)
{
	if (!data || width <= 0 || height <= 0)
		return nil;
	const bool indexed = kind == PixelKind::Index8;
	const bool packed16 = !indexed && kind != PixelKind::Rgba8;
	const unsigned bytesPerPixel = indexed ? 1 : packed16 ? 2 : 4;
	const unsigned levels = mipmapped ? levelCount(width, height) : 1;
	id<MTLTexture> replacement = makeTexture(device, width, height,
			indexed ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm, levels);
	if (!replacement)
		return nil;

	for (unsigned level = 0; level < (mipmapsIncluded ? levels : 1); ++level)
	{
		const unsigned w = std::max(1, width >> level);
		const unsigned h = std::max(1, height >> level);
		// PixelBuffer stores square Dreamcast mip levels smallest first.
		size_t offset = 0;
		if (mipmapsIncluded)
			for (unsigned smaller = level + 1; smaller < levels; ++smaller)
				offset += static_cast<size_t>(std::max(1, width >> smaller))
						* std::max(1, height >> smaller) * bytesPerPixel;
		const uint8_t *pixels = data + offset;
		std::vector<uint8_t> expanded;
		if (packed16)
		{
			expanded.resize(static_cast<size_t>(w) * h * 4);
			for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
			{
				uint16_t value;
				std::memcpy(&value, pixels + i * 2, sizeof(value));
				const auto expand5 = [](unsigned v) { return (v << 3) | (v >> 2); };
				const auto expand6 = [](unsigned v) { return (v << 2) | (v >> 4); };
				const auto expand4 = [](unsigned v) { return (v << 4) | v; };
				if (kind == PixelKind::Rgb565) {
					expanded[i*4] = expand5((value >> 11) & 31);
					expanded[i*4+1] = expand6((value >> 5) & 63);
					expanded[i*4+2] = expand5(value & 31);
					expanded[i*4+3] = 255;
				} else if (kind == PixelKind::Rgba5551) {
					expanded[i*4] = expand5((value >> 11) & 31);
					expanded[i*4+1] = expand5((value >> 6) & 31);
					expanded[i*4+2] = expand5((value >> 1) & 31);
					expanded[i*4+3] = (value & 1) ? 255 : 0;
				} else {
					expanded[i*4] = expand4((value >> 12) & 15);
					expanded[i*4+1] = expand4((value >> 8) & 15);
					expanded[i*4+2] = expand4((value >> 4) & 15);
					expanded[i*4+3] = expand4(value & 15);
				}
			}
			pixels = expanded.data();
		}
		[replacement replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level
				withBytes:pixels bytesPerRow:w * (packed16 ? 4 : bytesPerPixel)];
	}
	if (mipmapped && !mipmapsIncluded)
		generateMipmaps(queue, replacement);
	return replacement;
}
inline id<MTLTexture> custom(id<MTLDevice> device, id<MTLCommandQueue> queue,
        const PreparedCustomTexture& prepared, bool mipmapped)
{
	if (prepared.nativeFormat != NativeTextureFormat::Rgba8Unorm)
		return nil;
	const bool generate = mipmapped && prepared.generateMipmaps;
	const unsigned levels = generate ? levelCount(prepared.width, prepared.height)
			: static_cast<unsigned>(prepared.levels.size());
	id<MTLTexture> replacement = makeTexture(device, prepared.width, prepared.height,
			MTLPixelFormatRGBA8Unorm, levels);
	if (!replacement)
		return nil;
	for (size_t i = 0; i < prepared.levels.size(); ++i)
	{
		const auto& level = prepared.levels[i];
		[replacement replaceRegion:MTLRegionMake2D(0, 0, level.width, level.height)
				mipmapLevel:i withBytes:prepared.bytes.data() + level.byteOffset
				bytesPerRow:level.rowPitchBytes];
	}
	if (generate)
		generateMipmaps(queue, replacement);
	return replacement;
}
}
