// Portions Copyright 2026 The Hollycast Authors
#include "rend/vmu_layout.h"
#include "rend/vmu_frame_store.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

int checks = 0;
void require(bool condition, const char *message)
{
	++checks;
	if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
	using namespace vmu_window;
	TouchLayout layout;
	layout.reset();
	layout.takeDirty();
	layout.configure(1000, 600, {20, 40, 960, 540}, 96);
	layout.setVisible(1);
	require(!layout.begin(0, 0), "touch outside safe area passes to the game");
	require(layout.begin(.03f, .08f), "first VMU is touchable");
	layout.move(.53f, .58f);
	require(layout.takeDirty() == 0, "no config writes while a finger is moving");
	layout.end();
	require(layout.takeDirty() == 1, "release saves only the dragged slot");
	const Position saved = layout.position(0);
	require(saved.x > .5f && saved.y > .5f, "drag uses the original finger offset");
	layout.configure(600, 1000, {30, 60, 540, 900}, 96);
	Bounds rect = layout.bounds(0);
	require(rect.x >= 30 && rect.x + rect.width <= 570 && rect.y >= 60 && rect.y + rect.height <= 960,
		"rotation respects all safe-area edges");
	require(layout.position(0).x == saved.x && layout.position(0).y == saved.y, "rotation preserves saved fractions");
	layout.configure(50, 40, {0, 0, 50, 40}, 384);
	rect = layout.bounds(0);
	require(rect.width == 50 && rect.height < 40, "oversized LCD shrinks to fit split-screen");
	require(layout.position(0).x == saved.x, "temporary clamp preserves horizontal preference");
	layout.configure(1000, 600, {20, 40, 960, 540}, 96);
	rect = layout.bounds(0);
	require(layout.begin((rect.x + 5) / 1000, (rect.y + 5) / 600), "saved LCD is touchable after restoration");
	layout.move(2, -1);
	layout.setVisible(0);
	require(layout.takeDirty() == 1, "hiding during drag commits its last reachable position");
	require(layout.position(0).x == 1 && layout.position(0).y == 0, "drag clamps at viewport edges");
	require(!layout.begin(.9f, .1f), "hidden LCD cannot capture touches");
	layout.setPosition(7, layout.position(0));
	layout.setVisible(0x81);
	rect = layout.bounds(7);
	require(layout.begin((rect.x + 5) / 1000, (rect.y + 5) / 600), "overlapping LCD can be dragged");
	layout.move(.5f, .5f);
	layout.configure(600, 1000, {0, 0, 600, 1000}, 192);
	require(layout.takeDirty() == 0x80, "topmost LCD owns drag; resize ends it");
	const Position rotated = layout.position(7);
	layout.move(.1f, .1f);
	require(layout.position(7).x == rotated.x, "stale move after rotation cannot move the LCD");
	require(!layout.begin(std::numeric_limits<float>::quiet_NaN(), 0), "non-finite touch is ignored");
	layout.reset();
	require(layout.takeDirty() == 0xff, "reset persists every slot");
	require(layout.position(0).x == 0 && layout.position(7).x == 1 && layout.position(7).y == 1,
		"reset distributes slots along both screen edges");

	FrameStore frames;
	std::array<std::uint32_t, LcdWidth * LcdHeight> pixels;
	pixels.fill(0x12345678);
	frames.publish(0, pixels.data(), true);
	pixels.fill(0);
	auto copy = frames.snapshot();
	require(copy[0].pixels.front() == 0x12345678 && copy[0].pixels.back() == 0x12345678,
		"publication owns a copy rather than the producer's buffer");
	frames.publish(-1, pixels.data(), true);
	frames.publish(8, pixels.data(), true);
	frames.publish(0, nullptr, false);
	copy = frames.snapshot();
	require(!copy[0].active && copy[0].revision == 2, "blanking advances revision without accessing null pixels");
	std::atomic<bool> done {false};
	std::thread writer([&] {
		for (unsigned i = 1; i <= 2000; ++i) { pixels.fill(i); frames.publish(7, pixels.data(), true); }
		done = true;
	});
	bool torn = false;
	do {
		copy = frames.snapshot();
		for (auto pixel : copy[7].pixels)
			torn |= pixel != copy[7].pixels.front();
	} while (!done);
	writer.join();
	require(!torn, "concurrent consumers never receive a torn frame");
	std::cout << checks << " checks passed\n";
}
