// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// xcrun clang++ -std=c++17 -fobjc-arc -Icore tests/metal/target_preservation_regression.mm -framework Metal -framework Foundation -o /tmp/metal-target-preservation
#include "rend/metal/metal_targets.h"
#include <cstdio>
#include <cstdlib>

static void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static id<MTLRenderPipelineState> bluePipeline(id<MTLDevice> device) {
    NSString *source = @R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 v(uint i [[vertex_id]]) {
    float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1);
}
fragment float4 f() { return float4(0,0,1,1); }
)";
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    require(library != nil, "shader compilation");
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:@"v"]; d.fragmentFunction = [library newFunctionWithName:@"f"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> p = [device newRenderPipelineStateWithDescriptor:d error:&error];
    require(p != nil, "pipeline creation"); return p;
}
static void encodePass(id<MTLCommandBuffer> command, MetalRenderTargets& target, bool fresh,
        MTLClearColor color, id<MTLRenderPipelineState> blue = nil) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = target.output;
    p.colorAttachments[0].loadAction = fresh ? MTLLoadActionClear : MTLLoadActionLoad;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = color;
    p.depthAttachment.texture = target.depth; p.depthAttachment.loadAction = MTLLoadActionClear;
    p.stencilAttachment.texture = target.depth; p.stencilAttachment.loadAction = MTLLoadActionClear;
    id<MTLRenderCommandEncoder> e = [command renderCommandEncoderWithDescriptor:p];
    require(e != nil, "render encoder creation");
    if (blue) {
        [e setRenderPipelineState:blue]; [e setScissorRect:MTLScissorRect{0,0,1,1}];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    [e endEncoding];
}
static bool run(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline, bool rtt) {
    MetalRenderTargets target;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    bool fresh = target.begin(device, 4, 4, false);
    require(fresh && target.output && target.depth, "screen allocation");
    encodePass(command, target, fresh, MTLClearColorMake(1,0,0,1));
    if (rtt) {
        fresh = target.begin(device, 4, 4, true);
        require(fresh && target.output && target.depth, "RTT allocation");
        encodePass(command, target, fresh, MTLClearColorMake(0,1,0,1));
        target.end(true);
    } else {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> upload = [device newTextureWithDescriptor:d];
        unsigned char pixels[4*4*4] = {};
        [upload replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
        target.setFramebuffer(upload);
        require(target.frameOutput == upload, "framebuffer image is retained for presentation");
    }
    fresh = target.begin(device, 4, 4, false);
    require(target.output && target.depth, "screen resume allocation");
    encodePass(command, target, fresh, MTLClearColorMake(0,0,0,1), pipeline);
    id<MTLBuffer> pixels = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:target.output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
              sourceSize:MTLSizeMake(4,4,1) toBuffer:pixels destinationOffset:0
              destinationBytesPerRow:256 destinationBytesPerImage:1024];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "GPU command");
    const auto *bytes = static_cast<const unsigned char *>(pixels.contents);
    const bool preserved = bytes[4] == 255 && bytes[5] == 0 && bytes[6] == 0;
    const bool drawn = bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 255;
    if (!preserved || !drawn)
        std::fprintf(stderr, "FAIL %s resume: untouched pixel=(%u,%u,%u), edited=(%u,%u,%u), fresh=%d\n",
                rtt ? "RTT" : "framebuffer upload", bytes[4], bytes[5], bytes[6], bytes[0], bytes[1], bytes[2], fresh);
    return preserved && drawn;
}
static void runResize(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    MetalRenderTargets target;
    id<MTLCommandBuffer> command = [queue commandBuffer];
    bool fresh = target.begin(device, 4, 4, false);
    require(fresh && target.output.width == 4 && target.output.height == 4, "initial 4x4 target");
    encodePass(command, target, fresh, MTLClearColorMake(1,0,0,1));
    target.frameOutput = target.output;

    fresh = target.begin(device, 3, 2, true);
    require(fresh && target.output.width == 3 && target.output.height == 2, "first RTT target size");
    encodePass(command, target, fresh, MTLClearColorMake(1,1,0,1)); target.end(true);
    fresh = target.begin(device, 8, 8, false);
    require(fresh && target.output.width == 8 && target.output.height == 8
            && target.depth.width == 8 && target.depth.height == 8, "grow creates matching color/depth");
    encodePass(command, target, fresh, MTLClearColorMake(0,0,1,1));
    id<MTLTexture> grown = target.output;

    fresh = target.begin(device, 2, 3, true);
    require(fresh && target.output.width == 2 && target.output.height == 3, "second RTT target size");
    encodePass(command, target, fresh, MTLClearColorMake(1,1,0,1)); target.end(true);
    fresh = target.begin(device, 4, 4, false);
    require(fresh && target.output.width == 4 && target.output.height == 4
            && target.depth.width == 4 && target.depth.height == 4, "shrink creates matching color/depth");
    require(target.output != target.frameOutput && target.output != grown, "shrink allocates a new target");
    encodePass(command, target, fresh, MTLClearColorMake(0,1,0,1));
    id<MTLBuffer> oldPixels = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLBuffer> grownPixels = [device newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    id<MTLBuffer> shrunkPixels = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    auto copy = [&](id<MTLTexture> texture, id<MTLBuffer> buffer, int size) {
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                  sourceSize:MTLSizeMake(size,size,1) toBuffer:buffer destinationOffset:0
                  destinationBytesPerRow:256 destinationBytesPerImage:256*size];
    };
    copy(target.frameOutput, oldPixels, 4);
    copy(grown, grownPixels, 8);
    copy(target.output, shrunkPixels, 4);
    [blit endEncoding];
    // Encoded commands must keep the presented frame alive even if the owner
    // drops its reference before submission/completion.
    target.clear(); grown = nil;
    [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "resize GPU command");
    auto check = [](id<MTLBuffer> buffer, int size, unsigned char r, unsigned char g, unsigned char b, const char *message) {
        const auto *pixels = static_cast<const unsigned char *>(buffer.contents);
        const unsigned char *corners[] = {pixels, pixels + (size-1)*256 + (size-1)*4};
        for (const auto *pixel : corners)
            require(pixel[0] == r && pixel[1] == g && pixel[2] == b && pixel[3] == 255, message);
    };
    check(oldPixels, 4, 255,0,0, "previous presented frame survives queued resize");
    check(grownPixels, 8, 0,0,255, "grown target corners contain new frame");
    check(shrunkPixels, 4, 0,255,0, "shrunk target corners contain new frame");
}
int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); require(device != nil, "no Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue]; id<MTLRenderPipelineState> pipeline = bluePipeline(device);
    bool rtt = run(device, queue, pipeline, true);
    bool framebuffer = run(device, queue, pipeline, false);
    if (!rtt || !framebuffer) return 1;
    runResize(device, queue);
    std::puts("Metal screen target survives RTT, framebuffer upload and resize"); return 0;
} }
