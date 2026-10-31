// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "rend/transform_matrix.h"
#include "metal_common.h"
#include <cstdint>
#include <cstddef>
#include <functional>

// Layout matches OITControl/OITNode in metal_oit_shaders.mm.
struct MetalOITControl {
    uint32_t count, overflow, layerOverflow, capacity, width, height, maxLayers;
    float shadowScale;
};
struct MetalOITNode {
    uint32_t color0, color1;
    float depth;
    uint32_t next, sequence, primitive, tsp0, tsp1, flags;
};
static_assert(sizeof(MetalOITControl) == 32 && sizeof(MetalOITNode) == 36, "OIT buffer layout");

class MetalOITBuffers {
public:
    void init(id<MTLDevice> device, int width, int height, size_t nodeBudgetBytes, unsigned maxLayers, float shadowScale);
    void clear(id<MTLCommandBuffer> command);
    void resolve(id<MTLCommandBuffer> command, id<MTLTexture> input, id<MTLTexture> output,
            id<MTLTexture> secondaryInput, id<MTLTexture> secondaryOutput);
    void modifiers(id<MTLCommandQueue> queue, id<MTLTexture> color, id<MTLTexture> depth,
            const rend_context& context, TransformMatrix& matrices, const ModifierVolumeParam *params,
            int first, int count, const std::function<void(id<MTLCommandBuffer>)>& track = {});
    // Call only after the command buffer that wrote the counters completes.
    bool overflowed() const;
    bool layersExceeded() const;
    bool grow();
    id<MTLBuffer> nodes() const { return nodeBuffer; }
    id<MTLBuffer> heads() const { return headBuffer; }
    id<MTLBuffer> control() const { return controlBuffer; }
private:
    id<MTLDevice> device = nil;
    id<MTLBuffer> nodeBuffer = nil, headBuffer = nil, controlBuffer = nil;
    id<MTLComputePipelineState> clearPipeline = nil, resolvePipeline = nil;
    id<MTLRenderPipelineState> modifierPipeline = nil;
    size_t maxCapacity = 0, configuredBudget = 0;
    MetalOITControl *state() const { return static_cast<MetalOITControl *>(controlBuffer.contents); }
};

// Copy one aspect of combined depth/stencil between distinct render targets.
void metalOITCopyDepthStencilAspect(id<MTLDevice> device, id<MTLCommandBuffer> command,
        id<MTLTexture> source, id<MTLTexture> destination, int width, int height, bool stencil);
