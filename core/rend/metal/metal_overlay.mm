// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_overlay.h"
#include "rend/osd.h"
#include "cfg/option.h"
#include <array>

struct MetalOverlay::Impl
{
	id<MTLDevice> device;
	id<MTLRenderPipelineState> pipeline;
	id<MTLSamplerState> sampler;
	std::array<id<MTLTexture>, 8> vmus{};
	std::array<u64, 8> lastChanged{};
	id<MTLTexture> crosshair;

	id<MTLTexture> makeTexture(int width, int height, const void *pixels)
	{
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																						width:width
																					   height:height
																					mipmapped:NO];
		desc.storageMode = metalCpuTextureStorageMode(device);
		desc.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
		[texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
				   mipmapLevel:0
					 withBytes:pixels
				   bytesPerRow:width * 4];
		return texture;
	}
};

MetalOverlay::MetalOverlay(id<MTLDevice> device) : impl(std::make_unique<Impl>())
{
	impl->device = device;
	NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
struct Params { float4 rect; float4 color; float2 size; };
struct Out { float4 position [[position]]; float2 uv; };
vertex Out overlayVertex(uint i [[vertex_id]], constant Params &p [[buffer(0)]]) {
    float2 q[4] = {float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    float2 pos = p.rect.xy + q[i] * p.rect.zw;
    Out o; o.position = float4(pos.x / p.size.x * 2 - 1, 1 - pos.y / p.size.y * 2, 0, 1); o.uv = q[i]; return o;
}
fragment float4 overlayFragment(Out in [[stage_in]], constant Params &p [[buffer(0)]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    return tex.sample(s, in.uv) * p.color;
}
)";
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
	if (!library)
		throw FlycastException(metalErrorMessage("Metal overlay shader compilation failed", error));
	MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
	pipeline.vertexFunction = [library newFunctionWithName:@"overlayVertex"];
	pipeline.fragmentFunction = [library newFunctionWithName:@"overlayFragment"];
	pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	pipeline.colorAttachments[0].blendingEnabled = YES;
	pipeline.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	pipeline.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	pipeline.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	pipeline.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	impl->pipeline = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
	MTLSamplerDescriptor *sampler = [MTLSamplerDescriptor new];
	sampler.minFilter = MTLSamplerMinMagFilterNearest;
	sampler.magFilter = MTLSamplerMinMagFilterNearest;
	impl->sampler = [device newSamplerStateWithDescriptor:sampler];
	if (!impl->pipeline || !impl->sampler)
		throw FlycastException(metalErrorMessage("Metal overlay pipeline initialization failed", error));
}
MetalOverlay::~MetalOverlay() = default;

void MetalOverlay::draw(id<MTLRenderCommandEncoder> encoder, int width, int height, float scaling, bool showVmus,
						bool showCrosshairs)
{
	struct alignas(16) Params
	{
		float rect[4];
		float color[4];
		float size[2];
		float padding[2];
	};
	[encoder setRenderPipelineState:impl->pipeline];
	[encoder setFragmentSamplerState:impl->sampler atIndex:0];
	auto drawTexture = [&](id<MTLTexture> texture, float x, float y, float w, float h, const float color[4])
	{
		if (!texture || w <= 0 || h <= 0)
			return;
		Params p = {{x, y, w, h}, {color[0], color[1], color[2], color[3]}, {(float)width, (float)height}, {0, 0}};
		[encoder setVertexBytes:&p length:sizeof(p) atIndex:0];
		[encoder setFragmentBytes:&p length:sizeof(p) atIndex:0];
		[encoder setFragmentTexture:texture atIndex:0];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	};
	if (showVmus && settings.platform.isConsole())
	{
		float padding = 8.f * scaling, w = 96.f * scaling, h = 64.f * scaling;
		const float color[] = {.75f, .75f, .75f, .75f};
		for (size_t i = 0; i < impl->vmus.size(); ++i)
		{
			if (!vmu_lcd_status[i])
			{
				impl->vmus[i] = nil;
				continue;
			}
			if (!impl->vmus[i] || impl->lastChanged[i] != vmuLastChanged[i])
			{
				impl->vmus[i] = impl->makeTexture(48, 32, vmu_lcd_data[i]);
				impl->lastChanged[i] = vmuLastChanged[i];
			}
			if (vmu_window::ownsScreen((int)i))
				continue;
			float x = (i & 2) ? width - padding - w : padding;
			float y = (i & 4) ? height - padding - h - ((i & 1) ? padding + h : 0)
							  : padding + getScaledTopInset() + ((i & 1) ? padding + h : 0);
			drawTexture(impl->vmus[i], x, y, w, h, color);
		}
	}
	if (showCrosshairs)
	{
		if (!impl->crosshair)
			impl->crosshair = impl->makeTexture(16, 16, getCrosshairTextureData());
		for (size_t i = 0; i < config::CrosshairColor.size(); ++i)
		{
			if (!crosshairNeeded((int)i))
				continue;
			auto [x, y] = getCrosshairPosition((int)i);
			float size = config::CrosshairSize * scaling;
			u32 c = config::CrosshairColor[i];
			float color[] = {(c & 255) / 255.f, ((c >> 8) & 255) / 255.f, ((c >> 16) & 255) / 255.f,
							 ((c >> 24) & 255) / 255.f};
			drawTexture(impl->crosshair, x - size / 2, y - size / 2, size, size, color);
		}
	}
}
