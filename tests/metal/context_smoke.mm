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
#include <cmath>
#include <limits>
#include <objc/runtime.h>
#include <atomic>
#include <thread>
#include <chrono>

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
static std::atomic<int> presents{0};
static int presentationSamples = 0;
static int callbackRegistrations = 0;
static bool presentationTimingRequested = false;
static std::uint32_t presentationEpoch = 47, sampledEpoch = 0;
static std::uint64_t sampledMicroseconds = 0;
static double minimumDuration = 0;
static double scheduledTime = 0;
static NSMutableArray *pendingPresentationHandlers;
static bool completePresentations = true;

@interface TestPresentedDrawable : NSObject
@property CFTimeInterval presentedTime;
@end
@implementation TestPresentedDrawable
@end

namespace performance_overlay
{
void recordOutputViewport(int, int) {}
void recordPresentedFrame() { if (presentationTimingRequested) ++presentationSamples; }
bool isPresentationTimingRequested() { return presentationTimingRequested; }
std::uint32_t getPresentationTimingEpoch() { return presentationEpoch; }
void recordPresentedFrameAt(std::uint64_t microseconds, std::uint32_t epoch)
{
	++presentationSamples;
	sampledMicroseconds = microseconds;
	sampledEpoch = epoch;
}
} // namespace performance_overlay

static void finishPresentation(double seconds)
{
	TestPresentedDrawable *sample = [TestPresentedDrawable new];
	sample.presentedTime = seconds;
	NSArray *handlers = [pendingPresentationHandlers copy];
	[pendingPresentationHandlers removeAllObjects];
	for (MTLDrawablePresentedHandler handler in handlers)
		handler((id<MTLDrawable>)sample);
}

static void observePresentations(CAMetalLayer *layer, id<MTLCommandQueue> queue)
{
	pendingPresentationHandlers = [NSMutableArray new];
	id<MTLCommandBuffer> command = [queue commandBuffer];
	Class commandClass = object_getClass(command);
	auto observe = [&](SEL selector, id block)
	{
		Method method = class_getInstanceMethod(commandClass, selector);
		return method_setImplementation(method, imp_implementationWithBlock(block));
	};
	__block IMP present;
	present = observe(@selector(presentDrawable:), ^(id buffer, id<MTLDrawable> drawable) {
		++presents;
		((void (*)(id, SEL, id<MTLDrawable>))present)(buffer, @selector(presentDrawable:), drawable);
	});
	__block IMP presentMinimum;
	presentMinimum = observe(@selector(presentDrawable:afterMinimumDuration:), ^(id buffer, id<MTLDrawable> drawable, double duration) {
		++presents;
		minimumDuration = duration;
		((void (*)(id, SEL, id<MTLDrawable>, double))presentMinimum)(buffer, @selector(presentDrawable:afterMinimumDuration:), drawable, duration);
	});
	__block IMP presentAt;
	presentAt = observe(@selector(presentDrawable:atTime:), ^(id buffer, id<MTLDrawable> drawable, double time) {
		++presents;
		scheduledTime = time;
		// Inspect the requested deadline without holding the isolated test window on a synthetic timeline.
		((void (*)(id, SEL, id<MTLDrawable>, double))presentAt)(buffer, @selector(presentDrawable:atTime:), drawable, CACurrentMediaTime());
	});
	id<CAMetalDrawable> drawable = [layer nextDrawable];
	Method method = class_getInstanceMethod(object_getClass(drawable), @selector(addPresentedHandler:));
	method_setImplementation(method, imp_implementationWithBlock(^(id, MTLDrawablePresentedHandler handler) {
		++callbackRegistrations;
		[pendingPresentationHandlers addObject:[handler copy]];
		if (completePresentations)
			finishPresentation(1000.0 + callbackRegistrations / 60.0);
	}));
}
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
		require(layer.maximumDrawableCount == 2, "only one drawable can queue behind the displayed frame");
		observePresentations(layer, context->GetQueue());
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
		require(callbackRegistrations == 0 && presentationSamples == 0, "hidden output timing registers no callback or submission sample");
		presentationTimingRequested = true;
		completePresentations = false;
		context->PresentLastFrame();
		require(callbackRegistrations == 1 && presentationSamples == 0, "output timing waits for drawable presentation");
		finishPresentation(0);
		context->PresentLastFrame();
		finishPresentation(std::numeric_limits<double>::quiet_NaN());
		context->PresentLastFrame();
		finishPresentation(std::numeric_limits<double>::infinity());
		require(presentationSamples == 0, "zero or invalid drawable timestamps have no timing sample");
		context->PresentLastFrame();
		++presentationEpoch;
		finishPresentation(1234.5);
		require(presentationSamples == 1 && sampledMicroseconds == 1234500000 && sampledEpoch == 47,
				"actual display timestamp retains its submission epoch");
		presentationTimingRequested = false;
		completePresentations = true;
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
		presentationTimingRequested = true;
		const int callbacksBeforePacing = callbackRegistrations;
		const int samplesBeforePacing = presentationSamples;
		context->PresentFrame(texture, 1.f);
		const double ntscStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(callbackRegistrations == callbacksBeforePacing + 1 && presentationSamples == samplesBeforePacing + 1,
				"requested output timing observes the paced drawable");
		require(scheduledTime >= ntscStart + 2.0 / 60.0
				&& scheduledTime <= CACurrentMediaTime() + 2.0 / 60.0,
				"NTSC starts an absolute timeline one guest interval ahead");
		presentationTimingRequested = false;
		context->setVideoMode(true);
		context->PresentFrame(texture, 1.f);
		const double palStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime >= palStart + 2.0 / 50.0
				&& scheduledTime <= CACurrentMediaTime() + 2.0 / 50.0,
				"PAL transition starts a fresh 50 Hz guest timeline");
		require(callbackRegistrations == callbacksBeforePacing + 1 && presentationSamples == samplesBeforePacing + 1,
				"hidden output timing registers no game pacing callback");
		require(layer.maximumDrawableCount == 3, "slow game pacing permits three drawables");
		const double firstPalDeadline = scheduledTime;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		context->PresentFrame(texture, 1.f);
		context->PresentLastFrame();
		require(std::abs(scheduledTime - firstPalDeadline - 2.0 / 50.0) < 1e-8,
				"a delayed submission advances the guest timeline without host phase drift");
		const double beforeLateDeadline = scheduledTime;
		const double lateDelay = std::max(0.0, beforeLateDeadline + 3.0 / 50.0 - CACurrentMediaTime());
		std::this_thread::sleep_for(std::chrono::duration<double>(lateDelay));
		context->PresentFrame(texture, 1.f);
		const double lateStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime < lateStart && std::abs(scheduledTime - beforeLateDeadline - 2.0 / 50.0) < 1e-8,
				"a missed deadline requests immediate display without resetting the guest phase");
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
		context->PresentFrame(texture, 1.f);
		const double resumeStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime >= resumeStart + 2.0 / 50.0
				&& scheduledTime <= CACurrentMediaTime() + 2.0 / 50.0,
				"a long interruption rebases instead of replaying stale deadlines");
		require(minimumDuration == 0, "slow game pacing uses only absolute presentation deadlines");
		const int afterGameFrame = presents;
		context->PresentLastFrame();
		require(presents == afterGameFrame, "an OSD poll does not queue a second slow game frame");
		gui_state = GuiState::Settings;
		context->PresentLastFrame();
		require(presents == afterGameFrame + 1, "opening settings immediately presents over the last game frame");
		gui_state = GuiState::Closed;
		context->setVideoMode(false);
		settings.input.fastForwardMode = true;
		context->PresentLastFrame();
		require(!layer.displaySyncEnabled, "fast forward overrides game interval");
		settings.input.fastForwardMode = false;
		context->setSwapInterval(10);
		presentationTimingRequested = true;
		completePresentations = false;
		const int beforeUncompleted = callbackRegistrations;
		context->PresentFrame(texture, 1.f);
		const double firstStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(CACurrentMediaTime() - firstStart < 0.15,
				"first slow frame does not wait for its overlay display callback");
		context->PresentFrame(texture, 1.f);
		const double secondStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(CACurrentMediaTime() - secondStart < 0.15,
				"second buffered frame does not wait for the previous overlay display callback");
		require(callbackRegistrations == beforeUncompleted + 2 && pendingPresentationHandlers.count == 2,
				"uncompleted optional timing callbacks do not serialize guest presentation");
		context->PresentFrame(texture, 1.f);
		context->PresentLastFrame();
		require(scheduledTime <= CACurrentMediaTime() + 20.0 / 60.0,
				"immediately recycled drawables cannot run the timeline beyond two future guest periods");
		presentationTimingRequested = false;
		const int beforeHiddenTiming = callbackRegistrations;
		context->PresentFrame(texture, 1.f);
		context->PresentLastFrame();
		require(callbackRegistrations == beforeHiddenTiming,
				"hidden timing performs no callback registration even while buffering slow game frames");
		gui_state = GuiState::Settings;
		context->PresentLastFrame();
		require(layer.maximumDrawableCount == 2, "menus restore the two-drawable pool");
		gui_state = GuiState::Closed;
		settings.input.fastForwardMode = true;
		context->PresentFrame(texture, 1.f);
		context->PresentLastFrame();
		require(!layer.displaySyncEnabled && layer.maximumDrawableCount == 2,
				"fast forward uses unsynchronized presentation with two drawables");
		settings.input.fastForwardMode = false;
		finishPresentation(1237.0);
		completePresentations = true;
		context->PresentFrame(texture, 1.f);
		const double afterFastStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime >= afterFastStart + 10.0 / 60.0
				&& scheduledTime <= CACurrentMediaTime() + 10.0 / 60.0,
				"leaving fast forward starts a fresh guest timeline");
		require(layer.maximumDrawableCount == 3, "slow pacing restores the three-drawable pool");
		config::VSync = false;
		context->PresentLastFrame();
		require(layer.maximumDrawableCount == 2, "disabling VSync restores the two-drawable pool");
		config::VSync = true;
		context->PresentFrame(texture, 1.f);
		const double afterVSyncStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime >= afterVSyncStart + 10.0 / 60.0
				&& scheduledTime <= CACurrentMediaTime() + 10.0 / 60.0,
				"restoring VSync starts a fresh guest timeline");
		gui_state = GuiState::Settings;
		context->PresentLastFrame();
		gui_state = GuiState::Closed;
		context->PresentFrame(texture, 1.f);
		const double afterMenuStart = CACurrentMediaTime();
		context->PresentLastFrame();
		require(scheduledTime >= afterMenuStart + 10.0 / 60.0
				&& scheduledTime <= CACurrentMediaTime() + 10.0 / 60.0,
				"closing settings starts a fresh guest timeline");
		context->setSwapInterval(1);
		context->PresentFrame(texture, 1.f);
		context->PresentLastFrame();
		require(layer.maximumDrawableCount == 2, "60 FPS presentation retains two drawables");
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
