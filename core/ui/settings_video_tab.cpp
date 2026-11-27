/*
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
#include "settings_new.h"
#include "settings_new_internal.h"
#include "shader_preview_layout.h"
#include "gui_menu.h"
#include "app_update_package.h"
#ifdef USE_METAL
#include "rend/metal/metal_api.h"
#endif
#include "custom_theme.h"
#include "theme.h"
#include "theme_browser.h"
#include "gui_util.h"
#ifdef HOLLYCAST_TEST_HARNESS
#include "test_harness.h"
#include "test_harness_ui.h"
#endif
#include "imgui.h"
#include "imgui_stdlib.h"
#include "audio/audiostream.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "video_preset.h"
#include "network/ice.h"
#include "network/ggpo.h"
#include "widgets/widgets.h"
#include "version.h"
#include "wsi/context.h"
#include "input/dreamlink/dreampotato.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/dreamlink/maplelinkregistry.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/pvr/Renderer_if.h"
#ifdef USE_HIKARU
#include "hw/hikaru/frontend.h"
#include "hw/hikaru/roms.h"
#endif
#ifdef USE_DREAMLINK_DEVICES
#include "sdl/dreamlink/dreamlink_sdl_gamepad.h"
#endif
#include "log/LogManager.h"
#include "hw/mem/addrspace.h"
#include "settings.h"
#include "emulator.h"
#include "rewind.h"
#include "gui.h"
#include "boxart/boxart.h"
#include "boxart/screenscraper.h"
#include "boxart/gamesdb.h"
#include "boxart/artwork_adapter.h"
#include "IconsFontAwesome6.h"
#include "mainui.h"
#include "oslib/oslib.h"
#include "oslib/resources.h"
#include "oslib/storage.h"
#include "rend/postprocess/shader_chain.h"
#include "json.hpp"
#include "stdclass.h"
#include "achievements/achievements.h"
#include "vgamepad.h"
#include "rend/vmu_window.h"
#include "network/phone_vmu_backup.h"
#ifdef HAVE_PHONE_VMU
#include "deps/qrcodegen/qrcodegen.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cfloat>
#include <future>
#include <functional>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef __ANDROID__
#if HOST_CPU == CPU_ARM64 && USE_VULKAN
#include "rend/vulkan/adreno.h"
#endif
#endif

extern ImFont *largeFont;
extern ImFont *settingsTitleFont;
extern ImFont *settingsRightValueFont;
extern ImFont *settingsIconFont;
extern ImFont *aboutAsciiFont;

namespace SettingsNew {











void renderVideoTab()
{
	ScopedTwoLineRowStyle videoRowStyle(12.0f, true, 0.5f);


	// ============================================================
	// PRESET SELECTOR
	// ============================================================
	if (RenderCollapsingHeader("QualityPresetsSection", ICON_FA_BOOKMARK, T("Quality Presets"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		static int currentPresetIndex = 0;
		static bool presetJustApplied = false;

		// Include Custom so manual changes cannot leave a stale preset label.
		if (!presetJustApplied)
			currentPresetIndex = static_cast<int>(detectCurrentPreset());

		// Build preset labels
		const char* presetLabels[] = {
			T("Potato (Max Performance)"),
			T("Low"),
			T("Medium (Balanced)"),
			T("High"),
			T("God Mode (Max Quality)"),
			T("Custom")
		};

		// Preset dropdown (custom so we can drive footer text per option)
		ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
		const char* currentLabel = (currentPresetIndex >= 0 && currentPresetIndex < 6)
			? presetLabels[currentPresetIndex]
			: T("Custom");
		if (ImGui::BeginCombo("##VideoPreset", currentLabel))
		{
			for (int i = 0; i < 6; ++i)
			{
				const bool isSelected = (currentPresetIndex == i);
				if (ImGui::Selectable(presetLabels[i], isSelected))
				{
					currentPresetIndex = i;
					if (currentPresetIndex < 5)
					{
						const VideoPreset* preset = getPresetByLevel(
							static_cast<VideoPresetLevel>(currentPresetIndex)
						);
						if (preset)
						{
							applyVideoPreset(*preset);
							presetJustApplied = true;
						}
					}
				}
				if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
				{
					if (i < 5)
					{
						const VideoPreset* preset = getPresetByLevel(
							static_cast<VideoPresetLevel>(i)
						);
						if (preset && preset->description)
							SetSettingsFooterText(preset->description);
					}
					else
					{
						SetSettingsFooterText(T("Custom\nSettings have been manually modified from the last preset."));
					}
				}
				if (isSelected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}

		// Show preset guidance and the selected preset description in the footer bar
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
		{
			std::string help =
				T(
					"Quality Presets\n"
					"If you are having issues, try our presets. Unless you are on a very low-end device, starting at the Medium preset should be safe. "
					"Adjust the resolution for the fastest performance adjustment from within a preset, then tweak the preset as you like for your perfect experience. "
					"Turn on the FPS Counter setting to ensure you get a stable 30/60 FPS during gameplay while finding the best settings for your device. "
					"Note: Fog and Shadows are enabled on all preset tiers."
				);
			SetSettingsFooterText(help.c_str());
		}

		ImGui::Separator();
		presetJustApplied = false;
	}

	// Graphics API Selection section
	int renderApi;
	bool perPixel;
	switch (config::RendererType)
	{
	default:
	case RenderType::OpenGL:
		renderApi = 0;
		perPixel = false;
		break;
	case RenderType::OpenGL_OIT:
		renderApi = 0;
		perPixel = true;
		break;
	case RenderType::Vulkan:
		renderApi = 1;
		perPixel = false;
		break;
	case RenderType::Vulkan_OIT:
		renderApi = 1;
		perPixel = true;
		break;
	case RenderType::DirectX9:
		renderApi = 2;
		perPixel = false;
		break;
	case RenderType::DirectX11:
		renderApi = 3;
		perPixel = false;
		break;
	case RenderType::DirectX11_OIT:
		renderApi = 3;
		perPixel = true;
		break;
	case RenderType::Metal:
		renderApi = 4;
		perPixel = false;
		break;
	case RenderType::Metal_OIT:
		renderApi = 4;
		perPixel = true;
		break;
	}

#ifdef USE_METAL
	const bool metalAvailable = metalIsAvailable();
#endif
	const bool hikaruPlatform = settings.platform.isHikaru();
#ifdef USE_HIKARU
	if (hikaruPlatform)
		perPixel = config::HikaruSorting.get() == config::HikaruSortingMode::PerPixel;
#endif
	const int apiCount = 0
#ifdef USE_METAL
		+ (metalAvailable ? 1 : 0)
#endif
#ifdef USE_VULKAN
		+ 1
#endif
#ifdef USE_DX9
		+ (hikaruPlatform ? 0 : 1)
#endif
#ifdef USE_OPENGL
		+ 1
#endif
#ifdef USE_DX11
		+ 1
#endif
		;

	if (RenderCollapsingHeader("GraphicsAPISection", ICON_FA_GEAR, T("Graphics API"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (apiCount > 1)
		{
			std::array<const char*, 5> apiLabels {};
			std::array<int, 5> apiValues {};
			int apiOptionCount = 0;
#ifdef USE_OPENGL
			apiLabels[apiOptionCount] = "OpenGL";
			apiValues[apiOptionCount++] = 0;
#endif
#ifdef USE_VULKAN
#ifdef __APPLE
			apiLabels[apiOptionCount] = "Vulkan (Metal)";
#else
			apiLabels[apiOptionCount] = "Vulkan";
#endif
			apiValues[apiOptionCount++] = 1;
#endif
#ifdef USE_DX9
			if (!hikaruPlatform) {
				apiLabels[apiOptionCount] = "DirectX 9";
				apiValues[apiOptionCount++] = 2;
			}
#endif
#ifdef USE_DX11
			apiLabels[apiOptionCount] = "DirectX 11";
			apiValues[apiOptionCount++] = 3;
#endif

#ifdef USE_METAL
			if (metalAvailable) {
				apiLabels[apiOptionCount] = T("Metal (Experimental)");
				apiValues[apiOptionCount++] = 4;
			}
#endif

			int currentApiIndex = 0;
			for (int i = 0; i < apiOptionCount; i++)
			{
				if (apiValues[i] == renderApi)
				{
					currentApiIndex = i;
					break;
				}
			}

			SettingsUI::PopupConfig apiCfg {};
			apiCfg.type = SettingsUI::PopupType::Options;
			apiCfg.options.label = T("Graphics API");
			apiCfg.options.icon = ICON_FA_GEAR;
			apiCfg.options.popupID = Tnop("GraphicsApiPopup");
			apiCfg.options.options = apiLabels.data();
			apiCfg.options.optionCount = apiOptionCount;
			apiCfg.options.currentValue = &currentApiIndex;
			apiCfg.options.valueWidth = 220.0f;
			apiCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
				if (selectedIndex < 0 || selectedIndex >= apiOptionCount)
					return;
				switch (apiValues[selectedIndex])
				{
				case 0:
					SetSettingsFooterText(
						T(
							"OpenGL\n"
							"This is the API that will randomly work best depending on the game and hardware, sometimes for no obvious reason.\n"
							"Always worth a try on any hardware if a game likes it."
						)
					);
					break;
				case 1:
					SetSettingsFooterText(
						T(
							"Vulkan\n"
							"Tends to work best with NVIDIA GPUs, but remember this is just a recommendation, not a fact.\n"
							"Per-game and per-hardware mileage may vary."
						)
					);
					break;
				case 3:
					SetSettingsFooterText(
						T(
							"DirectX 11\n"
							"Tends to give higher frame rates on AMD devices.\n"
							"Older and integrated graphics can benefit from this option as well."
						)
					);
					break;
				case 4:
					SetSettingsFooterText(T("Native Metal rendering for macOS. Experimental; per-pixel sorting is available on supported devices."));
					break;
				case 2:
					SetSettingsFooterText(
						T(
							"DirectX 9\n"
							"Like DirectX 11, this tends to be best on AMD hardware.\n"
							"Lower-end and older devices often work even better with DX9, but again, this is just a thought, not a hard rule."
						)
					);
					break;
				default:
					break;
				}
			};
			apiCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= apiOptionCount)
					return false;
				renderApi = apiValues[selectedIndex];
				if (renderApi == 2)
					perPixel = false;
#ifdef USE_METAL
				if (renderApi == 4 && !settings.platform.isHikaru() && !metalSupportsPerPixel())
					perPixel = false;
#endif
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("GraphicsApiSetting"),
				T(
					"Graphics API\n"
					"One of the most important settings when you're not getting a good experience.\n"
					"You will find that, per game and hardware, you can get different performance depending on the API selected.\n"
					"The best way to figure it out: if you are having FPS issues or glitchy gameplay, change this per your hardware recommendations and see what gives you the smoothest frame rate.\n"
					"(Enable the FPS Counter and test in actual gameplay.)"
				),
				apiCfg
			);
		}
	}

	// Transparent Sorting section
	if (RenderCollapsingHeader("TransparentSortingSection", ICON_FA_WAND_SPARKLES, T("Transparent Sorting"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		{
			bool has_per_pixel = hikaruPlatform || GraphicsContext::Instance()->hasPerPixel();
#ifdef USE_METAL
			if (renderApi == 4 && !hikaruPlatform)
				has_per_pixel = metalSupportsPerPixel();
#endif
			int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;
#ifdef USE_HIKARU
			if (hikaruPlatform) renderer = static_cast<int>(config::HikaruSorting.get());
#endif

			std::array<const char*, 3> sortingLabels {};
			std::array<int, 3> sortingValues {};
			int sortingOptionCount = 0;
			if (has_per_pixel)
			{
				sortingLabels[sortingOptionCount] = T("Per Pixel");
				sortingValues[sortingOptionCount++] = 2;
			}
			sortingLabels[sortingOptionCount] = T("Per Triangle");
			sortingValues[sortingOptionCount++] = 0;
			sortingLabels[sortingOptionCount] = T("Per Strip");
			sortingValues[sortingOptionCount++] = 1;

			int sortingSelection = 0;
			for (int i = 0; i < sortingOptionCount; i++)
			{
				if (sortingValues[i] == renderer)
				{
					sortingSelection = i;
					break;
				}
			}

			SettingsUI::PopupConfig sortingCfg {};
			sortingCfg.type = SettingsUI::PopupType::Options;
			sortingCfg.options.label = T("Sorting Method");
			sortingCfg.options.icon = ICON_FA_WAND_SPARKLES;
			sortingCfg.options.popupID = Tnop("TransparentSortingPopup");
			sortingCfg.options.options = sortingLabels.data();
			sortingCfg.options.optionCount = sortingOptionCount;
			sortingCfg.options.currentValue = &sortingSelection;
			sortingCfg.options.valueWidth = 220.0f;
			sortingCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
				if (selectedIndex < 0 || selectedIndex >= sortingOptionCount)
					return;
				switch (sortingValues[selectedIndex])
				{
				case 2:
					SetSettingsFooterText(
						/* xgettext:no-c-format */ T(
							"Per Pixel\n"
							"Highest performance requirements. This matches the Dreamcast's sorting quality, but only about 15% of games need it and show a meaningful difference versus the middle sorting option.\n"
							"Use only when needed (or if you have the resource overhead). It can almost double resource requirements.\n"
							"If you need full speed at a target resolution, you may have to lower resolution when using Per Pixel sorting."
						)
					);
					break;
				case 0:
					SetSettingsFooterText(
						/* xgettext:no-c-format */ T(
							"Per Triangle\n"
							"Good enough for about 85% of games to play with no or only very brief flashes of incorrect sorting.\n"
							"For most mid-range devices trying to reach 4K resolutions, you will likely spend your time here unless you must use Per Pixel.\n"
							"This is the recommended option for most people, most of the time."
						)
					);
					break;
				case 1:
					SetSettingsFooterText(
						T(
							"Per Strip\n"
							"Introduces more graphics glitching than Per Triangle, but can sometimes reduce resource requirements further.\n"
							"Not recommended unless you have to use it. Performance gains range from none to minimal, and may not be worth the quality tradeoff."
						)
					);
					break;
				default:
					break;
				}
			};
			sortingCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= sortingOptionCount)
					return false;
				const int selectedRenderer = sortingValues[selectedIndex];
#ifdef USE_HIKARU
				if (hikaruPlatform) {
					config::HikaruSorting.set(static_cast<config::HikaruSortingMode>(selectedRenderer));
					perPixel = selectedRenderer == 2;
					return true;
				}
#endif
				switch (selectedRenderer)
				{
				case 0:
					perPixel = false;
					config::PerStripSorting.set(false);
					break;
				case 1:
					perPixel = false;
					config::PerStripSorting.set(true);
					break;
				case 2:
					perPixel = true;
					config::PerStripSorting.set(false);
					break;
				default:
					return false;
				}
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("TransparentSortingSetting"),
				/* xgettext:no-c-format */ T(
					"Sorting Option\n"
					"This controls how we sort transparent layers in games.\n"
					"A handful of games will not render certain images correctly at all (example: Shenmue journal), or might have layering issues (example: the white of Sonic's eyes in Sonic Adventure), and a handful of others can show clipping or incorrect texture layering in some spots.\n\n"
					"There are three options with very different resource usage. While the highest setting matches Dreamcast sorting quality, it is only needed in about 15% of games and carries a large speed penalty.\n"
					"The middle tier allows just about all games to be played with a small amount of slight issues, but uses about half the resources of the top tier.\n"
					"It is \"good enough\" almost all the time."
				),
				sortingCfg);
		}
	}

	// Rendering Options section
	if (RenderCollapsingHeader("RenderingOptionsSection", ICON_FA_SLIDERS, T("Rendering Options"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Internal Resolution
		{
			constexpr std::array<float, 20> scalings {
				0.5f, 0.75f, 1.f, 1.25f, 1.5f, 1.75f, 2.f, 2.25f, 2.5f, 2.75f,
				3.f, 3.5f, 4.f, 4.5f, 5.f, 5.5f, 6.f, 7.f, 8.f, 9.f
			};
			const std::array<const char*, 20> scalingNames {
				T("Half"), "x0.75", T("Native"), "x1.25", "x1.5", "x1.75", "x2", "x2.25", "x2.5", "x2.75",
				"x3", "x3.5", "x4", "x4.5", "x5", "x5.5", "x6", "x7", "x8", "x9"
			};

			std::array<int, scalings.size()> verticalRes {};
			std::array<std::string, scalings.size()> resolutionLabels {};
			std::array<const char*, scalings.size()> resolutionLabelPtrs {};
			int internalResSelection = -1;
			const int configuredRenderResolution = config::RenderResolution.get();
			// Read both toggles every frame so labels also preview widescreen cheats before a game is loaded.
			float widthScale = 1.f;
			if (!settings.platform.isHikaru() && (config::Widescreen || config::WidescreenGameHacks)
					&& !config::Rotate90 && !config::EmulateFramebuffer)
			{
				widthScale = config::Widescreen && config::SuperWidescreen
						&& settings.display.width > 0 && settings.display.height > 0
					? static_cast<float>(settings.display.width) / settings.display.height / 4.f * 3.f
					: 4.f / 3.f;
			}

			for (size_t i = 0; i < scalings.size(); i++)
			{
				verticalRes[i] = static_cast<int>(scalings[i] * 480.0f);
				// Keep the renderer's even-pixel width rounding for the selected aspect ratio.
				const int horizontalRes = static_cast<int>(std::round(scalings[i] * 640.f * widthScale / 2.f) * 2.f);

				if (verticalRes[i] == configuredRenderResolution)
					internalResSelection = static_cast<int>(i);

				resolutionLabels[i] = std::to_string(horizontalRes) + "x" + std::to_string(verticalRes[i]) + " (" + scalingNames[i] + ")";
				resolutionLabelPtrs[i] = resolutionLabels[i].c_str();
			}

			SettingsUI::PopupConfig internalResCfg {};
			internalResCfg.type = SettingsUI::PopupType::Options;
			internalResCfg.options.label = T("Internal Resolution");
			internalResCfg.options.icon = ICON_FA_DISPLAY;
			internalResCfg.options.popupID = Tnop("InternalResPopup");
			internalResCfg.options.options = resolutionLabelPtrs.data();
			internalResCfg.options.optionCount = static_cast<int>(resolutionLabelPtrs.size());
			internalResCfg.options.currentValue = &internalResSelection;
			// Older and hand-edited configurations can contain resolutions outside
			// the preset list. Show the stored vertical target instead of falsely
			// presenting the first (Half) preset, and preserve it until a preset is
			// explicitly selected.
			const std::string customResolutionLabel = std::to_string(configuredRenderResolution)
				+ "p (" + T("Custom") + ")";
			internalResCfg.options.valueToString = [&, customResolutionLabel](int selectedIndex) {
				if (selectedIndex >= 0 && selectedIndex < static_cast<int>(resolutionLabelPtrs.size()))
					return resolutionLabelPtrs[selectedIndex];
				return customResolutionLabel.c_str();
			};
			internalResCfg.options.valueWidth = 220.0f;
			internalResCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= static_cast<int>(verticalRes.size()))
					return false;
				// RenderResolution is the vertical target size; the renderer derives
				// its scale from this value relative to the native 480-line output.
				config::RenderResolution.set(verticalRes[selectedIndex]);
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("InternalResolutionSetting"),
				T("Internal render resolution (the biggest performance lever)."),
				internalResCfg,
				T(
					"Internal Resolution\n"
					"This is the single biggest graphics performance setting.\n"
					"Resolution labels update with the widescreen settings. With Widescreen Game Cheats alone, labels preview the stretched 16:9 output for supported games.\n\n"
					"Lower values reduce GPU load (faster). Higher values improve clarity, reduce shimmer/aliasing, and act as a form of supersampling.\n\n"
					"If you are not holding a stable 30/60 FPS, reduce Internal Resolution first before turning off visual features."
				)
			);
		}

#ifdef USE_METAL
		if (renderApi == 4)
		{
			const bool spatial = metalSupportsSpatialFX();
			RenderGeneralToggleSettingRow("MetalFXUpscaling", ICON_FA_DISPLAY, T("MetalFX Upscaling"),
				spatial ? T("Use spatial upscaling when the game image is smaller than its display area.")
				        : T("MetalFX spatial upscaling is unavailable on this Mac."),
				spatial && config::MetalFXUpscaling,
				[](bool enabled) { config::MetalFXUpscaling.set(enabled); },
				T(
					"MetalFX Upscaling\n"
					"Uses Apple's MetalFX spatial upscaler when the game image is smaller than its display area, improving sharpness compared to plain bilinear scaling.\n"
					"Requires macOS 13 or later and a supported GPU.\n\n"
					"Off by default. If the image looks soft when stretched to a large window or display, enable it for a sharper result."
				),
				!spatial);
			if (spatial && config::MetalFXUpscaling && !hikaruPlatform)
			{
				const bool temporal = metalSupportsTemporalFX();
				RenderGeneralToggleSettingRow("MetalFXTemporal", ICON_FA_WAND_MAGIC, T("Use Temporal Upscaling"),
					temporal ? T("Use depth and motion from successive frames. Frames without temporal inputs use spatial upscaling.")
					         : T("MetalFX temporal upscaling is unavailable on this Mac."),
					temporal && config::MetalFXTemporal,
					[](bool enabled) { config::MetalFXTemporal.set(enabled); },
					T(
						"Use Temporal Upscaling\n"
						"Reconstructs the upscaled image using depth and motion information from successive frames for higher quality than spatial upscaling alone.\n"
						"Frames without valid temporal inputs fall back to spatial upscaling. Requires MetalFX Upscaling, macOS 13 or later and a supported GPU.\n\n"
						"Experimental: fast motion or transparency may show artifacts. Off by default."
					),
					!temporal);
			}
		}
#endif

		RenderGeneralToggleSettingRow(
			"IntegerScaling",
			ICON_FA_COMPRESS,
			T("Integer Scaling"),
			T("Pixel-perfect scaling using whole-number multiples."),
			static_cast<bool>(config::IntegerScale),
			[](bool enabled) { config::IntegerScale.set(enabled); },
			T(
				"Integer Scaling\n"
				"Scales the output by the maximum whole-number multiple allowed by your display (1x, 2x, 3x, ...).\n"
				"Best for pixel-art/2D content because it avoids uneven scaling artifacts.\n\n"
				"Tip: for the crispest result, pair Integer Scaling with nearest-neighbor (disable Linear Interpolation)."
			));

		RenderGeneralToggleSettingRow(
			"LinearInterpolation",
			ICON_FA_WAND_MAGIC,
			T("Linear Interpolation"),
			T("Smoother scaling (linear) vs. sharper pixels (nearest-neighbor)."),
			static_cast<bool>(config::LinearInterpolation),
			[](bool enabled) { config::LinearInterpolation.set(enabled); },
			T(
				"Linear Interpolation\n"
				"Enabled: smoother scaling (can look blurrier).\n"
				"Disabled: nearest-neighbor scaling (sharper, more pixelated).\n\n"
				"If you enable Integer Scaling, many users prefer disabling Linear Interpolation for a pixel-perfect look."
			));

#ifndef TARGET_IPHONE
		RenderGeneralToggleSettingRow(
			"VSync",
			ICON_FA_ARROWS_ROTATE,
			T("VSync"),
			T("Sync presentation to your display to reduce tearing."),
			static_cast<bool>(config::VSync),
			[](bool enabled) { config::VSync.set(enabled); },
			T(
				"VSync\n"
				"Synchronizes frame presentation to your display refresh to reduce tearing and improve pacing.\n"
				"On some setups it can add input latency, and if you cannot maintain full speed it can contribute to stutter.\n\n"
				"Recommended for most users. If you notice unusual stutter or latency, try toggling it."
			));

		ImGui::Indent();

#ifdef __ANDROID__
		RenderGeneralToggleSettingRow(
			"FramePacing",
			ICON_FA_TIMELINE,
			T("Frame Pacing (Experimental)"),
			T("Provide the GPU with presentation timing for each frame to replicate original frame pacing."),
			static_cast<bool>(config::FramePacing),
			[](bool enabled) { config::FramePacing.set(enabled); },
			T("Provide the GPU with presentation timing for each frame to replicate original frame pacing.")
		);
#else
		if (isVulkan(config::RendererType))
		{
			RenderGeneralToggleSettingRow(
				"DuplicateFrames",
				ICON_FA_CLONE,
				T("Duplicate Frames"),
				T("Improve pacing on high refresh-rate displays."),
				static_cast<bool>(config::DupeFrames),
				[](bool enabled) { config::DupeFrames.set(enabled); },
				T(
					"Duplicate Frames\n"
					"Duplicates frames on high refresh-rate monitors (120 Hz and above) to improve perceived pacing.\n"
					"Only available for Vulkan, and only when VSync is disabled."
				),
				!config::VSync
			);
		}
#endif

		ImGui::Unindent();
#endif

		RenderGeneralToggleSettingRow(
			"Framebuffer",
			ICON_FA_FILE,
			T("Full Framebuffer Emulation"),
			T("Accurate effects, but extremely expensive."),
			static_cast<bool>(config::EmulateFramebuffer),
			[](bool enabled) { config::EmulateFramebuffer.set(enabled); },
			T(
				"Full Framebuffer Emulation\n"
				"Enables accurate VRAM framebuffer emulation. Required for some games' effects.\n"
				"Very slow, and generally incompatible with upscaling and widescreen.\n\n"
				"Only enable if a game needs it, and be prepared to lower Internal Resolution."
			));

		RenderGeneralToggleSettingRow(
			"CustomTextures",
			ICON_FA_IMAGE,
			T("Load Custom Textures"),
			T("Enable texture replacement packs."),
			static_cast<bool>(config::CustomTextures),
			[](bool enabled) { config::CustomTextures.set(enabled); },
			T(
				"Load Custom Textures\n"
				"Loads custom/high-res textures from `data/textures/<game id>`.\n"
				"Great for community texture packs, but can increase load time and memory usage.\n"
				"Supports KTX2/XUBC7, KTX2/XUASTC, KTX2/ETC1S, DDS/BC7, PNG, and JPEG.\n\n"
				"If you see stutter from texture streaming, consider enabling Preload Custom Textures."
			),
			game_started);

		const bool gpuPreloadSupported = rend_supports_gpu_texture_preload();
		int configuredMode = static_cast<int>(config::customTexturePreloadMode());

		std::vector<const char*> texturePreloadingOptions{T("Off"), T("System Memory")};

		if (gpuPreloadSupported) {
			// Assumption: (config::CustomTexturePreloadMode::VideoMemory) is 2 - not available when !gpuPreloadSupported
			assert(texturePreloadingOptions.size() == static_cast<size_t>(config::CustomTexturePreloadMode::VideoMemory));
			texturePreloadingOptions.push_back(T("Video Memory"));
		}

		SettingsUI::PopupConfig texturePreloadCfg {};
		texturePreloadCfg.type = SettingsUI::PopupType::Options;
		texturePreloadCfg.options.label = T("Preload Custom Textures");
		texturePreloadCfg.options.icon = ICON_FA_DOWNLOAD;
		texturePreloadCfg.options.popupID = Tnop("PreloadTexturesPopup");
		texturePreloadCfg.options.options = texturePreloadingOptions.data();
		texturePreloadCfg.options.optionCount = static_cast<int>(texturePreloadingOptions.size());
		texturePreloadCfg.options.currentValue = &configuredMode;
		texturePreloadCfg.options.valueWidth = 220.0f;
		texturePreloadCfg.options.onChange = [&](int selectedType) {
			if (selectedType < 0 || selectedType >= texturePreloadingOptions.size())
				return false;
			config::PreloadCustomTextures = selectedType;
			return true;
		};

		ImGui::Indent();
		RenderGeneralPopupSettingRow(
			Tnop("PreloadTextures"),
			T("Trade memory for fewer texture hitches."),
			texturePreloadCfg,
			T(
				"Preload Custom Textures\n"
				"Preloads custom textures at game start.\n"
				"Can reduce runtime stutter at the cost of increased memory usage and longer initial load times."
			),
			!config::CustomTextures
		);
		ImGui::Unindent();
	}

	// Aspect Ratio section
	if (RenderCollapsingHeader("AspectRatioSection", ICON_FA_TV, T("Aspect Ratio"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"Widescreen",
			ICON_FA_TV,
			T("Widescreen"),
			T("Renders a wider view (may reveal glitches)."),
			static_cast<bool>(config::Widescreen),
			[](bool enabled) { config::Widescreen.set(enabled); },
			T(
				"Widescreen\n"
				"Draws geometry outside the normal 4:3 view to render a wider scene.\n"
				"Some games were not authored for widescreen, so newly revealed areas can show visual glitches, missing geometry, or incorrect culling.\n\n"
				"If a game looks broken in widescreen, disable this (or try Widescreen Game Cheats if supported)."
			));

		ImGui::Indent();
		RenderGeneralToggleSettingRow(
			"SuperWidescreen",
			ICON_FA_EXPAND,
			T("Super Widescreen"),
			T("Fills ultra-wide displays (more likely to reveal issues)."),
			static_cast<bool>(config::SuperWidescreen),
			[](bool enabled) { config::SuperWidescreen.set(enabled); },
			T(
				"Super Widescreen\n"
				"Uses the full width of the screen/window when the aspect ratio is wider than 16:9.\n"
				"More likely to reveal out-of-bounds glitches than standard widescreen.\n\n"
				"Not compatible with Integer Scaling or Stretch to Fill."
			),
			(!config::Widescreen || config::IntegerScale || config::StretchToFill) && !config::SuperWidescreen);
		ImGui::Unindent();

		RenderGeneralToggleSettingRow(
			"WidescreenGameCheats",
			ICON_FA_CODE,
			T("Widescreen Game Cheats"),
			T("Game-specific widescreen patches (when available)."),
			static_cast<bool>(config::WidescreenGameHacks),
			[](bool enabled) { config::WidescreenGameHacks.set(enabled); },
			T(
				"Widescreen Game Cheats\n"
				"Modifies supported games to display in a 16:9 anamorphic format (game-specific patches).\n"
				"Only some games are supported, but when it works it can be cleaner than geometry widescreen."
			));

		RenderGeneralToggleSettingRow(
			"StretchToFill",
			ICON_FA_EXPAND,
			T("Stretch to Fill"),
			T("Stretch the screen to fill the entire screen or window."),
			static_cast<bool>(config::StretchToFill),
			[](bool enabled) { config::StretchToFill.set(enabled); },
			T("Stretch the screen to fill the entire screen or window."),
			config::SuperWidescreen);

		{
			SettingsUI::PopupSliderConfig stretchCfg {};
			stretchCfg.label = T("Horizontal Stretching");
			stretchCfg.icon = ICON_FA_EXPAND;
			stretchCfg.popupID = Tnop("HorizontalStretchPopup");
			stretchCfg.description = /* xgettext:no-c-format */ T("Stretches the image sideways to fill your display. Use 132% for 16:9 correction.");
			stretchCfg.currentValue = &config::ScreenStretching.get();
			stretchCfg.minValue = 100;
			stretchCfg.maxValue = 250;
			stretchCfg.format = "%d%%";
			stretchCfg.valueWidth = 220.0f;
			stretchCfg.sliderWidth = 320.0f;

			SettingsUI::PopupConfig stretchPopupCfg {};
			stretchPopupCfg.type = SettingsUI::PopupType::Slider;
			stretchPopupCfg.slider = stretchCfg;

			RenderGeneralPopupSettingRow(
				Tnop("HorizontalStretchingSetting"),
				/* xgettext:no-c-format */ T("Fine-tune horizontal scaling (often 132% for 16:9 correction)."),
				stretchPopupCfg,
				/* xgettext:no-c-format */ T(
					"Horizontal Stretching\n"
					"Stretches the image sideways.\n"
					"132% is a common value for 16:9 correction, but the best value depends on your display and personal preference."
				),
				config::StretchToFill
			);
		}

		RenderGeneralToggleSettingRow(
			"RotateScreen",
			ICON_FA_ROTATE_RIGHT,
			T("Rotate Screen 90°"),
			T("Rotate the entire output for rotated games."),
			static_cast<bool>(config::Rotate90),
			[](bool enabled) { config::Rotate90.set(enabled); },
			T(
				"Rotate Screen 90°\n"
				"Rotates the screen 90 degrees counterclockwise.\n"
				"Useful for games designed for rotated/vertical displays."
			));
	}

	// Per Pixel Settings (only shown when Per Pixel sorting is enabled)
	if (perPixel && RenderCollapsingHeader("PerPixelSettingsSection", ICON_FA_IMAGE, T("Per Pixel Settings"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		const std::array<int64_t, 4> bufSizes{ 512_MB, 1_GB, 2_GB, 4_GB };
		const std::array<const char*, 4> bufSizesText{ "512 MB", "1 GB", "2 GB", "4 GB" };
		int pixelBufferSelection = 0;
		for (int i = 0; i < static_cast<int>(bufSizes.size()); ++i)
		{
			if (bufSizes[i] == config::PixelBufferSize)
			{
				pixelBufferSelection = i;
				break;
			}
		}

		SettingsUI::PopupConfig pixelBufferCfg {};
		pixelBufferCfg.type = SettingsUI::PopupType::Options;
		pixelBufferCfg.options.label = T("Pixel Buffer Size");
		pixelBufferCfg.options.icon = ICON_FA_MEMORY;
		pixelBufferCfg.options.popupID = Tnop("PixelBufferSizePopup");
		pixelBufferCfg.options.options = bufSizesText.data();
		pixelBufferCfg.options.optionCount = static_cast<int>(bufSizesText.size());
		pixelBufferCfg.options.currentValue = &pixelBufferSelection;
		pixelBufferCfg.options.valueWidth = 220.0f;
		pixelBufferCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(bufSizes.size()))
				return;
			SetSettingsFooterText(
				(selectedIndex == 0)
					? T("Pixel Buffer Size\n512 MB is the smallest budget and is the fastest place to start. Use this if you want to minimize VRAM/RAM usage, but increase it if you see missing or incorrect transparency in heavier scenes.")
					: (selectedIndex == 1)
						? T("Pixel Buffer Size\n1 GB is a good middle ground if 512 MB is not enough. It gives Per-Pixel sorting more room for complex scenes without jumping straight to the larger memory costs.")
						: (selectedIndex == 2)
							? T("Pixel Buffer Size\n2 GB is useful for heavier scenes, higher internal resolutions, or games with lots of overlapping transparent effects. Use this if you still see transparency issues at 1 GB.")
							: T("Pixel Buffer Size\n4 GB is the largest buffer and is mainly for very demanding setups. It gives the most headroom for Per-Pixel sorting, but uses the most memory and is usually unnecessary unless you are pushing quality hard."));
		};
		pixelBufferCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(bufSizes.size()))
				return false;
			config::PixelBufferSize.set(bufSizes[selectedIndex]);
			return true;
		};

		RenderGeneralPopupSettingRow(
			Tnop("PixelBufferSizeSetting"),
			T("Memory budget for Per-Pixel transparency rendering."),
			pixelBufferCfg,
			hikaruPlatform
				? T("Pixel Buffer Size\n"
					"Controls the memory budget for Hikaru per-pixel transparency. Targets are allocated as needed.\n"
					"Higher resolutions and overlapping effects require more memory. Increase this budget if transparency is missing or the renderer falls back to depth peeling.")
				: T("Pixel Buffer Size\n"
					"Controls the memory budget for Per-Pixel (OIT) transparency rendering.\n"
					"If you increase Internal Resolution a lot or see missing or incorrect transparency in complex scenes, you may need a larger buffer.\n"
					"Lower values reduce memory usage, but can limit transparency accuracy in heavier scenes.")
		);

#ifdef USE_HIKARU
		auto& maximumLayers = hikaruPlatform ? config::HikaruMaxLayers : config::PerPixelLayers;
#else
		auto& maximumLayers = config::PerPixelLayers;
#endif
		static int perPixelLayersTemp = maximumLayers.get();
		if (!ImGui::IsPopupOpen("PerPixelMaximumLayersPopup"))
			perPixelLayersTemp = maximumLayers.get();

		SettingsUI::PopupSliderConfig perPixelLayersCfg {};
		perPixelLayersCfg.label = T("Maximum Layers");
		perPixelLayersCfg.icon = ICON_FA_LAYER_GROUP;
		perPixelLayersCfg.popupID = Tnop("PerPixelMaximumLayersPopup");
		perPixelLayersCfg.description = hikaruPlatform
			? T("Limits Hikaru depth-peeling layers. Supported OpenGL and Vulkan fragment lists use Pixel Buffer Size instead; this limit still applies to their peeling fallback.")
			: renderApi == 4
			? T("Controls how many transparent layers are resolved per pixel. Metal retains the nearest layers when the limit is exceeded. Lower values can improve performance.")
			: T("Controls how many transparent layers can be resolved per pixel in complex scenes.\n"
				"Increase this if you see missing transparency or incorrect layering.\n"
				"Lower values reduce memory pressure and can improve performance.");
		perPixelLayersCfg.currentValue = &perPixelLayersTemp;
		perPixelLayersCfg.minValue = hikaruPlatform ? 1 : 8;
		perPixelLayersCfg.maxValue = 128;
		perPixelLayersCfg.format = "%d";
		perPixelLayersCfg.valueWidth = 220.0f;
		perPixelLayersCfg.sliderWidth = 320.0f;
		perPixelLayersCfg.onValueChange = [&]() {
			maximumLayers.set(perPixelLayersTemp);
		};

		SettingsUI::PopupConfig perPixelLayersPopupCfg {};
		perPixelLayersPopupCfg.type = SettingsUI::PopupType::Slider;
		perPixelLayersPopupCfg.slider = perPixelLayersCfg;

		RenderGeneralPopupSettingRow(
			Tnop("PerPixelMaximumLayersSetting"),
			T("Maximum transparent layers resolved per pixel."),
			perPixelLayersPopupCfg,
			hikaruPlatform
				? T("Per-Pixel Maximum Layers\nLimits depth peeling and its fallback paths. Successful OpenGL and Vulkan fragment lists retain fragments within Pixel Buffer Size, without a fixed layer count.")
				: renderApi == 4
				? T("Per-Pixel Maximum Layers\nMetal retains the nearest layers when the limit is exceeded. Increase the limit if transparency is missing; lower it to reduce resolve work.")
				: T("Per-Pixel Maximum Layers\n"
					"Limits how many transparent layers can be resolved per pixel in complex scenes.\n"
					"Increase this if you see missing transparency or incorrect layering.\n"
					"Decrease it to improve performance and reduce memory pressure.")
		);
	}

	// Performance section
	if (RenderCollapsingHeader("PerformanceSection", ICON_FA_GAUGE_HIGH, T("Performance"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Spacing();

		{
			const std::array<const char*, 3> autoSkipLabels { T("Disabled"), T("Normal"), T("Maximum") };
			int autoSkipSelection = config::AutoSkipFrame.get();
			if (autoSkipSelection < 0 || autoSkipSelection >= static_cast<int>(autoSkipLabels.size()))
				autoSkipSelection = 0;

			SettingsUI::PopupConfig autoSkipCfg {};
			autoSkipCfg.type = SettingsUI::PopupType::Options;
			autoSkipCfg.options.label = T("Automatic Frame Skipping");
			autoSkipCfg.options.icon = ICON_FA_FORWARD;
			autoSkipCfg.options.popupID = Tnop("AutoFrameSkipPopup");
			autoSkipCfg.options.options = autoSkipLabels.data();
			autoSkipCfg.options.optionCount = static_cast<int>(autoSkipLabels.size());
			autoSkipCfg.options.currentValue = &autoSkipSelection;
			autoSkipCfg.options.valueWidth = 220.0f;
			autoSkipCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= static_cast<int>(autoSkipLabels.size()))
					return false;
				config::AutoSkipFrame.set(selectedIndex);
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("AutoFrameSkippingSetting"),
				T("Auto-skip frames to maintain full-speed gameplay."),
				autoSkipCfg,
				T(
					"Automatic Frame Skipping\n"
					"Skips rendering frames when the emulator cannot keep up, to help maintain full-speed gameplay.\n\n"
					"Disabled: never auto-skip.\n"
					"Normal: skip when CPU and GPU are both slow.\n"
					"Maximum: skip when the GPU is slow.\n\n"
					"For best image quality, prefer lowering Internal Resolution before relying on heavy frame skipping."
				)
			);
		}

		{
			SettingsUI::PopupSliderConfig frameSkipCfg {};
			frameSkipCfg.label = T("Frame Skipping");
			frameSkipCfg.icon = ICON_FA_FORWARD;
			frameSkipCfg.popupID = Tnop("FrameSkipPopup");
			frameSkipCfg.description = T("Set how many frames to skip between rendered frames.");
			frameSkipCfg.currentValue = &config::SkipFrame.get();
			frameSkipCfg.minValue = 0;
			frameSkipCfg.maxValue = 6;
			frameSkipCfg.format = "%d";
			frameSkipCfg.valueWidth = 220.0f;
			frameSkipCfg.sliderWidth = 320.0f;

			SettingsUI::PopupConfig frameSkipPopupCfg {};
			frameSkipPopupCfg.type = SettingsUI::PopupType::Slider;
			frameSkipPopupCfg.slider = frameSkipCfg;

			RenderGeneralPopupSettingRow(
				Tnop("FrameSkippingSetting"),
				T("Force a fixed number of skipped frames."),
				frameSkipPopupCfg,
				T(
					"Frame Skipping\n"
					"Forces a fixed number of frames to be skipped between rendered frames.\n"
					"This is more aggressive and less adaptive than Automatic Frame Skipping.\n\n"
					"Use only if you know a title benefits from it, or as a last resort when tuning for speed."
				)
			);
		}

		// 2x height toggle rows for Performance settings
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(SettingsLayoutScaled(8.0f), SettingsLayoutScaled(12.0f)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, SettingsLayoutScaled(8.0f)));

		// Shadows toggle
		RenderGeneralToggleSettingRow(
			"Shadows",
			ICON_FA_MOON,
			T("Shadows"),
			T("Modifier volumes (often used for shadows)."),
			static_cast<bool>(config::ModifierVolumes),
			[](bool enabled) { config::ModifierVolumes.set(enabled); },
			T(
				"Shadows (Modifier Volumes)\n"
				"Enables modifier volumes, which are usually used for shadowing and related effects.\n"
				"Disable if a game shows shadow-related artifacts, or if you need a little extra performance."
			));

		// Fog toggle
		RenderGeneralToggleSettingRow(
			"Fog",
			ICON_FA_CLOUD,
			T("Fog"),
			T("Atmospheric fog effects."),
			static_cast<bool>(config::Fog),
			[](bool enabled) { config::Fog.set(enabled); },
			T(
				"Fog\n"
				"Enables atmospheric fog effects.\n"
				"Disable if you see fog artifacts, or if you need extra performance in heavy scenes."
			));

		ImGui::PopStyleVar(2);
	}

	// Advanced section
	if (RenderCollapsingHeader("AdvancedSection", ICON_FA_WAND_MAGIC, T("Advanced"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Spacing();

		// 2x height toggle rows for Advanced settings
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(SettingsLayoutScaled(8.0f), SettingsLayoutScaled(12.0f)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, SettingsLayoutScaled(8.0f)));

		// Delay Frame Swapping
		RenderGeneralToggleSettingRow(
			"DelayFrameSwapping",
			ICON_FA_CLOCK,
			T("Delay Frame Swapping"),
			T("Delay the frame swap to reduce flashing or unstable video playback."),
			static_cast<bool>(config::DelayFrameSwapping),
			[](bool enabled) { config::DelayFrameSwapping = enabled; },
			T(
				"Delay Frame Swapping\n"
				"Waits for the Dreamcast read/write frame buffers to line up before presenting the next frame.\n\n"
				"This can reduce flashing screens, unstable video playback, or pacing issues in titles that swap buffers in unusual ways.\n"
				"Leave it off if it causes slowdown or extra latency on slower devices."
			));

		// Fix Upscale Bleeding Edge
		RenderGeneralToggleSettingRow(
			"FixUpscaleBleedingEdge",
			ICON_FA_PLUG,
			T("Fix Upscale Bleeding Edge"),
			T("Reduce texture bleeding when upscaling."),
			static_cast<bool>(config::FixUpscaleBleedingEdge),
			[](bool enabled) { config::FixUpscaleBleedingEdge = enabled; },
			T(
				"Fix Upscale Bleeding Edge\n"
				"Helps with texture bleeding when upscaling.\n"
				"Disable if pixels warp in 2D games (MVC2, CVS, KOF, etc.)."
			));

		// Native Depth Interpolation
		RenderGeneralToggleSettingRow(
			"NativeDepthInterpolation",
			ICON_FA_MICROCHIP,
			T("Native Depth Interpolation"),
			T("Fix depth/texture issues on some GPUs."),
			static_cast<bool>(config::NativeDepthInterpolation),
			[](bool enabled) { config::NativeDepthInterpolation = enabled; },
			T(
				"Native Depth Interpolation\n"
				"Helps with texture corruption and depth issues on some GPUs (often AMD, sometimes Intel).\n"
				"If you see depth-related flicker or corruption, try enabling this."
			));

		// Copy Rendered Textures to VRAM
		RenderGeneralToggleSettingRow(
			"RenderToTextureBuffer",
			ICON_FA_COPY,
			T("Copy Rendered Textures"),
			T("Accuracy option for render-to-texture effects."),
			static_cast<bool>(config::RenderToTextureBuffer),
			[](bool enabled) { config::RenderToTextureBuffer = enabled; },
			T(
				"Copy Rendered Textures\n"
				"Copies render-to-texture results back into VRAM.\n"
				"Slower, but more accurate. Enable if a game has missing or incorrect render-to-texture effects."
			));

		ImGui::PopStyleVar(2);

		// Anisotropic Filtering
		{
			const std::array<int, 5> anisoValues { 1, 2, 4, 8, 16 };
			const std::array<const char*, 5> anisoLabels { T("Disabled"), "2x", "4x", "8x", "16x" };
			int anisoSelection = 0;
			for (size_t i = 0; i < anisoValues.size(); i++)
			{
				if (anisoValues[i] == config::AnisotropicFiltering.get())
				{
					anisoSelection = static_cast<int>(i);
					break;
				}
			}

			SettingsUI::PopupConfig anisoCfg {};
			anisoCfg.type = SettingsUI::PopupType::Options;
			anisoCfg.options.label = T("Anisotropic Filtering");
			anisoCfg.options.icon = ICON_FA_GEM;
			anisoCfg.options.popupID = Tnop("AnisotropicPopup");
			anisoCfg.options.options = anisoLabels.data();
			anisoCfg.options.optionCount = static_cast<int>(anisoLabels.size());
			anisoCfg.options.currentValue = &anisoSelection;
			anisoCfg.options.valueWidth = 220.0f;
			anisoCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= static_cast<int>(anisoValues.size()))
					return false;
				config::AnisotropicFiltering.set(anisoValues[selectedIndex]);
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("AnisotropicFilteringSetting"),
				T("Sharper textures at steep angles (mipmapped textures only)."),
				anisoCfg,
				T(
					"Anisotropic Filtering\n"
					"Makes mipmapped textures viewed at sharp angles look cleaner (less shimmer).\n"
					"Increases GPU cost.\n\n"
					"Only affects mipmapped textures. If you want this to do anything, keep mipmaps enabled."
				));
		}

		// Texture Sampling (legacy sampler override)
		const std::array<const char*, 3> textureFilterLabels {
			T("Default"),
			T("Force Nearest-Neighbor"),
			T("Force Linear")
		};
		int textureFilteringSelection = config::TextureFiltering.get();
		if (textureFilteringSelection < 0 || textureFilteringSelection >= static_cast<int>(textureFilterLabels.size()))
			textureFilteringSelection = 0;

		SettingsUI::PopupConfig textureFilterCfg {};
		textureFilterCfg.type = SettingsUI::PopupType::Options;
		textureFilterCfg.options.label = T("Sampler Override");
		textureFilterCfg.options.icon = ICON_FA_FILTER;
		textureFilterCfg.options.popupID = Tnop("TextureFilteringPopup");
		textureFilterCfg.options.options = textureFilterLabels.data();
		textureFilterCfg.options.optionCount = static_cast<int>(textureFilterLabels.size());
		textureFilterCfg.options.currentValue = &textureFilteringSelection;
		textureFilterCfg.options.valueWidth = 220.0f;
		textureFilterCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(textureFilterLabels.size()))
				return false;
			config::TextureFiltering.set(selectedIndex);
			return true;
		};

		RenderGeneralPopupSettingRow(
			Tnop("TextureFilteringSetting"),
			T("Override the sampler mode used when textures are sampled."),
			textureFilterCfg,
			T(
				"Texture Sampling Override\n"
				"Default: uses game behavior.\n"
				"Force Nearest-Neighbor: crisp/pixelated look.\n"
				"Force Linear: smoother/blurrier look.\n\n"
				"For 2D/pixel-art, many users prefer Nearest-Neighbor. For 3D, Linear can look nicer."
			));

		const auto sampleTextureFilterLabels = getSampleTextureFilterLabels();
		const std::array<const char*, 20> sampleTextureFilterDescriptions {
			T(
				"Sample Texture Filter: Disabled\n"
				"Uses HollyCast's legacy texture sampling behavior."
			),
			T(
				"Sample Texture Filter: Bilinear\n"
				"Smooths texture samples with alpha-weighted bilinear interpolation. Good simple baseline."
			),
			T(
				"Sample Texture Filter: Bilinear / No Edge Blending\n"
				"Smooth bilinear filtering with harder alpha edges to reduce transparent-edge bleeding."
			),
			T(
				"Sample Texture Filter: Jinc2\n"
				"Windowed Jinc-style reconstruction. Premium smooth option, but sharp edges can show mild ringing."
			),
			T(
				"Sample Texture Filter: Jinc2 / No Edge Blending\n"
				"Jinc2 reconstruction with harder alpha edges for sprites, UI, and texture-sheet borders."
			),
			T(
				"Sample Texture Filter: MMPX\n"
				"Pixel-art style reconstruction that preserves hard shapes. Best for 2D artwork, not natural textures."
			),
			T(
				"Sample Texture Filter: MMPX Enhanced\n"
				"Softer MMPX variant that blends reconstructed pixels back toward the source."
			),
			T(
				"Sample Texture Filter: MMPX Advanced\n"
				"More aggressive MMPX edge rules. Useful for pixel-art tests, but can be stylized."
			),
			T(
				"Sample Texture Filter: Jinc2 Anti-Ring\n"
				"Jinc2-style reconstruction with halo clamping. Better for signs, logos, HUD art, decals, and alpha edges."
			),
			T(
				"Sample Texture Filter: Jinc2 Sharp Anti-Ring\n"
				"Sharper Jinc2-style reconstruction with anti-ringing. Advanced option for crisp Dreamcast-era texture sheets."
			),
			T(
				"Sample Texture Filter: Jinc2 Mild Anti-Ring\n"
				"Jinc2 reconstruction with mild sharpening and an anti-ringing clamp."
			),
			T(
				"Sample Texture Filter: Jinc2 Extra Sharp Anti-Ring\n"
				"Jinc2 reconstruction with stronger sharpening and an anti-ringing clamp."
			),
			T(
				"Sample Texture Filter: Lanczos2 Anti-Ring\n"
				"Sharp sinc/Lanczos reconstruction with a smaller radius and halo clamp. Strong everyday texture option."
			),
			T(
				"Sample Texture Filter: Lanczos3 Anti-Ring\n"
				"Sharper larger-radius Lanczos reconstruction with halo clamp. High quality, but more likely to emphasize edges."
			),
			"", // Retired ID 14; keep persisted filter IDs stable.
			T(
				"Sample Texture Filter: Robidoux\n"
				"Balanced cubic reconstruction. Good compromise for painted or photographic Dreamcast-era textures."
			),
			T(
				"Sample Texture Filter: Robidoux Sharp\n"
				"Sharper Robidoux-style cubic filter. A good candidate for texture packs and detailed environment art."
			),
			T(
				"Sample Texture Filter: Bicubic Catmull-Rom\n"
				"Classic sharper bicubic reconstruction. Crisp, familiar, and cheaper than the heavier premium filters."
			),
			T(
				"Sample Texture Filter: Bicubic Mitchell\n"
				"Softer Mitchell-Netravali bicubic reconstruction. Smooth low-ringing option for UI and texture sheets."
			),
			T(
				"Sample Texture Filter: Nearest-Neighbor\n"
				"Uses the sample-filter path with nearest-neighbor sampling only. This affects eligible textures up to the "
				"selected size limit and leaves the global Sampler Override set to Default."
			)
		};
		if (config::SampleTextureFilter.get() == 14)
			config::SampleTextureFilter.set(0);
		int sampleTextureFilterSelection = getSampleTextureFilterOptionIndex(config::SampleTextureFilter.get());

		SettingsUI::PopupConfig sampleTextureFilterCfg {};
		sampleTextureFilterCfg.type = SettingsUI::PopupType::Options;
		sampleTextureFilterCfg.options.label = T("Sample Texture Filter");
		sampleTextureFilterCfg.options.icon = ICON_FA_FILTER;
		sampleTextureFilterCfg.options.popupID = Tnop("SampleTextureFilterPopup");
		sampleTextureFilterCfg.options.options = sampleTextureFilterLabels.data();
		sampleTextureFilterCfg.options.optionCount = static_cast<int>(sampleTextureFilterLabels.size());
		sampleTextureFilterCfg.options.currentValue = &sampleTextureFilterSelection;
		sampleTextureFilterCfg.options.valueWidth = 270.0f;
		sampleTextureFilterCfg.options.description = sampleTextureFilterDescriptions[sampleTextureFilterValues[sampleTextureFilterSelection]];
		sampleTextureFilterCfg.options.onOptionHighlight = [&](int selectedIndex, const char*) {
			if (selectedIndex >= 0 && selectedIndex < static_cast<int>(sampleTextureFilterValues.size()))
				SetSettingsFooterText(sampleTextureFilterDescriptions[sampleTextureFilterValues[selectedIndex]]);
		};
		sampleTextureFilterCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(sampleTextureFilterLabels.size()))
				return false;
			config::SampleTextureFilter.set(sampleTextureFilterValues[selectedIndex]);
			return true;
		};

		const bool sampleTextureFilterSupported = config::RendererType != RenderType::DirectX9;
		RenderGeneralPopupSettingRow(
			Tnop("SampleTextureFilterSetting"),
			T("Choose the sample-time texture filtering algorithm."),
			sampleTextureFilterCfg,
			sampleTextureFilterSupported
				? sampleTextureFilterDescriptions[sampleTextureFilterValues[sampleTextureFilterSelection]]
				: T("Sample-time filtering is not available on DirectX 9."),
			!sampleTextureFilterSupported);
		const auto maxFilteredTextureSizeLabels = getMaxFilteredTextureSizeLabels();
		int maxFilteredTextureSizeSelection = 5;
		for (size_t i = 0; i < maxFilteredTextureSizeValues.size(); i++)
		{
			if (maxFilteredTextureSizeValues[i] == config::MaxFilteredTextureSize.get())
			{
				maxFilteredTextureSizeSelection = static_cast<int>(i);
				break;
			}
		}

		SettingsUI::PopupConfig maxFilteredTextureSizeCfg {};
		maxFilteredTextureSizeCfg.type = SettingsUI::PopupType::Options;
		maxFilteredTextureSizeCfg.options.label = T("Max Filtered Texture Size");
		maxFilteredTextureSizeCfg.options.icon = ICON_FA_EXPAND;
		maxFilteredTextureSizeCfg.options.popupID = Tnop("MaxFilteredTextureSizePopup");
		maxFilteredTextureSizeCfg.options.options = maxFilteredTextureSizeLabels.data();
		maxFilteredTextureSizeCfg.options.optionCount = static_cast<int>(maxFilteredTextureSizeLabels.size());
		maxFilteredTextureSizeCfg.options.currentValue = &maxFilteredTextureSizeSelection;
		maxFilteredTextureSizeCfg.options.valueWidth = 170.0f;
		maxFilteredTextureSizeCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(maxFilteredTextureSizeValues.size()))
				return false;
			config::MaxFilteredTextureSize.set(maxFilteredTextureSizeValues[selectedIndex]);
			return true;
		};

		RenderGeneralPopupSettingRow(
			Tnop("MaxFilteredTextureSizeSetting"),
			T("Limit filtering to smaller texture groups."),
			maxFilteredTextureSizeCfg,
			T(
				"Max Filtered Texture Size\n"
				"Controls the largest texture group affected by texture upscaling and sample texture filters.\n"
				"Upscaling checks the original width. Sample filters check the uploaded width, including upscaling and replacement textures.\n"
				"Buckets use texture width, so 8x8 includes 8x16 and 8x32, while 16x16 includes 16x8.\n"
				"1024x1024+ includes larger texture-pack assets.\n\n"
				"Live preview updates sample-time filtering immediately. Already-uploaded texture-upscale results may require a cache refresh or game restart."
			));
		ImGui::Spacing();
		const bool canLiveTune = game_started && !ggpo::active() && (!emu.running() || achievements::canPause());
		if (!canLiveTune)
			ImGui::BeginDisabled();
		const char* liveTuneLabel = T("Texture Filtering Live Tune");
		const float liveTuneWidth = std::max(uiScaled(260.f),
			ImGui::CalcTextSize(liveTuneLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f);
		if (ImGui::Button(liveTuneLabel, ImVec2(liveTuneWidth, uiScaled(34.f))))
		{
			applyPendingSettings();
			GamepadDevice::load_system_mappings();
			closeShaderLiveTune();
			g_textureFilterLiveTune.store(true, std::memory_order_relaxed);
			// Resume through Loading when replacement textures need a refresh or preload.
			gui_resume_game();
		}
		if (!canLiveTune)
			ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", !game_started ? T("Start a game to tune texture filtering over its current frame.")
					: T("The current game must be pausable for live texture filtering."));

	}

#ifdef VIDEO_ROUTING
	// Video Routing section (platform-specific)
#ifdef __APPLE__
	if (RenderCollapsingHeader("VideoRoutingSection", ICON_FA_SHARE_NODES, T("Video Routing (Syphon)"), ImGuiTreeNodeFlags_DefaultOpen))
#elif defined(_WIN32)
	if (((renderApi == 0) || (renderApi == 3))
		? RenderCollapsingHeader("VideoRoutingSection", ICON_FA_SHARE_NODES, T("Video Routing (Spout)"), ImGuiTreeNodeFlags_DefaultOpen)
		: RenderCollapsingHeader("VideoRoutingSection", ICON_FA_SHARE_NODES, T("Video Routing (Only available with OpenGL or DirectX 11)"), ImGuiTreeNodeFlags_DefaultOpen))
#else
	if (RenderCollapsingHeader("VideoRoutingSection", ICON_FA_SHARE_NODES, T("Video Routing"), ImGuiTreeNodeFlags_DefaultOpen))
#endif
	{
#ifdef _WIN32
		DisabledScope routingScope(!((renderApi == 0) || (renderApi == 3)));
#endif

		// Send Video to Another Program
		RenderGeneralToggleSettingRow(
			"VideoRouting",
			ICON_FA_SHARE_NODES,
			T("Send Video to Another Program"),
			T("Stream GPU texture directly to OBS Studio"),
			static_cast<bool>(config::VideoRouting),
			[](bool enabled) { config::VideoRouting.set(enabled); },
			T(
				"Video Routing\n"
				"Routes a GPU texture directly to OBS Studio (or a compatible capture tool), avoiding CPU-intensive Display/Window Capture.\n"
				"This can improve capture quality and reduce overhead compared to capturing the emulator window."
			));

		{
			DisabledScope scope(!config::VideoRouting);

			// Scale Down Before Sending (Indented)
			ImGui::Indent();
			{
				RenderGeneralToggleSettingRow(
					"VideoRoutingScale",
					ICON_FA_COMPRESS,
					T("Scale Down Before Sending"),
					T("Share smaller texture for better performance"),
					static_cast<bool>(config::VideoRoutingScale),
					[](bool enabled) { config::VideoRoutingScale.set(enabled); },
					T(
						"Scale Down Before Sending\n"
						"Shares a smaller texture to reduce capture bandwidth and GPU/CPU overhead.\n"
						"Can improve performance on some systems, but results vary."
					));
			}

			ImGui::Unindent();


			{
				static int vres = config::VideoRoutingVRes;
				if (vres != config::VideoRoutingVRes)
					vres = config::VideoRoutingVRes;

				SettingsUI::PopupSliderConfig vresCfg {};
				vresCfg.label = T("Output Vertical Resolution");
				vresCfg.icon = ICON_FA_SLIDERS;
				vresCfg.popupID = Tnop("VideoRoutingVResPopup");
				vresCfg.description = T("Set the vertical resolution used for the shared OBS texture output.");
				vresCfg.currentValue = &vres;
				vresCfg.minValue = 120;
				vresCfg.maxValue = 2160;
				vresCfg.defaultValue = 720;
				vresCfg.format = "%d px";
				vresCfg.valueWidth = 220.0f;
				vresCfg.sliderWidth = 320.0f;
				vresCfg.onValueChange = [&]() {
					config::VideoRoutingVRes = vres;
				};

				SettingsUI::PopupConfig vresPopupCfg {};
				vresPopupCfg.type = SettingsUI::PopupType::Slider;
				vresPopupCfg.slider = vresCfg;

				RenderGeneralPopupSettingRow(
					Tnop("VideoRoutingVResSetting"),
					T("Set vertical resolution for the shared output texture."),
					vresPopupCfg,
					T(
						"Output Vertical Resolution\n"
						"Sets the vertical resolution used for the shared texture routing output.\n"
						"Lower values reduce bandwidth and overhead. Higher values improve capture clarity."
					),
					!config::VideoRoutingScale);

				config::VideoRoutingVRes = vres;
			}

			const int outputWidth = config::VideoRoutingScale
				? config::VideoRoutingVRes * settings.display.width / settings.display.height
				: settings.display.width;
			const int outputHeight = config::VideoRoutingScale
				? config::VideoRoutingVRes
				: settings.display.height;
			const std::string outputSizeText = std::to_string(outputWidth) + " x " + std::to_string(outputHeight);

			ImGui::PushID("VideoRoutingOutputTextureSize");
				BeginTwoLineSettingRow(
					"##row",
					T(
						"Output Texture Size\n"
						"Shows the calculated size of the shared output texture based on your routing settings."
					),
					true);
			const ImVec2 line1Start = BeginTwoLineSettingRowContent();

			SettingIcon(ICON_FA_EXPAND, SettingsLayoutSize(20.0f, 20.0f));
			ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
			ImGui::PushFont(largeFont);
			ImGui::TextUnformatted(T("Output Texture Size"));
			ImGui::PopFont();

			const float valueWidth = SettingsLayoutScaled(220.0f);
			ImGui::SameLine(RightColumnX(valueWidth));
			const float valueStartX = ImGui::GetCursorPosX();
			const float valueTextWidth = ImGui::CalcTextSize(outputSizeText.c_str()).x;
			ImGui::SetCursorPosX(valueStartX + std::max(0.0f, (valueWidth - valueTextWidth) * 0.5f));
			ImGui::PushFont(largeFont);
			ImGui::TextUnformatted(outputSizeText.c_str());
			ImGui::PopFont();

			RenderTwoLineSettingDescription(line1Start, T("Calculated size of the shared output texture."));
			EndTwoLineSettingRow();
			ImGui::PopID();
		}
	}
#endif

	// Changing Hikaru sorting must not overwrite Dreamcast/Naomi's OIT choice.
	// The API remains a shared preference; the sorting modes are independent.
	const bool hostPerPixel = hikaruPlatform ? isPerPixel(config::RendererType) : perPixel;
	switch (renderApi)
	{
	case 0:
		config::RendererType = hostPerPixel ? RenderType::OpenGL_OIT : RenderType::OpenGL;
		break;
	case 1:
		config::RendererType = hostPerPixel ? RenderType::Vulkan_OIT : RenderType::Vulkan;
		break;
	case 4:
		config::RendererType = hostPerPixel ? RenderType::Metal_OIT : RenderType::Metal;
		break;
	case 2:
		config::RendererType = RenderType::DirectX9;
		break;
		case 3:
			config::RendererType = hostPerPixel ? RenderType::DirectX11_OIT : RenderType::DirectX11;
			break;
	}
}

} // namespace SettingsNew
