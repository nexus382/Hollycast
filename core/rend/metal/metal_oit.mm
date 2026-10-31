// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "hw/pvr/Renderer_if.h"
#include "metal_oit.h"
#include "metal_oit_shaders.h"
#include "metal_modvol.h"
#include "metal_modvol_shaders.h"
#include <simd/simd.h>
#include <cstring>
#include <algorithm>
#include <limits>

void MetalOITBuffers::init(id<MTLDevice> dev, int width, int height, size_t nodeBudgetBytes, unsigned maxLayers, float shadowScale)
{
    if (!dev || width <= 0 || height <= 0 || (uint64_t)width * height > std::numeric_limits<uint32_t>::max())
        throw RendererException("Metal per-pixel buffer dimensions are invalid");
    const unsigned layers = std::clamp(maxLayers, 1u, 256u);
    if (device == dev && controlBuffer && state()->width == (uint32_t)width && state()->height == (uint32_t)height
            && configuredBudget == nodeBudgetBytes) {
        state()->maxLayers = layers;
        state()->shadowScale = shadowScale;
        return;
    }
    device = dev;
    configuredBudget = nodeBudgetBytes;
    maxCapacity = std::min({nodeBudgetBytes / sizeof(MetalOITNode), (size_t)dev.maxBufferLength / sizeof(MetalOITNode), (size_t)UINT32_MAX});
    if (maxCapacity == 0 || (uint64_t)width * height * sizeof(uint32_t) > dev.maxBufferLength)
        throw RendererException("Metal per-pixel buffer budget or device limit is too small");
    if (!clearPipeline || !resolvePipeline || !modifierPipeline) {
    NSError *error = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:metalOITShaderSource] options:nil error:&error];
    if (!lib) throw RendererException(metalErrorMessage("Metal per-pixel shader compilation failed", error));
    clearPipeline = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"oitClear"] error:&error];
    resolvePipeline = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"oitResolve"] error:&error];
    if (!clearPipeline || !resolvePipeline) throw RendererException(metalErrorMessage("Metal per-pixel compute pipeline failed", error));
    std::string modifierSource = std::string(metalOITShaderSource) + metalModifierShaderSource + metalOITModifierShaderSource;
    id<MTLLibrary> modifierLibrary = [dev newLibraryWithSource:[NSString stringWithUTF8String:modifierSource.c_str()] options:nil error:&error];
    if (!modifierLibrary) throw RendererException(metalErrorMessage("Metal translucent modifier shader compilation failed", error));
    MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [modifierLibrary newFunctionWithName:@"modifierVertex"];
    p.fragmentFunction = [modifierLibrary newFunctionWithName:@"oitModifier"];
    p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; p.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    p.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8; p.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    modifierPipeline = [dev newRenderPipelineStateWithDescriptor:p error:&error];
    if (!modifierPipeline) throw RendererException(metalErrorMessage("Metal translucent modifier pipeline failed", error));
    }
    headBuffer = [dev newBufferWithLength:(size_t)width * height * sizeof(uint32_t) options:MTLResourceStorageModeShared];
    controlBuffer = [dev newBufferWithLength:sizeof(MetalOITControl) options:MTLResourceStorageModeShared];
    if (!headBuffer || !controlBuffer) throw RendererException("Metal per-pixel head buffer allocation failed");
    size_t initial = std::min(maxCapacity, std::max<size_t>((size_t)width * height * 2, 4096));
    nodeBuffer = [dev newBufferWithLength:initial * sizeof(MetalOITNode) options:MTLResourceStorageModeShared];
    if (!nodeBuffer) throw RendererException("Metal per-pixel fragment buffer allocation failed");
    *state() = {0, 0, 0, (uint32_t)initial, (uint32_t)width, (uint32_t)height, layers, shadowScale};
}
void MetalOITBuffers::clear(id<MTLCommandBuffer> command)
{
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) throw RendererException("Metal per-pixel clear encoder failed");
    [encoder setComputePipelineState:clearPipeline];
    [encoder setBuffer:headBuffer offset:0 atIndex:0]; [encoder setBuffer:controlBuffer offset:0 atIndex:1];
    NSUInteger count = (NSUInteger)state()->width * state()->height;
    NSUInteger groupSize = std::min<NSUInteger>(clearPipeline.maxTotalThreadsPerThreadgroup, 256);
    [encoder dispatchThreadgroups:MTLSizeMake((count + groupSize - 1) / groupSize, 1, 1)
           threadsPerThreadgroup:MTLSizeMake(groupSize, 1, 1)];
    [encoder endEncoding];
}
void MetalOITBuffers::resolve(id<MTLCommandBuffer> command, id<MTLTexture> input, id<MTLTexture> output,
        id<MTLTexture> secondaryInput, id<MTLTexture> secondaryOutput)
{
    if (!input || !output || !secondaryInput || !secondaryOutput || input == output || secondaryInput == secondaryOutput) throw RendererException("Metal per-pixel resolve requires distinct color targets");
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    if (!encoder) throw RendererException("Metal per-pixel resolve encoder failed");
    [encoder setComputePipelineState:resolvePipeline];
    [encoder setTexture:input atIndex:0]; [encoder setTexture:output atIndex:1];
    [encoder setTexture:secondaryInput atIndex:2]; [encoder setTexture:secondaryOutput atIndex:3];
    [encoder setBuffer:headBuffer offset:0 atIndex:0]; [encoder setBuffer:nodeBuffer offset:0 atIndex:1]; [encoder setBuffer:controlBuffer offset:0 atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(((NSUInteger)state()->width + 7) / 8, ((NSUInteger)state()->height + 7) / 8, 1)
           threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [encoder endEncoding];
}
bool MetalOITBuffers::overflowed() const { return state()->overflow != 0; }
bool MetalOITBuffers::layersExceeded() const { return state()->layerOverflow != 0; }
bool MetalOITBuffers::grow()
{
    size_t old = state()->capacity;
    size_t next = std::min(maxCapacity, std::max(old + 1, old * 2));
    if (next <= old) return false;
    id<MTLBuffer> replacement = [device newBufferWithLength:next * sizeof(MetalOITNode) options:MTLResourceStorageModeShared];
    if (!replacement) return false;
    nodeBuffer = replacement;
    state()->capacity = (uint32_t)next;
    return true;
}

void MetalOITBuffers::modifiers(id<MTLCommandQueue> queue, id<MTLTexture> color, id<MTLTexture> depth,
        const rend_context& context, TransformMatrix& matrices, const ModifierVolumeParam *params, int first, int count,
        const std::function<void(id<MTLCommandBuffer>)>& track)
{
    if (count <= 0 || context.modtrig.empty()) return;
    struct alignas(16) ModUniforms { simd_float4x4 matrix; simd_float4 clip, values; simd_uint4 flags; };
    id<MTLBuffer> vertices = [device newBufferWithBytes:context.modtrig.data()
        length:context.modtrig.size() * sizeof(ModTriangle) options:MTLResourceStorageModeShared];
    if (!vertices) throw RendererException("Metal per-pixel modifier geometry allocation failed");
    ModUniforms u {};
    std::memcpy(&u.matrix, &matrices.GetNormalMatrix()[0][0], sizeof(u.matrix));
    u.flags.y = config::NativeDepthInterpolation;
    auto drawVolume = [&](const ModifierVolumeParam& param, unsigned mode, u32 start, u32 triangles) {
        if (param.isNaomi2()) throw RendererException("Metal per-pixel Naomi 2 modifier volume is not supported");
        Rect clip {glm::ivec2(0),glm::ivec2(0)};
        TileClipping tile = matrices.getTileClip(param.tileclip,clip);
        Rect scissor = matrices.getBaseScissor();
        if (tile == TileClipping::Outside) scissor = intersect(scissor,clip);
        int x=std::clamp(scissor.origin.x,0,(int)state()->width), y=std::clamp(scissor.origin.y,0,(int)state()->height);
        int right=std::clamp(scissor.origin.x+scissor.size.x,x,(int)state()->width), bottom=std::clamp(scissor.origin.y+scissor.size.y,y,(int)state()->height);
        if (right==x || bottom==y) return;
        u.flags.x = tile == TileClipping::Inside;
        u.clip = simd_make_float4(clip.origin.x,clip.origin.y,clip.origin.x+clip.size.x,clip.origin.y+clip.size.y);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture=color; pass.colorAttachments[0].loadAction=MTLLoadActionLoad; pass.colorAttachments[0].storeAction=MTLStoreActionStore;
        pass.depthAttachment.texture=depth; pass.depthAttachment.loadAction=MTLLoadActionLoad; pass.depthAttachment.storeAction=MTLStoreActionStore;
        pass.stencilAttachment.texture=depth; pass.stencilAttachment.loadAction=MTLLoadActionLoad; pass.stencilAttachment.storeAction=MTLStoreActionStore;
        id<MTLRenderCommandEncoder> enc=[command renderCommandEncoderWithDescriptor:pass];
        [enc setRenderPipelineState:modifierPipeline]; [enc setVertexBuffer:vertices offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:1]; [enc setFragmentBytes:&u length:sizeof(u) atIndex:1];
        [enc setFragmentBytes:&mode length:sizeof(mode) atIndex:2];
        [enc setFragmentBuffer:nodeBuffer offset:0 atIndex:3]; [enc setFragmentBuffer:headBuffer offset:0 atIndex:4]; [enc setFragmentBuffer:controlBuffer offset:0 atIndex:5];
        [enc setFrontFacingWinding:MTLWindingCounterClockwise]; [enc setCullMode:metalModvolCullMode(param.isp.CullMode)];
        [enc setScissorRect:MTLScissorRect{(NSUInteger)x,(NSUInteger)y,(NSUInteger)(right-x),(NSUInteger)(bottom-y)}];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:start*3 vertexCount:triangles*3];
        [enc endEncoding]; if (track) track(command); [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) throw RendererException(metalErrorMessage("Metal per-pixel modifier pass failed",command.error));
    };
    int base=-1;
    for (int i=first;i<first+count;i++) {
        const ModifierVolumeParam& p=params[i]; if (!p.count) continue;
        if (p.first + p.count > context.modtrig.size()) throw RendererException("Metal per-pixel modifier range is invalid");
        if (base<0) base=p.first;
        unsigned mode=(!p.isp.VolumeLast && p.isp.DepthMode>0) ? 1 : 0;
        drawVolume(p,mode,p.first,p.count);
        if (p.isp.DepthMode==1 || p.isp.DepthMode==2) {
            drawVolume(p,p.isp.DepthMode==1 ? 2 : 3,base,p.first+p.count-base);
            base=-1;
        }
    }
}

void metalOITCopyDepthStencilAspect(id<MTLDevice> device, id<MTLCommandBuffer> command,
        id<MTLTexture> source, id<MTLTexture> destination, int width, int height, bool stencil)
{
    size_t row = ((size_t)width * (stencil ? 1 : 4) + 255) & ~size_t(255);
    id<MTLBuffer> buffer = [device newBufferWithLength:row * height options:MTLResourceStorageModePrivate];
    if (!buffer) throw RendererException("Metal per-pixel depth/stencil copy allocation failed");
    MTLBlitOption option = stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:row*height options:option];
    [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:row*height
            sourceSize:MTLSizeMake(width,height,1) toTexture:destination destinationSlice:0 destinationLevel:0
            destinationOrigin:MTLOriginMake(0,0,0) options:option];
    [blit endEncoding];
}
