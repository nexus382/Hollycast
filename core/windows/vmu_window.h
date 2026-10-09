// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "types.h"

namespace vmu_window {
#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
// LCD producers publish copies; Windows and configuration are owned by the UI threads.
void publishScreen(int index, const u32 *pixels, bool active);
void update(bool pointerAvailable);
void shutdown();
void resetPositions();
void setPlacementEnabled(bool enabled);
bool placementEnabled();
bool ownsScreen(int index);
bool blocksMouseInput();
#else
inline bool ownsScreen(int) { return false; }
inline bool placementEnabled() { return false; }
#endif
}
