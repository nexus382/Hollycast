// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone: xcrun clang++ -std=c++17 -fobjc-arc -Icore tests/metal/texture_upload_smoke.mm -framework Metal -framework Foundation -o /tmp/hollycast-metal-texture-smoke
#include "rend/metal/metal_texture_upload.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

static std::vector<uint8_t> readLevel(id<MTLDevice> device, id<MTLCommandQueue> queue,
        id<MTLTexture> texture, unsigned level, unsigned bpp) {
    unsigned w = std::max(1u, unsigned(texture.width) >> level);
    unsigned h = std::max(1u, unsigned(texture.height) >> level);
    size_t row = (w * bpp + 255) & ~size_t(255);
    id<MTLBuffer> out = [device newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [blit endEncoding]; [command commit]; [command waitUntilCompleted];
    require(command.status == MTLCommandBufferStatusCompleted, "GPU readback command failed");
    std::vector<uint8_t> pixels(w * h * bpp);
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(pixels.data() + y * w * bpp, static_cast<const uint8_t *>(out.contents) + y * row, w * bpp);
    return pixels;
}

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "no Metal device");
        id<MTLCommandQueue> queue = [device newCommandQueue];
        using namespace metal_texture_upload;
        // Dreamcast decoded mip layout is 1x1, 2x2, 4x4 (smallest first).
        std::vector<uint8_t> mips(21 * 4);
        for (size_t i = 0; i < 21; ++i) {
            mips[i*4] = i < 1 ? 11 : i < 5 ? 22 : 33;
            mips[i*4+3] = 255;
        }
        id<MTLTexture> tex = decoded(device, queue, 4, 4, mips.data(), PixelKind::Rgba8, true, true);
        require(tex && tex.mipmapLevelCount == 3, "decoded mip allocation");
        for (unsigned level = 0; level < 3; ++level) {
            auto bytes = readLevel(device, queue, tex, level, 4);
            const uint8_t expected = level == 0 ? 33 : level == 1 ? 22 : 11;
            for (size_t i = 0; i < bytes.size(); i += 4)
                require(bytes[i] == expected && bytes[i+3] == 255, "decoded mip content");
        }
        id<MTLTexture> previous = tex;
        std::vector<uint8_t> solid(4 * 4 * 4, 64);
        tex = decoded(device, queue, 4, 4, solid.data(), PixelKind::Rgba8, true, false);
        require(tex && readLevel(device, queue, tex, 2, 4)[0] == 64, "generated mip content");
        require(readLevel(device, queue, previous, 0, 4)[0] == 33, "previous resource remains unchanged");
        uint16_t packed[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
        id<MTLTexture> packedTexture = decoded(device, queue, 2, 2,
                reinterpret_cast<const uint8_t *>(packed), PixelKind::Rgb565, false, false);
        require(packedTexture && readLevel(device, queue, packedTexture, 0, 4) ==
                std::vector<uint8_t>({255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255}), "packed 565 expansion");
        uint8_t indices[] = { 2, 7, 9, 15 };
        tex = decoded(device, queue, 2, 2, indices, PixelKind::Index8, false, false);
        require(tex && readLevel(device, queue, tex, 0, 1) == std::vector<uint8_t>(indices, indices+4), "indexed upload");
        PreparedCustomTexture customTexture;
        customTexture.width = customTexture.height = 2;
        customTexture.bytes = { 1,2,3,4, 5,6,7,8, 9,10,11,12, 13,14,15,16, 17,18,19,20 };
        customTexture.levels = { {2,2,2,2,0,16,8}, {1,1,1,1,16,4,4} };
        tex = custom(device, queue, customTexture, true);
        require(tex && readLevel(device, queue, tex, 0, 4) == std::vector<uint8_t>(customTexture.bytes.begin(), customTexture.bytes.begin()+16), "custom base level");
        require(readLevel(device, queue, tex, 1, 4) == std::vector<uint8_t>(customTexture.bytes.begin()+16, customTexture.bytes.end()), "custom mip level");
        customTexture.nativeFormat = NativeTextureFormat::Bc7Unorm;
        require(!custom(device, queue, customTexture, true), "unsupported compressed format rejected");
        std::puts("Metal texture upload smoke passed");
    }
}
