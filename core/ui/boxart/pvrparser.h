/*
	Copyright 2022 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include "rend/texconv.h"
#include "hw/pvr/ta_structs.h"

extern const u32 VQMipPoint[11];
extern const u32 OtherMipPoint[11];

enum PvrDataFormat {
	PvrSquareTwiddled           = 0x01,
	PvrSquareTwiddledMipmaps    = 0x02,
    PvrVQ                       = 0x03,
    PvrVQMipmaps                = 0x04,
    PvrPal4                     = 0x05,
    PvrPal4Mipmaps              = 0x06,
    PvrPal8                     = 0x07,
    PvrPal8Mipmaps              = 0x08,
    PvrRectangle                = 0x09,
    PvrStride                   = 0x0B,
    PvrRectangleTwiddled        = 0x0D,
    PvrSmallVQ                  = 0x10,
    PvrSmallVQMipmaps           = 0x11,
    PvrSquareTwiddledMipmapsAlt = 0x12,
};

static inline bool pvrParse(const u8 *data, u32 len, u32& width, u32& height, std::vector<u8>& out)
{
	if (data == nullptr || len < 16)
		return false;
	const u8 *p = data;
	const u8 *end = data + len;
	auto remaining = [&]() -> size_t { return static_cast<size_t>(end - p); };
	if (!memcmp("GBIX", p, 4))
	{
		p += 4;
		if (remaining() < sizeof(u32))
			return false;
		u32 idxSize;
		memcpy(&idxSize, p, sizeof(idxSize));
		p += sizeof(idxSize);
		if (static_cast<size_t>(idxSize) > remaining())
			return false;
		p += static_cast<size_t>(idxSize);
	}
	if (remaining() < 4)
		return false;
	if (memcmp("PVRT", p, 4))
	{
		WARN_LOG(COMMON, "Invalid PVR file: header not found");
		return false;
	}
	if (remaining() < 16) {
		WARN_LOG(COMMON, "Invalid PVR file: too small");
		return false;
	}
	p += 4;
	u32 size;
	memcpy(&size, p, sizeof(size));
	p += 4;
	PixelFormat pixelFormat = (PixelFormat)*p++;
	PvrDataFormat imgType = (PvrDataFormat)*p++;
	p += 2;
	u16 dimension;
	memcpy(&dimension, p, sizeof(dimension));
	width = dimension;
	p += 2;
	memcpy(&dimension, p, sizeof(dimension));
	height = dimension;
	p += 2;
	if (width == 0 || height == 0 || width > 1024 || height > 1024) {
		WARN_LOG(COMMON, "Invalid PVR file: wrong texture dimensions: %d x %d", width, height);
		return false;
	}
	const bool planar = imgType == PvrRectangle || imgType == PvrStride;
	if (planar ? width % 4 != 0
			: width < 2 || height < 2 || (width & (width - 1)) != 0 || (height & (height - 1)) != 0)
		return false;
	// The converters read 16-bit texels from the payload or VQ codebook.
	if (reinterpret_cast<uintptr_t>(p) % alignof(u16) != 0)
		return false;

	const u8 *vqCodebook = p;
	TexConvFP32 texConv;
	switch (pixelFormat)
	{
	case Pixel1555:
		if (imgType == PvrSquareTwiddled || imgType == PvrSquareTwiddledMipmaps
				|| imgType == PvrRectangleTwiddled || imgType == PvrSquareTwiddledMipmapsAlt)
			texConv = opengl::tex1555_TW32;
		else if (imgType == PvrVQ || imgType == PvrVQMipmaps)
			texConv = opengl::tex1555_VQ32;
		else if (imgType == PvrRectangle || imgType == PvrStride)
			texConv = opengl::tex1555_PL32;
		else
		{
			WARN_LOG(COMMON, "Unsupported 1555 image type: %d", imgType);
			return false;
		}
		break;

	case Pixel565:
		if (imgType == PvrSquareTwiddled || imgType == PvrSquareTwiddledMipmaps
				|| imgType == PvrRectangleTwiddled || imgType == PvrSquareTwiddledMipmapsAlt)
			texConv = opengl::tex565_TW32;
		else if (imgType == PvrVQ || imgType == PvrVQMipmaps)
			texConv = opengl::tex565_VQ32;
		else if (imgType == PvrRectangle || imgType == PvrStride)
			texConv = opengl::tex565_PL32;
		else
		{
			WARN_LOG(COMMON, "Unsupported 565 image type: %d", imgType);
			return false;
		}
		break;

	case Pixel4444:
		if (imgType == PvrSquareTwiddled || imgType == PvrSquareTwiddledMipmaps
				|| imgType == PvrRectangleTwiddled || imgType == PvrSquareTwiddledMipmapsAlt)
			texConv = opengl::tex4444_TW32;
		else if (imgType == PvrVQ || imgType == PvrVQMipmaps)
			texConv = opengl::tex4444_VQ32;
		else if (imgType == PvrRectangle || imgType == PvrStride)
			texConv = opengl::tex4444_PL32;
		else
		{
			WARN_LOG(COMMON, "Unsupported 4444 image type: %d", imgType);
			return false;
		}
		break;

	default:
		WARN_LOG(COMMON, "Unsupported PVR pixel type: %d", pixelFormat);
		return false;
	}
	DEBUG_LOG(COMMON, "PVR file: size %d pixelFmt %d imgType %d w %d h %d", size, pixelFormat, imgType, width, height);
	u32 texU = 3;
	while (1u << texU < width)
		texU++;
	if (texU >= 11)
		return false;
	size_t skip = 0;
	if (imgType == PvrSquareTwiddledMipmapsAlt)
		// Hardcoding pixel size. Not correct for palette texs
		skip = static_cast<size_t>(OtherMipPoint[texU]) * 2;
	else if (imgType == PvrSquareTwiddledMipmaps)
		// Hardcoding pixel size. Not correct for palette texs
		skip = static_cast<size_t>(OtherMipPoint[texU] - 2) * 2;
	else if (imgType == PvrVQMipmaps)
		skip = VQMipPoint[texU];
	else if (imgType == PvrVQ)
		skip = VQ_CODEBOOK_SIZE;
	if (skip > remaining())
		return false;
	p += skip;

	u64 expectedSize = static_cast<u64>(width) * height;
	if (imgType == PvrVQ || imgType == PvrVQMipmaps)
		expectedSize /= 4;	// 4 pixels per byte
	else
		expectedSize *= 2;	// 2 bytes per pixel
	if (expectedSize > remaining())
	{
		WARN_LOG(COMMON, "Invalid texture: expected %llu bytes, %zu remaining", static_cast<unsigned long long>(expectedSize), remaining());
		return false;
	}

	PixelBuffer<u32> pb;
	pb.init(width, height);
	pb.setVQCodebook(vqCodebook);
	texConv(&pb, p, width, height);
	out.resize(width * height * 4);
	memcpy(out.data(), pb.data(), out.size());

	return true;
}
