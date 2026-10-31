// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_context.h"
#include "metal_api.h"
#include "metal_common.h"
#include "metal_overlay.h"
#include "sdl/sdl.h"
#include "ui/imgui_driver.h"
#include "ui/gui.h"
#include "ui/performance_overlay.h"
#include "cfg/option.h"
#include "rend/transform_matrix.h"
#include "rend/osd.h"
#ifdef TEST_AUTOMATION
#include "rend/TexCache.h"
#include <cstdlib>
#include <cstring>
#endif
#include <SDL.h>
#include <SDL_metal.h>

#include <algorithm>
#include <cmath>

#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
void os_VideoRoutingPublishMetalTexture(id<MTLDevice>, id<MTLTexture>, id<MTLCommandBuffer>);
void os_VideoRoutingTermMetal();
#endif

struct MetalContext::Impl
{
	SDL_MetalView view = nullptr;
	~Impl()
	{
		if (view)
			SDL_Metal_DestroyView(view);
	}
	dispatch_semaphore_t slots = dispatch_semaphore_create(3);
	int swapInterval = 1;
	CFTimeInterval nextPresentationTime = 0;
	CAMetalLayer *layer = nil;
	id<MTLRenderPipelineState> pipeline = nil;
	id<MTLSamplerState> sampler = nil;
	id<MTLSamplerState> pointSampler = nil;
	std::unique_ptr<MetalOverlay> overlay;
	id<MTLTexture> lastFrame = nil;
	id<MTLTexture> routingFrame = nil;
	id<MTLTexture> routingScaled = nil;
	float aspect = 1;
};

static id<MTLRenderPipelineState> createPresentationPipeline(id<MTLDevice> device, NSError **error)
{
	NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
struct O { float4 pos [[position]]; float2 uv; };
vertex O frameVertex(uint i [[vertex_id]], constant float3 &params [[buffer(0)]]) {
 float2 p[4]={float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1)};
 float2 t[4]={float2(0,1),float2(1,1),float2(0,0),float2(1,0)};
 O o; o.pos=float4(p[i]+params.xy,0,1); o.uv=params.z > 0.5 ? float2(1-t[i].y,t[i].x) : t[i]; return o;
}
fragment float4 frameFragment(O i [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) { return tex.sample(s,i.uv); }
)";
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:error];
	if (!library)
		return nil;
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	descriptor.vertexFunction = [library newFunctionWithName:@"frameVertex"];
	descriptor.fragmentFunction = [library newFunctionWithName:@"frameFragment"];
	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

bool metalIsAvailable()
{
	static const bool available = [] {
		@autoreleasepool {
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (!device || ![device newCommandQueue])
				return false;
			NSError *error = nil;
			id<MTLRenderPipelineState> pipeline = createPresentationPipeline(device, &error);
			if (!pipeline)
			{
				WARN_LOG(RENDERER, "Metal availability probe failed: %s",
				         error.localizedDescription.UTF8String ?: "pipeline creation failed");
				return false;
			}
			MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
			return [device newSamplerStateWithDescriptor:sampler] != nil;
		}
	}();
	return available;
}

// Probe the operations and render-target formats used by the per-pixel renderer.
// Family numbers alone do not prove the complete pipeline can be created.
bool metalSupportsPerPixel()
{
	static const bool supported = [] {
		@autoreleasepool {
			if (!metalIsAvailable())
				return false;
			id<MTLDevice> device = MTLCreateSystemDefaultDevice();
			if (!device)
				return false;
			NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
struct Result { float4 color [[color(0)]]; float depth [[color(1)]]; };
vertex float4 probeVertex(uint i [[vertex_id]]) { return float4(float(i == 1), float(i == 2), 0, 1); }
fragment Result probeFragment(device atomic_uint *values [[buffer(0)]], uint primitive [[primitive_id]]) {
    atomic_fetch_add_explicit(values, 1u, memory_order_relaxed);
    atomic_exchange_explicit(values, primitive, memory_order_relaxed);
    Result result; result.color = float4(1); result.depth = 1; return result;
}
)";
			NSError *error = nil;
			id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
			if (!library)
			{
				WARN_LOG(RENDERER, "Metal per-pixel capability shader failed: %s",
						 error.localizedDescription.UTF8String ?: "unknown error");
				return false;
			}
			MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
			desc.vertexFunction = [library newFunctionWithName:@"probeVertex"];
			desc.fragmentFunction = [library newFunctionWithName:@"probeFragment"];
			desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
			desc.colorAttachments[1].pixelFormat = MTLPixelFormatR32Float;
			id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&error];
			if (!pipeline)
				WARN_LOG(RENDERER, "Metal per-pixel capability pipeline failed: %s",
						 error.localizedDescription.UTF8String ?: "unknown error");
			return pipeline != nil;
		}
	}();
	return supported;
}
bool MetalContext::hasPerPixel() { return metalSupportsPerPixel(); }

void metalCreateContext(void *window, void *display) { MetalContext::Create(window, display); }
void MetalContext::Create(void *window, void *display) { new MetalContext(window, display); }
MetalContext::MetalContext(void *window, void *display)
	: GraphicsContext(window, display), impl(std::make_unique<Impl>()), device(nil), queue(nil)
{
	if (!sdl_recreate_window(SDL_WINDOW_METAL))
	{
		throw FlycastException(std::string("Metal window creation failed: ") + SDL_GetError());
	}
	impl->view = SDL_Metal_CreateView((SDL_Window *)this->window);
	device = MTLCreateSystemDefaultDevice();
	queue = [device newCommandQueue];
	if (!impl->view)
		throw FlycastException(std::string("Metal view creation failed: ") + SDL_GetError());
	if (!device)
		throw FlycastException("Metal initialization failed: no Metal device available");
	if (!queue)
		throw FlycastException("Metal initialization failed: command queue creation failed");
	impl->layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(impl->view);
	impl->layer.device = device;
	impl->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	impl->layer.framebufferOnly = YES;
	NSError *error = nil;
	impl->pipeline = createPresentationPipeline(device, &error);
	MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
	s.minFilter = MTLSamplerMinMagFilterLinear;
	s.magFilter = MTLSamplerMinMagFilterLinear;
	impl->sampler = [device newSamplerStateWithDescriptor:s];
	s.minFilter = MTLSamplerMinMagFilterNearest;
	s.magFilter = MTLSamplerMinMagFilterNearest;
	impl->pointSampler = [device newSamplerStateWithDescriptor:s];
	if (!impl->pipeline || !impl->sampler || !impl->pointSampler)
	{
		throw FlycastException(metalErrorMessage("Metal presentation pipeline initialization failed", error));
	}
	impl->overlay = std::make_unique<MetalOverlay>(device);
	resize();
	metalInitImGui();
}
MetalContext::~MetalContext()
{
	WaitIdle();
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
	os_VideoRoutingTermMetal();
#endif
	imguiDriver.reset();
	impl->lastFrame = nil;
}
void MetalContext::WaitIdle()
{
	if (queue)
	{
		id<MTLCommandBuffer> b = [queue commandBuffer];
		[b commit];
		[b waitUntilCompleted];
	}
}
void MetalContext::setSwapInterval(int interval)
{
	impl->swapInterval = std::max(0, interval);
}
void MetalContext::resize()
{
	int w = 0, h = 0;
	SDL_Metal_GetDrawableSize((SDL_Window *)window, &w, &h);
	if (w <= 0 || h <= 0)
		return;
	impl->layer.drawableSize = CGSizeMake(w, h);
	settings.display.width = w;
	settings.display.height = h;
	int ww = 0, wh = 0;
	SDL_GetWindowSize((SDL_Window *)window, &ww, &wh);
	if (ww > 0)
		settings.display.pointScale = (float)w / ww;
}
std::string MetalContext::getDriverName() { return device.name.UTF8String ?: "Metal"; }
std::string MetalContext::getDriverVersion() { return "Metal"; }
void MetalContext::PresentFrame(id<MTLTexture> texture, float aspectRatio)
{
	if (texture)
	{
		impl->lastFrame = texture;
		impl->aspect = aspectRatio > 0 ? aspectRatio : (float)texture.width / texture.height;
	}
	gui_draw_osd();
	imguiDriver->renderDrawData(ImGui::GetDrawData(), false);
}
void MetalContext::PresentLastFrame()
{
	@autoreleasepool
	{
#ifdef TEST_AUTOMATION
		extern bool do_screenshot;
		if (do_screenshot && impl->lastFrame)
		{
			std::vector<u8> pixels;
			int width = 0, height = 0;
			if (readFrame(pixels, width, height, false))
			{
				dump_screenshot(pixels.data(), width, height);
				std::_Exit(0);
			}
		}
		const char *backgroundTest = std::getenv("HOLLYCAST_TEST_BACKGROUND");
		if (backgroundTest != nullptr && std::strcmp(backgroundTest, "1") == 0)
			return;
#endif
		if (SDL_GetWindowFlags((SDL_Window *)window) & SDL_WINDOW_MINIMIZED)
			return;
		resize();
		if (settings.display.width <= 0 || settings.display.height <= 0)
			return;
		const bool synchronized = config::VSync && !settings.input.fastForwardMode && impl->swapInterval > 0;
		if (impl->layer.displaySyncEnabled != synchronized)
			impl->layer.displaySyncEnabled = synchronized;
		if (dispatch_semaphore_wait(impl->slots, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) != 0)
			return;
		id<CAMetalDrawable> drawable = [impl->layer nextDrawable];
		if (!drawable)
		{
			dispatch_semaphore_signal(impl->slots);
			return;
		}
		id<MTLCommandBuffer> cmd = [queue commandBuffer];
		if (!cmd)
		{
			dispatch_semaphore_signal(impl->slots);
			return;
		}
		dispatch_semaphore_t slots = impl->slots;
		[cmd addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		  if (completed.status == MTLCommandBufferStatusError)
			  ERROR_LOG(RENDERER, "Metal presentation failed: %s",
						completed.error.localizedDescription.UTF8String ?: "unknown GPU error");
		  dispatch_semaphore_signal(slots);
		}];
		id<MTLTexture> renderTarget = drawable.texture;
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
		if (config::VideoRouting)
		{
			NSUInteger w = drawable.texture.width, h = drawable.texture.height;
			if (!impl->routingFrame || impl->routingFrame.width != w || impl->routingFrame.height != h)
			{
				MTLTextureDescriptor *desc =
					[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																	   width:w
																	  height:h
																   mipmapped:NO];
				desc.storageMode = MTLStorageModePrivate;
				desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				impl->routingFrame = [device newTextureWithDescriptor:desc];
			}
			if (impl->routingFrame)
				renderTarget = impl->routingFrame;
		}
		else
		{
			os_VideoRoutingTermMetal();
			impl->routingFrame = nil;
			impl->routingScaled = nil;
		}
#endif
		MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
		pass.colorAttachments[0].texture = renderTarget;
		pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
		id<MTLRenderCommandEncoder> enc = [cmd renderCommandEncoderWithDescriptor:pass];
		if (impl->lastFrame && (!gui_is_open() || (renderer && renderer->RenderLastFrame())))
		{
			int width = (int)drawable.texture.width, height = (int)drawable.texture.height;
			int top = std::min(height - 1, std::max(0, getScaledTopInset())), dx = 0, dy = 0;
			getWindowboxDimensions(width, height - top, impl->aspect, dx, dy, config::Rotate90);
			int vw = std::max(1, width - 2 * dx), vh = std::max(1, height - top - 2 * dy);
			performance_overlay::recordOutputViewport(vw, vh);
			[enc setViewport:MTLViewport{(double)dx, (double)(top + dy), (double)vw, (double)vh, 0, 1}];
			float shiftX, shiftY;
			getVideoShift(shiftX, shiftY);
			float x = 2.f * shiftX / impl->lastFrame.width;
			float y = 2.f * shiftY / impl->lastFrame.height;
			float scale[4] = {config::Rotate90 ? y : x, config::Rotate90 ? x : -y, config::Rotate90 ? 1.f : 0.f, 0.f};
			[enc setScissorRect:MTLScissorRect{(NSUInteger)dx, (NSUInteger)(top + dy), (NSUInteger)vw, (NSUInteger)vh}];
			[enc setRenderPipelineState:impl->pipeline];
			[enc setVertexBytes:scale length:sizeof(scale) atIndex:0];
			[enc setFragmentTexture:impl->lastFrame atIndex:0];
			[enc setFragmentSamplerState:config::LinearInterpolation ? impl->sampler : impl->pointSampler atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
			[enc setViewport:MTLViewport{0, 0, (double)width, (double)height, 0, 1}];
			[enc setScissorRect:MTLScissorRect{0, 0, (NSUInteger)width, (NSUInteger)height}];
		}
		if (!gui_is_open() && impl->lastFrame)
			impl->overlay->draw(enc, (int)drawable.texture.width, (int)drawable.texture.height,
								settings.display.uiScale, config::FloatVMUs, true);
		metalDrawImGui(enc, (int)drawable.texture.width, (int)drawable.texture.height);
		[enc endEncoding];
#if defined(VIDEO_ROUTING) && defined(TARGET_MAC)
		if (renderTarget != drawable.texture)
		{
			auto copyTo = [&](id<MTLTexture> target)
			{
				MTLRenderPassDescriptor *copyPass = [MTLRenderPassDescriptor renderPassDescriptor];
				copyPass.colorAttachments[0].texture = target;
				copyPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
				copyPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> copy = [cmd renderCommandEncoderWithDescriptor:copyPass];
				float scale[4] = {0, 0, 0, 0};
				[copy setRenderPipelineState:impl->pipeline];
				[copy setVertexBytes:scale length:sizeof(scale) atIndex:0];
				[copy setFragmentTexture:renderTarget atIndex:0];
				[copy setFragmentSamplerState:impl->sampler atIndex:0];
				[copy drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
				[copy endEncoding];
			};
			copyTo(drawable.texture);
			id<MTLTexture> routed = renderTarget;
			if (config::VideoRoutingScale)
			{
				int height = std::clamp((int)config::VideoRoutingVRes, 1, 8192);
				int width = std::clamp((int)((double)height * renderTarget.width / renderTarget.height), 1, 8192);
				if (!impl->routingScaled || impl->routingScaled.width != width || impl->routingScaled.height != height)
				{
					MTLTextureDescriptor *desc =
						[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																		   width:width
																		  height:height
																	   mipmapped:NO];
					desc.storageMode = MTLStorageModePrivate;
					desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
					impl->routingScaled = [device newTextureWithDescriptor:desc];
				}
				if (impl->routingScaled)
				{
					copyTo(impl->routingScaled);
					routed = impl->routingScaled;
				}
			}
			os_VideoRoutingPublishMetalTexture(device, routed, cmd);
		}
#endif
		if (synchronized && impl->swapInterval > 1)
		{
			if (@available(macOS 10.15.4, *))
				[cmd presentDrawable:drawable afterMinimumDuration:impl->swapInterval / 60.0];
			else
			{
				impl->nextPresentationTime = std::max(CACurrentMediaTime(), impl->nextPresentationTime)
					+ impl->swapInterval / 60.0;
				[cmd presentDrawable:drawable atTime:impl->nextPresentationTime];
			}
		}
		else
		{
			impl->nextPresentationTime = 0;
			[cmd presentDrawable:drawable];
		}
		[cmd commit];
		performance_overlay::recordPresentedFrame();
	}
}
bool MetalContext::GetLastFrame(std::vector<u8> &data, int &width, int &height)
{
	return readFrame(data, width, height, true);
}
bool MetalContext::readFrame(std::vector<u8> &data, int &width, int &height, bool normalize)
{
	id<MTLTexture> texture = impl->lastFrame;
	if (!texture)
		return false;
	int sw = (int)texture.width, sh = (int)texture.height;
	if (sw <= 0 || sh <= 0)
		return false;
	if (!normalize)
	{
		width = sw;
		height = sh;
	}
	else if (width > 0 && height > 0)
	{
		if ((float)width / height > impl->aspect)
			width = std::max(1, (int)(height * impl->aspect));
		else
			height = std::max(1, (int)(width / impl->aspect));
	}
	else if (width > 0)
		height = std::max(1, (int)(width / impl->aspect));
	else if (height > 0)
		width = std::max(1, (int)(height * impl->aspect));
	else
	{
		width = sw;
		height = sh;
		if (config::Rotate90)
			std::swap(width, height);
		int w = std::max(1, (int)(impl->aspect * height));
		if (width > w)
			height = std::max(1, (int)(width / impl->aspect));
		else
			width = w;
	}
	NSUInteger row = ((NSUInteger)sw * 4 + 255) & ~255u;
	id<MTLBuffer> buffer = [device newBufferWithLength:row * sh options:MTLResourceStorageModeShared];
	if (!buffer)
		return false;
	id<MTLCommandBuffer> cmd = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:texture
					 sourceSlice:0
					 sourceLevel:0
					sourceOrigin:MTLOriginMake(0, 0, 0)
					  sourceSize:MTLSizeMake(sw, sh, 1)
						toBuffer:buffer
			   destinationOffset:0
		  destinationBytesPerRow:row
		destinationBytesPerImage:row * sh];
	[blit endEncoding];
	[cmd commit];
	[cmd waitUntilCompleted];
	if (cmd.status != MTLCommandBufferStatusCompleted)
		return false;
	const u8 *src = (const u8 *)buffer.contents;
	data.resize((size_t)width * height * 3);
	const bool bgra = texture.pixelFormat == MTLPixelFormatBGRA8Unorm;
	if (!normalize)
	{
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const u8 *pixel = src + y * row + x * 4;
				u8 *dst = data.data() + ((size_t)y * width + x) * 3;
				dst[0] = pixel[bgra ? 2 : 0];
				dst[1] = pixel[1];
				dst[2] = pixel[bgra ? 0 : 2];
			}
		return true;
	}
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
		{
			float sx = ((x + .5f) * sw / width) - .5f;
			float sy = ((y + .5f) * sh / height) - .5f;
			if (config::Rotate90)
			{
				sx = sw - (y + .5f) * sw / height - .5f;
				sy = (x + .5f) * sh / width - .5f;
			}
			sx = std::clamp(sx, 0.f, (float)(sw - 1));
			sy = std::clamp(sy, 0.f, (float)(sh - 1));
			int x0 = (int)sx, y0 = (int)sy;
			int x1 = std::min(x0 + 1, sw - 1), y1 = std::min(y0 + 1, sh - 1);
			float fx = sx - x0, fy = sy - y0;
			u8 *dst = data.data() + ((size_t)y * width + x) * 3;
			for (int channel = 0; channel < 3; ++channel)
			{
				int sourceChannel = bgra ? 2 - channel : channel;
				float top = src[y0 * row + x0 * 4 + sourceChannel] * (1 - fx)
					+ src[y0 * row + x1 * 4 + sourceChannel] * fx;
				float bottom = src[y1 * row + x0 * 4 + sourceChannel] * (1 - fx)
					+ src[y1 * row + x1 * 4 + sourceChannel] * fx;
				dst[channel] = (u8)std::lround(top * (1 - fy) + bottom * fy);
			}
		}
	return true;
}
