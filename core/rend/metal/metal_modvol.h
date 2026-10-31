// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "rend/transform_matrix.h"
#include "metal_common.h"
#include <array>

inline MTLCullMode metalModvolCullMode(unsigned ispCullMode)
{
    // Metal's viewport reverses the winding relative to the GL modifier path.
    const unsigned cull = ispCullMode ^ 1;
    return cull < 2 ? MTLCullModeNone : cull == 2 ? MTLCullModeFront : MTLCullModeBack;
}

class MetalModifierVolumes
{
public:
    void Init(id<MTLDevice> device);
    void Term();
    void Draw(id<MTLRenderCommandEncoder> encoder, const rend_context& context,
              TransformMatrix& matrices, int first, int count, int width, int height, bool finalize = true);
    void ClearStencil(id<MTLRenderCommandEncoder> encoder, int width, int height);
private:
    enum Mode { Xor, Or, Inclusion, Exclusion, Final, ModeCount };
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> geometryPipeline = nil, finalPipeline = nil, clearPipeline = nil;
    std::array<id<MTLDepthStencilState>, ModeCount> states {};
};
