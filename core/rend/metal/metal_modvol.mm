// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_modvol.h"
#include "metal_modvol_shaders.h"
#include <simd/simd.h>
#include <algorithm>
#include <cstring>

namespace {
struct alignas(16) ModUniforms {
    simd_float4x4 matrix;
    simd_float4 clip, values;
    simd_uint4 flags;
};

}

void MetalModifierVolumes::Init(id<MTLDevice> nativeDevice)
{
    device = nativeDevice;
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:metalModifierShaderSource]
                                               options:nil error:&error];
    if (!library)
        throw RendererException(std::string("Metal modifier shaders: ") + error.localizedDescription.UTF8String);
    for (int final = 0; final < 3; ++final) {
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:final ? @"shadowVertex" : @"modifierVertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"modifierFragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        descriptor.colorAttachments[0].writeMask = final == 1 ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
        descriptor.colorAttachments[0].blendingEnabled = final == 1;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline)
            throw RendererException(std::string("Metal modifier pipeline: ") + error.localizedDescription.UTF8String);
        if (final == 0) geometryPipeline = pipeline; else if (final == 1) finalPipeline = pipeline; else clearPipeline = pipeline;
    }
    for (int mode = 0; mode < ModeCount; ++mode) {
        MTLStencilDescriptor *stencil = [MTLStencilDescriptor new];
        stencil.stencilCompareFunction = mode < Inclusion ? MTLCompareFunctionAlways
            : mode == Inclusion ? MTLCompareFunctionLessEqual : MTLCompareFunctionEqual;
        stencil.readMask = mode == Final ? 0x81 : 3;
        stencil.writeMask = mode < Inclusion ? 2 : 3;
        stencil.stencilFailureOperation = mode >= Inclusion ? MTLStencilOperationZero : MTLStencilOperationKeep;
        stencil.depthFailureOperation = mode >= Inclusion ? MTLStencilOperationZero : MTLStencilOperationKeep;
        stencil.depthStencilPassOperation = mode == Xor ? MTLStencilOperationInvert
            : mode == Or || mode == Inclusion ? MTLStencilOperationReplace
            : mode == Final ? MTLStencilOperationZero : MTLStencilOperationKeep;
        MTLDepthStencilDescriptor *depth = [MTLDepthStencilDescriptor new];
        depth.depthWriteEnabled = NO;
        depth.depthCompareFunction = mode < Inclusion ? MTLCompareFunctionGreater : MTLCompareFunctionAlways;
        depth.frontFaceStencil = stencil;
        depth.backFaceStencil = stencil;
        states[mode] = [device newDepthStencilStateWithDescriptor:depth];
    }
}

void MetalModifierVolumes::Term()
{
    states.fill(nil);
    geometryPipeline = nil;
    finalPipeline = nil;
    clearPipeline = nil;
    device = nil;
}

void MetalModifierVolumes::Draw(id<MTLRenderCommandEncoder> encoder, const rend_context& context,
        TransformMatrix& matrices, int first, int count, int width, int height, bool finalize)
{
    if (!config::ModifierVolumes || count <= 0 || context.modtrig.empty()) return;
    id<MTLBuffer> vertices = [device newBufferWithBytes:context.modtrig.data()
        length:context.modtrig.size() * sizeof(ModTriangle) options:MTLResourceStorageModeShared];
    if (!vertices) throw RendererException("Metal modifier-volume allocation failed");
    [encoder setRenderPipelineState:geometryPipeline];
    [encoder setVertexBuffer:vertices offset:0 atIndex:0];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    ModUniforms uniforms {};
    std::memcpy(&uniforms.matrix, &matrices.GetNormalMatrix()[0][0], sizeof(uniforms.matrix));
    uniforms.flags.y = config::NativeDepthInterpolation;
    int base = -1;
    const auto bind = [&](Mode mode) {
        [encoder setDepthStencilState:states[mode]];
        [encoder setStencilReferenceValue:mode == Final ? 0x81 : mode == Or ? 2 : mode >= Inclusion ? 1 : 0];
    };
    for (int i = first; i < first + count; ++i) {
        const ModifierVolumeParam& param = context.global_param_mvo[i];
        if (!param.count) continue;
        if (param.isNaomi2()) throw RendererException("Native Metal does not yet support Naomi 2 modifier volumes");
        if (base == -1) base = param.first;
        Rect clip {};
        TileClipping tile = matrices.getTileClip(param.tileclip, clip);
        Rect scissor = matrices.getBaseScissor();
        if (tile == TileClipping::Outside) scissor = intersect(scissor, clip);
        int x = std::clamp(scissor.origin.x, 0, width), y = std::clamp(scissor.origin.y, 0, height);
        int right = std::clamp(scissor.origin.x + scissor.size.x, x, width);
        int bottom = std::clamp(scissor.origin.y + scissor.size.y, y, height);
        if (right <= x || bottom <= y) continue;
        [encoder setScissorRect:MTLScissorRect{(NSUInteger)x,(NSUInteger)y,(NSUInteger)(right-x),(NSUInteger)(bottom-y)}];
        uniforms.flags.x = tile == TileClipping::Inside;
        uniforms.clip = simd_make_float4(clip.origin.x, clip.origin.y,
            clip.origin.x + clip.size.x, clip.origin.y + clip.size.y);
        [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
        [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
        [encoder setCullMode:metalModvolCullMode(param.isp.CullMode)];
        bind(!param.isp.VolumeLast && param.isp.DepthMode > 0 ? Or : Xor);
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:param.first*3 vertexCount:param.count*3];
        if (param.isp.DepthMode == 1 || param.isp.DepthMode == 2) {
            bind(param.isp.DepthMode == 1 ? Inclusion : Exclusion);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:base*3
                        vertexCount:(param.first+param.count-base)*3];
            base = -1;
        }
    }
    if (!finalize) return;
    [encoder setRenderPipelineState:finalPipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setScissorRect:MTLScissorRect{0,0,(NSUInteger)width,(NSUInteger)height}];
    uniforms.flags.x = 0;
    uniforms.flags.y = 0; // Full-screen shadow quad does not use projected geometry.
    uniforms.values.x = 1.f - FPU_SHAD_SCALE.scale_factor / 256.f;
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    bind(Final);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

void MetalModifierVolumes::ClearStencil(id<MTLRenderCommandEncoder> encoder, int width, int height)
{
    ModUniforms uniforms {};
    [encoder setRenderPipelineState:clearPipeline];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setScissorRect:MTLScissorRect{0,0,(NSUInteger)width,(NSUInteger)height}];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:1];
    [encoder setDepthStencilState:states[Final]];
    [encoder setStencilReferenceValue:0x81];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}
