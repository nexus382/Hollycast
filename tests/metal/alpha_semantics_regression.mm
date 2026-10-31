// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Regression test for punch-through and bump-map alpha on the actual Metal shader.
// Build with: clang++ -std=c++17 -fobjc-arc -Icore tests/metal/alpha_semantics_regression.mm
//   core/rend/metal/metal_shaders.mm -framework Foundation -framework Metal -o /tmp/metal-alpha-regression
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
    pass.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);
    pass.depthAttachment.texture=depth; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=0; pass.depthAttachment.storeAction=MTLStoreActionStore;
    pass.stencilAttachment.texture=depth; pass.stencilAttachment.loadAction=MTLLoadActionClear; pass.stencilAttachment.storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:pipeline]; [e setDepthStencilState:ds]; [e setVertexBuffer:vb offset:0 atIndex:0];
    [e setVertexBytes:&u length:sizeof(u) atIndex:1]; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setFragmentTexture:sampled atIndex:0]; [e setFragmentTexture:palette atIndex:1]; [e setFragmentTexture:fog atIndex:2]; [e setFragmentSamplerState:sampler atIndex:0];
    // At the same sampled alpha, threshold 0.25 accepts and 0.75 rejects.
    // GL's accepted punch-through writes alpha 1 after testing.
    u.flags2 = {0,1,0,2}; u.values.x = 0.25f;
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1]; [e setScissorRect:MTLScissorRect{0,0,1,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    u.values.x = 0.75f;
    [e setFragmentBytes:&u length:sizeof(u) atIndex:1]; [e setScissorRect:MTLScissorRect{1,0,1,1}];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    // For bump maps GL ignores IgnoreTexA and preserves the computed alpha.
    // Set offset RGB to zero and A=64, making the bump alpha exactly 64/255.
    for (int i=0;i<3;i++) verts[i].offset[3]=64;
    id<MTLBuffer> bump = [device newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
    [e setVertexBuffer:bump offset:0 atIndex:0];
    u.values.x = 0; u.flags2 = {0,0,0,2}; u.flags3 = {0,1,0,0};
    u.flags = {1,1,0,0}; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setScissorRect:MTLScissorRect{2,0,1,1}]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    u.flags.z = 1; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setScissorRect:MTLScissorRect{3,0,1,1}]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { std::fprintf(stderr, "GPU command failed: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const auto *px = static_cast<const unsigned char *>(result.contents);
    const unsigned char expected[16] = {64,128,192,255, 0,0,0,0, 255,255,255,64, 255,255,255,64};
    bool failed = false;
    for (int i=0;i<16;i++) if (std::abs(int(px[i])-int(expected[i])) > 1) {
        std::fprintf(stderr,"pixel %d channel %d = %u, expected %u\n", i/4, i%4, px[i], expected[i]); failed = true;
    }
    if (failed) return 1;
    std::puts("Metal punch-through and bump-map alpha semantics passed"); return 0;
} }
