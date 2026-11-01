/*
	Copyright 2020 flyinghead
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

#include "audio/ui_sound.h"
#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "gui_menu.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "rend/CustomTexture.h"
#include "imgui_driver.h"
#include "rend/vmu_window.h"
#include "profiler/fc_profiler.h"
#include "oslib/i18n.h"
#ifdef USE_HIKARU
#include "hw/hikaru/frontend.h"
#ifdef __APPLE__
#include "wsi/gl_context.h"
#endif
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

static int renderApi(RenderType type)
{
	if (isOpenGL(type)) return 0;
	if (isVulkan(type)) return 1;
	if (type == RenderType::DirectX9) return 2;
	if (isMetal(type)) return 4;
	return 3;
}

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();
#if defined(__ANDROID__) && defined(HAVE_MOVABLE_VMUS)
	vmu_window::update(!gui_is_open());
#endif

	if (gui_is_open())
	{
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			// Assume this is a graphics API issue
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Hollycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error(i18n::T("Renderer initialization failed.\nPlease select a different graphics API"));
	}
}

void mainui_term()
{
#ifdef USE_HIKARU
	if (settings.platform.isHikaru())
		hikaru::frontend::releaseGraphics();
#endif
#if defined(__ANDROID__) && defined(HAVE_MOVABLE_VMUS)
	vmu_window::shutdown();
#endif
	rend_term_renderer();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("Flycast-rend");
	if (forceStart)
		mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");
		tickUiSound();

#ifdef USE_HIKARU
		static const bool traceHikaru = std::getenv("HIKARU_TRACE_TIMING") != nullptr;
		const bool measure = traceHikaru && settings.platform.isHikaru() && !gui_is_open();
		const auto frameStart = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		const bool rendered = mainui_rend_frame();
#ifdef USE_HIKARU
		const auto renderEnd = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
#endif
		if (rendered && imguiDriver != nullptr)
		{
			try {
#if defined(USE_HIKARU) && defined(__APPLE__)
				// Submit the game and overlay before the macOS SDL display wait,
				// so the GPU can work while presentation is synchronized.
				if (settings.platform.isHikaru() && !gui_is_open() && config::VSync
					&& !settings.input.fastForwardMode)
					glFlush();
#endif
				imguiDriver->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}
#ifdef USE_HIKARU
		if (measure && rendered && !gui_is_open())
		{
			static unsigned frames;
			static double renderMs, presentMs;
			const auto end = std::chrono::steady_clock::now();
			renderMs += std::chrono::duration<double, std::milli>(renderEnd - frameStart).count();
			presentMs += std::chrono::duration<double, std::milli>(end - renderEnd).count();
			if (++frames == 60)
			{
				std::fprintf(stderr, "Hikaru host timing: frames=%u render_ms=%.3f present_ms=%.3f total_ms=%.3f vsync=%d fast_forward=%d\n",
					frames, renderMs / frames, presentMs / frames, (renderMs + presentMs) / frames,
					int(bool(config::VSync)), int(settings.input.fastForwardMode));
				frames = 0; renderMs = presentMs = 0;
			}
		}
#endif
		if (imguiDriver == nullptr)
			forceReinit = true;

		if (config::RendererType != currentRenderer || forceReinit)
		{
			mainui_term();
			int prevApi = renderApi(currentRenderer);
			int newApi = renderApi(config::RendererType);
			if (newApi != prevApi || forceReinit)
			{
				try {
					switchRenderApi();
				} catch (const FlycastException& e) {
					ERROR_LOG(RENDERER, "switchRenderApi failed: %s", e.what());
					if (prevApi == newApi)
						// fatal
						throw;
					// try to go back to the previous API
					config::RendererType = currentRenderer;
					try {
						switchRenderApi();
					} catch (const FlycastException& e) {
						ERROR_LOG(RENDERER, "Falling back to previous renderer also failed: %s", e.what());
						// fatal
						throw;
					}
				}
			}
			mainui_init();
			if (!gui_is_open() && custom_texture.needsRefresh())
			{
				try {
					emu.stop();
					gui_setState(GuiState::Loading);
				} catch (const FlycastException& e) {
					gui_stop_game(e.what());
				}
			}
			forceReinit = false;
			currentRenderer = config::RendererType;
		}

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
