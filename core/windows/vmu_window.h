// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "types.h"

namespace vmu_window {
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
// LCD producers publish copies; Windows and configuration are owned by the UI threads.
// True when detached LCDs are configured and the running platform can show them;
// SDL input handling keys off this so arcade games keep normal mouse behavior.
bool detachedActive();
void publishScreen(int index, const u32 *pixels, bool active);
void update(bool pointerAvailable);
void shutdown();
void resetPositions();
bool ownsScreen(int index);
bool blocksMouseInput();
#else
inline bool ownsScreen(int) { return false; }
#endif
}
