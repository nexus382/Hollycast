// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Regression for a flat-shaded PVR punch-through triangle.  The PVR uses the
// last vertex; Metal uses the first unless production geometry is adapted.
// The fixture uses the real shader and the same parsed-geometry upload boundary
// as MetalRenderer, with the unmodified PVR index and vertex order.
// Build with: clang++ -std=c++17 -fobjc-arc -Icore -Icore/deps -Icore/deps/nowide/include -Icore/deps/glm tests/metal/flat_punch_regression.mm
//   -ffunction-sections -fdata-sections core/rend/metal/metal_shaders.mm core/rend/sorter.cpp \
//   -Wl,-dead_strip -framework Foundation -framework Metal -o /tmp/metal-flat-regression
#include "rend/metal/metal_geometry.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>
#include "rend/metal/metal_shaders.h"
#include <cmath>
#include <cstdio>
#include <cstring>

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
    // A strip's two triangles use their respective last colors. Check the
    // uploaded buffers, so this exercises the production preparation path.
    rend_context strip {};
    strip.verts.resize(4); strip.idx = {0, 1, 2, 3};
    const float quad[4][2] = {{-1,-1}, {1,-1}, {-1,1}, {1,1}};
    for (int i=0;i<4;i++) {
        strip.verts[i].x = quad[i][0]; strip.verts[i].y = quad[i][1]; strip.verts[i].z = 1;
        strip.verts[i].col[0] = 10 * (i + 1); strip.verts[i].col[3] = 255;
    }
    strip.global_param_op.emplace_back(); strip.global_param_op.back().init();
    strip.global_param_op.back().count = 4;
    MetalGeometryBuffers stripGeometry = metalUploadGeometry(device, strip);
    const auto *stripVertices = static_cast<const Vertex *>(stripGeometry.vertices.contents);
    if (stripVertices[0].col[0] != 30 || stripVertices[1].col[0] != 40) {
        std::fprintf(stderr, "FAIL flat strip: first colors are %u,%u, expected 30,40\n", stripVertices[0].col[0], stripVertices[1].col[0]); return 1;
    }
    // Sorted triangles share vertices: rotating indices preserves winding and
    // leaves each triangle's last vertex first without overwriting neighbors.
    rend_context sorted {};
    sorted.verts.resize(4); sorted.idx = {0,1,2, 2,1,3};
    for (int i=0;i<4;i++) {
        sorted.verts[i].x = quad[i][0]; sorted.verts[i].y = quad[i][1]; sorted.verts[i].z = 1;
        sorted.verts[i].col[0] = 10 * (i + 1); sorted.verts[i].col[3] = 255;
    }
    sorted.global_param_tr.emplace_back(); sorted.global_param_tr.back().init();
    sorted.sortedTriangles.push_back({0,0,6});
    MetalGeometryBuffers sortedGeometry = metalUploadGeometry(device, sorted);
    const auto *sortedIndices = static_cast<const u32 *>(sortedGeometry.indices.contents);
    const u32 expectedIndices[] = {2,0,1, 3,2,1};
    if (std::memcmp(sortedIndices, expectedIndices, sizeof(expectedIndices)) != 0) {
        std::fprintf(stderr, "FAIL sorted flat triangles: indices were not rotated\n"); return 1;
    }
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
    const unsigned char texel[] = { 255, 255, 255, 255 }; [sampled replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:texel bytesPerRow:4];
    const unsigned char black[] = { 0, 0, 0, 255 }; [palette replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:black bytesPerRow:4];
    unsigned char fogData[256]; std::memset(fogData, 128, sizeof(fogData)); [fog replaceRegion:MTLRegionMake2D(0,0,128,2) mipmapLevel:0 withBytes:fogData bytesPerRow:128];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sd];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionGreater; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [device newDepthStencilStateWithDescriptor:dd];
    rend_context scene {};
    scene.verts.resize(3);
    scene.idx = {0, 1, 2};
    scene.global_param_pt.emplace_back();
    auto& poly = scene.global_param_pt.back();
    poly.init(); poly.first = 0; poly.count = 3; poly.pcw.Gouraud = 0;
    const float positions[3][2] = {{-1,-1},{3,-1},{-1,3}};
    // First red/transparent, middle green/opaque, last blue/opaque.
    const unsigned char vertexColors[3][4] = {{255,0,0,0}, {0,255,0,255}, {0,0,255,255}};
    for (int i=0;i<3;i++) {
        auto& v = scene.verts[i]; v.x=positions[i][0]; v.y=positions[i][1]; v.z=1;
        std::memcpy(v.col, vertexColors[i], 4);
    }
    MetalGeometryBuffers geometry = metalUploadGeometry(device, scene);
    id<MTLBuffer> vb = geometry.vertices;
    id<MTLBuffer> result = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    Uniforms u {}; u.matrix = matrix_identity_float4x4; u.values = {0.5f,1,1,0}; u.flags = {1,1,0,3}; u.flags2 = {0,1,0,2}; u.flags4 = {0,0,0,0};
    id<MTLCommandBuffer> cb = [queue commandBuffer]; MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture=color; pass.colorAttachments[0].loadAction=MTLLoadActionClear; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
    pass.depthAttachment.texture=depth; pass.depthAttachment.loadAction=MTLLoadActionClear; pass.depthAttachment.clearDepth=0; pass.depthAttachment.storeAction=MTLStoreActionStore;
    pass.stencilAttachment.texture=depth; pass.stencilAttachment.loadAction=MTLLoadActionClear; pass.stencilAttachment.storeAction=MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:pipeline]; [e setDepthStencilState:ds]; [e setVertexBuffer:vb offset:0 atIndex:0];
    [e setVertexBytes:&u length:sizeof(u) atIndex:1]; [e setFragmentBytes:&u length:sizeof(u) atIndex:1];
    [e setFragmentTexture:sampled atIndex:0]; [e setFragmentTexture:palette atIndex:1]; [e setFragmentTexture:fog atIndex:2]; [e setFragmentSamplerState:sampler atIndex:0];
    [e drawIndexedPrimitives:MTLPrimitiveTypeTriangleStrip indexCount:3 indexType:MTLIndexTypeUInt32 indexBuffer:geometry.indices indexBufferOffset:0];
    [e endEncoding];
    id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    [b copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { std::fprintf(stderr, "Metal command failed: %s\n", cb.error.localizedDescription.UTF8String); return 2; }
    const auto *px = static_cast<const unsigned char *>(result.contents);
    const unsigned char expected[] = {0,0,255,255};
    for (int i=0;i<4;i++) if (std::abs(int(px[i])-int(expected[i])) > 1) {
        std::fprintf(stderr, "FAIL flat punch-through: RGBA=(%u,%u,%u,%u), expected (0,0,255,255)\n",px[0],px[1],px[2],px[3]); return 1;
    }
    auto checkQuad = [&](const MetalGeometryBuffers& geom, MTLPrimitiveType primitive, int indexCount, const char *label) {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline]; [encoder setDepthStencilState:ds];
        [encoder setVertexBuffer:geom.vertices offset:0 atIndex:0];
        [encoder setVertexBytes:&u length:sizeof(u) atIndex:1]; [encoder setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [encoder setFragmentTexture:sampled atIndex:0]; [encoder setFragmentTexture:palette atIndex:1]; [encoder setFragmentTexture:fog atIndex:2];
        [encoder setFragmentSamplerState:sampler atIndex:0];
        [encoder drawIndexedPrimitives:primitive indexCount:indexCount indexType:MTLIndexTypeUInt32 indexBuffer:geom.indices indexBufferOffset:0];
        [encoder endEncoding];
        id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
        [copy copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1) toBuffer:result destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
        [copy endEncoding]; [command commit]; [command waitUntilCompleted];
        const auto *pixels = static_cast<const unsigned char *>(result.contents);
        unsigned bottomLeft = pixels[3*256], topRight = pixels[3*4];
        if (command.status != MTLCommandBufferStatusCompleted || bottomLeft != 30 || topRight != 40) {
            std::fprintf(stderr, "FAIL %s: triangle colors are %u,%u, expected 30,40\n", label, bottomLeft, topRight); return false;
        }
        return true;
    };
    if (!checkQuad(stripGeometry, MTLPrimitiveTypeTriangleStrip, 4, "flat strip") ||
        !checkQuad(sortedGeometry, MTLPrimitiveTypeTriangle, 6, "sorted flat triangles")) return 1;
    std::puts("flat textured punch-through, strips, and sorted triangles use the PVR last vertex"); return 0;
} }
