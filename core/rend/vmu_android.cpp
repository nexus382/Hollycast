// Portions Copyright 2026 The Hollycast Authors
#include "vmu_window.h"
#include "vmu_frame_store.h"
#include "vmu_layout.h"
#include "cfg/option.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "ui/settings.h"
#include "ui/gui_menu.h"
#include "ui/imgui_driver.h"
#include <atomic>

namespace vmu_window {
namespace {
FrameStore frames;
TouchLayout layout;
std::mutex layoutMutex;
std::atomic<unsigned> displayed {0};
std::atomic<bool> mouseReset {false};
std::array<u64, ScreenCount> uploaded {};
bool loaded = false;
bool enabled = false;

void savePositions()
{
	std::array<Position, ScreenCount> positions;
	unsigned dirty;
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		dirty = layout.takeDirty();
		for (int i = 0; i < ScreenCount; ++i)
			positions[i] = layout.position(i);
	}
	if (dirty == 0)
		return;
	config::setAutoSave(false);
	for (int i = 0; i < ScreenCount; ++i)
		if ((dirty & (1u << i)) != 0)
		{
			const std::string key = std::to_string(i);
			config::saveInt("vmu-touch", key + ".x", std::lround(positions[i].x * 10000.f));
			config::saveInt("vmu-touch", key + ".y", std::lround(positions[i].y * 10000.f));
		}
	config::setAutoSave(true);
}
}

bool detachedActive()
{
	return config::MovableVMUs && config::FloatVMUs && settings.platform.isConsole();
}

void publishScreen(int index, const u32 *pixels, bool active) { frames.publish(index, pixels, active); }

void update(bool visible)
{
	if (!loaded)
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.reset();
		layout.takeDirty();
		for (int i = 0; i < ScreenCount; ++i)
		{
			const std::string key = std::to_string(i);
			const Position fallback = layout.position(i);
			layout.setPosition(i, {
				config::loadInt("vmu-touch", key + ".x", std::lround(fallback.x * 10000.f)) / 10000.f,
				config::loadInt("vmu-touch", key + ".y", std::lround(fallback.y * 10000.f)) / 10000.f});
		}
		loaded = true;
	}
	enabled = visible && detachedActive();
	if (!enabled)
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.setVisible(0);
		displayed = 0;
	}
	savePositions();
}

void drawInApp()
{
	if (!enabled || imguiDriver == nullptr)
		return;
	const auto screens = frames.snapshot();
	const ImVec2 view = ImGui::GetIO().DisplaySize;
	const float top = std::max(float(insetTop), GuiMenu::mainMenuBarHeight()) + 8.f;
	const Bounds safe {float(insetLeft) + 8.f, top,
		std::max(0.f, view.x - insetLeft - insetRight - 16.f),
		std::max(0.f, view.y - top - insetBottom - 8.f)};
	const float scale = std::clamp(settings.display.dpi / 100.f * 0.75f, 1.f, 4.f);
	std::array<Bounds, ScreenCount> rectangles;
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.configure(view.x, view.y, safe, 96.f * scale * std::clamp(config::VmuWindowSize.get(), 50, 400) / 100.f);
		for (int i = 0; i < ScreenCount; ++i)
			rectangles[i] = layout.bounds(i);
	}
	unsigned visible = 0;
	for (int i = 0; i < ScreenCount; ++i)
	{
		if (!screens[i].active || safe.width <= 0 || safe.height <= 0)
			continue;
		const std::string key = ":movable-vmu:" + std::to_string(i);
		ImTextureID texture = imguiDriver->getTexture(key);
		if (texture == ImTextureID() || uploaded[i] != screens[i].revision)
		{
			try {
				texture = imguiDriver->updateTexture(key, reinterpret_cast<const u8 *>(screens[i].pixels.data()), LcdWidth, LcdHeight, true);
				uploaded[i] = screens[i].revision;
			} catch (...) {
				// Leave the standard LCD renderer in charge if the overlay cannot draw.
				continue;
			}
		}
		if (texture == ImTextureID())
			continue;
		const Bounds& rect = rectangles[i];
		ImGui::GetForegroundDrawList()->AddImage(texture, ImVec2(rect.x, rect.y),
			ImVec2(rect.x + rect.width, rect.y + rect.height), ImVec2(0, 1), ImVec2(1, 0), 0xc0bfbfbf);
		visible |= 1u << i;
	}
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.setVisible(visible);
	}
	displayed = visible;
}

bool ownsScreen(int index)
{
	return index >= 0 && index < ScreenCount && (displayed.load() & (1u << index)) != 0;
}

bool touchDown(float x, float y)
{
	std::lock_guard<std::mutex> lock(layoutMutex);
	const bool captured = layout.begin(x, y);
	if (captured)
		mouseReset = true;
	return captured;
}
void touchMove(float x, float y) { std::lock_guard<std::mutex> lock(layoutMutex); layout.move(x, y); }
void touchUp()
{
	std::lock_guard<std::mutex> lock(layoutMutex);
	layout.end();
	mouseReset = true;
}
bool consumeMousePositionReset() { return mouseReset.exchange(false); }

void shutdown()
{
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.setVisible(0);
	}
	displayed = 0;
	enabled = false;
	savePositions();
}

void resetPositions()
{
	{
		std::lock_guard<std::mutex> lock(layoutMutex);
		layout.reset();
	}
	savePositions();
}
}
