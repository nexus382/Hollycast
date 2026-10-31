// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "hw/pvr/Renderer_if.h"
#include "rend/sorter.h"
#include "metal_common.h"

// PVR face selection in the Metal viewport coordinate system.
inline MTLCullMode metalCullMode(unsigned ispCullMode)
{
    const unsigned cull = ispCullMode;
    return cull < 2 ? MTLCullModeNone : cull == 2 ? MTLCullModeFront : MTLCullModeBack;
}

// Upload the parsed TA scene's actual geometry to the command queue's device.
struct MetalGeometryBuffers {
    id<MTLBuffer> vertices;
    id<MTLBuffer> indices;
};
inline MetalGeometryBuffers metalUploadGeometry(id<MTLDevice> device, rend_context& context)
{
    // PVR flat shading takes colors from the last vertex; Metal uses the first.
    // The shared conversion also rotates sorted triangles without changing winding.
    setFirstProvokingVertex(context);
    MetalGeometryBuffers result {
        [device newBufferWithBytes:context.verts.data() length:context.verts.size() * sizeof(Vertex) options:MTLResourceStorageModeShared],
        [device newBufferWithBytes:context.idx.data() length:context.idx.size() * sizeof(u32) options:MTLResourceStorageModeShared]
    };
    if (!result.vertices || !result.indices)
        throw RendererException("Metal geometry buffer allocation failed");
    return result;
}
