// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "types.h"

#if !defined(LIBRETRO) && !defined(TARGET_UWP)
#if defined(_WIN32) || defined(__ANDROID__)
#define HAVE_MOVABLE_VMUS
#elif defined(USE_SDL) && (defined(__linux__) || (defined(__APPLE__) && !defined(TARGET_IPHONE)))
#define HAVE_MOVABLE_VMUS
#define HAVE_SDL_VMUS
#endif
#endif

#ifdef HAVE_SDL_VMUS
struct SDL_Window;
union SDL_Event;
#endif

namespace vmu_window {
#ifdef HAVE_MOVABLE_VMUS
// Producers publish copies. Window, texture and configuration work stays on the UI thread.
bool detachedActive();
void publishScreen(int index, const u32 *pixels, bool active);
void shutdown();
void resetPositions();
bool ownsScreen(int index);
// Rebase absolute game input after an interaction consumed by a VMU.
bool consumeMousePositionReset();
#ifdef HAVE_SDL_VMUS
void update(SDL_Window *parent, bool pointerAvailable);
bool handleEvent(const SDL_Event& event);
#elif defined(__ANDROID__)
void update(bool visible);
void drawInApp();
// Coordinates are relative to the Android view, independent of its pixel density.
bool touchDown(float x, float y);
void touchMove(float x, float y);
void touchUp();
#else
void update(bool pointerAvailable);
bool blocksMouseInput();
#endif
#else
inline bool ownsScreen(int) { return false; }
#endif
}
