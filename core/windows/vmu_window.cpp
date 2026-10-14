// Portions Copyright 2026 The Hollycast Authors
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vmu_window.h"

#if defined(_WIN32) && !defined(TARGET_UWP) && !defined(LIBRETRO)
#include "cfg/option.h"
#include "rend/osd.h"
#include "ui/gui.h"
#include "oslib/oslib.h"
#include "hw/pvr/Renderer_if.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cwchar>
#include <limits>
#include <mutex>
#include <thread>

HWND getNativeHwnd();

namespace vmu_window {
namespace {
constexpr wchar_t WindowClass[] = L"HollycastVmuLcd";
constexpr int ScreenCount = 8;
constexpr int LcdWidth = 48;
constexpr int LcdHeight = 32;
constexpr LONG Unplaced = std::numeric_limits<LONG>::min();
constexpr char PositionSection[] = "vmu-windows";

struct Screen {
	std::array<u32, LcdWidth * LcdHeight> pixels {};
	u64 revision = 0;
	bool active = false;
};

struct View {
	HWND owner = nullptr;
	RECT bounds {};
	float scale = 1.f;
	int sizePercent = 100;
	UINT dpi = 96;
	bool enabled = false;
	bool visible = false;
	bool menu = false;
	bool dimWhite = false;
	bool interactive = false;
};

UINT windowDpi(HWND window)
{
	// Resolve dynamically to retain compatibility with older Windows installations.
	using GetDpi = UINT (WINAPI *)(HWND);
	static const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	return getDpi != nullptr ? getDpi(window) : 96;
}

class Windows {
public:
	Windows() { positions.fill({Unplaced, Unplaced}); }
	~Windows() { stop(); }

	void publish(int index, const u32 *pixels, bool active)
	{
		if (index < 0 || index >= ScreenCount)
			return;
		std::lock_guard<std::mutex> lock(mutex);
		Screen& screen = screens[index];
		if (pixels != nullptr)
			std::copy_n(pixels, screen.pixels.size(), screen.pixels.begin());
		screen.active = active;
		++screen.revision;
	}

	void update(bool pointerAvailable)
	{
		View next;
		next.owner = getNativeHwnd();
		next.enabled = detachedActive();
		next.visible = next.enabled && next.owner != nullptr && IsWindowVisible(next.owner)
				&& !IsIconic(next.owner) && settings.platform.isConsole()
				&& gui_state != GuiState::Main && gui_state != GuiState::Onboarding
				&& gui_state != GuiState::Loading && gui_state != GuiState::NetworkStart;
		next.menu = gui_is_open();
		next.interactive = pointerAvailable;
		next.dimWhite = !next.menu && (isOpenGL(config::RendererType) || isVulkan(config::RendererType));
		// Match the existing size at default UI scaling, including its low-res cap,
		// while keeping the user's VMU size independent of the UI scaling setting.
		float defaultScale = std::max(1.f, settings.display.dpi / 100.f * 0.75f);
		if (settings.display.width <= 640 || settings.display.height <= 480)
			defaultScale = std::min(1.2f, defaultScale);
		next.scale = std::clamp(defaultScale, 0.5f, 4.f);
		next.sizePercent = std::clamp(config::VmuWindowSize.get(), 50, 400);
		if (next.owner != nullptr)
		{
			GetClientRect(next.owner, &next.bounds);
			MapWindowPoints(next.owner, nullptr, reinterpret_cast<POINT *>(&next.bounds), 2);
			next.dpi = std::max(96u, windowDpi(next.owner));
		}
		if (!next.enabled)
		{
			if (started)
				stop();
		}
		{
			std::lock_guard<std::mutex> lock(mutex);
			view = next;
		}
		if (next.enabled && !started)
		{
			loadPositions();
			started = true;
			stopping = false;
			try {
				worker = std::thread(&Windows::run, this);
			} catch (const std::system_error& e) {
				WARN_LOG(RENDERER, "Cannot start VMU windows: %s", e.what());
			}
		}
		savePositions();
	}

	void stop()
	{
		stopping = true;
		const DWORD id = threadId.load();
		if (id != 0)
			PostThreadMessageW(id, WM_QUIT, 0, 0);
		if (worker.joinable())
			worker.join();
		threadId = 0;
		displayed = 0;
		dragging = false;
		interactive = false;
		started = false;
	}

	void reset()
	{
		std::lock_guard<std::mutex> lock(mutex);
		positions.fill({Unplaced, Unplaced});
		dirty = true;
		++resetRevision;
	}

	void savePositions()
	{
		std::array<POINT, ScreenCount> saved;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!dirty)
				return;
			saved = positions;
			dirty = false;
		}
		// Configuration is never accessed by the worker or emulation thread.
		// Each save rewrites emu.cfg, so batch all 16 writes into a single save.
		config::setAutoSave(false);
		for (int i = 0; i < ScreenCount; ++i)
		{
			const std::string prefix = std::to_string(i);
			config::saveInt(PositionSection, prefix + ".left", saved[i].x);
			config::saveInt(PositionSection, prefix + ".top", saved[i].y);
		}
		config::setAutoSave(true);
	}

	std::atomic<u32> displayed {0};
	std::atomic<bool> dragging {false};
	std::atomic<bool> interactive {false};

private:
	struct Window {
		Windows *host = nullptr;
		int index = 0;
		HWND handle = nullptr;
		HDC dc = nullptr;
		HBITMAP bitmap = nullptr;
		HGDIOBJ previousBitmap = nullptr;
		u32 *pixels = nullptr;
		SIZE size {};
		POINT dragOffset {};
		u64 revision = 0;
		BYTE alpha = 0;
		BYTE white = 0;
		bool moving = false;
		bool failed = false;

		void finishDrag()
		{
			if (!moving)
				return;
			// Clear first: ReleaseCapture synchronously sends WM_CAPTURECHANGED.
			moving = false;
			host->dragging = false;
			if (GetCapture() == handle)
				ReleaseCapture();
			RECT rect;
			if (GetWindowRect(handle, &rect))
				host->remember(index, {rect.left, rect.top});
		}

		void destroy()
		{
			finishDrag();
			if (handle != nullptr)
				DestroyWindow(handle);
			handle = nullptr;
			if (bitmap != nullptr)
			{
				SelectObject(dc, previousBitmap);
				DeleteObject(bitmap);
			}
			bitmap = nullptr;
			pixels = nullptr;
			if (dc != nullptr)
				DeleteDC(dc);
			dc = nullptr;
			size = {};
			alpha = 0;
		}

		bool allocate(SIZE requested)
		{
			if (bitmap != nullptr && size.cx == requested.cx && size.cy == requested.cy)
				return true;
			if (dc == nullptr)
				dc = CreateCompatibleDC(nullptr);
			if (dc == nullptr)
				return false;
			BITMAPINFO info {};
			info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			info.bmiHeader.biWidth = requested.cx;
			info.bmiHeader.biHeight = -requested.cy;
			info.bmiHeader.biPlanes = 1;
			info.bmiHeader.biBitCount = 32;
			info.bmiHeader.biCompression = BI_RGB;
			void *bits = nullptr;
			HBITMAP next = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
			if (next == nullptr)
				return false;
			HGDIOBJ old = SelectObject(dc, next);
			if (old == nullptr || old == HGDI_ERROR)
			{
				DeleteObject(next);
				return false;
			}
			if (bitmap != nullptr)
				DeleteObject(bitmap);
			else
				previousBitmap = old;
			bitmap = next;
			pixels = static_cast<u32 *>(bits);
			size = requested;
			alpha = 0;
			return true;
		}

		bool paint(const Screen& screen, BYTE opacity, BYTE whiteLevel)
		{
			if (revision == screen.revision && alpha == opacity && white == whiteLevel)
				return true;
			// Layered windows require premultiplied BGRA. Apply opacity exactly once;
			// retain the existing GL/Vulkan tint and the LCD's vertical orientation.
			const u32 channel = (whiteLevel * opacity + 127) / 255;
			const u32 on = (u32(opacity) << 24) | channel * 0x010101u;
			const u32 off = u32(opacity) << 24;
			for (LONG y = 0; y < size.cy; ++y)
				for (LONG x = 0; x < size.cx; ++x)
					pixels[y * size.cx + x] = (screen.pixels[(LcdHeight - 1 - y * LcdHeight / size.cy) * LcdWidth
							+ x * LcdWidth / size.cx] & 0x00ffffffu) != 0 ? on : off;
			POINT source {};
			BLENDFUNCTION blend {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
			if (!UpdateLayeredWindow(handle, nullptr, nullptr, &size, dc, &source, 0, &blend, ULW_ALPHA))
				return false;
			revision = screen.revision;
			alpha = opacity;
			white = whiteLevel;
			return true;
		}
	};

	static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
	{
		auto *window = reinterpret_cast<Window *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
		if (message == WM_NCCREATE)
		{
			window = static_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
			SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
		}
		if (window == nullptr)
			return DefWindowProcW(handle, message, wparam, lparam);
		switch (message)
		{
		case WM_NCHITTEST:
			if (!window->host->interactive)
				return HTTRANSPARENT;
			break;
		case WM_MOUSEACTIVATE:
			return MA_NOACTIVATE;
		case WM_SETCURSOR:
			SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
			return TRUE;
		case WM_LBUTTONDOWN:
		{
			if (!window->host->interactive
					|| GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) != GetAncestor(handle, GA_ROOTOWNER))
				return 0;
			POINT cursor;
			RECT rect;
			if (!GetCursorPos(&cursor) || !GetWindowRect(handle, &rect))
				return 0;
			SetCapture(handle);
			if (GetCapture() != handle)
				return 0;
			window->dragOffset = {cursor.x - rect.left, cursor.y - rect.top};
			window->moving = true;
			window->host->dragging = true;
			return 0;
		}
		case WM_MOUSEMOVE:
			if (window->moving)
			{
				POINT cursor;
				GetCursorPos(&cursor);
				SetWindowPos(handle, nullptr, cursor.x - window->dragOffset.x, cursor.y - window->dragOffset.y,
						0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
			}
			return 0;
		case WM_LBUTTONUP:
		case WM_CAPTURECHANGED:
		case WM_CANCELMODE:
			window->finishDrag();
			return 0;
		case WM_CLOSE:
			// Visibility belongs to the Show VMU setting, never to emulator shutdown.
			return 0;
		}
		return DefWindowProcW(handle, message, wparam, lparam);
	}

	void loadPositions()
	{
		std::array<POINT, ScreenCount> loaded;
		for (int i = 0; i < ScreenCount; ++i)
		{
			const std::string prefix = std::to_string(i);
			loaded[i] = {config::loadInt(PositionSection, prefix + ".left", Unplaced),
					config::loadInt(PositionSection, prefix + ".top", Unplaced)};
		}
		std::lock_guard<std::mutex> lock(mutex);
		if (!dirty)
			positions = loaded;
	}

	void remember(int index, POINT position)
	{
		std::lock_guard<std::mutex> lock(mutex);
		positions[index] = position;
		dirty = true;
	}

	POINT defaultPosition(int index, const View& current, SIZE size)
	{
		const LONG padding = LONG(8 * current.scale);
		return {(index & 2) ? current.bounds.right - padding - size.cx : current.bounds.left + padding,
				(index & 4) ? current.bounds.bottom - padding - size.cy - (index & 1) * (padding + size.cy)
						: current.bounds.top + padding + (index & 1) * (padding + size.cy)};
	}

	POINT reachablePosition(POINT position, SIZE size, POINT fallback)
	{
		// Bounds-check before addition: configuration files may contain arbitrary ints.
		if (position.x == Unplaced || position.y == Unplaced
				|| position.x > LONG_MAX - size.cx || position.y > LONG_MAX - size.cy)
			position = fallback;
		RECT rect {position.x, position.y, position.x + size.cx, position.y + size.cy};
		HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONULL);
		if (monitor == nullptr)
		{
			position = fallback;
			monitor = MonitorFromPoint(position, MONITOR_DEFAULTTONEAREST);
		}
		MONITORINFO info {sizeof(MONITORINFO)};
		if (GetMonitorInfoW(monitor, &info))
		{
			position.x = std::clamp(position.x, info.rcWork.left, std::max(info.rcWork.left, info.rcWork.right - size.cx));
			position.y = std::clamp(position.y, info.rcWork.top, std::max(info.rcWork.top, info.rcWork.bottom - size.cy));
		}
		return position;
	}

	void tick(std::array<Window, ScreenCount>& windows, u64& lastReset)
	{
		View current;
		std::array<Screen, ScreenCount> frames;
		std::array<POINT, ScreenCount> locations;
		u64 reset;
		{
			std::lock_guard<std::mutex> lock(mutex);
			current = view;
			frames = screens;
			locations = positions;
			reset = resetRevision;
		}
		const bool focused = current.owner != nullptr
				&& GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == current.owner;
		const bool canInteract = current.enabled && current.visible && current.interactive && focused;
		interactive = canInteract;
		u32 mask = 0;
		for (int i = 0; i < ScreenCount; ++i)
		{
			Window& window = windows[i];
			if (window.moving)
			{
				// Async mouse state uses physical buttons, including swapped-button setups.
				const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
				if (!canInteract || GetCapture() != window.handle || (GetAsyncKeyState(button) & 0x8000) == 0)
					window.finishDrag();
			}
			if (!current.enabled || !frames[i].active)
			{
				window.destroy();
				window.failed = false;
				continue;
			}
			if (!current.visible)
			{
				if (window.handle != nullptr && IsWindowVisible(window.handle))
					ShowWindow(window.handle, SW_HIDE);
				continue;
			}
			if (window.failed && reset == lastReset)
				continue;
			window.failed = false;
			const float monitorScale = window.handle == nullptr ? current.scale
					: std::clamp(current.scale * windowDpi(window.handle) / current.dpi, 0.5f, 8.f);
			const float scale = monitorScale * current.sizePercent / 100.f;
			SIZE size {LONG(96 * scale), LONG(64 * scale)};
			POINT fallback = defaultPosition(i, current, size);
			if (window.handle == nullptr)
			{
				POINT location = reachablePosition(locations[i], size, fallback);
				const std::wstring title = L"Hollycast VMU " + std::to_wstring(i / 2 + 1) + L" / " + std::to_wstring(i % 2 + 1);
				window.handle = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
						WindowClass, title.c_str(), WS_POPUP, location.x, location.y, size.cx, size.cy,
						current.owner, nullptr, GetModuleHandleW(nullptr), &window);
			}
			else if (!window.moving)
			{
				RECT rect;
				GetWindowRect(window.handle, &rect);
				POINT location = reachablePosition(reset != lastReset ? POINT{Unplaced, Unplaced} : POINT{rect.left, rect.top}, size, fallback);
				if (location.x != rect.left || location.y != rect.top)
				{
					SetWindowPos(window.handle, nullptr, location.x, location.y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
					remember(i, location);
				}
			}
			const BYTE opacity = current.menu ? 128 : 192;
			const BYTE white = current.dimWhite ? 191 : 255;
			if (window.handle != nullptr)
			{
				// Hidden/captured cursors and background applications pass through the LCDs.
				// The extended style also handles hit testing across the two UI threads.
				const LONG_PTR style = GetWindowLongPtrW(window.handle, GWL_EXSTYLE);
				const LONG_PTR nextStyle = canInteract ? style & ~WS_EX_TRANSPARENT : style | WS_EX_TRANSPARENT;
				if (style != nextStyle)
				{
					SetWindowLongPtrW(window.handle, GWL_EXSTYLE, nextStyle);
					SetWindowPos(window.handle, nullptr, 0, 0, 0, 0,
							SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
				}
				// HWND_BOTTOM clears topmost status and lowers inactive LCDs behind other
				// applications without raising or moving their owner window.
				if (((style & WS_EX_TOPMOST) != 0) != focused)
					SetWindowPos(window.handle, focused ? HWND_TOPMOST : HWND_BOTTOM, 0, 0, 0, 0,
							SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
			}
			if (window.handle == nullptr || !window.allocate(size) || !window.paint(frames[i], opacity, white))
			{
				WARN_LOG(RENDERER, "Cannot display VMU window %d (Windows error %lu); using in-game overlay", i, GetLastError());
				window.destroy();
				window.failed = true;
				continue;
			}
			if (!IsWindowVisible(window.handle))
				ShowWindow(window.handle, SW_SHOWNOACTIVATE);
			mask |= 1u << i;
		}
		lastReset = reset;
		displayed = mask;
	}

	void run()
	{
		// A separate message pump keeps LCD dragging independent of game frame pacing.
		MSG message;
		PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
		threadId = GetCurrentThreadId();
		WNDCLASSW windowClass {};
		windowClass.lpfnWndProc = procedure;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = WindowClass;
		const ATOM registered = RegisterClassW(&windowClass);
		if (registered == 0)
		{
			WARN_LOG(RENDERER, "Cannot register VMU windows (Windows error %lu)", GetLastError());
			threadId = 0;
			return;
		}
		const UINT_PTR timer = SetTimer(nullptr, 0, 16, nullptr);
		std::array<Window, ScreenCount> windows;
		for (int i = 0; i < ScreenCount; ++i)
		{
			windows[i].host = this;
			windows[i].index = i;
		}
		u64 lastReset = 0;
		if (timer == 0)
			WARN_LOG(RENDERER, "Cannot schedule VMU windows (Windows error %lu)", GetLastError());
		else
			while (!stopping && GetMessageW(&message, nullptr, 0, 0) > 0)
			{
				if (message.message == WM_TIMER && message.hwnd == nullptr)
				{
					try {
						tick(windows, lastReset);
					} catch (const std::exception& e) {
						WARN_LOG(RENDERER, "VMU window update failed: %s; using in-game overlays", e.what());
						break;
					}
				}
				else
				{
					TranslateMessage(&message);
					DispatchMessageW(&message);
				}
			}
		if (timer != 0)
			KillTimer(nullptr, timer);
		for (Window& window : windows)
			window.destroy();
		UnregisterClassW(WindowClass, windowClass.hInstance);
		displayed = 0;
		threadId = 0;
	}

	std::mutex mutex;
	std::array<Screen, ScreenCount> screens;
	std::array<POINT, ScreenCount> positions;
	View view;
	u64 resetRevision = 0;
	bool dirty = false;
	bool started = false;
	std::thread worker;
	std::atomic<DWORD> threadId {0};
	std::atomic<bool> stopping {false};
};

Windows windows;
}

bool detachedActive()
{
	return config::MovableVMUs && config::FloatVMUs && settings.platform.isConsole();
}
void publishScreen(int index, const u32 *pixels, bool active) { windows.publish(index, pixels, active); }
void update(bool pointerAvailable) { windows.update(pointerAvailable); }
void shutdown() { windows.stop(); windows.savePositions(); }
void resetPositions() { windows.reset(); }

bool ownsScreen(int index)
{
	return config::MovableVMUs && config::FloatVMUs && index >= 0 && index < ScreenCount
			&& (windows.displayed.load() & (1u << index)) != 0;
}

bool blocksMouseInput()
{
	if (windows.dragging)
		return true;
	if (!windows.interactive || windows.displayed == 0)
		return false;
	POINT cursor;
	if (!GetCursorPos(&cursor))
		return false;
	wchar_t name[64];
	return GetClassNameW(WindowFromPoint(cursor), name, int(std::size(name))) != 0
			&& std::wcscmp(name, WindowClass) == 0;
}
}
#endif
