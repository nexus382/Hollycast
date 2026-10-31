// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "types.h"
#include "rend/metal/metal_selection.h"
#include <cstdio>

int main()
{
    const RenderType oldTypes[] = {RenderType::OpenGL, RenderType::DirectX9, RenderType::DirectX11,
        RenderType::OpenGL_OIT, RenderType::Vulkan, RenderType::Vulkan_OIT,
        RenderType::DirectX11_OIT, RenderType::Metal};
    for (int i = 0; i < 8; ++i)
        if (static_cast<int>(oldTypes[i]) != i || isMetal(oldTypes[i]) != (i == 7)
                || isPerPixel(oldTypes[i]) != (i == 3 || i == 5 || i == 6)) {
            std::fprintf(stderr, "renderer ID or Metal family mismatch at %d\n", i);
            return 1;
        }
    if (static_cast<int>(RenderType::Metal_OIT) != 8 || !isMetal(RenderType::Metal_OIT)
            || !isPerPixel(RenderType::Metal_OIT)
            || isOpenGL(RenderType::Metal_OIT) || isVulkan(RenderType::Metal_OIT)
            || isDirectX(RenderType::Metal_OIT)) {
        std::fprintf(stderr, "Metal per-pixel renderer classification failed\n");
        return 1;
    }
    for (RenderType requested : {RenderType::Metal, RenderType::Metal_OIT}) {
        RenderType selected = RenderType::OpenGL;
        if (!metalSelectFallback(requested, true, true, true, selected) || selected != requested
                || !metalSelectFallback(requested, false, true, true, selected) || selected != RenderType::Vulkan
                || !metalSelectFallback(requested, false, false, true, selected) || selected != RenderType::OpenGL
                || metalSelectFallback(requested, false, false, false, selected)) {
            std::fprintf(stderr, "Metal availability fallback failed for %d\n", int(requested));
            return 1;
        }
    }
    std::puts("Renderer IDs, Metal families and availability fallbacks passed");
    return 0;
}
