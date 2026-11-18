// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "hw/pvr/Renderer_if.h"
#include "rend/sorter.h"
#include "metal_common.h"
#include <array>
#include <atomic>
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
};
inline MetalGeometryBuffers metalUploadGeometry(id<MTLDevice> device, rend_context& context,
        MetalGeometryBuffers result = {})
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
    return result;
}

class MetalGeometryUploadPool {
    struct Slot {
        MetalGeometryBuffers buffers;
        std::weak_ptr<std::atomic<bool>> pending;
    };
    std::array<Slot, 3> slots;
public:
    MetalGeometryBuffers upload(id<MTLDevice> device, rend_context& context, id<MTLCommandBuffer> command)
    {
        if (!command) throw RendererException("Metal geometry upload requires a command buffer");
        for (Slot& slot : slots) {
            auto pending = slot.pending.lock();
            if (pending && pending->load(std::memory_order_acquire)) continue;
            slot.buffers = metalUploadGeometry(device, context, slot.buffers);
            auto token = std::make_shared<std::atomic<bool>>(true);
            // An uncommitted command releases its token when encoding unwinds.
            [command addCompletedHandler:^(id<MTLCommandBuffer>) {
                token->store(false, std::memory_order_release);
            }];
            slot.pending = token;
            return slot.buffers;
        }
        return metalUploadGeometry(device, context);
    }
    void clear() { slots = {}; }
};
