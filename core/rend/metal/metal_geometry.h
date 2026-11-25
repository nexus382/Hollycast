// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "hw/pvr/Renderer_if.h"
#include "rend/sorter.h"
#include "metal_common.h"
#include <array>
#include <cstring>
#include <memory>

// PVR face selection in the Metal viewport coordinate system.
inline MTLCullMode metalCullMode(unsigned ispCullMode)
{
    const unsigned cull = ispCullMode;
    return cull < 2 ? MTLCullModeNone : cull == 2 ? MTLCullModeFront : MTLCullModeBack;
}

// Upload the parsed TA scene's actual geometry to the command queue's device.
struct MetalGeometryBuffers {
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> indices = nil;
    id<MTLBuffer> modifiers = nil;
};
inline MetalGeometryBuffers metalUploadGeometry(id<MTLDevice> device, rend_context& context,
        MetalGeometryBuffers result = {}, bool uploadModifiers = false)
{
    // PVR flat shading takes colors from the last vertex; Metal uses the first.
    // The shared conversion also rotates sorted triangles without changing winding.
    setFirstProvokingVertex(context);
    const size_t vertexBytes = context.verts.size() * sizeof(Vertex);
    const size_t indexBytes = context.idx.size() * sizeof(u32);
    if (!result.vertices || result.vertices.length < vertexBytes)
        result.vertices = [device newBufferWithLength:vertexBytes options:MTLResourceStorageModeShared];
    if (!result.indices || result.indices.length < indexBytes)
        result.indices = [device newBufferWithLength:indexBytes options:MTLResourceStorageModeShared];
    if (!result.vertices || !result.indices)
        throw RendererException("Metal geometry buffer allocation failed");
    if (vertexBytes) std::memcpy(result.vertices.contents, context.verts.data(), vertexBytes);
    if (indexBytes) std::memcpy(result.indices.contents, context.idx.data(), indexBytes);
    if (uploadModifiers && !context.modtrig.empty()) {
        const size_t bytes = context.modtrig.size() * sizeof(ModTriangle);
        if (!result.modifiers || result.modifiers.length < bytes)
            result.modifiers = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!result.modifiers) throw RendererException("Metal modifier geometry allocation failed");
        std::memcpy(result.modifiers.contents, context.modtrig.data(), bytes);
    }
    return result;
}

class MetalGeometryUploadPool {
public:
    using Upload = std::shared_ptr<MetalGeometryBuffers>;

    Upload acquire(id<MTLDevice> device, rend_context& context, bool uploadModifiers)
    {
        for (Upload& slot : slots) {
            if (slot && slot.use_count() != 1) continue;
            if (!slot) slot = std::make_shared<MetalGeometryBuffers>();
            *slot = metalUploadGeometry(device, context, *slot, uploadModifiers);
            return slot;
        }
        return std::make_shared<MetalGeometryBuffers>(metalUploadGeometry(device, context, {}, uploadModifiers));
    }

    MetalGeometryBuffers upload(id<MTLDevice> device, rend_context& context, id<MTLCommandBuffer> command,
            bool uploadModifiers = false)
    {
        if (!command) throw RendererException("Metal geometry upload requires a command buffer");
        Upload geometry = acquire(device, context, uploadModifiers);
        __block Upload pending = geometry;
        [command addCompletedHandler:^(id<MTLCommandBuffer>) { pending.reset(); }];
        return *geometry;
    }
    void clear() { slots = {}; }
private:
    std::array<Upload, 3> slots;
};
