// Portions Copyright 2026 The Hollycast Authors
#include <SDL.h>
#include "cfg/option.h"
#include "ui/gui.h"
#include <cstdarg>
#include <cstdlib>
#include <iostream>
#include <map>

TestSettings settings;
GuiState gui_state = GuiState::Closed;
namespace config {
bool MovableVMUs = true, FloatVMUs = true;
bool VmuLayoutEnabled = false;
TestIntOption VmuWindowSize;
TestIntOption VmuWindowTransparency {25};
RenderType RendererType = RenderType::OpenGL;
std::map<std::string, int> values;
int loadInt(const std::string& section, const std::string& key, int fallback)
{
	auto it = values.find(section + "/" + key);
	return it == values.end() ? fallback : it->second;
}
void saveInt(const std::string& section, const std::string& key, int value) { values[section + "/" + key] = value; }
void setAutoSave(bool) {}
}
void testWarning(const char *, ...) {}

bool failCreate = false, failPaint = false, pretendWayland = false;
int creates = 0;
int workAreaWidth = 0;
float requestedOpacity = -1.f;
Uint64 ticks = 1000;
SDL_Window *testFocus = nullptr;
SDL_Window *createWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	++creates;
	return failCreate ? nullptr : SDL_CreateWindow(title, x, y, w, h, flags);
}
int updateSurface(SDL_Window *window) { return failPaint ? -1 : SDL_UpdateWindowSurface(window); }
int setOpacity(SDL_Window *window, float alpha)
{
	requestedOpacity = alpha;
	return SDL_SetWindowOpacity(window, alpha);
}
SDL_Window *keyboardFocus() { return testFocus; }
Uint64 getTicks() { return ticks; }
const char *videoDriver() { return pretendWayland ? "wayland" : SDL_GetCurrentVideoDriver(); }
int usableBounds(int display, SDL_Rect *area)
{
	const int result = SDL_GetDisplayUsableBounds(display, area);
	if (result == 0 && workAreaWidth > 0)
		area->w = workAreaWidth;
	return result;
}

#define SDL_CreateWindow createWindow
#define SDL_UpdateWindowSurface updateSurface
#define SDL_SetWindowOpacity setOpacity
#define SDL_GetKeyboardFocus keyboardFocus
#define SDL_GetTicks64 getTicks
#define SDL_GetCurrentVideoDriver videoDriver
#define SDL_GetDisplayUsableBounds usableBounds
#include "../../core/sdl/vmu_window.cpp"
#undef SDL_CreateWindow
#undef SDL_UpdateWindowSurface
#undef SDL_SetWindowOpacity
#undef SDL_GetKeyboardFocus
#undef SDL_GetTicks64
#undef SDL_GetCurrentVideoDriver
#undef SDL_GetDisplayUsableBounds

int checks = 0;
void require(bool condition, const char *message)
{
	++checks;
	if (!condition) { std::cerr << message << ": " << SDL_GetError() << '\n'; std::exit(1); }
}
SDL_Event windowEvent(SDL_Window *window, Uint8 kind, int x = 0, int y = 0)
{
	SDL_Event event {};
	event.type = SDL_WINDOWEVENT;
	event.window.windowID = SDL_GetWindowID(window);
	event.window.event = kind;
	event.window.data1 = x;
	event.window.data2 = y;
	return event;
}

int main()
{
	using namespace vmu_window;
	require(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL video starts");
	SDL_Window *parent = SDL_CreateWindow("VMU regression owner", 100, 100, 640, 480, SDL_WINDOW_OPENGL);
	require(parent != nullptr, "owner window creates");
	SDL_GLContext context = SDL_GL_CreateContext(parent);
	require(context != nullptr, "owner GL context creates");
	testFocus = parent;
	std::array<u32, LcdWidth * LcdHeight> pixels {};
	pixels.fill(0xff000000);
	pixels[0] = 0xffffffff;
	publishScreen(0, pixels.data(), true);
	failCreate = true;
	update(parent, true);
	require(!ownsScreen(0), "failed window keeps in-game fallback");
	const int attempts = creates;
	update(parent, true);
	require(creates == attempts, "creation failure is throttled");
	failCreate = false;
	ticks += 1000;
	update(parent, true);
	require(ownsScreen(0) && !ownsScreen(-1) && !ownsScreen(8), "successful paint owns exactly the active LCD");
	require(SDL_GL_GetCurrentContext() == context && SDL_GL_GetCurrentWindow() == parent, "SDL painting preserves owner GL context");
	require(requestedOpacity == .75f, "default gameplay opacity is unchanged");
	gui_state = GuiState::Commands;
	update(parent, true);
	require(requestedOpacity == .5f, "default menu opacity is unchanged");
	gui_state = GuiState::Closed;
	config::VmuWindowTransparency = 50;
	update(parent, true);
	require(requestedOpacity == .5f, "transparency changes live without publishing another LCD frame");
	config::VmuWindowTransparency = -1;
	update(parent, true);
	require(requestedOpacity == 1.f, "invalid negative transparency clamps to solid");
	config::VmuWindowTransparency = 101;
	update(parent, true);
	require(ownsScreen(0) && (SDL_GetWindowFlags(windows[0].window) & SDL_WINDOW_HIDDEN) != 0,
		"full transparency hides the input target and suppresses the fallback LCD");
	update(parent, false);
	require(ownsScreen(0), "capture cannot revive the fallback for an intentionally hidden LCD");
	config::VmuWindowTransparency = 25;
	update(parent, true);
	require(ownsScreen(0) && (SDL_GetWindowFlags(windows[0].window) & SDL_WINDOW_HIDDEN) == 0 && requestedOpacity == .75f,
		"restoring transparency reuses the current LCD without republication");
	SDL_Surface *surface = SDL_GetWindowSurface(windows[0].window);
	require(surface != nullptr, "LCD window has a readable surface");
	SDL_LockSurface(surface);
	Uint32 top, bottom;
	std::memcpy(&top, surface->pixels, sizeof(top));
	std::memcpy(&bottom, static_cast<Uint8 *>(surface->pixels) + (surface->h - 1) * surface->pitch, sizeof(bottom));
	SDL_UnlockSurface(surface);
	Uint8 r, g, b;
	SDL_GetRGB(top, surface->format, &r, &g, &b);
	require(r == 0 && g == 0 && b == 0, "LCD top row is flipped correctly");
	SDL_GetRGB(bottom, surface->format, &r, &g, &b);
	require(r == 255 && g == 255 && b == 255, "LCD bottom row preserves first producer pixel");
	SDL_GL_MakeCurrent(parent, context);
	update(parent, true);
	testFocus = windows[0].window;
	SDL_Event event = windowEvent(testFocus, SDL_WINDOWEVENT_MOVED, 320, 240);
	require(handleEvent(event), "VMU move is consumed");
	require(config::values.empty(), "movement does not write config on every event");
	ticks += 250;
	update(parent, true);
	require(config::values["vmu-sdl/0.left"] == 320 && config::values["vmu-sdl/0.top"] == 240, "user placement is saved");
	require(consumeMousePositionReset() && !consumeMousePositionReset(), "returning to game rebases the mouse once");
	SDL_SetWindowPosition(testFocus, 320, 240);
	int beforeLockX, beforeLockY;
	SDL_GetWindowPosition(testFocus, &beforeLockX, &beforeLockY);
	config::VmuLayoutEnabled = true;
	config::MovableVMUs = false;
	update(parent, false);
	require(ownsScreen(0) && (SDL_GetWindowFlags(testFocus) & SDL_WINDOW_BORDERLESS) != 0,
		"locked LCD remains visible during captured gameplay without a drag title bar");
	int lockedX, lockedY;
	SDL_GetWindowPosition(testFocus, &lockedX, &lockedY);
	require(lockedX == beforeLockX && lockedY == beforeLockY, "locking preserves the visible window position");
	config::VmuWindowTransparency = 60;
	update(parent, false);
	require(std::abs(requestedOpacity - .4f) < .0001f, "locked LCD transparency remains adjustable");
	config::VmuWindowTransparency = 25;
	event.window.data1 = lockedX + 50;
	event.window.data2 = lockedY + 50;
	SDL_SetWindowPosition(testFocus, event.window.data1, event.window.data2);
	handleEvent(event);
	int restoredX, restoredY;
	SDL_GetWindowPosition(testFocus, &restoredX, &restoredY);
	require(lockedX == restoredX && lockedY == restoredY, "locked window rejects window-manager movement");
	require(config::values["vmu-sdl/0.left"] == 320, "locked movement cannot overwrite the saved placement");
	workAreaWidth = 150;
	event.window.data1 = 0;
	handleEvent(event);
	SDL_GetWindowPosition(testFocus, &restoredX, &restoredY);
	SDL_Rect workArea;
	usableBounds(SDL_GetWindowDisplayIndex(parent), &workArea);
	int lcdWidth, lcdHeight;
	SDL_GetWindowSize(testFocus, &lcdWidth, &lcdHeight);
	require(restoredX >= workArea.x && restoredX + lcdWidth <= workArea.x + workArea.w,
		"locked LCD recovers into a smaller work area");
	require(windows[0].left == lockedX && config::values["vmu-sdl/0.left"] == 320,
		"temporary recovery preserves the preferred locked and saved coordinates");
	workAreaWidth = 0;
	event.window.data1 = restoredX;
	handleEvent(event);
	SDL_GetWindowPosition(testFocus, &restoredX, &restoredY);
	require(restoredX == lockedX && restoredY == lockedY, "locked placement returns when its work area is restored");
	resetPositions();
	testFocus = parent;
	update(parent, true);
	require(ownsScreen(0) && !config::MovableVMUs, "reset restores a visible default LCD without unlocking movement");
	config::MovableVMUs = true;
	config::VmuLayoutEnabled = false;
	update(parent, true);
	update(parent, true); // Border-change events settle before the next user drag.
	testFocus = windows[0].window;
	event = windowEvent(testFocus, SDL_WINDOWEVENT_MOVED, 320, 240);
	handleEvent(event);
	ticks += 250;
	update(parent, true);
	pretendWayland = true;
	update(parent, true);
	event.window.data1 = 0;
	event.window.data2 = 0;
	handleEvent(event);
	require(config::values["vmu-sdl/0.left"] == 320, "Wayland synthetic coordinates never overwrite desktop preference");
	require(reachablePosition(0, 96, 64).x == SDL_WINDOWPOS_UNDEFINED, "Wayland leaves placement to compositor");
	pretendWayland = false;
	event = {};
	event.type = SDL_MOUSEBUTTONDOWN;
	event.button.windowID = SDL_GetWindowID(testFocus);
	require(handleEvent(event), "VMU click cannot reach the game");
	event.type = SDL_KEYDOWN;
	event.key.windowID = SDL_GetWindowID(testFocus);
	require(handleEvent(event), "VMU keyboard input cannot reach the game");
	event = windowEvent(parent, SDL_WINDOWEVENT_CLOSE);
	require(!handleEvent(event), "main close remains available to the application exit handler");
	event = windowEvent(testFocus, SDL_WINDOWEVENT_CLOSE);
	require(handleEvent(event) && !ownsScreen(0), "closing one VMU restores its in-game LCD");
	testFocus = parent;
	update(parent, true);
	require(!ownsScreen(0), "dismissed window stays closed");
	resetPositions();
	update(parent, true);
	require(ownsScreen(0), "reset positions also reopens dismissed LCDs");
	update(parent, false);
	require(!ownsScreen(0), "captured gameplay uses the standard in-game renderer");
	update(parent, true);
	require(ownsScreen(0), "releasing capture restores the utility window");
	config::VmuWindowSize = 200;
	update(parent, true);
	int width, height;
	SDL_GetWindowSize(windows[0].window, &width, &height);
	require(width >= 192 && width * 2 == height * 3, "live scale keeps LCD aspect ratio");
	failPaint = true;
	publishScreen(0, pixels.data(), true);
	const Uint32 staleId = SDL_GetWindowID(windows[0].window);
	update(parent, true);
	require(!ownsScreen(0) && windows[0].window == nullptr, "paint failure restores fallback and destroys broken surface");
	event = {};
	event.type = SDL_WINDOWEVENT;
	event.window.windowID = staleId;
	event.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
	require(handleEvent(event), "queued events from a destroyed utility cannot resize the main renderer");
	failPaint = false;
	ticks += 1000;
	update(parent, true);
	require(ownsScreen(0), "failed drawing retries successfully");
	testFocus = nullptr;
	update(parent, true);
	require(!ownsScreen(0), "background application does not leave topmost VMUs over other apps");
	testFocus = parent;
	publishScreen(0, nullptr, false);
	update(parent, true);
	require(windows[0].window == nullptr, "disconnect removes LCD window");
	publishScreen(0, pixels.data(), true);
	settings.platform.console = false;
	update(parent, true);
	require(!ownsScreen(0), "arcade platforms retain existing behavior");
	shutdown();
	SDL_GL_DeleteContext(context);
	SDL_DestroyWindow(parent);
	SDL_Quit();
	std::cout << checks << " checks passed\n";
}
