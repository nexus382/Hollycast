// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>

namespace vmu_window {
constexpr int ScreenCount = 8;
constexpr int LcdWidth = 48;
constexpr int LcdHeight = 32;

struct Frame
{
	std::array<std::uint32_t, LcdWidth * LcdHeight> pixels {};
	std::uint64_t revision = 0;
	bool active = false;
};

// Maple writes on the emulation thread; consumers only use complete LCD copies.
class FrameStore
{
public:
	void publish(int index, const std::uint32_t *pixels, bool active)
	{
		if (index < 0 || index >= ScreenCount)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		Frame& frame = frames[index];
		if (pixels != nullptr)
			std::copy_n(pixels, frame.pixels.size(), frame.pixels.begin());
		frame.active = active;
		++frame.revision;
	}

	std::array<Frame, ScreenCount> snapshot()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return frames;
	}

private:
	std::mutex mutex;
	std::array<Frame, ScreenCount> frames;
};
}
