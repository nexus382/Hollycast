// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone integration smoke test for the actual Metal regular shader.
// Build with: clang++ -std=c++17 -fobjc-arc -Icore tests/metal/regular_shader_smoke.mm
//   core/rend/metal/metal_shaders.mm -framework Foundation -framework Metal -o /tmp/metal-regular-smoke
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include "rend/metal/metal_shaders.h"
#include <cmath>
#include <cstdio>
#include <cstring>

struct Vertex { float xyz[3]; unsigned char color[4], offset[4]; float uv[2]; char rest[28]; };
struct alignas(16) Uniforms {
    simd_float4x4 matrix;
    simd_float4 clip, fogColor, fogVert, fogClampMin, fogClampMax, values;
    simd_uint4 flags, flags2, flags3, flags4;
};
static_assert(sizeof(Vertex) == 56 && sizeof(Uniforms) == 224);

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format, int width, int height, MTLTextureUsage usage) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}
int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { std::fprintf(stderr, "No Metal device\n"); return 2; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:metalRegularShaderSource] options:nil error:&error];
    if (!library) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"pvr_vertex"]; pd.fragmentFunction = [library newFunctionWithName:@"pvr_fragment"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; pd.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat3; vd.attributes[0].offset = 0;
    vd.attributes[1].format = MTLVertexFormatUChar4Normalized; vd.attributes[1].offset = 12;
    vd.attributes[2].format = MTLVertexFormatUChar4Normalized; vd.attributes[2].offset = 16;
    vd.attributes[3].format = MTLVertexFormatFloat2; vd.attributes[3].offset = 20;
    vd.layouts[0].stride = sizeof(Vertex); pd.vertexDescriptor = vd;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!pipeline) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    id<MTLTexture> color = texture(device, MTLPixelFormatRGBA8Unorm, 4, 4, MTLTextureUsageRenderTarget);
    id<MTLTexture> depth = texture(device, MTLPixelFormatDepth32Float_Stencil8, 4, 4, MTLTextureUsageRenderTarget);
    id<MTLTexture> sampled = texture(device, MTLPixelFormatRGBA8Unorm, 1, 1, MTLTextureUsageShaderRead);
    id<MTLTexture> palette = texture(device, MTLPixelFormatRGBA8Unorm, 1, 1, MTLTextureUsageShaderRead);
    id<MTLTexture> fog = texture(device, MTLPixelFormatR8Unorm, 128, 2, MTLTextureUsageShaderRead);
    const unsigned char texel[] = { 64, 128, 192, 128 }; [sampled replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    const unsigned char black[] = { 0, 0, 0, 255 }; [palette replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:black bytesPerRow:4];
    unsigned char fogData[256]; std::memset(fogData, 128, sizeof(fogData)); [fog replaceRegion:MTLRegionMake2D(0,0,128,2) mipmapLevel:0 withBytes:fogData bytesPerRow:128];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionGreater; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [device newDepthStencilStateWithDescriptor:dd];
    Vertex verts[3] = {};
    const float positions[3][2] = {{-1,-1},{3,-1},{-1,3}};
    for (int i=0;i<3;i++) { verts[i].xyz[0]=positions[i][0]; verts[i].xyz[1]=positions[i][1]; verts[i].xyz[2]=1; std::memset(verts[i].color,255,4); }
    id<MTLBuffer> vb = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    id<MTLBuffer> result = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    Uniforms u {}; u.matrix = matrix_identity_float4x4; u.values = {0,1,1,0}; u.flags = {1,1,0,0}; u.flags2 = {0,0,0,2}; u.flags4 = {1,0,0,0};
    id<MTLCommandBuffer> cb = [queue commandBuffer]; MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=color; pass.colorAttachments[0].loadAction=MTLLoadActionClear; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=0; pass.depthAttachment.storeAction=MTLStoreActionStore;
    pass.stencilAttachment.texture=depth; pass.stencilAttachment.loadAction=MTLLoadActionClear; pass.stencilAttachment.storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:pipeline]; [e setDepthStencilState:ds]; [e setVertexBuffer:vb offset:0 atIndex:0];
    [e setVertexBytes:&u length:sizeof(u) atIndex:1]; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setFragmentTexture:sampled atIndex:0]; [e setFragmentTexture:palette atIndex:1]; [e setFragmentTexture:fog atIndex:2]; [e setFragmentSamplerState:sampler atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // A second, farther fragment must fail the greater-depth test.
    for (int i=0;i<3;i++) verts[i].xyz[2]=0.25f;
    id<MTLBuffer> farther = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    [e setVertexBuffer:farther offset:0 atIndex:0]; u.flags = {0,1,0,0}; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // A failed punch-through test leaves the first pixel's texture color unchanged.
    dd.depthCompareFunction = MTLCompareFunctionAlways;
    [e setDepthStencilState:[device newDepthStencilStateWithDescriptor:dd]];
    [e setVertexBuffer:vb offset:0 atIndex:0]; [e setScissorRect:MTLScissorRect{1,0,1,1}];
    u.flags = {1,1,0,0}; u.flags2 = {0,1,0,2}; u.values.x = 0.75f;
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // Constant half-strength fog mixes the sampled texture with red.
    [e setScissorRect:MTLScissorRect{2,0,1,1}]; u.flags2 = {0,0,0,0}; u.fogColor = {1,0,0,1};
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // Blend half-transparent red over the existing texture color.
    pd.colorAttachments[0].blendingEnabled = YES;
    pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    id<MTLRenderPipelineState> blended = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    if (!blended) { std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    for (int i=0;i<3;i++) { verts[i].xyz[2]=1; verts[i].color[0]=255; verts[i].color[1]=verts[i].color[2]=0; verts[i].color[3]=128; }
    id<MTLBuffer> red = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    [e setRenderPipelineState:blended]; [e setVertexBuffer:red offset:0 atIndex:0]; [e setScissorRect:MTLScissorRect{3,0,1,1}];
    u.flags = {0,1,0,0}; u.flags2 = {0,0,0,2};
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // Reconstruction blends premultiplied colors, so transparent blue cannot tint red.
    id<MTLTexture> taps = texture(device, MTLPixelFormatRGBA8Unorm, 2, 2, MTLTextureUsageShaderRead);
    const unsigned char tapsData[16] = {255,0,0,255, 0,0,255,0, 0,0,255,0, 0,0,255,0};
    [taps replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:tapsData bytesPerRow:8];
    for (int i=0;i<3;i++) { verts[i].uv[0]=0.5; verts[i].uv[1]=0.5; }
    id<MTLBuffer> filtered = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    [e setRenderPipelineState:pipeline]; [e setVertexBuffer:filtered offset:0 atIndex:0]; [e setFragmentTexture:taps atIndex:0];
    u.flags = {1,1,0,0}; u.flags4.y = 1;
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1]; [e setScissorRect:MTLScissorRect{0,1,1,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    u.flags4.y = 2; [e setFragmentBytes:&u length:sizeof(u) atIndex:1]; [e setScissorRect:MTLScissorRect{1,1,1,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status == MTLCommandBufferStatusError) { std::fprintf(stderr, "%s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const auto *px = static_cast<const unsigned char *>(result.contents);
    for (int i=0;i<4;i++) if (std::abs(int(px[i])-int(texel[i])) > 1) { std::fprintf(stderr,"pixel[%d]=%u, expected %u\n",i,px[i],texel[i]); return 1; }
    for (int i=0;i<4;i++) if (std::abs(int(px[4+i])-int(texel[i])) > 1) { std::fprintf(stderr,"punch-through pixel mismatch\n"); return 1; }
    const unsigned char mixed[] = {160,64,96,128};
    for (int x=2;x<=3;x++) for (int i=0;i<4;i++)
        if (std::abs(int(px[x*4+i])-int(mixed[i])) > 2) { std::fprintf(stderr,"pixel %d channel %d = %u, expected %u\n",x,i,px[x*4+i],mixed[i]); return 1; }
    const unsigned char *filteredRow = px + 256;
    const unsigned char expectedFilter[] = {255,0,0,64, 255,0,0,0};
    for (int i=0;i<8;i++) if (std::abs(int(filteredRow[i])-int(expectedFilter[i])) > 2) {
        std::fprintf(stderr,"filter channel %d = %u, expected %u\n", i, filteredRow[i], expectedFilter[i]); return 1;
    }
    std::puts("Metal regular texture, depth, punch-through, fog, blending and reconstruction smoke passed"); return 0;
} }
