// Portions Copyright 2026 The Hollycast Authors
#include "rend/vmu_window.h"
#include "rend/vmu_layout.h"
#include <cstdlib>
#include <iostream>
#include <limits>

int checks = 0;
void require(bool condition, const char *message)
{
	++checks;
	if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
	using namespace vmu_window;
	require(opacity(DefaultTransparencyPercent) == .75f && opacityByte(DefaultTransparencyPercent) == 192,
		"default desktop and Android gameplay appearance is unchanged");
	require(opacity(DefaultTransparencyPercent, true) == .5f && opacityByte(DefaultTransparencyPercent, true) == 128,
		"desktop menu dimming is unchanged");
	require(opacity(0) == 1.f && opacityByte(0) == 255, "zero transparency is fully opaque");
	require(opacity(100) == 0.f && opacityByte(100) == 0, "full transparency is invisible");
	require(opacityByte(50) == 128 && opacityByte(80) == 51, "intermediate values retain 8-bit alpha precision");
	require(opacityByte(std::numeric_limits<int>::min()) == 255, "corrupt negative setting clamps without overflow");
	require(opacityByte(std::numeric_limits<int>::max()) == 0, "corrupt positive setting clamps without overflow");
	bool monotonic = true;
	for (int percent = 1; percent <= 100; ++percent)
		monotonic &= opacityByte(percent) < opacityByte(percent - 1)
			&& opacityByte(percent, true) < opacityByte(percent - 1, true);
	require(monotonic, "every slider step increases transparency in gameplay and menus");
	TouchLayout layout;
	layout.configure(1000, 600, {20, 40, 960, 540}, 96);
	layout.setVisible(1);
	require(layout.begin(.03f, .08f), "visible Android LCD can be dragged");
	layout.move(.5f, .5f);
	layout.setVisible(0);
	const Position saved = layout.position(0);
	layout.move(.8f, .8f);
	require(layout.takeDirty() == 1 && layout.position(0).x == saved.x,
		"hiding mid-drag commits the last Android position and releases the gesture");
	require(!layout.begin(.5f, .5f), "hidden Android LCD does not capture a new touch");
	layout.setVisible(1);
	const Bounds bounds = layout.bounds(0);
	require(layout.begin((bounds.x + 5) / 1000, (bounds.y + 5) / 600),
		"restoring visibility restores touch at the saved Android position");
	std::cout << checks << " checks passed\n";
}
