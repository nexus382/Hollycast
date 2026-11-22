// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <cstdint>
#include <vector>

namespace test_harness {

// Read the emulator's composited GPU target, including menus and OSD. Called
// only on the render owner after ImGui draws and before the swapchain presents.
void captureFrame(std::vector<std::uint8_t>& rgba, int& width, int& height);

} // namespace test_harness
