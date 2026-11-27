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

void renderAudioTab()
{
	ScopedTwoLineRowStyle audioRowStyle(12.0f, true, 0.5f);


	// Audio Playback Section
	if (RenderCollapsingHeader("PlaybackSection", ICON_FA_VOLUME_HIGH, T("Playback"), ImGuiTreeNodeFlags_DefaultOpen))
	{
	#ifdef USE_HIKARU
		if (settings.platform.isHikaru())
			RenderGeneralToggleSettingRow(
				"HikaruAudioPacing", ICON_FA_VOLUME_HIGH,
				T("Match audio to game speed"), T("Keep pitch steady when the game runs slowly"),
				static_cast<bool>(config::HikaruAudioPacing),
				[](bool enabled) { config::HikaruAudioPacing.set(enabled); },
				T("Match audio to game speed\nWhen the game runs slowly, slows audio to match while keeping its pitch. Adds some audio latency."));
	#endif

		RenderGeneralToggleSettingRow(
			"EnableDSP",
			ICON_FA_SLIDERS,
			T("Enable DSP"),
			T("Dreamcast Digital Sound Processor"),
			static_cast<bool>(config::DSPEnabled),
			[](bool enabled) { config::DSPEnabled.set(enabled); },
			T(
				"Enable DSP\n"
				"Enables the Dreamcast Digital Sound Processor.\n"
				"Only recommended on fast platforms. If you experience audio stutter or performance drops, try disabling this."
			));

		RenderGeneralToggleSettingRow(
			"EnableVmuSounds",
			ICON_FA_BELL,
			T("VMU Sounds"),
			T("Play VMU beeps when enabled"),
			static_cast<bool>(config::VmuSound),
			[](bool enabled) { config::VmuSound.set(enabled); },
			T(
				"VMU Sounds\n"
				"Plays VMU beep sounds when enabled.\n"
				"Purely cosmetic; disable if you prefer silence or want to avoid extra audio events."
			));

	// Volume Level
	{
		static int volumeTemp = config::AudioVolume.get();

		SettingsUI::PopupSliderConfig volumeConfig {};
		volumeConfig.label = T("Volume Level");
		volumeConfig.icon = ICON_FA_VOLUME_HIGH;
		volumeConfig.tooltip = T("Adjust the emulator's audio level");
		volumeConfig.popupID = Tnop("VolumeLevelPopup");
		volumeConfig.description = T("Adjust the emulator's audio level");
		volumeConfig.currentValue = &volumeTemp;
		volumeConfig.minValue = 0;
		volumeConfig.maxValue = 100;
		volumeConfig.defaultValue = 100;
		volumeConfig.format = "%d%%";
		volumeConfig.applyButtonText = T("Apply");
		volumeConfig.onApply = nullptr;
		volumeConfig.onValueChange = []() {
			config::AudioVolume.set(volumeTemp);
			config::AudioVolume.calcDbPower();
		};
		volumeConfig.iconSize = 20.0f;
		volumeConfig.iconSpacing = 8.0f;
		volumeConfig.valueWidth = 220.0f;
		volumeConfig.sliderWidth = 320.0f;
		volumeConfig.showApplyFlag = nullptr;
		volumeConfig.hasPendingChanges = false;

		SettingsUI::PopupConfig volumePopupCfg {};
		volumePopupCfg.type = SettingsUI::PopupType::Slider;
		volumePopupCfg.slider = volumeConfig;

			RenderGeneralPopupSettingRow(
				Tnop("VolumeLevelSetting"),
				T("Master audio volume."),
				volumePopupCfg,
				T(
					"Volume Level\n"
					"Adjusts the emulator's master audio volume.\n"
					"If audio clips or is too quiet, tune this before changing latency settings."
				));
	}

	}

	const bool showAudioLatency = RenderCollapsingHeader("AudioLatencySection", ICON_FA_CLOCK, T("Audio Latency"), ImGuiTreeNodeFlags_DefaultOpen);
	if (showAudioLatency)
	{
#ifdef __ANDROID__
		if (config::AudioBackend.get() == "auto" || config::AudioBackend.get() == "android")
	{
			RenderGeneralToggleSettingRow(
				"AutoLatency",
				ICON_FA_WAND_MAGIC,
				T("Automatic Latency"),
				T("Automatically set audio latency"),
				static_cast<bool>(config::AutoLatency),
				[](bool enabled) { config::AutoLatency.set(enabled); },
				T(
					"Automatic Latency\n"
					"Automatically chooses an audio buffer size.\n"
					"Recommended for most users. Disable only if you need to manually tune latency vs stability."
				));
	}
#endif

	if (!config::AutoLatency
		|| (config::AudioBackend.get() != "auto" && config::AudioBackend.get() != "android"))
	{
		static int latencyTemp = (int)roundf(config::AudioBufferSize.get() * 1000.f / audio::OutputSampleRate);
		static int openingAudioBufferSize = 0;

		SettingsUI::PopupSliderConfig latencyConfig {};
		latencyConfig.label = T("Audio Latency");
		latencyConfig.icon = ICON_FA_STOPWATCH;
		latencyConfig.tooltip = T("Buffer size in milliseconds");
		latencyConfig.popupID = Tnop("AudioLatencyPopup");
		latencyConfig.description = T("Lower values reduce audio lag but may cause audio issues. "
		                            "Higher values are more stable but increase latency.");
		latencyConfig.currentValue = &latencyTemp;
		latencyConfig.minValue = 12;
		latencyConfig.maxValue = 512;
		latencyConfig.defaultValue = 64;
		latencyConfig.format = "%d ms";
		latencyConfig.applyButtonText = T("Apply");
		latencyConfig.onApply = nullptr;
		latencyConfig.onOpen = []() {
			openingAudioBufferSize = config::AudioBufferSize.get();
			latencyTemp = (int)roundf(openingAudioBufferSize * 1000.f / audio::OutputSampleRate);
		};
		latencyConfig.onCancel = []() {
			// Milliseconds round the sample count. Restore the exact original
			// samples rather than round-tripping through the displayed value.
			config::AudioBufferSize.set(openingAudioBufferSize);
		};
		latencyConfig.onValueChange = []() {
			config::AudioBufferSize.set((int)roundf(static_cast<float>(latencyTemp) * audio::OutputSampleRate / 1000.f));
		};
		latencyConfig.iconSize = 20.0f;
		latencyConfig.iconSpacing = 8.0f;
		latencyConfig.valueWidth = 220.0f;
		latencyConfig.sliderWidth = 320.0f;
		latencyConfig.showApplyFlag = nullptr;
		latencyConfig.hasPendingChanges = false;

		SettingsUI::PopupConfig latencyPopupCfg {};
		latencyPopupCfg.type = SettingsUI::PopupType::Slider;
		latencyPopupCfg.slider = latencyConfig;

			RenderGeneralPopupSettingRow(
				Tnop("AudioLatencySetting"),
				T("Audio buffer size in milliseconds."),
				latencyPopupCfg,
				T(
					"Audio Latency\n"
					"Lower values reduce audio lag but can cause crackling/stutter if your system cannot keep up.\n"
					"Higher values are more stable but add latency.\n\n"
					"If you hear pops, increase latency. If audio feels delayed, decrease it carefully."
				));
		}
	}

		// Audio Driver Selection
	if (RenderCollapsingHeader("AudioDriverSection", ICON_FA_HEADPHONES, T("Audio Driver"), ImGuiTreeNodeFlags_DefaultOpen))
	{

		// Get current backend name for display
		AudioBackend *currentBackend = AudioBackend::getBackend(config::AudioBackend.get());
		std::string currentBackendName = currentBackend ? currentBackend->getName() : T("Unknown");

		// Build dynamic options list
		int backendCount = (int)AudioBackend::getCount();
		std::vector<std::string> backendLabels(backendCount);
		for (int i = 0; i < backendCount; i++)
		{
			const AudioBackend* backend = AudioBackend::getBackend(i);
			backendLabels[i] = backend->slug + " - " + backend->getName();
		}

		std::vector<const char*> backendOptions(backendCount);
		for (int i = 0; i < backendCount; i++)
			backendOptions[i] = backendLabels[i].c_str();

		// Find current index
		int currentBackendIndex = 0;
		for (int i = 0; i < backendCount; i++)
		{
			if (config::AudioBackend.get() == AudioBackend::getBackend(i)->slug)
				{
				currentBackendIndex = (int)i;
				break;
			}
		}

		// Static storage for selected index
		static int selectedBackendIndex = currentBackendIndex;

		SettingsUI::PopupOptionsConfig driverConfig {};
		driverConfig.label = T("Audio Driver");
		driverConfig.icon = ICON_FA_HEADPHONES;
		driverConfig.popupID = Tnop("AudioDriverPopup");
		driverConfig.options = backendOptions.data();
		driverConfig.optionCount = backendCount;
		driverConfig.currentValue = &selectedBackendIndex;
		driverConfig.valueToString = nullptr;  // Use options array directly
		driverConfig.storageIndexMap = nullptr;
		driverConfig.onChange = [](int selectedIndex) {
			const AudioBackend* backend = AudioBackend::getBackend(selectedIndex);
			if (backend)
			{
				config::AudioBackend.set(backend->slug);
				selectedBackendIndex = selectedIndex;
			}
			return true;
		};
		driverConfig.disabled = false;
		driverConfig.iconSize = 20.0f;
		driverConfig.iconSpacing = 8.0f;
		driverConfig.valueWidth = 220.0f;

		SettingsUI::PopupConfig driverPopupCfg {};
		driverPopupCfg.type = SettingsUI::PopupType::Options;
		driverPopupCfg.options = driverConfig;

			RenderGeneralPopupSettingRow(
				Tnop("AudioDriverSetting"),
				T("Select the host audio backend/driver."),
				driverPopupCfg,
				T(
					"Audio Driver\n"
					"Selects the audio backend used to submit sound to your OS.\n"
					"If you have crackling, missing audio, or unusually high latency, try changing the driver.\n\n"
					"After changing drivers, re-check Audio Latency settings."
				));

		AudioBackend *current_backend = currentBackend;

		// Backend-specific options
		if (current_backend != nullptr)
		{
			// Get backend specific options
			int option_count;
			const AudioBackend::Option *options = current_backend->getOptions(&option_count);

			if (option_count > 0 && RenderCollapsingHeader("DriverOptionsSection", ICON_FA_SLIDERS, T("Driver Options"), ImGuiTreeNodeFlags_DefaultOpen))
			{
				for (int o = 0; o < option_count; o++)
				{
					std::string value = cfgLoadStr(current_backend->slug, options->name, "");

					if (options->type == AudioBackend::Option::integer)
					{
						static std::unordered_map<std::string, int> g_audioBackendIntValues;
						const std::string optionKey = current_backend->slug + "." + options->name;
						const std::string popupId = Tnop("AudioBackendInt_") + optionKey;
						const int cfgVal = stoi(value);

						if (!ImGui::IsPopupOpen(popupId.c_str()))
							g_audioBackendIntValues[optionKey] = cfgVal;

						int& tempVal = g_audioBackendIntValues[optionKey];

						SettingsUI::PopupSliderConfig backendIntCfg {};
						backendIntCfg.label = options->caption.c_str();
						backendIntCfg.icon = ICON_FA_SLIDERS;
						backendIntCfg.popupID = popupId.c_str();
						backendIntCfg.description = options->caption.c_str();
						backendIntCfg.currentValue = &tempVal;
						backendIntCfg.minValue = options->minValue;
						backendIntCfg.maxValue = options->maxValue;
						backendIntCfg.defaultValue = options->minValue;
						backendIntCfg.format = "%d";
						backendIntCfg.valueWidth = 220.0f;
						backendIntCfg.sliderWidth = 320.0f;
						backendIntCfg.onValueChange = [&]() {
							cfgSaveStr(current_backend->slug, options->name, std::to_string(tempVal));
						};

						SettingsUI::PopupConfig backendIntPopupCfg {};
						backendIntPopupCfg.type = SettingsUI::PopupType::Slider;
						backendIntPopupCfg.slider = backendIntCfg;

						RenderGeneralPopupSettingRow(
							popupId.c_str(),
							options->caption.c_str(),
							backendIntPopupCfg);
					}
					else if (options->type == AudioBackend::Option::checkbox)
					{
						bool check = value == "1";
						const std::string optionName = options->name;
						const std::string rowId = "AudioBackendOption_" + optionName;
						RenderGeneralToggleSettingRow(
							rowId.c_str(),
							ICON_FA_CHECK,
							options->caption.c_str(),
							"",
							check,
							[&, optionName](bool enabled) {
								cfgSaveStr(current_backend->slug, optionName, enabled ? "1" : "0");
							},
							options->caption.c_str());
					}
					else if (options->type == AudioBackend::Option::list)
					{
						std::vector<const char*> listOptions;
						std::vector<std::string> listLabels;
						listLabels.reserve(options->values.size());
						listOptions.reserve(options->values.size());
						for (const auto& cur : options->values)
						{
							listLabels.push_back(cur);
							listOptions.push_back(listLabels.back().c_str());
						}

						int currentIndex = 0;
						for (size_t idx = 0; idx < options->values.size(); idx++)
						{
							if (options->values[idx] == value)
							{
								currentIndex = static_cast<int>(idx);
								break;
							}
						}

						static std::unordered_map<std::string, int> g_audioBackendListIndex;
						const std::string optionKey = current_backend->slug + "." + options->name;
						const std::string popupId = Tnop("AudioBackendList_") + optionKey;
						if (!ImGui::IsPopupOpen(popupId.c_str()))
							g_audioBackendListIndex[optionKey] = currentIndex;
						int& tempIndex = g_audioBackendListIndex[optionKey];

						SettingsUI::PopupOptionsConfig listCfg {};
						listCfg.label = options->caption.c_str();
						listCfg.icon = ICON_FA_LIST;
						listCfg.popupID = popupId.c_str();
						listCfg.options = listOptions.data();
						listCfg.optionCount = static_cast<int>(listOptions.size());
						listCfg.currentValue = &tempIndex;
						listCfg.valueWidth = 220.0f;
						listCfg.onChange = [&](int selectedIndex) {
							if (selectedIndex < 0 || selectedIndex >= static_cast<int>(options->values.size()))
								return false;
							cfgSaveStr(current_backend->slug, options->name, options->values[selectedIndex]);
							return true;
						};

						SettingsUI::PopupConfig listPopupCfg {};
						listPopupCfg.type = SettingsUI::PopupType::Options;
						listPopupCfg.options = listCfg;

						RenderGeneralPopupSettingRow(
							popupId.c_str(),
							options->caption.c_str(),
							listPopupCfg);
					}
					else
					{
						WARN_LOG(RENDERER, "Unknown audio backend option type");
					}

					options++;
				}
			}
		}
	}
}


} // namespace SettingsNew
