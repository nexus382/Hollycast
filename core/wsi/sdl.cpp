/*
    Created on: Oct 18, 2019

	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#if defined(USE_SDL)
#include "sdl.h"
#include "ui/gui.h"
#include "ui/performance_overlay.h"
#include "sdl/sdl.h"
#include "cfg/option.h"
#ifdef USE_HIKARU
#include "audio/audiostream.h"
#include <limits>
#endif

SDLGLGraphicsContext::SDLGLGraphicsContext(void *window, void *display)
	: GLGraphicsContext(window, display)
{
	if (!init())
		throw FlycastException("OpenGL initialization failed");
}

SDLGLGraphicsContext::~SDLGLGraphicsContext() {
	term();
}

bool SDLGLGraphicsContext::init()
{
#ifdef GLES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	if (!sdl_recreate_window(SDL_WINDOW_OPENGL))
		return false;

	SDL_Window * const sdlWindow = (SDL_Window *)window;
	glcontext = SDL_GL_CreateContext(sdlWindow);
#ifndef GLES
	if (glcontext == SDL_GLContext())
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef __APPLE__
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
		glcontext = SDL_GL_CreateContext(sdlWindow);
	}
#endif
	if (glcontext == SDL_GLContext())
	{
		ERROR_LOG(RENDERER, "Error creating SDL GL context");
		SDL_DestroyWindow(sdlWindow);
		window = nullptr;
		return false;
	}
	SDL_GL_MakeCurrent(sdlWindow, NULL);

	sdl_update_display_metrics(sdlWindow, SDL_WINDOW_OPENGL);

	INFO_LOG(RENDERER, "Created SDL Window and GL Context successfully");

	SDL_GL_MakeCurrent(sdlWindow, glcontext);
	gameSwapIntervalChanged = true;
	changeGLSwapInterval();

#ifdef GLES
	if (gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress) == 0)
	{
		ERROR_LOG(RENDERER, "gladLoadGLES2 failed");
		return false;
	}
#else
	if (!gladLoadGL((GLADloadfunc) SDL_GL_GetProcAddress) || !GLAD_GL_VERSION_3_0)
	{
		ERROR_LOG(RENDERER, "gladLoadGL failed or GL 3.0 not supported");
		return false;
	}
#endif
	postInit();
	
	return true;
}

void SDLGLGraphicsContext::swap()
{
	do_swap_automation();
	changeGLSwapInterval();
	SDL_GL_SwapWindow((SDL_Window *)window);
	// SDL2 exposes no swap result; count completed swap calls, not render requests.
	performance_overlay::recordPresentedFrame();

	// Check if drawable has been resized
	SDL_GL_GetDrawableSize((SDL_Window *)window, &settings.display.width, &settings.display.height);
#ifdef __SWITCH__
	float newScaling = settings.display.height == 720 ? 1.5f : 1.4f;
	if (newScaling != settings.display.uiScale)
	{
		// Restart the UI to take the new scaling factor into account
		settings.display.uiScale = newScaling;
		resetUIDriver();
	}
#endif
}

void SDLGLGraphicsContext::changeGLSwapInterval()
{
	const bool onVSync = !settings.input.fastForwardMode && config::VSync;
#ifdef USE_HIKARU
	const bool forHikaru = settings.platform.isHikaru() && config::LimitFPS
		&& AudioOutputQueuedFrames() != std::numeric_limits<u32>::max();
	if (!gameSwapIntervalChanged && swapOnVSync == onVSync && swapForHikaru == forHikaru)
#else
	if (!gameSwapIntervalChanged && swapOnVSync == onVSync)
#endif
		return;
	swapOnVSync = onVSync;
#ifdef USE_HIKARU
	swapForHikaru = forHikaru;
#endif
	gameSwapIntervalChanged = false;
	int swapInterval = 0;
	if (swapOnVSync)
	{
#ifdef USE_HIKARU
		// Hikaru runs both CPUs and draws every frame on this thread. Adaptive
		// sync avoids a second display wait after a frame misses its deadline;
		// the audio output provides the 60 Hz speed limit when we are faster.
		if (forHikaru)
			swapInterval = -1;
		else
#endif
		if (settings.display.refreshRate > 60.f)
			swapInterval = settings.display.refreshRate / 60.f * gameSwapInterval;
		else
			swapInterval = gameSwapInterval;
		INFO_LOG(RENDERER, "Swap interval changed to %d", swapInterval);
	}
	if (SDL_GL_SetSwapInterval(swapInterval) < 0 && swapInterval == -1)
		SDL_GL_SetSwapInterval(1);
}


void SDLGLGraphicsContext::term()
{
	preTerm();
	if (glcontext != nullptr)
	{
		SDL_GL_DeleteContext(glcontext);
		glcontext = nullptr;
	}
}

void SDLGLGraphicsContext::Create(void *window, void *display) {
	new SDLGLGraphicsContext(window, display);
}

#endif

