// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the actual context composition pass. The video-routing publisher is
// a GPU readback seam, not a Syphon implementation; no app configuration is used.
// Build with core/rend/metal/metal_context.mm and core/rend/transform_matrix.cpp,
// USE_SDL, VIDEO_ROUTING, TARGET_MAC, ARC and linker dead stripping enabled.
#define METAL_CONTEXT_REAL_TRANSFORMS
#define main context_basic_smoke_main
#include "context_smoke.mm"
#undef main
#include "rend/transform_matrix.h"
#include "rend/metal/metal_fx.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace config
{
int loadInt(const std::string &, const std::string &, int fallback) { return fallback; }
void saveInt(const std::string &, const std::string &, int) {}
Option<bool, false> VideoRouting("routing", true);
Option<bool, false> VideoRoutingScale("routingScale", false);
Option<int, false> VideoRoutingVRes("routingHeight", 60);
Option<bool> StretchToFill("stretch", false);
Option<bool> EmulateFramebuffer("fb", true);
Option<bool> Widescreen("wide", false);
Option<bool> SuperWidescreen("superWide", false);
Option<int> RenderResolution("resolution", 480);
Option<int> ScreenStretching("screenStretch", 100);
} // namespace config
u8 pvr_regs[pvr_RegSize]{};
static id<MTLBuffer> captured;
static NSUInteger captureWidth, captureHeight, captureStride;
void os_VideoRoutingTermMetal() {}
void os_VideoRoutingPublishMetalTexture(id<MTLDevice> device, id<MTLTexture> texture, id<MTLCommandBuffer> cmd)
{
	captureWidth = texture.width;
	captureHeight = texture.height;
	captureStride = (captureWidth * 4 + 255) & ~(NSUInteger)255;
	captured = [device newBufferWithLength:captureStride * captureHeight options:MTLResourceStorageModeShared];
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:texture
					 sourceSlice:0
					 sourceLevel:0
					sourceOrigin:MTLOriginMake(0, 0, 0)
					  sourceSize:MTLSizeMake(captureWidth, captureHeight, 1)
						toBuffer:captured
			   destinationOffset:0
		  destinationBytesPerRow:captureStride
		destinationBytesPerImage:captureStride * captureHeight];
	[blit endEncoding];
}
struct RGB
{
	int r, g, b;
};
static RGB pixel(int x, int y)
{
	const u8 *p = (const u8 *)captured.contents + y * captureStride + x * 4;
	return {p[2], p[1], p[0]};
}
static bool nearColor(RGB actual, RGB expected, int tolerance = 8)
{
	return std::abs(actual.r - expected.r) <= tolerance && std::abs(actual.g - expected.g) <= tolerance &&
		   std::abs(actual.b - expected.b) <= tolerance;
}

struct CapturedFrame
{
	NSUInteger width, height;
	std::vector<RGB> pixels;
};

static CapturedFrame capturePresentedFrame(MetalContext *ctx, id<MTLTexture> color, id<MTLTexture> output,
		int viewportW, int viewportH)
{
	ctx->PresentFrame(color, float(color.width) / color.height, output, viewportW, viewportH);
	ctx->PresentLastFrame();
	ctx->WaitIdle();
	CapturedFrame frame {captureWidth, captureHeight, {}};
	frame.pixels.reserve(frame.width * frame.height);
	for (NSUInteger y = 0; y < frame.height; ++y)
		for (NSUInteger x = 0; x < frame.width; ++x)
			frame.pixels.push_back(pixel((int)x, (int)y));
	return frame;
}

static simd_float2 magentaCentroid(const CapturedFrame &frame)
{
	double xSum = 0, ySum = 0;
	NSUInteger count = 0;
	for (NSUInteger y = 0; y < frame.height; ++y)
		for (NSUInteger x = 0; x < frame.width; ++x) {
			const RGB p = frame.pixels[y * frame.width + x];
			if (p.r > 150 && p.g < 110 && p.b > 150) {
				xSum += x;
				ySum += y;
				++count;
			}
		}
	require(count >= 4, "composed temporal frame contains the fixture marker");
	return simd_make_float2(float(xSum / count), float(ySum / count));
}

static id<MTLComputePipelineState> makeFixturePipeline(id<MTLDevice> device)
{
	NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void moveFixture(texture2d<float, access::sample> source [[texture(0)]],
		texture2d<float, access::write> destination [[texture(1)]], constant float2& translation [[buffer(0)]],
		uint2 p [[thread_position_in_grid]]) {
	if (p.x >= destination.get_width() || p.y >= destination.get_height()) return;
	constexpr sampler s(coord::pixel, address::clamp_to_edge, filter::linear);
	destination.write(source.sample(s, float2(p) + 0.5 - translation), p);
})";
	NSError *error = nil;
	auto library = [device newLibraryWithSource:source options:nil error:&error];
	auto pipeline = library ? [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"moveFixture"] error:&error] : nil;
	require(pipeline != nil, error.localizedDescription.UTF8String ?: "Metal motion fixture pipeline");
	return pipeline;
}

static id<MTLTexture> makeMotionReference(id<MTLDevice> device, NSUInteger w, NSUInteger h)
{
	auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																width:w height:h mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = metalCpuTextureStorageMode(device);
	auto texture = [device newTextureWithDescriptor:desc];
	std::vector<u8> pixels(w * h * 4);
	for (NSUInteger y = 0; y < h; ++y)
		for (NSUInteger x = 0; x < w; ++x) {
			u8 *p = pixels.data() + (y * w + x) * 4;
			p[0] = p[1] = p[2] = 24;
			p[3] = 255;
			if (x >= 16 && x < 48 && y >= 16 && y < 40) {
				u32 hash = u32(x) * 0x9e3779b9u ^ u32(y) * 0x85ebca6bu;
				hash ^= hash >> 16;
				hash *= 0x7feb352du;
				hash ^= hash >> 15;
				hash *= 0x846ca68bu;
				hash ^= hash >> 16;
				p[0] = 40 + (hash & 0xbf);
				p[1] = 30 + ((hash >> 8) & 0x7f);
				p[2] = 45 + ((hash >> 16) & 0x9f);
				// A distinctive, off-center marker makes final presentation placement measurable.
				if (x >= 28 && x < 34 && y >= 31 && y < 37) {
					p[0] = 180 + (hash & 0x4f);
					p[1] = 15 + ((hash >> 14) & 0x1f);
					p[2] = 180 + ((hash >> 7) & 0x4f);
				}
			}
		}
	[texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pixels.data() bytesPerRow:w * 4];
	return texture;
}

static void encodeMotionFixture(id<MTLCommandBuffer> cmd, id<MTLComputePipelineState> pipeline,
		id<MTLTexture> reference, id<MTLTexture> fixture, id<MTLTexture> destination, simd_float2 translation)
{
	auto enc = [cmd computeCommandEncoder];
	[enc setComputePipelineState:pipeline];
	[enc setTexture:reference atIndex:0];
	[enc setTexture:fixture atIndex:1];
	[enc setBytes:&translation length:sizeof(translation) atIndex:0];
	[enc dispatchThreadgroups:MTLSizeMake((fixture.width + 7) / 8, (fixture.height + 7) / 8, 1)
		threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
	[enc endEncoding];
	auto blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:fixture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(fixture.width, fixture.height, 1) toTexture:destination destinationSlice:0
		destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	[blit endEncoding];
}

struct MotionReadback
{
	id<MTLBuffer> buffer;
	NSUInteger stride;
};

static MotionReadback scheduleMotionReadback(id<MTLDevice> device, id<MTLCommandBuffer> cmd, id<MTLTexture> texture)
{
	NSUInteger stride = (texture.width * 4 + 255) & ~(NSUInteger)255;
	auto buffer = [device newBufferWithLength:stride * texture.height options:MTLResourceStorageModeShared];
	auto blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(texture.width, texture.height, 1) toBuffer:buffer destinationOffset:0
		destinationBytesPerRow:stride destinationBytesPerImage:stride * texture.height];
	[blit endEncoding];
	return {buffer, stride};
}

static float halfToFloat(u16 bits)
{
	const bool negative = (bits & 0x8000) != 0;
	const unsigned exponent = (bits >> 10) & 0x1f;
	const unsigned mantissa = bits & 0x3ff;
	float value;
	if (exponent == 0)
		value = std::ldexp(float(mantissa), -24);
	else if (exponent == 0x1f)
		value = mantissa ? NAN : INFINITY;
	else
		value = std::ldexp(float(1024 + mantissa), int(exponent) - 25);
	return negative ? -value : value;
}

static simd_float2 motionAt(const MotionReadback &readback, NSUInteger x, NSUInteger y)
{
	const u8 *p = (const u8 *)readback.buffer.contents + y * readback.stride + x * 4;
	u16 xy[2];
	std::memcpy(xy, p, sizeof(xy));
	return simd_make_float2(halfToFloat(xy[0]), halfToFloat(xy[1]));
}

static void checkKnownMetalFXMotion(MetalContext *ctx, id<MTLTexture> color, id<MTLTexture> depth,
		int viewportW, int viewportH)
{
	if (!metalSupportsTemporalFX()) {
		std::puts("SKIP MetalFX known-motion: temporal scaling unsupported on this device");
		return;
	}
	const NSUInteger w = color.width, h = color.height;
	constexpr int motionX = 4, motionY = -3;
	const simd_float2 expected = simd_make_float2(-motionX, -motionY);
	auto device = ctx->GetDevice();
	auto queue = ctx->GetQueue();
	auto reference = makeMotionReference(device, w, h);
	auto fixturePipeline = makeFixturePipeline(device);
	auto fixtureDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																width:w height:h mipmapped:NO];
	fixtureDesc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	fixtureDesc.storageMode = MTLStorageModePrivate;
	auto fixture = [device newTextureWithDescriptor:fixtureDesc];
	require(reference && fixture, "allocate known-motion fixture textures");
	MetalTemporalUpscaler knownMotion;
	require(knownMotion.prepare(color, w * 2, h * 2), "prepare fresh known-motion temporal history");
	CapturedFrame composed[2];
	MotionReadback reads[2];
	for (int frame = 0; frame < 2; ++frame) {
		auto cmd = [queue commandBuffer];
		simd_float2 jitter = knownMotion.jitter();
		simd_float2 sceneMotion = frame == 0 ? simd_make_float2(0, 0) : simd_make_float2(motionX, motionY);
		encodeMotionFixture(cmd, fixturePipeline, reference, fixture, color, sceneMotion + jitter);
		auto output = knownMotion.encode(cmd, color, depth);
		require(output != nil, "temporal fixture produced a composed output");
		reads[frame] = scheduleMotionReadback(device, cmd, knownMotion.motionTextureForTesting());
		[cmd commit];
		[cmd waitUntilCompleted];
		require(cmd.status == MTLCommandBufferStatusCompleted, cmd.error.localizedDescription.UTF8String ?: "known-motion Metal pass");
		composed[frame] = capturePresentedFrame(ctx, color, output, viewportW, viewportH);
	}
	float motionSumX = 0, motionSumY = 0;
	unsigned motionSamples = 0;
	bool vectorsMatch = true;
	const simd_uint2 movingBlocks[] = {simd_make_uint2(32, 28), simd_make_uint2(36, 28)};
	for (simd_uint2 block : movingBlocks) {
		simd_float2 first = motionAt(reads[0], block.x + 2, block.y + 2);
		require(std::abs(first.x) < 0.25f && std::abs(first.y) < 0.25f,
				"fresh temporal frame has zero motion");
		simd_float2 actual = motionAt(reads[1], block.x + 2, block.y + 2);
		if (std::abs(actual.x - expected.x) > 0.75f || std::abs(actual.y - expected.y) > 0.75f)
			std::fprintf(stderr, "known-motion block (%u,%u): measured (%+.2f,%+.2f), expected (%+.2f,%+.2f)\n",
				block.x, block.y, actual.x, actual.y, expected.x, expected.y);
		vectorsMatch &= std::abs(actual.x - expected.x) <= 0.75f && std::abs(actual.y - expected.y) <= 0.75f;
		motionSumX += actual.x;
		motionSumY += actual.y;
		++motionSamples;
	}
	require(vectorsMatch, "interior 4x4 block tracks current-to-previous motion in input pixels");
	for (simd_uint2 p : {simd_make_uint2(8, 8), simd_make_uint2(64, 48)}) {
		simd_float2 background = motionAt(reads[1], p.x, p.y);
		require(std::abs(background.x) < 0.25f && std::abs(background.y) < 0.25f,
				"static background has near-zero motion");
	}
	simd_float2 firstMarker = magentaCentroid(composed[0]);
	simd_float2 secondMarker = magentaCentroid(composed[1]);
	const float composedDx = secondMarker.x - firstMarker.x;
	const float composedDy = secondMarker.y - firstMarker.y;
	const float expectedDx = motionX * float(composed[0].width) / w;
	const float expectedDy = motionY * float(composed[0].height) / h;
	const float toleranceX = std::max(4.f, expectedDx * 0.2f);
	const float toleranceY = std::max(4.f, -expectedDy * 0.2f);
	if (std::abs(composedDx - expectedDx) > toleranceX || std::abs(composedDy - expectedDy) > toleranceY)
		std::fprintf(stderr, "known-motion composed delta (%+.1f,%+.1f), expected (%+.1f,%+.1f), tolerance (+/-%0.1f,+/-%0.1f)\n",
			composedDx, composedDy, expectedDx, expectedDy, toleranceX, toleranceY);
	require(std::abs(composedDx - expectedDx) <= toleranceX && std::abs(composedDy - expectedDy) <= toleranceY,
			"composed temporal presentation places moved marker at expected output position");
	std::printf("MetalFX known motion: scene moved (+%d, %d) input px; mean vector (%+.2f, %+.2f), expected (%+.2f, %+.2f), tolerance 0.75 input px; composed delta (%+.1f, %+.1f), tolerance (+/-%0.1f, +/-%0.1f) output px\n",
		motionX, motionY, motionSumX / motionSamples, motionSumY / motionSamples, expected.x, expected.y,
		composedDx, composedDy, toleranceX, toleranceY);
	std::puts("MetalFX known-motion checks passed");
}

static void checkMetalFX(MetalContext *ctx)
{
	testInset = 0;
	ctx->resize();
	if (!metalSupportsSpatialFX()) {
		std::puts("SKIP MetalFX: spatial scaling unsupported on this device");
		return;
	}
	auto device = ctx->GetDevice();
	auto queue = ctx->GetQueue();
	const NSUInteger w = 80, h = 60;
	auto desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
	desc.storageMode = MTLStorageModePrivate;
	auto color = [device newTextureWithDescriptor:desc];
	desc.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
	auto depth = [device newTextureWithDescriptor:desc];
	auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = color;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.depthAttachment.texture = depth;
	pass.depthAttachment.loadAction = MTLLoadActionClear;
	pass.depthAttachment.storeAction = MTLStoreActionStore;
	pass.depthAttachment.clearDepth = 0.5;

	MetalSpatialUpscaler spatial;
	MetalTemporalUpscaler temporal;
	const bool supportsTemporal = metalSupportsTemporalFX();
	int viewportW = settings.display.width, viewportH = std::max(1, settings.display.height - getScaledTopInset());
	int padX = 0, padY = 0;
	getWindowboxDimensions(viewportW, viewportH, float(w) / h, padX, padY, false);
	viewportW = std::max(1, viewportW - 2 * padX);
	viewportH = std::max(1, viewportH - 2 * padY);
	id<MTLTexture> temporalResult = nil;
	for (int frame = 0; frame < 3; ++frame) {
		auto cmd = [queue commandBuffer];
		pass.colorAttachments[0].clearColor = MTLClearColorMake(1, frame * .25, 0, 1);
		[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];
		require(spatial.encode(cmd, color, w, h) == color, "native-size frame bypasses spatial scaling");
		auto output = spatial.encode(cmd, color, w * 2, h * 2);
		require(output != color && output.width == w * 2 && output.height == h * 2, "spatial upscale");
		if (supportsTemporal) {
			require(temporal.prepare(color, w * 2, h * 2), "prepare temporal scaler");
			auto jitter = temporal.jitter();
			output = temporal.encode(cmd, color, depth);
			require(output && output.width == w * 2 && output.height == h * 2, "temporal upscale");
			require(jitter.x != temporal.jitter().x, "temporal sample advances on new input");
			temporalResult = output;
		}
		[cmd commit];
		[cmd waitUntilCompleted];
		require(cmd.status == MTLCommandBufferStatusCompleted, cmd.error.localizedDescription.UTF8String ?: "MetalFX GPU pass");
		config::MetalFXUpscaling = true;
		config::MetalFXTemporal = supportsTemporal;
		ctx->PresentFrame(color, float(w) / h, supportsTemporal ? output : nil, viewportW, viewportH);
		ctx->PresentLastFrame();
		ctx->WaitIdle();
		RGB center = pixel((int)captureWidth / 2, (int)captureHeight / 2);
		require(center.r > 190 && center.b < 25, "MetalFX game image reaches presentation");
		ctx->PresentLastFrame();
		ctx->WaitIdle();
		require(nearColor(pixel((int)captureWidth / 2, (int)captureHeight / 2), center, 1), "retained MetalFX frame");
	}
	// A framebuffer upload has no scene depth. Requesting temporal must
	// produce the same spatial result as explicitly selecting spatial.
	desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
	desc.storageMode = metalCpuTextureStorageMode(device);
	desc.usage = MTLTextureUsageShaderRead;
	auto upload = [device newTextureWithDescriptor:desc];
	std::vector<u32> pattern(w * h);
	for (NSUInteger y = 0; y < h; ++y)
		for (NSUInteger x = 0; x < w; ++x)
			pattern[y * w + x] = (x + 2 * y) % 7 < 3 ? 0xffffffff : 0xff000000;
	[upload replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:pattern.data() bytesPerRow:w * 4];
	auto captureUpload = [&](id<MTLTexture> temporalOutput = nil, int temporalW = 0, int temporalH = 0) {
		ctx->PresentFrame(upload, float(w) / h, temporalOutput, temporalW, temporalH);
		ctx->PresentLastFrame();
		ctx->WaitIdle();
		std::vector<u8> pixels;
		const auto *bytes = (const u8 *)captured.contents;
		for (NSUInteger y = 0; y < captureHeight; ++y)
			pixels.insert(pixels.end(), bytes + y * captureStride, bytes + y * captureStride + captureWidth * 4);
		return pixels;
	};
	config::MetalFXUpscaling = false;
	auto unfiltered = captureUpload();
	config::MetalFXUpscaling = true;
	config::MetalFXTemporal = false;
	auto spatialFrame = captureUpload();
	require(spatialFrame != unfiltered, "spatial setting changes presented pixels");
	config::MetalFXTemporal = true;
	require(captureUpload() == spatialFrame, "framebuffer video uses spatial fallback");
	if (supportsTemporal) {
		captureUpload(temporalResult, viewportW, viewportH);
		RGB center = pixel((int)captureWidth / 2, (int)captureHeight / 2);
		require(center.r > 190 && center.b < 25, "matching viewport dimensions present the temporal output");
		require(captureUpload(temporalResult, viewportW + 2, viewportH) == spatialFrame,
				"stale temporal dimensions fall back to spatial scaling");
	}
	if (supportsTemporal)
		checkKnownMetalFXMotion(ctx, color, depth, viewportW, viewportH);
	config::MetalFXUpscaling = false;
	config::MetalFXTemporal = false;
	std::printf("MetalFX spatial%s GPU presentation passed on %s\n", supportsTemporal ? " and temporal" : "", device.name.UTF8String);
}

int main()
{
	@autoreleasepool
	{
		require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
		MetalContext::Create(nullptr, nullptr);
		MetalContext *ctx = MetalContext::Instance();
		const u8 data[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																					 width:2
																					height:2
																				 mipmapped:NO];
		d.storageMode = metalCpuTextureStorageMode(ctx->GetDevice());
		d.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> tex = [ctx->GetDevice() newTextureWithDescriptor:d];
		[tex replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:data bytesPerRow:8];
		SPG_LOAD.hcount = 857;
		SPG_LOAD.vcount = 524;
		VO_STARTX.HStart = 0xa4;
		VO_STARTY.VStart_field1 = 0x12;
		float aspect = (float)settings.display.width / settings.display.height;
		auto present = [&]
		{
			ctx->PresentFrame(tex, aspect);
			ctx->PresentLastFrame();
			ctx->WaitIdle();
			require(captured != nil, "routed frame captured");
		};
		config::VSync = false;
		present();
		int w = (int)captureWidth, h = (int)captureHeight;
		require(w > 20 && h > 20, "native routed size");
		require(nearColor(pixel(w / 4, h / 4), {255, 0, 0}) && nearColor(pixel(3 * w / 4, h / 4), {0, 255, 0}) &&
					nearColor(pixel(w / 4, 3 * h / 4), {0, 0, 255}) &&
					nearColor(pixel(3 * w / 4, 3 * h / 4), {255, 255, 255}),
				"unrotated quad orientation");
		config::Rotate90 = true;
		present();
		require(nearColor(pixel(w / 4, h / 4), {0, 255, 0}) && nearColor(pixel(3 * w / 4, h / 4), {255, 255, 255}) &&
					nearColor(pixel(w / 4, 3 * h / 4), {255, 0, 0}) &&
					nearColor(pixel(3 * w / 4, 3 * h / 4), {0, 0, 255}),
				"rotated quad orientation");
		config::Rotate90 = false;
		config::EmulateFramebuffer = false;
		config::Widescreen = true;
		aspect = getOutputFramebufferAspectRatio();
		present();
		require(nearColor(pixel(w / 2, 2), {0, 0, 0}) && nearColor(pixel(w / 4, 3 * h / 4), {0, 0, 255}),
				"widescreen aspect windowboxing");
		config::StretchToFill = true;
		aspect = getOutputFramebufferAspectRatio();
		present();
		require(nearColor(pixel(w / 4, 2), {255, 0, 0}), "stretch to fill");
		config::StretchToFill = false;
		config::Widescreen = false;
		config::EmulateFramebuffer = true;
		config::ScreenStretching = 75;
		aspect = getOutputFramebufferAspectRatio();
		present();
		require(nearColor(pixel(2, h / 2), {0, 0, 0}), "screen stretching aspect");
		config::ScreenStretching = 100;
		aspect = 1.f;
		present();
		require(nearColor(pixel(2, h / 2), {0, 0, 0}) && nearColor(pixel(w / 2, h / 4), {0, 255, 0}),
				"aspect windowboxing");
		aspect = (float)w / h;
		testInset = 12;
		present();
		require(nearColor(pixel(w / 2, 2), {0, 0, 0}) && nearColor(pixel(w / 4, 22), {255, 0, 0}),
				"viewport top inset");
		testInset = 0;
		config::IntegerScale = true;
		config::RenderResolution = 100;
		aspect = 1.f;
		present();
		require(nearColor(pixel(2, h / 2), {0, 0, 0}) && nearColor(pixel(w / 2, h / 2), {255, 255, 255}),
				"integer scaling");
		config::IntegerScale = false;
		aspect = (float)w / h;
		VO_STARTX.HStart = 0xa5;
		present();
		require(nearColor(pixel(w / 8, h / 4), {0, 0, 0}) && nearColor(pixel(3 * w / 4, h / 4), {0, 255, 0}) == false &&
					nearColor(pixel(3 * w / 4, h / 4), {255, 0, 0}),
				"horizontal video shift and clipping");
		config::ScreenStretching = 200;
		present();
		require(nearColor(pixel(3 * w / 4, h / 4), {0, 0, 0}), "screen stretching scales video shift");
		config::ScreenStretching = 100;
		VO_STARTX.HStart = 0xa4;
		config::Rotate90 = true;
		VO_STARTX.HStart = 0xa5;
		present();
		require(nearColor(pixel(w / 4, h / 4), {255, 0, 0}) && nearColor(pixel(w / 4, 3 * h / 4), {0, 0, 0}),
				"rotated horizontal video shift");
		VO_STARTX.HStart = 0xa4;
		VO_STARTY.VStart_field1 = 0x13;
		present();
		require(nearColor(pixel(w / 4, h / 4), {0, 0, 0}) && nearColor(pixel(3 * w / 4, h / 4), {0, 255, 0}),
				"rotated vertical video shift");
		VO_STARTY.VStart_field1 = 0x12;
		config::Rotate90 = false;
		config::LinearInterpolation = true;
		present();
		RGB middle = pixel(w / 2, h / 2);
		require(middle.r > 70 && middle.r < 190 && middle.g > 70 && middle.g < 190 && middle.b > 70 && middle.b < 190,
				"linear interpolation");
		config::LinearInterpolation = false;
		checkMetalFX(ctx);
		config::VideoRoutingScale = true;
		present();
		require(captureHeight == 60 && captureWidth == (NSUInteger)(60 * w / h), "scaled routing dimensions");
		require(nearColor(pixel((int)captureWidth / 4, 15), {255, 0, 0}, 12) &&
					nearColor(pixel(3 * (int)captureWidth / 4, 15), {0, 255, 0}, 12),
				"scaled routing orientation");
		ctx->WaitIdle();
		GraphicsContext::Term();
		SDL_DestroyWindow(testWindow);
		SDL_Quit();
		std::puts("Metal presentation orientation, inset, shift, filtering and scaled routing passed");
	}
}
