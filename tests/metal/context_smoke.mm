// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

// Integration smoke test for the actual MetalContext implementation.
// Creates an isolated SDL window; no user configuration is read or written.
// Standalone (Homebrew SDL2):
// clang++ -std=c++17 -fobjc-arc -DUSE_SDL -Icore -Icore/deps -Icore/deps/glm \
//   -Icore/deps/imgui -Icore/deps/nowide/include -Icore/deps/SDL/include \
//   -I/opt/homebrew/include/SDL2 tests/metal/context_smoke.mm core/rend/metal/metal_context.mm \
//   -L/opt/homebrew/lib -lSDL2 -framework Cocoa -framework Metal -framework QuartzCore \
//   -o /tmp/hollycast-metal-context-smoke
#include "rend/metal/metal_context.h"
#include "rend/metal/metal_api.h"
#include "rend/metal/metal_overlay.h"
#include "ui/imgui_driver.h"
#include "ui/gui.h"
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include <SDL.h>
#include <SDL_syswm.h>
#define RGBColor MacOSRGBColor
#define Rect MacOSRect
#import <AppKit/AppKit.h>
#undef Rect
#undef RGBColor
#include <cstdio>
#include <cstdlib>

settings_t settings{};
GuiState gui_state = GuiState::Closed;
Renderer *renderer = nullptr;
std::unique_ptr<ImGuiDriver> imguiDriver;
namespace config
{
void deleteEntry(const std::string &, const std::string &) {}
bool isTransient(const std::string &, const std::string &) { return false; }
bool loadBool(const std::string &, const std::string &, bool fallback) { return fallback; }
void saveBool(const std::string &, const std::string &, bool) {}
Option<bool> LinearInterpolation("linear", false);
Option<bool> VSync("vsync", true);
Option<bool> Rotate90("rotate", false);
Option<bool> FloatVMUs("vmus", false);
} // namespace config
GraphicsContext *GraphicsContext::instance = nullptr;
GraphicsContext::GraphicsContext(void *w, void *d) : window(w), display(d) { instance = this; }
GraphicsContext::~GraphicsContext() { instance = nullptr; }
static SDL_Window *testWindow;
bool sdl_recreate_window(u32 flags)
{
	testWindow = SDL_CreateWindow("Hollycast Metal context test", 9000, 9000, 160, 120,
								  flags | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SKIP_TASKBAR | SDL_WINDOW_BORDERLESS);
	if (!testWindow)
		return false;
	GraphicsContext::Instance()->setWindow(testWindow);
	return true;
}
void gui_updateStyle() {}
void gui_draw_osd() {}
#ifndef METAL_CONTEXT_REAL_IMGUI
ImDrawData *ImGui::GetDrawData() { return nullptr; }
struct TestDriver : ImGuiDriver
{
	void newFrame() override {}
	void renderDrawData(ImDrawData *, bool) override {}
	void present() override {}
	ImTextureID getTexture(const std::string &) override { return {}; }
	ImTextureID updateTexture(const std::string &, const u8 *, int, int, bool) override { return {}; }
	void deleteTexture(const std::string &) override {}
};
void metalInitImGui() { imguiDriver.reset(new TestDriver()); }
void metalDrawImGui(id<MTLRenderCommandEncoder>, int, int) {}
#endif
struct MetalOverlay::Impl
{
};
MetalOverlay::MetalOverlay(id<MTLDevice>) : impl(new Impl) {}
MetalOverlay::~MetalOverlay() = default;
void MetalOverlay::draw(id<MTLRenderCommandEncoder>, int, int, float, bool, bool) {}
static int testInset = 0;
#ifndef METAL_CONTEXT_REAL_TRANSFORMS
static float testShiftX = 0, testShiftY = 0;
static int testPaddingX = 0, testPaddingY = 0;
void getVideoShift(float &x, float &y)
{
	x = testShiftX;
	y = testShiftY;
}
void getWindowboxDimensions(int, int, float, int &x, int &y, bool)
{
	x = testPaddingX;
	y = testPaddingY;
}
#endif
int getScaledTopInset() { return testInset; }
static int presents = 0;
namespace performance_overlay
{
void recordOutputViewport(int, int) {}
void recordPresentedFrame() { ++presents; }
} // namespace performance_overlay
void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...) {}

static void require(bool ok, const char *message)
{
	if (!ok)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::exit(1);
	}
}
static CAMetalLayer *findLayer(NSView *view)
{
	if ([view.layer isKindOfClass:[CAMetalLayer class]])
		return (CAMetalLayer *)view.layer;
	for (NSView *child in view.subviews)
		if (CAMetalLayer *layer = findLayer(child))
			return layer;
	return nil;
}
static void pump()
{
	SDL_PumpEvents();
	[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}
int main()
{
	@autoreleasepool
	{
		require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
		MetalContext::Create(nullptr, nullptr);
		MetalContext *context = MetalContext::Instance();
		require(context && context->GetDevice(), "context/device initialization");
		require(metalIsAvailable(), "actual device capability probe");
		require(context->hasPerPixel() == metalSupportsPerPixel(), "active and default Metal capability agree");
		std::printf("Per-pixel capability: %s\n", context->hasPerPixel() ? "available" : "unavailable");
		SDL_SysWMinfo info{};
		SDL_VERSION(&info.version);
		require(SDL_GetWindowWMInfo(testWindow, &info), "native window lookup");
		CAMetalLayer *layer = findLayer([info.info.cocoa.window contentView]);
		require(layer != nil, "SDL Metal layer lookup");
		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
																						width:2
																					   height:2
																					mipmapped:NO];
		desc.storageMode = metalCpuTextureStorageMode(context->GetDevice());
		desc.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> texture = [context->GetDevice() newTextureWithDescriptor:desc];
		const u8 pixels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
		[texture replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:pixels bytesPerRow:8];
		context->PresentFrame(texture, 1.f);
		std::vector<u8> rgb;
		int width = 0, height = 0;
		require(context->GetLastFrame(rgb, width, height) && width == 2 && height == 2, "native readback dimensions");
		const u8 expected[] = {255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255};
		require(std::equal(rgb.begin(), rgb.end(), expected), "native readback pixels");
		width = 4;
		height = 0;
		require(context->GetLastFrame(rgb, width, height) && width == 4 && height == 4,
				"resized readback dimensions");
		size_t mixed = (size_t)(width + 1) * 3;
		require(std::abs((int)rgb[mixed] - 159) <= 1 && std::abs((int)rgb[mixed + 1] - 64) <= 1 &&
				std::abs((int)rgb[mixed + 2] - 64) <= 1, "normalized readback uses linear filtering");
		config::Rotate90 = true;
		width = 0;
		height = 0;
		require(context->GetLastFrame(rgb, width, height) && width == 2 && height == 2, "rotated readback dimensions");
		const u8 rotated[] = {0, 255, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255};
		require(std::equal(rgb.begin(), rgb.end(), rotated), "rotated readback orientation");
		width = 4;
		height = 2;
		require(context->GetLastFrame(rgb, width, height) && width == 2 && height == 2,
				"readback respects both maximum bounds");
		context->PresentFrame(texture, .5f);
		width = 0;
		height = 0;
		require(context->GetLastFrame(rgb, width, height) && width == 2 && height == 4,
				"rotated aspect is not inverted twice");
		config::Rotate90 = false;
		context->setSwapInterval(1);
		config::VSync = true;
		settings.input.fastForwardMode = false;
		context->PresentLastFrame();
		require(layer.displaySyncEnabled && presents == 1, "VSync on and frame presented");
		config::VSync = false;
		context->PresentLastFrame();
		require(!layer.displaySyncEnabled, "VSync off");
		config::VSync = true;
		settings.input.fastForwardMode = true;
		context->PresentLastFrame();
		require(!layer.displaySyncEnabled, "fast forward disables VSync");
		settings.input.fastForwardMode = false;
		context->PresentLastFrame();
		require(layer.displaySyncEnabled, "VSync restores after fast forward");
		context->setSwapInterval(2);
		settings.input.fastForwardMode = true;
		context->PresentLastFrame();
		require(!layer.displaySyncEnabled, "fast forward overrides game interval");
		settings.input.fastForwardMode = false;
		context->setSwapInterval(1);
		int initialW = 0, initialH = 0;
		SDL_Metal_GetDrawableSize(testWindow, &initialW, &initialH);
		SDL_SetWindowSize(testWindow, 80, 60);
		pump();
		context->resize();
		int w = 0, h = 0;
		SDL_Metal_GetDrawableSize(testWindow, &w, &h);
		require(w > 0 && h > 0 && w < initialW && h < initialH && (int)layer.drawableSize.width == w &&
					(int)layer.drawableSize.height == h,
				"downsize updates drawable");
		int smallW = w, smallH = h;
		SDL_SetWindowSize(testWindow, 200, 140);
		pump();
		context->resize();
		SDL_Metal_GetDrawableSize(testWindow, &w, &h);
		require(w > smallW && h > smallH && (int)layer.drawableSize.width == w && (int)layer.drawableSize.height == h,
				"resize updates drawable");
		context->PresentLastFrame();
		int beforeMinimize = presents;
		SDL_MinimizeWindow(testWindow);
		pump();
		if (SDL_GetWindowFlags(testWindow) & SDL_WINDOW_MINIMIZED)
		{
			context->PresentLastFrame();
			require(presents == beforeMinimize, "minimized window does not present");
			SDL_RestoreWindow(testWindow);
			pump();
			context->PresentLastFrame();
			require(presents > beforeMinimize, "restored window presents");
		}
		else
			std::puts("SKIP minimize/restore: SDL window manager did not minimize the test window");
		context->WaitIdle();
		GraphicsContext::Term();
		require(GraphicsContext::Instance() == nullptr, "context teardown");
		SDL_DestroyWindow(testWindow);
		testWindow = nullptr;
		MetalContext::Create(nullptr, nullptr);
		require(MetalContext::Instance() && MetalContext::Instance()->GetDevice(), "context recreation");
		GraphicsContext::Term();
		require(GraphicsContext::Instance() == nullptr, "recreated context teardown");
		SDL_DestroyWindow(testWindow);
		SDL_Quit();
		std::puts("Metal context readback, orientation, resize, VSync and teardown passed");
	}
	return 0;
}
