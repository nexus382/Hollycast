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
static void checkMetalFX(MetalContext *ctx)
{
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
	int padX, padY;
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
