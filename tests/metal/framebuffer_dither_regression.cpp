// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// xcrun clang++ -std=c++17 -Icore tests/metal/framebuffer_dither_regression.cpp -o /tmp/metal-framebuffer-dither-test
#include "rend/metal/metal_framebuffer.h"
#include <array>
#include <cstdlib>
#include <cstdio>

static void require(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main()
{
    constexpr std::array<uint8_t,16> rgb555 = {122,126,123,127,124,120,125,121,123,127,122,126,125,121,124,120};
    constexpr std::array<uint8_t,16> green565 = {121,123,121,123,122,120,122,120,121,123,121,123,122,120,122,120};
    constexpr std::array<uint8_t,16> rgb444 = {125,133,127,135,129,121,131,123,126,134,124,132,130,122,128,120};
    for (unsigned mode=0;mode<4;++mode) {
        std::array<uint8_t,64> pixels;
        pixels.fill(120);
        metalDitherFramebuffer(pixels.data(),4,4,mode,true);
        for (size_t i=0;i<16;++i) {
            const auto& redBlue = mode == 2 ? rgb444 : rgb555;
            const auto& green = mode == 2 ? rgb444 : mode == 1 ? green565 : rgb555;
            require(pixels[i*4] == redBlue[i] && pixels[i*4+1] == green[i] && pixels[i*4+2] == redBlue[i], "PVR dither matrix RGB");
            require(pixels[i*4+3] == 121, "PVR dither alpha");
        }
    }
    std::array<uint8_t,4> clamped {254,254,254,255};
    metalDitherFramebuffer(clamped.data(),1,1,2,true);
    require((clamped == std::array<uint8_t,4>{255,255,255,255}), "clamp overflow");
    std::array<uint8_t,4> untouched {10,20,30,40};
    metalDitherFramebuffer(untouched.data(),1,1,0,false);
    metalDitherFramebuffer(untouched.data(),1,1,4,true);
    require((untouched == std::array<uint8_t,4>{10,20,30,40}), "disabled and unsupported pack modes");

    MetalPvrTimingEvidence evidence;
    require(!evidence.consume(true,17), "CPU-only video has no evidence"); // CPU-only video has no PVR sample.
    evidence.submitted(17);
    require(evidence.consume(true,17), "PVR sample preserved through upload"); // A following framebuffer upload preserves it.
    require(!evidence.consume(true,17), "evidence consumed once"); // Evidence applies to one upload only.
    evidence.submitted(17);
    require(!evidence.consume(true,18), "stale or cleared evidence"); // An old metrics epoch cannot become valid.
    evidence.submitted(18);
    require(!evidence.consume(false,18), "disabled collection clears evidence"); // Disabling collection clears pending evidence.
    evidence.submitted(18); evidence.reset();
    require(!evidence.consume(true,18), "stale or cleared evidence");
    std::puts("Metal framebuffer dither and PVR timing evidence passed");
}
