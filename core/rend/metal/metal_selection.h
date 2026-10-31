// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

// Keep the persisted renderer unchanged when Metal works; otherwise choose a
// compiled fallback, or report failure for a Metal-only build.
inline bool metalSelectFallback(RenderType requested, bool available, bool vulkan, bool opengl, RenderType& selected)
{
    selected = requested;
    if (!isMetal(requested) || available) return true;
    if (vulkan) selected = RenderType::Vulkan;
    else if (opengl) selected = RenderType::OpenGL;
    else return false;
    return true;
}
