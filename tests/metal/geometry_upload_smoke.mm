// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "rend/metal/metal_geometry.h"
#include <cstdio>
#include <cstdlib>

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    require(device != nil, "no Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    rend_context scene {};
    scene.verts.resize(3); scene.idx = {0, 1, 2};
    scene.verts[0].x = 11;
    MetalGeometryUploadPool pool;
    id<MTLCommandBuffer> commands[3];
    MetalGeometryBuffers frames[3];
    for (unsigned i = 0; i < 3; ++i) {
        commands[i] = [queue commandBuffer];
        frames[i] = pool.upload(device, scene, commands[i]);
        for (unsigned previous = 0; previous < i; ++previous)
            require(frames[i].vertices != frames[previous].vertices, "pending uploads keep distinct buffers");
    }
    scene.verts[0].x = 22;
    id<MTLCommandBuffer> overflow = [queue commandBuffer];
    MetalGeometryBuffers transient = pool.upload(device, scene, overflow);
    for (const auto& frame : frames) {
        require(transient.vertices != frame.vertices, "busy pool uses a transient upload");
        require(static_cast<const Vertex *>(frame.vertices.contents)[0].x == 11, "pending geometry stays unchanged");
    }
    id<MTLBuffer> readback = [device newBufferWithLength:sizeof(Vertex) options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> blit = [commands[0] blitCommandEncoder];
    [blit copyFromBuffer:frames[0].vertices sourceOffset:0 toBuffer:readback destinationOffset:0 size:sizeof(Vertex)];
    [blit endEncoding]; [commands[0] commit]; [commands[0] waitUntilCompleted];
    require(commands[0].status == MTLCommandBufferStatusCompleted, "geometry GPU readback completed");
    [commands[1] commit]; [commands[2] commit]; [overflow commit];
    [overflow waitUntilCompleted];
    require(static_cast<const Vertex *>(readback.contents)[0].x == 11, "GPU sees the original pending geometry");
    id<MTLCommandBuffer> reuseCommand = [queue commandBuffer];
    MetalGeometryBuffers reused = pool.upload(device, scene, reuseCommand);
    bool reusedSlot = false;
    for (const auto& frame : frames) reusedSlot |= reused.vertices == frame.vertices;
    require(reusedSlot, "completed uploads reuse a retained buffer");
    require(static_cast<const Vertex *>(reused.vertices.contents)[0].x == 22, "reused buffer receives new geometry");
    [reuseCommand commit]; [reuseCommand waitUntilCompleted];
    pool.clear();
    MetalGeometryBuffers abandoned;
    @autoreleasepool {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        abandoned = pool.upload(device, scene, command);
    }
    id<MTLCommandBuffer> afterAbandon = [queue commandBuffer];
    MetalGeometryBuffers recovered = pool.upload(device, scene, afterAbandon);
    require(recovered.vertices == abandoned.vertices, "abandoned command releases its upload slot");
    [afterAbandon commit]; [afterAbandon waitUntilCompleted];
    std::puts("Metal geometry upload pooling smoke passed");
} }
