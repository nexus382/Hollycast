// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "metal_fx.h"
#include "metal_api.h"
#include "types.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#endif

bool metalSupportsSpatialFX()
{
#if __has_include(<MetalFX/MetalFX.h>)
	if (@available(macOS 13, *)) {
		static const bool supported = [] {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			return device && [MTLFXSpatialScalerDescriptor supportsDevice:device];
		}();
		return supported;
	}
#endif
	return false;
}

bool metalSupportsTemporalFX()
{
#if __has_include(<MetalFX/MetalFX.h>)
	if (@available(macOS 13, *)) {
		static const bool supported = [] {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			return device && [MTLFXTemporalScalerDescriptor supportsDevice:device];
		}();
		return supported;
	}
#endif
	return false;
}

#if __has_include(<MetalFX/MetalFX.h>)
static id<MTLTexture> makeTexture(id<MTLDevice> device, NSUInteger width, NSUInteger height,
		MTLPixelFormat format, MTLTextureUsage usage)
{
	auto *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
	desc.usage = usage;
	desc.storageMode = MTLStorageModePrivate;
	return [device newTextureWithDescriptor:desc];
}

static void copyColor(id<MTLCommandBuffer> cmd, id<MTLTexture> from, id<MTLTexture> to)
{
	auto blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:from sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(from.width, from.height, 1) toTexture:to destinationSlice:0 destinationLevel:0
		destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
}

static bool needsUpscaling(id<MTLTexture> color, NSUInteger w, NSUInteger h)
{
	return color && w >= color.width && h >= color.height && (w > color.width || h > color.height);
}
#endif

id<MTLTexture> MetalSpatialUpscaler::encode(id<MTLCommandBuffer> cmd, id<MTLTexture> color, NSUInteger w, NSUInteger h)
{
#if __has_include(<MetalFX/MetalFX.h>)
	if (@available(macOS 13, *)) {
		if (!needsUpscaling(color, w, h) || !metalSupportsSpatialFX())
			return color;
		std::array<NSUInteger, 5> key {color.width, color.height, w, h, color.pixelFormat};
		if (configuration != key) {
			configuration = {};
			MTLFXSpatialScalerDescriptor *desc = [MTLFXSpatialScalerDescriptor new];
			desc.inputWidth = color.width;
			desc.inputHeight = color.height;
			desc.outputWidth = w;
			desc.outputHeight = h;
			desc.colorTextureFormat = color.pixelFormat;
			desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
			desc.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
			scaler = [desc newSpatialScalerWithDevice:color.device];
			inputCopy = nil;
			output = scaler ? makeTexture(color.device, w, h, desc.outputTextureFormat,
					scaler.outputTextureUsage | MTLTextureUsageShaderRead) : nil;
			// Scaler creation failure is deterministic, but texture allocation can fail
			// transiently; leave the key uncommitted in that case to allow a retry.
			if (output || !scaler)
				configuration = key;
			if (!output && warnedKey != key) {
				warnedKey = key;
				WARN_LOG(RENDERER, "MetalFX spatial scaler unavailable for %lux%lu -> %lux%lu", color.width, color.height, w, h);
			}
		}
		if (output) {
			if ((color.usage & scaler.colorTextureUsage) != scaler.colorTextureUsage) {
				if (!inputCopy)
					inputCopy = makeTexture(color.device, color.width, color.height, color.pixelFormat, scaler.colorTextureUsage);
				if (!inputCopy)
					return color;
				copyColor(cmd, color, inputCopy);
				color = inputCopy;
			}
			scaler.colorTexture = color;
			scaler.outputTexture = output;
			scaler.inputContentWidth = color.width;
			scaler.inputContentHeight = color.height;
			[scaler encodeToCommandBuffer:cmd];
			return output;
		}
	}
#endif
	return color;
}

#if __has_include(<MetalFX/MetalFX.h>)
static id<MTLComputePipelineState> createMotionPipeline(id<MTLDevice> device)
{
	// PVR vertices arrive in screen space without stable object IDs. Estimate
	// current-to-previous motion per 4x4 block, comparing at unjittered positions.
	NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void prepareMotion(texture2d<float> current [[texture(0)]], texture2d<float> previous [[texture(1)]],
		depth2d<float, access::read> depth [[texture(2)]], texture2d<float, access::write> motion [[texture(3)]],
		texture2d<float, access::write> depthOut [[texture(4)]], constant float2& jitterDelta [[buffer(0)]],
		uint2 block [[thread_position_in_grid]]) {
	const uint2 base = block * 4;
	const uint2 size(current.get_width(), current.get_height());
	if (any(base >= size)) return;
	constexpr sampler s(coord::pixel, address::clamp_to_edge, filter::linear);
	float2 pos[4];
	float3 reference[4];
	for (uint i = 0; i < 4; ++i) {
		pos[i] = float2(base + uint2((i & 1) * 2, (i >> 1) * 2)) + 1.0;
		reference[i] = current.sample(s, pos[i]).rgb;
	}
	float2 best(0);
	float bestCost = INFINITY;
	for (int step = 4; step >= 1; step /= 2) {
		float2 center = best;
		for (int y = -1; y <= 1; ++y)
			for (int x = -1; x <= 1; ++x) {
				float2 v = center + float2(x, y) * float(step);
				float cost = 0.002 * dot(v, v);
				for (uint i = 0; i < 4; ++i) {
					float3 d = abs(reference[i] - previous.sample(s, pos[i] + v + jitterDelta).rgb);
					cost += dot(d, float3(0.299, 0.587, 0.114));
				}
				if (cost < bestCost) { bestCost = cost; best = v; }
			}
		if (bestCost < 0.001) break;
	}
	for (uint y = 0; y < 4; ++y)
		for (uint x = 0; x < 4; ++x) {
			uint2 p = base + uint2(x, y);
			if (all(p < size)) {
				motion.write(float4(best * MOTION_VECTOR_TEST_SCALE, 0, 0), p);
				depthOut.write(float4(depth.read(p)), p);
			}
		}
})";
#if defined(METAL_FX_TEST_READBACK)
	// The smoke's negative control deliberately corrupts the actual generated
	// vectors before both readback and MetalFX consumption.
	NSString *testScale = std::getenv("METAL_FX_TEST_REVERSE_MOTION") ? @"-1.0" : @"1.0";
	source = [source stringByReplacingOccurrencesOfString:@"MOTION_VECTOR_TEST_SCALE" withString:testScale];
#else
	source = [source stringByReplacingOccurrencesOfString:@"MOTION_VECTOR_TEST_SCALE" withString:@"1.0"];
#endif
	NSError *error = nil;
	auto lib = [device newLibraryWithSource:source options:nil error:&error];
	auto pipeline = lib ? [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"prepareMotion"] error:&error] : nil;
	if (!pipeline)
		WARN_LOG(RENDERER, "%s", metalErrorMessage("MetalFX motion estimation", error).c_str());
	return pipeline;
}
#endif

bool MetalTemporalUpscaler::prepare(id<MTLTexture> color, NSUInteger w, NSUInteger h)
{
#if __has_include(<MetalFX/MetalFX.h>)
	if (@available(macOS 13, *)) {
		if (!needsUpscaling(color, w, h) || !metalSupportsTemporalFX()) {
			hasHistory = false;
			return false;
		}
		// Preserve the input aspect ratio for reconstruction. The presentation
		// quad handles any remaining display stretch or rotation.
		double scale = std::min({2.0, double(w) / color.width, double(h) / color.height});
		w = std::lround(color.width * scale);
		h = std::lround(color.height * scale);
		if (w == color.width && h == color.height) {
			hasHistory = false;
			return false;
		}
		std::array<NSUInteger, 5> key {color.width, color.height, w, h, color.pixelFormat};
		if (configuration == key)
			return output != nil;
		configuration = {};
		frame = 0;
		hasHistory = false;
		MTLFXTemporalScalerDescriptor *desc = [MTLFXTemporalScalerDescriptor new];
		desc.inputWidth = color.width;
		desc.inputHeight = color.height;
		desc.outputWidth = w;
		desc.outputHeight = h;
		desc.colorTextureFormat = color.pixelFormat;
		desc.depthTextureFormat = MTLPixelFormatR32Float;
		desc.motionTextureFormat = MTLPixelFormatRG16Float;
		desc.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
		desc.autoExposureEnabled = YES;
		scaler = [desc newTemporalScalerWithDevice:color.device];
		if (!motionPipeline)
			motionPipeline = createMotionPipeline(color.device);
		output = nil;
		if (scaler && motionPipeline && (color.usage & scaler.colorTextureUsage) == scaler.colorTextureUsage) {
			previous = makeTexture(color.device, color.width, color.height, color.pixelFormat, MTLTextureUsageShaderRead);
			sceneDepth = makeTexture(color.device, color.width, color.height, desc.depthTextureFormat,
					scaler.depthTextureUsage | MTLTextureUsageShaderWrite);
			motion = makeTexture(color.device, color.width, color.height, desc.motionTextureFormat,
					scaler.motionTextureUsage | MTLTextureUsageShaderWrite);
			if (previous && sceneDepth && motion)
				output = makeTexture(color.device, w, h, desc.outputTextureFormat, scaler.outputTextureUsage | MTLTextureUsageShaderRead);
			if (output) {
				configuration = key;
			} else {
				// Texture allocation can fail transiently, so release partial
				// allocations and leave the key uncommitted to allow a retry.
				previous = nil;
				sceneDepth = nil;
				motion = nil;
			}
		} else {
			configuration = key;
		}
		if (!output && warnedKey != key) {
			warnedKey = key;
			WARN_LOG(RENDERER, "MetalFX temporal scaler unavailable for %lux%lu -> %lux%lu", color.width, color.height, w, h);
		}
		return output != nil;
	}
#endif
	return false;
}

simd_float2 MetalTemporalUpscaler::jitter() const
{
	// A repeating 32-sample Halton(2,3) sequence in input pixels.
	auto halton = [](unsigned index, unsigned base) {
		float value = 0.f, weight = 1.f;
		while (index) {
			weight /= base;
			value += float(index % base) * weight;
			index /= base;
		}
		return value - 0.5f;
	};
	unsigned index = frame % 32 + 1;
	return {halton(index, 2), halton(index, 3)};
}

id<MTLTexture> MetalTemporalUpscaler::encode(id<MTLCommandBuffer> cmd, id<MTLTexture> color, id<MTLTexture> depth)
{
#if __has_include(<MetalFX/MetalFX.h>)
	if (@available(macOS 13, *)) {
		if (!cmd || !output || !depth || depth.width != color.width || depth.height != color.height)
			return nil;
		auto enc = [cmd computeCommandEncoder];
		auto offset = jitter();
		simd_float2 delta = hasHistory ? lastJitter - offset : simd_make_float2(0, 0);
		[enc setComputePipelineState:motionPipeline];
		[enc setTexture:color atIndex:0];
		[enc setTexture:hasHistory ? previous : color atIndex:1];
		[enc setTexture:depth atIndex:2];
		[enc setTexture:motion atIndex:3];
		[enc setTexture:sceneDepth atIndex:4];
		[enc setBytes:&delta length:sizeof(delta) atIndex:0];
		[enc dispatchThreadgroups:MTLSizeMake((color.width + 31) / 32, (color.height + 31) / 32, 1)
			threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
		[enc endEncoding];
		copyColor(cmd, color, previous);
		scaler.reset = !hasHistory;
		scaler.depthReversed = YES;
		scaler.motionVectorScaleX = scaler.motionVectorScaleY = 1.f;
		scaler.jitterOffsetX = offset.x;
		scaler.jitterOffsetY = offset.y;
		scaler.colorTexture = color;
		scaler.depthTexture = sceneDepth;
		scaler.motionTexture = motion;
		scaler.outputTexture = output;
		scaler.inputContentWidth = color.width;
		scaler.inputContentHeight = color.height;
		[scaler encodeToCommandBuffer:cmd];
		lastJitter = offset;
		++frame;
		hasHistory = true;
		return output;
	}
#endif
	return nil;
}
