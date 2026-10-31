// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Match the local PVR/Vulkan 4x4 dither before packing an RGBA8 framebuffer.
// Input is the top-to-bottom native Metal readback, with tightly packed rows.
inline void metalDitherFramebuffer(uint8_t *rgba, int width, int height, unsigned packMode, bool enabled)
{
    if (!enabled || packMode > 3 || !rgba || width <= 0 || height <= 0) return;
    static constexpr unsigned matrix[16] = {5,13,7,15, 9,1,11,3, 6,14,4,12, 10,2,8,0};
    const unsigned divisor[3] = {packMode == 2 ? 1u : 2u,
        packMode == 2 ? 1u : packMode == 1 ? 4u : 2u, packMode == 2 ? 1u : 2u};
    for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
        uint8_t *pixel = rgba + ((size_t)y * width + x) * 4;
        unsigned threshold = matrix[(y & 3) * 4 + (x & 3)];
        for (int channel=0;channel<3;++channel)
            pixel[channel] = (uint8_t)std::min(255u, unsigned(pixel[channel]) + threshold / divisor[channel]);
        pixel[3] = (uint8_t)std::min(255u, unsigned(pixel[3]) + 1u);
    }
}

// A CPU-only framebuffer upload must not erase a PVR timing sample already
// submitted for the same emulated frame. Consume once, and reject old epochs.
class MetalPvrTimingEvidence
{
public:
    void submitted(uint32_t epoch) { pending.store(uint64_t(epoch) + 1, std::memory_order_release); }
    bool consume(bool requested, uint32_t epoch) {
        const uint64_t value = pending.exchange(0, std::memory_order_acq_rel);
        return requested && value == uint64_t(epoch) + 1;
    }
    void reset() { pending.store(0, std::memory_order_release); }
private:
    std::atomic<uint64_t> pending {0};
};
