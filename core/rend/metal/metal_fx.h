// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "metal_common.h"
#include <array>
#include <simd/simd.h>

@protocol MTLFXSpatialScaler, MTLFXTemporalScaler;

class MetalSpatialUpscaler
{
public:
	id<MTLTexture> encode(id<MTLCommandBuffer> cmd, id<MTLTexture> color, NSUInteger width, NSUInteger height);

private:
	std::array<NSUInteger, 5> configuration {};
	std::array<NSUInteger, 5> warnedKey {};
	id<MTLFXSpatialScaler> scaler = nil;
	id<MTLTexture> output = nil, inputCopy = nil;
};

class MetalTemporalUpscaler
{
public:
	bool prepare(id<MTLTexture> color, NSUInteger width, NSUInteger height);
	simd_float2 jitter() const;
	id<MTLTexture> encode(id<MTLCommandBuffer> cmd, id<MTLTexture> color, id<MTLTexture> depth);
#if defined(METAL_FX_TEST_READBACK)
	id<MTLTexture> motionTextureForTesting() const { return motion; }
#endif

private:
	std::array<NSUInteger, 5> configuration {};
	std::array<NSUInteger, 5> warnedKey {};
	id<MTLFXTemporalScaler> scaler = nil;
	id<MTLComputePipelineState> motionPipeline = nil;
	id<MTLTexture> previous = nil, motion = nil, sceneDepth = nil, output = nil;
	unsigned frame = 0;
	bool hasHistory = false;
	simd_float2 lastJitter {};
};
