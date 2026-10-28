// Portions Copyright 2026 The Hollycast Authors
#include <SDL.h>
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>

namespace vmu_window {
void configureMacWindow(SDL_Window *window, bool movable)
{
	SDL_SysWMinfo info {};
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA)
		return;
	NSWindow *panel = info.info.cocoa.window;
	panel.movable = movable;
	// SDL's non-resizable windows otherwise remain on the regular desktop when
	// Hollycast enters its own fullscreen Space. Run on SDL's main/UI thread.
	panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary
		| NSWindowCollectionBehaviorMoveToActiveSpace;
	panel.hidesOnDeactivate = YES;
}
}
