// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone: xcrun clang++ -std=c++17 -fobjc-arc -Icore tests/metal/modifier_shader_smoke.mm -framework Metal -framework Foundation -o /tmp/hollycast-metal-modifier-smoke
#include "rend/metal/metal_common.h"
#include "rend/metal/metal_modvol_shaders.h"
#include <simd/simd.h>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct alignas(16) Uniforms { simd_float4x4 matrix; simd_float4 clip, values; simd_uint4 flags; };

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "no Metal device");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError *error = nil;
        id<MTLLibrary> modifiers = [device newLibraryWithSource:[NSString stringWithUTF8String:metalModifierShaderSource] options:nil error:&error];
        if (!modifiers) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
        require(modifiers != nil, "modifier shader compile");
        NSString *sceneSource = @R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 sceneVertex(uint i [[vertex_id]]) {
    float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return float4(p[i], 0, 1);
}
struct F { float4 color [[color(0)]]; float depth [[depth(any)]]; };
fragment F sceneFragment() { return {float4(1), log2(1.0 + 100000.0 * 0.25) / 34.0}; }
)";
        id<MTLLibrary> scene = [device newLibraryWithSource:sceneSource options:nil error:&error];
        require(scene != nil, "scene shader compile");
        auto pipeline = [&](id<MTLFunction> vertex, id<MTLFunction> fragment, bool color, bool blend) {
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vertex; d.fragmentFunction = fragment;
            d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            d.colorAttachments[0].writeMask = color ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            d.colorAttachments[0].blendingEnabled = blend;
            d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
            d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
            id<MTLRenderPipelineState> result = [device newRenderPipelineStateWithDescriptor:d error:&error];
            if (!result) std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
            require(result != nil, "pipeline creation");
            return result;
        };
        id<MTLRenderPipelineState> scenePipeline = pipeline([scene newFunctionWithName:@"sceneVertex"], [scene newFunctionWithName:@"sceneFragment"], true, false);
        id<MTLRenderPipelineState> volumePipeline = pipeline([modifiers newFunctionWithName:@"modifierVertex"], [modifiers newFunctionWithName:@"modifierFragment"], false, false);
        id<MTLRenderPipelineState> shadowPipeline = pipeline([modifiers newFunctionWithName:@"shadowVertex"], [modifiers newFunctionWithName:@"modifierFragment"], true, true);
        auto state = [&](MTLCompareFunction depthCompare, bool depthWrite, MTLCompareFunction stencilCompare,
                         unsigned readMask, unsigned writeMask, MTLStencilOperation pass, MTLStencilOperation fail) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = stencilCompare; s.readMask = readMask; s.writeMask = writeMask;
            s.stencilFailureOperation = fail; s.depthFailureOperation = fail; s.depthStencilPassOperation = pass;
            MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
            d.depthCompareFunction = depthCompare; d.depthWriteEnabled = depthWrite;
            d.frontFaceStencil = s; d.backFaceStencil = s;
            return [device newDepthStencilStateWithDescriptor:d];
        };
        auto sceneState = state(MTLCompareFunctionAlways, true, MTLCompareFunctionAlways, 255, 128, MTLStencilOperationReplace, MTLStencilOperationKeep);
        auto orState = state(MTLCompareFunctionGreater, false, MTLCompareFunctionAlways, 3, 2, MTLStencilOperationReplace, MTLStencilOperationKeep);
        auto inclusionState = state(MTLCompareFunctionAlways, false, MTLCompareFunctionLessEqual, 3, 3, MTLStencilOperationReplace, MTLStencilOperationZero);
        auto finalState = state(MTLCompareFunctionAlways, false, MTLCompareFunctionEqual, 0x81, 3, MTLStencilOperationZero, MTLStencilOperationZero);
        MTLTextureDescriptor *colorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        colorDesc.storageMode = MTLStorageModePrivate; colorDesc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> color = [device newTextureWithDescriptor:colorDesc];
        MTLTextureDescriptor *depthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:4 height:4 mipmapped:NO];
        depthDesc.storageMode = MTLStorageModePrivate; depthDesc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> depth = [device newTextureWithDescriptor:depthDesc];
        require(color && depth, "framebuffer allocation");
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color; pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
        pass.depthAttachment.texture = depth; pass.depthAttachment.loadAction = MTLLoadActionClear; pass.depthAttachment.clearDepth = 0;
        pass.stencilAttachment.texture = depth; pass.stencilAttachment.loadAction = MTLLoadActionClear; pass.stencilAttachment.clearStencil = 0;
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:scenePipeline]; [encoder setDepthStencilState:sceneState]; [encoder setStencilReferenceValue:0x80];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        const float vertices[] = { -1,-1,1, 3,-1,1, -1,3,1 };
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
        Uniforms uniforms{}; uniforms.matrix = matrix_identity_float4x4; uniforms.values.x = 0.5f;
        [encoder setRenderPipelineState:volumePipeline]; [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1]; [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
        [encoder setDepthStencilState:orState]; [encoder setStencilReferenceValue:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder setDepthStencilState:inclusionState]; [encoder setStencilReferenceValue:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder setRenderPipelineState:shadowPipeline]; [encoder setDepthStencilState:finalState]; [encoder setStencilReferenceValue:0x81];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];
        id<MTLBuffer> readback = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(4,4,1)
                    toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024];
        [blit endEncoding]; [commands commit]; [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) std::fprintf(stderr, "%s\n", commands.error.localizedDescription.UTF8String);
        require(commands.status == MTLCommandBufferStatusCompleted, "GPU command failed");
        const uint8_t *pixel = static_cast<const uint8_t *>(readback.contents);
        require(pixel[0] >= 126 && pixel[0] <= 129 && pixel[1] == pixel[0] && pixel[2] == pixel[0], "modifier shadow failed depth/stencil test");
        std::puts("Metal modifier depth/stencil smoke passed");
    }
}
