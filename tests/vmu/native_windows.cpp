// Portions Copyright 2026 The Hollycast Authors
// Real Win32 windows and input exercise the production module with simulated LCD data.
#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <stdexcept>
#include <cstdint>

extern HWND owner;
namespace faults
{
enum class Failure
{
	None,
	Register,
	Timer,
	Create,
	Allocate,
	Paint,
	Exception
};
std::atomic<Failure> failure{Failure::None};
std::atomic<bool> persistent{false};
std::atomic<int> attempts{0};
std::atomic<int> dpiRatio{0};
std::atomic<bool> inspectPixels{false};
std::atomic<bool> pixelsMatch{false};
std::atomic<int> pixelChecks{0};
std::atomic<LONG> paintedWidth{0}, paintedHeight{0};
std::atomic<unsigned> expectedOpacity{192}, expectedWhite{255};

bool fail(Failure point)
{
	if (failure != point)
		return false;
	++attempts;
	if (!persistent)
		failure = Failure::None;
	SetLastError(ERROR_NOT_ENOUGH_MEMORY);
	return true;
}

ATOM WINAPI registerClass(const WNDCLASSW *cls)
{
	if (std::wcscmp(cls->lpszClassName, L"HollycastVmuLcd") == 0 && fail(Failure::Register))
		return 0;
	return RegisterClassW(cls);
}

UINT_PTR WINAPI setTimer(HWND window, UINT_PTR id, UINT interval, TIMERPROC callback)
{
	return fail(Failure::Timer) ? 0 : SetTimer(window, id, interval, callback);
}

HWND WINAPI createWindow(DWORD extended, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y, int width, int height,
						 HWND parent, HMENU menu, HINSTANCE instance, LPVOID param)
{
	if (fail(Failure::Create))
		return nullptr;
	return CreateWindowExW(extended, cls, title, style, x, y, width, height, parent, menu, instance, param);
}

HBITMAP WINAPI createDib(HDC dc, const BITMAPINFO *info, UINT usage, void **bits, HANDLE section, DWORD offset)
{
	return fail(Failure::Allocate) ? nullptr : CreateDIBSection(dc, info, usage, bits, section, offset);
}

bool pattern(LONG x, LONG y)
{
	return (x * 7 + y * 11) % 13 < 6;
}

BOOL WINAPI updateLayered(HWND window, HDC destination, POINT *position, SIZE *size, HDC source, POINT *origin,
						  COLORREF key, BLENDFUNCTION *blend, DWORD flags)
{
	if (fail(Failure::Exception))
		throw std::runtime_error("injected paint exception");
	if (fail(Failure::Paint))
		return FALSE;
	if (inspectPixels)
	{
		DIBSECTION dib{};
		bool matches = GetObjectW(GetCurrentObject(source, OBJ_BITMAP), sizeof(dib), &dib) == sizeof(dib);
		const auto *pixels = static_cast<const std::uint32_t *>(dib.dsBm.bmBits);
		const unsigned opacity = expectedOpacity;
		const unsigned channel = (expectedWhite * opacity + 127) / 255;
		const std::uint32_t off = opacity << 24;
		const std::uint32_t on = off | channel * 0x010101u;
		// Independent destination-pixel oracle covers fractional sizing, downscaling,
		// vertical orientation, and every premultiplied alpha byte.
		for (LONG y = 0; matches && y < size->cy; ++y)
			for (LONG x = 0; x < size->cx; ++x)
				if (pixels[y * size->cx + x] != (pattern(x * 48 / size->cx, 31 - y * 32 / size->cy) ? on : off))
				{
					matches = false;
					break;
				}
		paintedWidth = size->cx;
		paintedHeight = size->cy;
		pixelsMatch = matches;
		++pixelChecks;
	}
	return UpdateLayeredWindow(window, destination, position, size, source, origin, key, blend, flags);
}

UINT WINAPI windowDpi(HWND window)
{
	if (dpiRatio != 0)
		return (window == owner ? dpiRatio < 0 : dpiRatio > 0) ? 192 : 96;
	using GetDpi = UINT(WINAPI *)(HWND);
	static const auto getDpi =
		reinterpret_cast<GetDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
	return getDpi != nullptr ? getDpi(window) : 96;
}

FARPROC WINAPI getProcAddress(HMODULE module, LPCSTR name)
{
	if (std::strcmp(name, "GetDpiForWindow") == 0)
		return reinterpret_cast<FARPROC>(windowDpi);
	return GetProcAddress(module, name);
}
} // namespace faults

// Fault injection stays in this translation unit; production needs no test hooks.
#define RegisterClassW faults::registerClass
#define SetTimer faults::setTimer
#define CreateWindowExW faults::createWindow
#define CreateDIBSection faults::createDib
#define UpdateLayeredWindow faults::updateLayered
#define GetProcAddress faults::getProcAddress
#include "../../core/windows/vmu_window.cpp"
#undef RegisterClassW
#undef SetTimer
#undef CreateWindowExW
#undef CreateDIBSection
#undef UpdateLayeredWindow
#undef GetProcAddress
#include <cstdio>
#include <cstdlib>
#include <map>
#include <chrono>
#include <functional>
#include <vector>

TestSettings settings;
GuiState gui_state = GuiState::Closed;
namespace config
{
bool MovableVMUs = false;
TestIntOption VmuWindowSize;
bool FloatVMUs = true;
RenderType RendererType = RenderType::DirectX11;
std::map<std::string, int> values;
bool autoSaving = true;
int fileSaves = 0;
int loadInt(const std::string &s, const std::string &k, int fallback)
{
	auto it = values.find(s + k);
	return it == values.end() ? fallback : it->second;
}
void saveInt(const std::string &s, const std::string &k, int v)
{
	values[s + k] = v;
	if (autoSaving)
		++fileSaves;
}
void setAutoSave(bool enabled)
{
	autoSaving = enabled;
	if (enabled)
		++fileSaves;
}
} // namespace config
HWND owner;
HWND other;
HWND previousForeground;
POINT previousCursor;
bool pointerAvailable = true;
bool buttonHeld = false;
int checks = 0;
int menuInset = 30;
int getScaledTopInset()
{
	return menuInset;
}
HWND getNativeHwnd()
{
	return owner;
}
void testWarning(const char *, ...) {}

bool primaryButton(bool down)
{
	INPUT input{};
	input.type = INPUT_MOUSE;
	const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
	input.mi.dwFlags = swapped ? (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP)
							   : (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
	if (SendInput(1, &input, sizeof(input)) != 1)
		return false;
	buttonHeld = down;
	return true;
}
void cleanup()
{
	if (buttonHeld)
		primaryButton(false);
	vmu_window::shutdown();
	DestroyWindow(other);
	DestroyWindow(owner);
	if (IsWindow(previousForeground))
		SetForegroundWindow(previousForeground);
	SetCursorPos(previousCursor.x, previousCursor.y);
}
void require(bool ok, const char *label)
{
	if (!ok)
	{
		std::fprintf(stderr, "FAIL %s\n", label);
		cleanup();
		std::exit(1);
	}
	++checks;
	std::printf("PASS %s\n", label);
}
std::vector<HWND> lcds()
{
	std::vector<HWND> handles;
	EnumWindows(
		[](HWND w, LPARAM p) -> BOOL
		{
			DWORD process = 0;
			GetWindowThreadProcessId(w, &process);
			wchar_t name[64];
			if (process == GetCurrentProcessId() && GetClassNameW(w, name, 64) != 0
				&& std::wcscmp(name, L"HollycastVmuLcd") == 0 && IsWindowVisible(w))
				reinterpret_cast<std::vector<HWND> *>(p)->push_back(w);
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&handles));
	return handles;
}
void pump()
{
	MSG msg;
	while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	vmu_window::update(pointerAvailable);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
bool until(const std::function<bool()> &predicate)
{
	for (int i = 0; i < 1000; ++i)
	{
		pump();
		if (predicate())
			return true;
	}
	return false;
}
HWND firstLcd()
{
	for (HWND w : lcds())
	{
		wchar_t title[80];
		if (GetWindowTextW(w, title, 80) != 0 && std::wcscmp(title, L"Hollycast VMU 1 / 1") == 0)
			return w;
	}
	return nullptr;
}
int redAt(HWND w)
{
	RECT rect;
	GetWindowRect(w, &rect);
	HDC dc = GetDC(nullptr);
	COLORREF color = GetPixel(dc, rect.left + 12, rect.top + 12);
	ReleaseDC(nullptr, dc);
	return color == CLR_INVALID ? -1 : GetRValue(color);
}
void startDrag(HWND w)
{
	RECT rect;
	GetWindowRect(w, &rect);
	// Size/reset tests can overlap LCDs. Raise only the selected test LCD so the
	// injected click cannot land on another slot or an unrelated application.
	SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
	SetCursorPos(rect.left + 12, rect.top + 12);
	const POINT click{rect.left + 12, rect.top + 12};
	if (WindowFromPoint(click) != w)
		require(false, "drag click is confined to its test LCD");
	if (!primaryButton(true))
		require(false, "inject primary mouse button");
	// Wait for the native thread to acquire capture before moving across monitors.
	const DWORD thread = GetWindowThreadProcessId(w, nullptr);
	if (!until(
			[&]
			{
				GUITHREADINFO info{sizeof(GUITHREADINFO)};
				return GetGUIThreadInfo(thread, &info) && info.hwndCapture == w;
		}))
	{
		DWORD process = 0;
		const HWND foreground = GetForegroundWindow();
		GetWindowThreadProcessId(foreground, &process);
		std::fprintf(stderr, "Drag diagnostic: foreground=%p owner=%p process=%lu test=%lu\n",
			foreground, owner, process, GetCurrentProcessId());
		require(false, "native drag acquires capture");
	}
}
void releaseButton()
{
	if (!primaryButton(false))
		require(false, "release primary mouse button");
	for (int i = 0; i < 4; ++i)
		pump();
}
void moveAway()
{
	SetCursorPos(450, 350);
}

int main()
{
	SetProcessDPIAware();
	GetCursorPos(&previousCursor);
	previousForeground = GetForegroundWindow();
	HBRUSH brush = CreateSolidBrush(RGB(255, 0, 0));
	WNDCLASSW cls{};
	cls.lpfnWndProc = DefWindowProcW;
	cls.hInstance = GetModuleHandleW(nullptr);
	cls.lpszClassName = L"VmuCleanupOwner";
	cls.hbrBackground = brush;
	RegisterClassW(&cls);
	owner = CreateWindowExW(0, cls.lpszClassName, L"VMU cleanup validation", WS_OVERLAPPEDWINDOW, 100, 100, 700, 500,
							nullptr, nullptr, cls.hInstance, nullptr);
	other = CreateWindowExW(0, cls.lpszClassName, L"VMU focus validation", WS_OVERLAPPEDWINDOW, 900, 100, 400, 350,
							nullptr, nullptr, cls.hInstance, nullptr);
	ShowWindow(owner, SW_SHOW);
	UpdateWindow(owner);
	// Windows blocks foreground stealing from a background launcher. Click only our
	// temporarily raised test window, then return it to the emulator's normal Z order.
	SetWindowPos(owner, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	SetCursorPos(450, 350);
	POINT activationPoint{450, 350};
	require(WindowFromPoint(activationPoint) == owner, "activation click is confined to the test window");
	if (!primaryButton(true) || !primaryButton(false))
		require(false, "activate the test owner");
	pump();
	SetForegroundWindow(owner);
	require(until([] { return GetForegroundWindow() == owner; }), "validation owner becomes foreground");
	SetWindowPos(owner, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	DWORD initialGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
	std::array<u32, 48 * 32> frame{};
	frame.fill(0xff000000u);
	for (int i = 0; i < 8; ++i)
		vmu_window::publishScreen(i, frame.data(), true);
	for (int i = 0; i < 5; ++i)
		pump();
	require(lcds().empty(), "disabled mode creates no windows");
	config::MovableVMUs = true;
	require(until([] { return lcds().size() == 8 && vmu_window::ownsScreen(7); }),
			"eight LCD windows and overlay ownership");
	HWND first = firstLcd();
	RECT client{};
	GetClientRect(owner, &client);
	MapWindowPoints(owner, nullptr, reinterpret_cast<POINT *>(&client), 2);
	RECT placed{};
	GetWindowRect(first, &placed);
	require(placed.top == client.top + menuInset + 8, "default first LCD clears the menu bar inset");
	menuInset = 44;
	require(until(
				[&]
				{
					GetWindowRect(first, &placed);
					return placed.top == client.top + menuInset + 8;
				}),
			"unplaced LCD follows a menu inset published after startup");
	menuInset = 30;
	require(until(
				[&]
				{
					GetWindowRect(first, &placed);
					return placed.top == client.top + menuInset + 8;
				}),
			"default placement follows the current UI inset without changing VMU size");
	const auto allSizes = [](LONG width, LONG height)
	{
		const auto handles = lcds();
		if (handles.size() != 8)
			return false;
		for (HWND handle : handles)
		{
			RECT rect{};
			if (!GetWindowRect(handle, &rect) || rect.right - rect.left != width || rect.bottom - rect.top != height)
				return false;
		}
		return true;
	};
	require(until([&] { return allSizes(96, 64); }), "100 percent preserves the previous default size for all slots");
	RECT sizeOrigin{};
	GetWindowRect(first, &sizeOrigin);
	config::VmuWindowSize = 50;
	require(until([&] { return allSizes(48, 32); }), "50 percent resizes all slots live without LCD republication");
	require(firstLcd() == first, "live resize preserves the native window handle");
	config::VmuWindowSize = 137;
	require(until([&] { return allSizes(131, 87); }), "arbitrary slider percentages scale both dimensions");
	config::VmuWindowSize = 400;
	require(until([&] { return allSizes(384, 256); }), "400 percent resizes every slot live");
	config::VmuWindowSize = 0;
	require(until([&] { return allSizes(48, 32); }), "invalid configuration clamps to the minimum size");
	config::VmuWindowSize = INT_MAX;
	require(until([&] { return allSizes(384, 256); }), "invalid configuration clamps to the maximum size");
	config::VmuWindowSize = 100;
	require(until([&] { return allSizes(96, 64); }), "100 percent restores default dimensions");
	RECT resized{};
	GetWindowRect(first, &resized);
	require(resized.left == sizeOrigin.left && resized.top == sizeOrigin.top,
			"resize preserves a reachable window position");
	settings.display.uiScale = 0.5f;
	for (int i = 0; i < 5; ++i)
		pump();
	require(allSizes(96, 64), "50 percent UI scaling does not change VMU size");
	settings.display.uiScale = 4.f;
	for (int i = 0; i < 5; ++i)
		pump();
	require(allSizes(96, 64), "400 percent UI scaling does not change VMU size");
	settings.display.dpi = 200;
	require(until([&] { return allSizes(144, 96); }), "100 percent retains the existing default DPI scaling");
	settings.display.width = 640;
	require(until([&] { return allSizes(115, 76); }), "100 percent retains the default low-resolution size cap");
	config::VmuWindowSize = 50;
	require(until([&] { return allSizes(57, 38); }), "50 percent applies after the default DPI cap");
	settings.display.width = 1280;
	settings.display.dpi = 96;
	settings.display.uiScale = 1.f;
	config::VmuWindowSize = 175;
	require(until([&] { return allSizes(168, 112); }), "size selection applies before worker restart");
	vmu_window::shutdown();
	require(until([&] { return allSizes(168, 112); }), "size selection survives window worker recreation");
	first = firstLcd();
	config::VmuWindowSize = 100;
	require(until([&] { return allSizes(96, 64); }), "default size restored before drag and opacity regression checks");
	require(first && (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_NOACTIVATE), "non-activating window style");
	require(GetForegroundWindow() == owner, "creation preserves owner focus");
	require(until([&] { return (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }),
			"foreground LCDs remain above the emulator");
	pointerAvailable = false;
	require(until([&] { return (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0; }),
			"captured gameplay makes VMUs click-through");
	SendMessageW(first, WM_LBUTTONDOWN, MK_LBUTTON, 0);
	require(!vmu_window::blocksMouseInput(), "captured gameplay neither drags nor blocks input");
	pointerAvailable = true;
	require(until([&] { return (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0; }),
			"free pointer restores interactive VMUs without placement mode");
	require(until([&] { return std::abs(redAt(first) - 63) <= 4; }), "gameplay opacity remains 75 percent");
	gui_state = GuiState::Commands;
	require(until([&] { return std::abs(redAt(first) - 127) <= 4; }), "menu opacity remains 50 percent");
	gui_state = GuiState::Closed;
	RECT root;
	GetWindowRect(owner, &root);
	const int savesBeforeDrag = config::fileSaves;
	startDrag(first);
	require(vmu_window::blocksMouseInput(), "drag suppresses emulator mouse input");
	SetCursorPos(root.right + 60, root.top + 140);
	RECT moved{};
	require(until(
				[&]
				{
					GetWindowRect(first, &moved);
					return moved.left >= root.right;
				}),
			"real mouse drag moves beyond emulator boundary");
	releaseButton();
	moveAway();
	require(!vmu_window::blocksMouseInput(), "release restores normal emulator clicks");
	require(config::values["vmu-windows0.left"] == moved.left, "dragged position is saved by slot");
	require(config::fileSaves == savesBeforeDrag + 1, "one completed drag batches positions into one config save");
	menuInset = 60;
	for (int i = 0; i < 5; ++i)
		pump();
	GetWindowRect(first, &placed);
	require(placed.top == moved.top, "menu inset changes preserve deliberately dragged positions");
	menuInset = 30;
	std::vector<HMONITOR> monitors;
	EnumDisplayMonitors(
		nullptr, nullptr,
		[](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL
		{
			reinterpret_cast<std::vector<HMONITOR> *>(data)->push_back(monitor);
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&monitors));
	bool secondMonitor = false;
	for (HMONITOR monitor : monitors)
	{
		if (monitor == MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST))
			continue;
		MONITORINFO info{sizeof(MONITORINFO)};
		GetMonitorInfoW(monitor, &info);
		startDrag(first);
		SetCursorPos(info.rcWork.left + 120, info.rcWork.top + 120);
		bool reached = until([&] { return MonitorFromWindow(first, MONITOR_DEFAULTTONEAREST) == monitor; });
		if (!reached)
		{
			RECT diagnostic;
			GetWindowRect(first, &diagnostic);
			POINT cursor;
			GetCursorPos(&cursor);
			GUITHREADINFO threadInfo{sizeof(GUITHREADINFO)};
			GetGUIThreadInfo(GetWindowThreadProcessId(first, nullptr), &threadInfo);
			std::printf("Drag diagnostic: rect=%ld,%ld cursor=%ld,%ld target=%ld,%ld focus=%p owner=%p capture=%p "
						"lcd=%p button=%d\n",
						diagnostic.left, diagnostic.top, cursor.x, cursor.y, info.rcWork.left + 120,
						info.rcWork.top + 120, GetForegroundWindow(), owner, threadInfo.hwndCapture, first,
						GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON));
		}
		require(reached, "real mouse drag reaches the second monitor");
		releaseButton();
		moveAway();
		secondMonitor = true;
		break;
	}
	if (!secondMonitor)
		std::puts("SKIP second-monitor drag: one monitor connected");
	require(GetForegroundWindow() == owner, "cross-window drag preserves owner focus");
	vmu_window::shutdown();
	require(lcds().empty(), "shutdown destroys LCD windows");
	require(until([] { return lcds().size() == 8; }), "windows restart after renderer recreation");
	first = firstLcd();
	GetWindowRect(first, &moved);
	require(moved.left == config::values["vmu-windows0.left"] && moved.top == config::values["vmu-windows0.top"],
			"saved placement survives recreation");
	vmu_window::resetPositions();
	require(until(
				[&]
				{
					GetWindowRect(first, &moved);
					return moved.left < root.right;
				}),
			"reset restores reachable placement");
	require(moved.top == client.top + menuInset + 8, "reset placement also clears the menu bar");
	startDrag(first);
	SendMessageW(first, WM_CANCELMODE, 0, 0);
	moveAway();
	require(!vmu_window::blocksMouseInput(), "cancel mode clears dragging before button release");
	releaseButton();
	GetWindowRect(first, &moved);
	SetCursorPos(moved.left + 12, moved.top + 12);
	// No physical button is held: emulate a missing release message after a stale down.
	SendMessageW(first, WM_LBUTTONDOWN, MK_LBUTTON, 0);
	for (int i = 0; i < 5; ++i)
		pump();
	moveAway();
	require(!vmu_window::blocksMouseInput(), "physical-button watchdog clears a missing release");
	startDrag(first);
	pointerAvailable = false;
	require(until([&] { return (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0; }),
			"capture change cancels an active VMU drag");
	moveAway();
	require(!vmu_window::blocksMouseInput(), "capture cancellation leaves no input suppression");
	releaseButton();
	pointerAvailable = true;
	require(until([&] { return (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0; }),
			"release of capture restores dragging");
	startDrag(first);
	ShowWindow(other, SW_SHOW);
	SetForegroundWindow(other);
	require(until(
				[&]
				{
					return GetForegroundWindow() == other &&
						   (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0 &&
						   (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0;
				}),
			"focus loss cancels drag and lowers click-through LCDs");
	moveAway();
	require(!vmu_window::blocksMouseInput(), "background LCDs do not suppress input");
	releaseButton();
	GetWindowRect(first, &moved);
	SendMessageW(first, WM_LBUTTONDOWN, MK_LBUTTON, 0);
	SendMessageW(first, WM_MOUSEMOVE, MK_LBUTTON, 0);
	RECT background;
	GetWindowRect(first, &background);
	require(background.left == moved.left && background.top == moved.top && GetForegroundWindow() == other,
			"background LCD cannot start dragging or steal focus");
	SetForegroundWindow(owner);
	require(until(
				[&]
				{
					return GetForegroundWindow() == owner &&
						   (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TRANSPARENT) == 0 &&
						   (GetWindowLongPtrW(first, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
				}),
			"foreground restoration restores draggable topmost LCDs");
	startDrag(first);
	ShowWindow(owner, SW_MINIMIZE);
	require(until([] { return lcds().empty() && !vmu_window::ownsScreen(0); }),
			"minimizing during drag hides LCDs and clears ownership");
	moveAway();
	require(!vmu_window::blocksMouseInput(), "minimizing during drag clears input suppression");
	releaseButton();
	ShowWindow(owner, SW_RESTORE);
	SetForegroundWindow(owner);
	require(until([] { return lcds().size() == 8 && GetForegroundWindow() == owner; }),
			"restoring owner restores LCD windows");
	first = firstLcd();
	startDrag(first);
	vmu_window::publishScreen(0, nullptr, false);
	require(until([] { return lcds().size() == 7 && !vmu_window::ownsScreen(0); }),
			"slot removal during drag clears display ownership");
	moveAway();
	require(!vmu_window::blocksMouseInput(), "slot removal during drag clears input suppression");
	releaseButton();
	vmu_window::publishScreen(0, frame.data(), true);
	require(until([] { return vmu_window::ownsScreen(0); }), "slot publication recreates its LCD");
	std::thread producer(
		[&]
		{
			for (int i = 0; i < 2000; ++i)
				vmu_window::publishScreen(0, frame.data(), true);
		});
	for (int i = 0; i < 10; ++i)
		pump();
	producer.join();
	require(vmu_window::ownsScreen(0), "concurrent LCD publication remains live");
	settings.platform.console = false;
	require(!vmu_window::detachedActive(), "arcade platform disables detached VMU activation");
	require(until([] { return lcds().empty() && !vmu_window::ownsScreen(0); }),
			"arcade platform restores original overlay ownership");
	settings.platform.console = true;
	require(until([] { return lcds().size() == 8 && vmu_window::ownsScreen(0); }),
			"console platform restores movable VMUs");
	config::MovableVMUs = false;
	require(until([] { return lcds().empty() && !vmu_window::ownsScreen(0); }),
			"disable restores original overlay ownership");
	vmu_window::shutdown();

	// Keep one slot active so each injected failure has an unambiguous owner.
	for (int i = 1; i < 8; ++i)
		vmu_window::publishScreen(i, nullptr, false);
	config::MovableVMUs = true;
	for (auto failure : {faults::Failure::Register, faults::Failure::Timer, faults::Failure::Create,
						 faults::Failure::Allocate, faults::Failure::Paint, faults::Failure::Exception})
	{
		vmu_window::shutdown();
		faults::persistent = true;
		faults::failure = failure;
		const int before = faults::attempts;
		require(until([&] { return faults::attempts > before; }), "injected Windows failure is exercised");
		require(!vmu_window::ownsScreen(0), "failed worker or slot leaves the in-game overlay available");
		for (int i = 0; i < 10; ++i)
			pump();
		require(faults::attempts == before + 1, "persistent failure retries are throttled");
		faults::persistent = false;
		faults::failure = faults::Failure::None;
		require(until([] { return vmu_window::ownsScreen(0); }),
				"worker or slot recovers without toggling or resetting");
	}

	WNDCLASSW existing{};
	require(GetClassInfoW(GetModuleHandleW(nullptr), L"HollycastVmuLcd", &existing), "read the registered VMU class");
	vmu_window::shutdown();
	require(RegisterClassW(&existing) != 0, "pre-register the VMU class");
	require(until([] { return vmu_window::ownsScreen(0); }), "already registered VMU class permits startup");
	vmu_window::shutdown();
	require(GetClassInfoW(GetModuleHandleW(nullptr), L"HollycastVmuLcd", &existing),
			"worker preserves a borrowed class registration");
	UnregisterClassW(L"HollycastVmuLcd", GetModuleHandleW(nullptr));

	for (LONG y = 0; y < 32; ++y)
		for (LONG x = 0; x < 48; ++x)
			frame[y * 48 + x] = faults::pattern(x, y) ? 0xffffffffu : 0xff000000u;
	faults::inspectPixels = true;
	struct PixelCase
	{
		int percent;
		int dpi;
		int ratio;
		LONG width;
		LONG height;
		bool menu;
		bool dim;
	};
	for (const auto test : {PixelCase{100, 96, 0, 96, 64, false, false}, PixelCase{137, 96, 0, 131, 87, true, false},
							PixelCase{400, 96, 0, 384, 256, false, true}, PixelCase{50, 96, -1, 24, 16, false, false},
							PixelCase{400, 600, 1, 3072, 2048, false, false}})
	{
		config::VmuWindowSize = test.percent;
		settings.display.dpi = test.dpi;
		faults::dpiRatio = test.ratio;
		gui_state = test.menu ? GuiState::Commands : GuiState::Closed;
		config::RendererType = test.dim ? RenderType::OpenGL : RenderType::DirectX11;
		faults::expectedOpacity = test.menu ? 128 : 192;
		faults::expectedWhite = test.dim ? 191 : 255;
		const int before = faults::pixelChecks;
		vmu_window::publishScreen(0, frame.data(), true);
		require(until(
					[&]
					{
						return faults::pixelChecks > before && faults::pixelsMatch &&
							   faults::paintedWidth == test.width && faults::paintedHeight == test.height;
					}),
				"scaled bitmap matches nearest sampling, tint, orientation and alpha at every pixel");
	}
	faults::inspectPixels = false;
	vmu_window::shutdown();
	require(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= initialGdi, "shutdown releases VMU GDI resources");
	if (GetForegroundWindow() != owner)
	{
		DWORD process = 0;
		const HWND foreground = GetForegroundWindow();
		GetWindowThreadProcessId(foreground, &process);
		std::fprintf(stderr, "Focus diagnostic: foreground=%p owner=%p process=%lu test=%lu\n",
			foreground, owner, process, GetCurrentProcessId());
	}
	require(GetForegroundWindow() == owner, "normal VMU interaction preserves emulator focus");
	cleanup();
	UnregisterClassW(cls.lpszClassName, cls.hInstance);
	DeleteObject(brush);
	std::printf("Native VMU size and cleanup validation complete: %d checks\n", checks);
}
