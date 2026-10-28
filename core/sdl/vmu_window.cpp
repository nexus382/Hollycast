// Portions Copyright 2026 The Hollycast Authors
#include "rend/vmu_window.h"
#include "rend/vmu_frame_store.h"
#include "cfg/option.h"
#include "ui/gui.h"
#include <SDL.h>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstring>
#include <string>

namespace vmu_window {
#ifdef __APPLE__
void configureMacWindow(SDL_Window *window, bool movable);
#endif
namespace {
struct LcdWindow
{
	SDL_Window *window = nullptr;
	u64 revision = 0;
	Uint64 retryAt = 0;
	bool displayed = false;
	bool dismissed = false;
	bool repaint = true;
	bool placing = false;
	bool positionDirty = false;
	int left = 0, top = 0;
	Uint64 saveAt = 0;
};

FrameStore frames;
std::array<LcdWindow, ScreenCount> windows;
SDL_Window *owner = nullptr;
bool wayland = false;
std::atomic<bool> mouseReset {false};

void savePositions(bool flush)
{
	bool saving = false;
	for (int i = 0; i < ScreenCount; ++i)
	{
		LcdWindow& lcd = windows[i];
		if (!lcd.positionDirty || (!flush && SDL_GetTicks64() < lcd.saveAt))
			continue;
		if (!saving)
			config::setAutoSave(false);
		saving = true;
		const std::string key = std::to_string(i);
		config::saveInt("vmu-sdl", key + ".left", lcd.left);
		config::saveInt("vmu-sdl", key + ".top", lcd.top);
		lcd.positionDirty = false;
	}
	if (saving)
		config::setAutoSave(true);
}

// SDL window surfaces can create an internal GL context. Preserve the emulator's
// context across creation, painting and destruction, including Vulkan's null context.
class PreserveGLContext
{
	SDL_Window *window = SDL_GL_GetCurrentWindow();
	SDL_GLContext context = SDL_GL_GetCurrentContext();
public:
	~PreserveGLContext()
	{
		if (SDL_GL_GetCurrentContext() != context || SDL_GL_GetCurrentWindow() != window)
			SDL_GL_MakeCurrent(window, context);
	}
};

void destroy(LcdWindow& lcd)
{
	if (lcd.window != nullptr)
		SDL_DestroyWindow(lcd.window);
	lcd.window = nullptr;
	lcd.displayed = false;
	lcd.repaint = true;
}

bool isVmuWindow(SDL_Window *window)
{
	if (window == nullptr)
		return false;
	for (const auto& lcd : windows)
		if (lcd.window == window)
			return true;
	return false;
}

SDL_Point reachablePosition(int index, int width, int height, int x = INT_MIN, int y = INT_MIN)
{
	// Wayland deliberately has no global placement API. Let its compositor place
	// the utility window; never persist synthetic coordinates returned by SDL.
	if (wayland)
		return {SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED};
	const std::string key = std::to_string(index);
	if (x == INT_MIN && y == INT_MIN)
	{
		x = config::loadInt("vmu-sdl", key + ".left", INT_MIN);
		y = config::loadInt("vmu-sdl", key + ".top", INT_MIN);
	}
	SDL_Rect area;
	int display = SDL_GetWindowDisplayIndex(owner);
	if (x != INT_MIN && y != INT_MIN)
	{
		const SDL_Point point {x, y};
		// Keep compatibility with the project's SDL 2.0.18 minimum.
		for (int candidate = 0; candidate < SDL_GetNumVideoDisplays(); ++candidate)
			if (SDL_GetDisplayBounds(candidate, &area) == 0 && SDL_PointInRect(&point, &area))
			{
				display = candidate;
				break;
			}
	}
	if (display < 0 || SDL_GetDisplayUsableBounds(display, &area) != 0)
		return {SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED};
	if (x == INT_MIN || y == INT_MIN)
	{
		SDL_GetWindowPosition(owner, &x, &y);
		x += 16 + (index / 4) * (width + 12);
		y += 48 + (index % 4) * (height + 36);
	}
	// A monitor's temporary absence must not overwrite the saved placement.
	return {std::clamp(x, area.x, area.x + std::max(0, area.w - width)),
		std::clamp(y, area.y + 32, area.y + std::max(32, area.h - height))};
}

bool paint(LcdWindow& lcd, const Frame& frame)
{
	SDL_Surface *target = SDL_GetWindowSurface(lcd.window);
	if (target == nullptr)
		return false;
	std::array<u32, LcdWidth * LcdHeight> pixels;
	// LCD producers use bottom-up rows, while SDL surfaces use top-down rows.
	for (int row = 0; row < LcdHeight; ++row)
		std::copy_n(frame.pixels.data() + (LcdHeight - 1 - row) * LcdWidth,
			LcdWidth, pixels.data() + row * LcdWidth);
	SDL_Surface *source = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), LcdWidth, LcdHeight,
		32, LcdWidth * sizeof(u32), SDL_PIXELFORMAT_ARGB8888);
	if (source == nullptr)
		return false;
	SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);
	const bool success = SDL_BlitScaled(source, nullptr, target, nullptr) == 0
		&& SDL_UpdateWindowSurface(lcd.window) == 0;
	SDL_FreeSurface(source);
	return success;
}
}

bool detachedActive()
{
	return config::MovableVMUs && config::FloatVMUs && settings.platform.isConsole();
}
bool placementActive()
{
	return (config::MovableVMUs || config::VmuLayoutEnabled) && config::FloatVMUs && settings.platform.isConsole();
}

void publishScreen(int index, const u32 *pixels, bool active) { frames.publish(index, pixels, active); }

void update(SDL_Window *parent, bool pointerAvailable)
{
	PreserveGLContext preserveContext;
	owner = parent;
	savePositions(false);
	const char *driver = SDL_GetCurrentVideoDriver();
	wayland = driver != nullptr && std::strcmp(driver, "wayland") == 0;
	const bool enabled = placementActive();
	const auto screens = frames.snapshot();
	SDL_Window *focus = SDL_GetKeyboardFocus();
	const bool focused = focus == parent || isVmuWindow(focus);
	const bool visible = enabled && parent != nullptr && (pointerAvailable || !config::MovableVMUs) && focused
		&& (SDL_GetWindowFlags(parent) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0
		&& gui_state != GuiState::Main && gui_state != GuiState::Onboarding
		&& gui_state != GuiState::Loading && gui_state != GuiState::NetworkStart;
	float dpi = 96.f;
	if (parent != nullptr)
		SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(parent), &dpi, nullptr, nullptr);
	const float scale = std::clamp(dpi / 100.f * 0.75f, 1.f, 4.f)
		* std::clamp(config::VmuWindowSize.get(), MinSizePercent, MaxSizePercent) / 100.f;
	const int width = int(std::lround(96.f * scale));
	const int height = int(std::lround(64.f * scale));
	for (int i = 0; i < ScreenCount; ++i)
	{
		LcdWindow& lcd = windows[i];
		lcd.placing = false;
		if (!enabled || !screens[i].active)
		{
			destroy(lcd);
			lcd.dismissed = false;
			continue;
		}
		if (!visible || lcd.dismissed)
		{
			if (lcd.window != nullptr)
				SDL_HideWindow(lcd.window);
			lcd.displayed = false;
			continue;
		}
		if (lcd.window == nullptr)
		{
			if (SDL_GetTicks64() < lcd.retryAt)
				continue;
			lcd.retryAt = SDL_GetTicks64() + 1000;
			const SDL_Point pos = reachablePosition(i, width, height);
			const std::string title = "Hollycast VMU " + std::string(1, char('A' + i / 2)) + std::to_string(i % 2 + 1);
			lcd.window = SDL_CreateWindow(title.c_str(), pos.x, pos.y, width, height,
				SDL_WINDOW_HIDDEN | SDL_WINDOW_UTILITY | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_ALLOW_HIGHDPI);
			if (lcd.window == nullptr)
			{
				WARN_LOG(RENDERER, "Cannot create VMU window: %s", SDL_GetError());
				continue;
			}
			lcd.placing = true;
#ifdef __APPLE__
			configureMacWindow(lcd.window, config::MovableVMUs);
#endif
		}
		int oldWidth, oldHeight;
		const bool bordered = (SDL_GetWindowFlags(lcd.window) & SDL_WINDOW_BORDERLESS) == 0;
		if (bordered != bool(config::MovableVMUs))
		{
			// Removing the drag title bar locks the window without recreating it.
			int x, y;
			SDL_GetWindowPosition(lcd.window, &x, &y);
			lcd.left = x;
			lcd.top = y;
			SDL_SetWindowBordered(lcd.window, config::MovableVMUs ? SDL_TRUE : SDL_FALSE);
#ifdef __APPLE__
			configureMacWindow(lcd.window, config::MovableVMUs);
#endif
			if (!wayland)
				SDL_SetWindowPosition(lcd.window, x, y);
			lcd.placing = true;
		}
		SDL_GetWindowSize(lcd.window, &oldWidth, &oldHeight);
		if (oldWidth != width || oldHeight != height)
		{
			SDL_SetWindowSize(lcd.window, width, height);
			lcd.repaint = true;
			lcd.placing = true;
		}
		// Opacity and always-on-top are best effort on compositors that restrict them.
		SDL_SetWindowOpacity(lcd.window, gui_is_open() ? 0.5f : 0.75f);
		SDL_SetWindowAlwaysOnTop(lcd.window, SDL_TRUE);
		if ((SDL_GetWindowFlags(lcd.window) & SDL_WINDOW_HIDDEN) != 0)
		{
			SDL_ShowWindow(lcd.window);
			lcd.repaint = true;
			lcd.placing = true;
			if (focus == parent)
				SDL_RaiseWindow(parent);
		}
		if (lcd.repaint || lcd.revision != screens[i].revision)
		{
			if (!paint(lcd, screens[i]))
			{
				WARN_LOG(RENDERER, "Cannot draw VMU window: %s", SDL_GetError());
				destroy(lcd);
				lcd.retryAt = SDL_GetTicks64() + 1000;
				continue;
			}
			lcd.revision = screens[i].revision;
			lcd.repaint = false;
		}
		lcd.displayed = true;
	}
}

bool handleEvent(const SDL_Event& event)
{
	Uint32 id;
	switch (event.type)
	{
	case SDL_WINDOWEVENT: id = event.window.windowID; break;
	case SDL_KEYDOWN: case SDL_KEYUP: id = event.key.windowID; break;
	case SDL_TEXTINPUT: case SDL_TEXTEDITING: id = event.text.windowID; break;
	case SDL_MOUSEMOTION: id = event.motion.windowID; break;
	case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: id = event.button.windowID; break;
	case SDL_MOUSEWHEEL: id = event.wheel.windowID; break;
	case SDL_DROPFILE: case SDL_DROPTEXT: case SDL_DROPBEGIN: case SDL_DROPCOMPLETE: id = event.drop.windowID; break;
	default: return false;
	}
	if (id == 0 || (owner != nullptr && id == SDL_GetWindowID(owner)))
		return false;
	for (int i = 0; i < ScreenCount; ++i)
	{
		LcdWindow& lcd = windows[i];
		if (lcd.window == nullptr || SDL_GetWindowID(lcd.window) != id)
			continue;
		mouseReset = true;
		if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT)
			SDL_free(event.drop.file);
		if (event.type == SDL_WINDOWEVENT)
		{
			switch (event.window.event)
			{
			case SDL_WINDOWEVENT_CLOSE:
				// Closing one utility window restores that slot's in-game LCD.
				SDL_HideWindow(lcd.window);
				lcd.displayed = false;
				lcd.dismissed = true;
				break;
			case SDL_WINDOWEVENT_EXPOSED: case SDL_WINDOWEVENT_SIZE_CHANGED:
				lcd.repaint = true;
				break;
			case SDL_WINDOWEVENT_MOVED:
				if (!config::MovableVMUs)
				{
					// Also undo window-manager move shortcuts while locked. Wayland
					// owns global placement and does not support restoring coordinates.
					if (!wayland)
					{
						int width, height;
						SDL_GetWindowSize(lcd.window, &width, &height);
						// Clamp only the visible target if its monitor/work area changed.
						const SDL_Point pos = reachablePosition(i, width, height, lcd.left, lcd.top);
						if (event.window.data1 != pos.x || event.window.data2 != pos.y)
							SDL_SetWindowPosition(lcd.window, pos.x, pos.y);
					}
					break;
				}
				if (!wayland && !lcd.placing && SDL_GetKeyboardFocus() == lcd.window)
				{
					// Window managers send many move events per drag. Save after it
					// settles, and flush on shutdown, instead of writing every frame.
					lcd.left = event.window.data1;
					lcd.top = event.window.data2;
					lcd.positionDirty = true;
					lcd.saveAt = SDL_GetTicks64() + 250;
				}
				break;
			default: break;
			}
		}
		return true;
	}
	// Destroyed utility windows may still have queued events. Do not dispatch
	// another window's input/resize/close events against the main game window.
	if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT)
		SDL_free(event.drop.file);
	return true;
}

bool ownsScreen(int index)
{
	return index >= 0 && index < ScreenCount && windows[index].displayed;
}

bool focusGameWindow()
{
	if (owner == nullptr)
		return false;
	if (isVmuWindow(SDL_GetKeyboardFocus()))
		SDL_RaiseWindow(owner);
	return SDL_GetKeyboardFocus() == owner;
}

bool consumeMousePositionReset() { return mouseReset.exchange(false); }

void shutdown()
{
	PreserveGLContext preserveContext;
	savePositions(true);
	for (auto& lcd : windows)
	{
		destroy(lcd);
		lcd.dismissed = false;
		lcd.retryAt = 0;
	}
	owner = nullptr;
}

void resetPositions()
{
	shutdown();
	config::setAutoSave(false);
	for (int i = 0; i < ScreenCount; ++i)
	{
		config::saveInt("vmu-sdl", std::to_string(i) + ".left", INT_MIN);
		config::saveInt("vmu-sdl", std::to_string(i) + ".top", INT_MIN);
	}
	config::setAutoSave(true);
}
}
