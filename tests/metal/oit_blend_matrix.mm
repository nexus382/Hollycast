// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Differential resolve test: production Metal kernels versus a double-precision
// CPU implementation of the PVR blend factors and accumulation-buffer rules.
#include "hw/pvr/Renderer_if.h"
#include "rend/metal/metal_oit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <tuple>
#include <vector>

using Color = std::array<double, 4>;
static void require(bool value, const char *message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static Color unpack(uint32_t packed) {
    return {double(packed & 255) / 255, double((packed >> 8) & 255) / 255,
            double((packed >> 16) & 255) / 255, double(packed >> 24) / 255};
}
static double factor(unsigned mode, const Color& src, const Color& dst, unsigned channel, bool source) {
    switch (mode) {
        case 0: return 0;
        case 1: return 1;
        case 2: return source ? dst[channel] : src[channel];
        case 3: return 1 - (source ? dst[channel] : src[channel]);
        case 4: return src[3];
        case 5: return 1 - src[3];
        case 6: return dst[3];
        case 7: return 1 - dst[3];
    }
    std::abort();
}
static Color reference(std::vector<MetalOITNode> nodes, Color primary, unsigned layers) {
    std::sort(nodes.begin(), nodes.end(), [](const auto& a, const auto& b) {
        return std::tie(a.depth, a.sequence, a.primitive) < std::tie(b.depth, b.sequence, b.primitive);
    });
    if (nodes.size() > layers) nodes.erase(nodes.begin(), nodes.end() - layers);
    Color secondary{};
    for (const auto& node : nodes) {
        bool shadow = (node.flags & 2) != 0;
        bool area1 = shadow && (node.flags & 8) != 0;
        uint32_t tsp = area1 ? node.tsp1 : node.tsp0;
        bool selectSource = (tsp & (1u << 25)) != 0;
        Color src = selectSource ? secondary : unpack(area1 ? node.color1 : node.color0);
        if (shadow && !area1 && !selectSource)
            for (unsigned channel = 0; channel < 3; ++channel) src[channel] *= .5;
        Color& dst = (tsp & (1u << 24)) ? secondary : primary;
        Color next{};
        for (unsigned channel = 0; channel < 4; ++channel)
            next[channel] = std::clamp(src[channel] * factor((tsp >> 29) & 7, src, dst, channel, true)
                    + dst[channel] * factor((tsp >> 26) & 7, src, dst, channel, false), 0., 1.);
        dst = next;
    }
    return primary;
}

int main() { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); require(device != nil, "Metal device");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    constexpr unsigned width = 32, height = 32, pixels = width * height, fragments = 6;
    MetalOITBuffers buffers;
    buffers.init(device, width, height, pixels * fragments * sizeof(MetalOITNode), 8, .5f);
    auto *control = static_cast<MetalOITControl *>(buffers.control().contents);
    while (control->capacity < pixels * fragments) require(buffers.grow(), "test buffer growth");
    auto *nodes = static_cast<MetalOITNode *>(buffers.nodes().contents);
    auto *heads = static_cast<uint32_t *>(buffers.heads().contents);
    std::vector<uint32_t> backgrounds(pixels);
    std::vector<std::vector<MetalOITNode>> scenes(pixels);
    std::mt19937 random(0x48504354);
    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
        unsigned src = pixel & 7, dst = (pixel >> 3) & 7, select = (pixel >> 6) & 3, shade = pixel >> 8;
        backgrounds[pixel] = random();
        std::array<unsigned, fragments> permutation;
        std::iota(permutation.begin(), permutation.end(), 0);
        std::shuffle(permutation.begin(), permutation.end(), random);
        uint32_t tsp0 = (src << 29) | (dst << 26) | (select << 24);
        uint32_t tsp1 = ((7-src) << 29) | ((7-dst) << 26) | ((3-select) << 24);
        for (unsigned fragment = 0; fragment < fragments; ++fragment) {
            MetalOITNode node{};
            node.color0 = random(); node.color1 = random();
            node.depth = fragment == 3 ? .3f : (fragment + 1) * .1f;
            node.sequence = fragment == 2 || fragment == 3 ? 5 : fragment;
            node.primitive = fragment == 2 ? 1 : 0;
            node.tsp0 = tsp0; node.tsp1 = tsp1;
            node.flags = std::array<unsigned, 4>{0, 2, 10, 8}[shade];
            // Seed secondary independently, then use it as the final source.
            if (fragment == 0) node.tsp0 = node.tsp1 = (1u << 29) | (1u << 24);
            if (fragment == fragments-1) node.tsp0 = node.tsp1 = (1u << 29) | (1u << 26) | (1u << 25);
            scenes[pixel].push_back(node);
        }
        heads[pixel] = pixel * fragments + permutation[0];
        for (unsigned n = 0; n < fragments; ++n) {
            auto node = scenes[pixel][permutation[n]];
            node.next = n + 1 == fragments ? UINT32_MAX : pixel * fragments + permutation[n+1];
            nodes[pixel * fragments + permutation[n]] = node;
        }
    }
    control->count = pixels * fragments;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
    descriptor.storageMode = metalCpuTextureStorageMode(device);
    descriptor.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> input = [device newTextureWithDescriptor:descriptor];
    [input replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:backgrounds.data() bytesPerRow:width*4];
    id<MTLTexture> secondaryInput = [device newTextureWithDescriptor:descriptor];
    const std::vector<uint32_t> emptySecondary(pixels, 0);
    [secondaryInput replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
                        withBytes:emptySecondary.data() bytesPerRow:width*4];
    descriptor.usage = MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> output = [device newTextureWithDescriptor:descriptor];
    id<MTLTexture> secondaryOutput = [device newTextureWithDescriptor:descriptor];
    constexpr unsigned rowBytes = 256;
    id<MTLBuffer> readback = [device newBufferWithLength:rowBytes*height options:MTLResourceStorageModeShared];
    unsigned assertions = 0;
    for (unsigned layers : {1u, 2u, 4u, 8u}) {
        control->maxLayers = layers; control->layerOverflow = 0;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        buffers.resolve(command, input, output, secondaryInput, secondaryOutput);
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(width,height,1) toBuffer:readback destinationOffset:0
                destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes*height];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        require(command.status == MTLCommandBufferStatusCompleted, "resolve command completed");
        require(buffers.layersExceeded() == (layers < fragments), "layer-limit status");
        const auto *actual = static_cast<const unsigned char *>(readback.contents);
        for (unsigned pixel = 0; pixel < pixels; ++pixel) {
            Color expected = reference(scenes[pixel], unpack(backgrounds[pixel]), layers);
            for (unsigned channel = 0; channel < 4; ++channel) {
                int expectedByte = int(std::round(expected[channel] * 255));
                unsigned offset = (pixel / width) * rowBytes + (pixel % width) * 4 + channel;
                if (std::abs(int(actual[offset]) - expectedByte) > 1) {
                    std::fprintf(stderr, "FAIL pixel=%u layers=%u channel=%u got=%u expected=%d\n",
                            pixel, layers, channel, actual[offset], expectedByte);
                    return 1;
                }
                ++assertions;
            }
        }
    }
    // Direct factor cases: seed secondary independently, exercise exactly one
    // blend, and copy secondary to primary without adding it when selected.
    // Low-valued colors keep every sum below saturation, so a wrong factor
    // cannot be hidden by a later fragment or clamp.
    auto pack = [](unsigned r, unsigned g, unsigned b, unsigned a) {
        return r | (g << 8) | (b << 16) | (a << 24);
    };
    const uint32_t background = pack(30, 38, 46, 50);
    const uint32_t secondarySeed = pack(18, 29, 40, 60);
    const uint32_t source0 = pack(25, 45, 65, 75);
    const uint32_t source1 = pack(35, 49, 71, 84);
    std::fill(backgrounds.begin(), backgrounds.end(), background);
    [input replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0
               withBytes:backgrounds.data() bytesPerRow:width * 4];
    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
        const unsigned src = pixel & 7, dst = (pixel >> 3) & 7;
        const unsigned select = (pixel >> 6) & 3, shade = pixel >> 8;
        scenes[pixel].clear();
        MetalOITNode seed{}; seed.color0 = seed.color1 = secondarySeed;
        seed.depth = .1f; seed.sequence = 0;
        seed.tsp0 = seed.tsp1 = (1u << 29) | (1u << 24);
        scenes[pixel].push_back(seed);
        MetalOITNode tested{}; tested.color0 = source0; tested.color1 = source1;
        tested.depth = .2f; tested.sequence = 1;
        tested.tsp0 = tested.tsp1 = (src << 29) | (dst << 26) | (select << 24);
        tested.flags = std::array<unsigned, 4>{0, 2, 10, 8}[shade];
        scenes[pixel].push_back(tested);
        if (select & 1) {
            MetalOITNode copy{}; copy.depth = .3f; copy.sequence = 2;
            copy.tsp0 = copy.tsp1 = (1u << 29) | (1u << 25);
            scenes[pixel].push_back(copy);
        }
        const unsigned base = pixel * fragments;
        heads[pixel] = base;
        for (unsigned i = 0; i < scenes[pixel].size(); ++i) {
            MetalOITNode node = scenes[pixel][i];
            node.next = i + 1 == scenes[pixel].size() ? UINT32_MAX : base + i + 1;
            nodes[base + i] = node;
        }
    }
    control->maxLayers = 8; control->layerOverflow = 0;
    id<MTLCommandBuffer> direct = [queue commandBuffer];
    buffers.resolve(direct, input, output, secondaryInput, secondaryOutput);
    id<MTLBlitCommandEncoder> directBlit = [direct blitCommandEncoder];
    [directBlit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(width,height,1) toBuffer:readback destinationOffset:0
            destinationBytesPerRow:rowBytes destinationBytesPerImage:rowBytes*height];
    [directBlit endEncoding]; [direct commit]; [direct waitUntilCompleted];
    require(direct.status == MTLCommandBufferStatusCompleted, "direct factor resolve command completed");
    require(!buffers.layersExceeded(), "direct factor scenes do not exceed layer cap");
    const auto *directPixels = static_cast<const unsigned char *>(readback.contents);
    for (unsigned pixel = 0; pixel < pixels; ++pixel) {
        Color expected = reference(scenes[pixel], unpack(background), 8);
        for (unsigned channel = 0; channel < 4; ++channel) {
            int expectedByte = int(std::round(expected[channel] * 255));
            unsigned offset = (pixel / width) * rowBytes + (pixel % width) * 4 + channel;
            if (std::abs(int(directPixels[offset]) - expectedByte) > 1) {
                std::fprintf(stderr, "FAIL direct src=%u dst=%u select=%u shade=%u channel=%u got=%u expected=%d\n",
                        pixel & 7, (pixel >> 3) & 7, (pixel >> 6) & 3, pixel >> 8,
                        channel, directPixels[offset], expectedByte);
                return 1;
            }
            ++assertions;
        }
    }
    std::printf("Metal OIT blend/secondary/shadow/tie/layer matrix passed (%u channel checks)\n", assertions);
} }
