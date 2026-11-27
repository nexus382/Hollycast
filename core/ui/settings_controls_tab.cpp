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

static std::shared_ptr<GamepadDevice> g_currentGamepadForSettings;

static void gamepadSettingsPopup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	centerNextWindow();
	ImGui::SetNextWindowSize(min(ImGui::GetIO().DisplaySize, ScaledVec2(450.f, 300.f)));

	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	const std::string gamepadSettingsTitle = std::string(T("Gamepad Settings")) + "###Gamepad Settings";
	if (ImGui::BeginPopupModal(gamepadSettingsTitle.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_DragScrolling))
	{
		static int openingRumble = 0;
		static float openingDeadzone = 0.0f;
		static float openingSaturation = 1.0f;
		if (ImGui::IsWindowAppearing())
		{
			openingRumble = gamepad->get_rumble_power();
			if (gamepad->has_analog_stick())
			{
				openingDeadzone = gamepad->get_dead_zone();
				openingSaturation = gamepad->get_saturation();
			}
		}
		const bool done = ImGui::Button(T("Done"), ScaledVec2(100, 30));
		ImGui::SameLine();
		if (ImGui::Button(T("Cancel"), ScaledVec2(100, 30)) || ControllerPopupCancelPressed()
				|| ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			gamepad->set_rumble_power(openingRumble);
			if (gamepad->has_analog_stick())
			{
				gamepad->set_dead_zone(openingDeadzone);
				gamepad->set_saturation(openingSaturation);
			}
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		if (done)
		{
			gamepad->save_mapping();
			// Update both console and arcade profile/mapping
			int rumblePower = gamepad->get_rumble_power();
			float deadzone = gamepad->get_dead_zone();
			float saturation = gamepad->get_saturation();
			int otherPlatform = settings.platform.isConsole() ? DC_PLATFORM_NAOMI : DC_PLATFORM_DREAMCAST;
			if (!gamepad->find_mapping(otherPlatform))
				if (otherPlatform == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
					gamepad->resetMappingToDefault(otherPlatform != DC_PLATFORM_DREAMCAST, true);
			std::shared_ptr<InputMapping> mapping = gamepad->get_input_mapping();
			if (mapping != nullptr)
			{
				if (gamepad->is_rumble_enabled() && rumblePower != mapping->rumblePower) {
					mapping->rumblePower = rumblePower;
					mapping->set_dirty();
				}
				if (gamepad->has_analog_stick())
				{
					if (deadzone != mapping->dead_zone) {
						mapping->dead_zone = deadzone;
						mapping->set_dirty();
					}
					if (saturation != mapping->saturation) {
						mapping->saturation = saturation;
						mapping->set_dirty();
					}
				}
				if (mapping->is_dirty())
					gamepad->save_mapping(otherPlatform);
			}
			gamepad->find_mapping();

			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		ImGui::NewLine();
		if (gamepad->has_force_feedback() || gamepad->is_rumble_enabled())
		{
			header(gamepad->has_force_feedback() ? T("Force Feedback") : T("Rumble"));
			int power = gamepad->get_rumble_power();
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			const bool powerChanged = allowSettingsTouchDrag(ImGui::SliderInt(T("Power"), &power, 0, 100, "%d%%"));
#ifdef HOLLYCAST_TEST_HARNESS
			if (test_harness::ui::collectingInspection())
				annotateTestInteger("controls.gamepad", (gamepad->unique_id() + ".rumble_power").c_str(), T("Power"), power, 0, 100);
#endif
			if (powerChanged)
				gamepad->set_rumble_power(power);
			ImGui::SameLine();
			ShowFooterHelpMarker(gamepad->has_force_feedback() ? T("Force-feedback strength") : T("Rumble power"));
		}
		if (gamepad->has_analog_stick())
		{
			header(T("Thumbsticks"));
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			const bool deadzoneChanged = allowSettingsTouchDrag(ImGui::SliderInt(T("Dead zone"), &deadzone, 0, 100, "%d%%"));
#ifdef HOLLYCAST_TEST_HARNESS
			if (test_harness::ui::collectingInspection())
				annotateTestInteger("controls.gamepad", (gamepad->unique_id() + ".dead_zone").c_str(), T("Dead zone"), deadzone, 0, 100);
#endif
			if (deadzoneChanged)
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowFooterHelpMarker(T("Minimum deflection to register as input"));
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			const bool saturationChanged = allowSettingsTouchDrag(ImGui::SliderInt(T("Saturation"), &saturation, 50, 200, "%d%%"));
#ifdef HOLLYCAST_TEST_HARNESS
			if (test_harness::ui::collectingInspection())
				annotateTestInteger("controls.gamepad", (gamepad->unique_id() + ".saturation").c_str(), T("Saturation"), saturation, 50, 200);
#endif
			if (saturationChanged)
				gamepad->set_saturation(saturation / 100.f);
			ImGui::SameLine();
			ShowFooterHelpMarker(T("Value sent to the game at 100% thumbstick deflection. "
					"Values greater than 100% will saturate before full deflection of the thumbstick."));
		}
		scrollWhenDraggingOnVoid();
		windowDragScroll();
		ImGui::EndPopup();
	}
}

// ============================================================
// CONTROLLER MAPPING - Static state
// ============================================================
static std::shared_ptr<GamepadDevice> g_currentGamepadForMapping;
static InputMapping::InputSet g_mapped_codes;  // Stores multiple buttons in the order they were entered
static u64 g_map_start_time;
static bool g_arcade_button_mode;
static u32 g_gamepad_port_for_mapping;
static std::unordered_set<DreamcastKey> g_buttonState;

static const char* const kMaplePorts[] = { Tnop("None"), "A", "B", "C", "D", Tnop("All") };

struct MappingEntry
{
	DreamcastKey key;
	const char* name;
};

static constexpr MappingEntry dcButtons_src[] = {
	{ EMU_BTN_NONE, Tnop("Directions") },
	{ DC_DPAD_UP, Tnop("Up") },
	{ DC_DPAD_DOWN, Tnop("Down") },
	{ DC_DPAD_LEFT, Tnop("Left") },
	{ DC_DPAD_RIGHT, Tnop("Right") },

	{ DC_AXIS_UP, Tnop("Thumbstick Up") },
	{ DC_AXIS_DOWN, Tnop("Thumbstick Down") },
	{ DC_AXIS_LEFT, Tnop("Thumbstick Left") },
	{ DC_AXIS_RIGHT, Tnop("Thumbstick Right") },

	{ DC_AXIS2_UP, Tnop("R.Thumbstick Up") },
	{ DC_AXIS2_DOWN, Tnop("R.Thumbstick Down") },
	{ DC_AXIS2_LEFT, Tnop("R.Thumbstick Left") },
	{ DC_AXIS2_RIGHT, Tnop("R.Thumbstick Right") },

	{ DC_AXIS3_UP, Tnop("Axis 3 Up") },
	{ DC_AXIS3_DOWN, Tnop("Axis 3 Down") },
	{ DC_AXIS3_LEFT, Tnop("Axis 3 Left") },
	{ DC_AXIS3_RIGHT, Tnop("Axis 3 Right") },

	{ DC_DPAD2_UP, Tnop("DPad2 Up") },
	{ DC_DPAD2_DOWN, Tnop("DPad2 Down") },
	{ DC_DPAD2_LEFT, Tnop("DPad2 Left") },
	{ DC_DPAD2_RIGHT, Tnop("DPad2 Right") },

	{ EMU_BTN_NONE, Tnop("Buttons") },
	{ DC_BTN_A, "A" },
	{ DC_BTN_B, "B" },
	{ DC_BTN_X, "X" },
	{ DC_BTN_Y, "Y" },
	{ DC_BTN_C, "C" },
	{ DC_BTN_D, "D" },
	{ DC_BTN_Z, "Z" },

	{ EMU_BTN_NONE, Tnop("Triggers") },
	{ DC_AXIS_LT, Tnop("Left Trigger") },
	{ DC_AXIS_RT, Tnop("Right Trigger") },
	{ DC_AXIS_LT2, Tnop("Left Trigger 2") },
	{ DC_AXIS_RT2, Tnop("Right Trigger 2") },

	{ EMU_BTN_NONE, Tnop("System Buttons") },
	{ DC_BTN_START, Tnop("Start") },
	{ DC_BTN_RELOAD, Tnop("Reload") },

	{ EMU_BTN_NONE, Tnop("Emulator") },
	{ EMU_BTN_MENU, Tnop("Menu") },
	{ EMU_BTN_ESCAPE, Tnop("Exit") },
	{ EMU_BTN_PAUSE, Tnop("Pause") },
	{ EMU_BTN_FFORWARD, Tnop("Fast-forward") },
	{ EMU_BTN_REWIND, Tnop("Rewind (hold)") },
	{ EMU_BTN_LOADSTATE, Tnop("Load State") },
	{ EMU_BTN_SAVESTATE, Tnop("Save State") },
	{ EMU_BTN_LOADSTATE_RAM, Tnop("Load State in RAM") },
	{ EMU_BTN_SAVESTATE_RAM, Tnop("Save State in RAM") },
	{ EMU_BTN_BYPASS_KB, Tnop("Bypass Emulated Keyboard") },
	{ EMU_BTN_SCREENSHOT, Tnop("Save Screenshot") },

	{ EMU_BTN_NONE, nullptr }
};
static MappingEntry dcButtons[std::size(dcButtons_src)];

static constexpr MappingEntry arcadeButtons_src[] = {
	{ EMU_BTN_NONE, Tnop("Directions") },
	{ DC_DPAD_UP, Tnop("Up") },
	{ DC_DPAD_DOWN, Tnop("Down") },
	{ DC_DPAD_LEFT, Tnop("Left") },
	{ DC_DPAD_RIGHT, Tnop("Right") },

	{ DC_AXIS_UP, Tnop("Thumbstick Up") },
	{ DC_AXIS_DOWN, Tnop("Thumbstick Down") },
	{ DC_AXIS_LEFT, Tnop("Thumbstick Left") },
	{ DC_AXIS_RIGHT, Tnop("Thumbstick Right") },

	{ DC_AXIS2_UP, Tnop("R.Thumbstick Up") },
	{ DC_AXIS2_DOWN, Tnop("R.Thumbstick Down") },
	{ DC_AXIS2_LEFT, Tnop("R.Thumbstick Left") },
	{ DC_AXIS2_RIGHT, Tnop("R.Thumbstick Right") },

	{ EMU_BTN_NONE, Tnop("Buttons") },
	{ DC_BTN_A, Tnop("Button 1") },
	{ DC_BTN_B, Tnop("Button 2") },
	{ DC_BTN_C, Tnop("Button 3") },
	{ DC_BTN_X, Tnop("Button 4") },
	{ DC_BTN_Y, Tnop("Button 5") },
	{ DC_BTN_Z, Tnop("Button 6") },
	{ DC_DPAD2_LEFT, Tnop("Button 7") },
	{ DC_DPAD2_RIGHT, Tnop("Button 8") },

	{ EMU_BTN_NONE, Tnop("Triggers") },
	{ DC_AXIS_LT, Tnop("Left Trigger") },
	{ DC_AXIS_RT, Tnop("Right Trigger") },
	{ DC_AXIS_LT2, Tnop("Left Trigger 2") },
	{ DC_AXIS_RT2, Tnop("Right Trigger 2") },

	{ EMU_BTN_NONE, Tnop("System Buttons") },
	{ DC_BTN_START, Tnop("Start") },
	{ DC_BTN_RELOAD, Tnop("Reload") },
	{ DC_BTN_D, Tnop("Coin") },
	{ DC_DPAD2_UP, Tnop("Service") },
	{ DC_DPAD2_DOWN, Tnop("Test") },
	{ DC_BTN_INSERT_CARD, Tnop("Insert Card") },

	{ EMU_BTN_NONE, Tnop("Emulator") },
	{ EMU_BTN_MENU, Tnop("Menu") },
	{ EMU_BTN_ESCAPE, Tnop("Exit") },
	{ EMU_BTN_PAUSE, Tnop("Pause") },
	{ EMU_BTN_FFORWARD, Tnop("Fast-forward") },
	{ EMU_BTN_REWIND, Tnop("Rewind (hold)") },
	{ EMU_BTN_LOADSTATE, Tnop("Load State") },
	{ EMU_BTN_SAVESTATE, Tnop("Save State") },
	{ EMU_BTN_LOADSTATE_RAM, Tnop("Load State in RAM") },
	{ EMU_BTN_SAVESTATE_RAM, Tnop("Save State in RAM") },
	{ EMU_BTN_BYPASS_KB, Tnop("Bypass Emulated Keyboard") },
	{ EMU_BTN_SCREENSHOT, Tnop("Save Screenshot") },

	{ EMU_BTN_NONE, nullptr }
};
static MappingEntry arcadeButtons[std::size(arcadeButtons_src)];

// Device type arrays for dropdowns
static constexpr const char *maple_device_types_src[] =
{
	Tnop("None"),
	Tnop("Sega Controller"),
	Tnop("Light Gun"),
	Tnop("Keyboard"),
	Tnop("Mouse"),
	Tnop("Twin Stick"),
	Tnop("Arcade/Ascii Stick"),
	Tnop("Maracas Controller"),
	Tnop("Fishing Controller"),
	Tnop("Pop'n Music controller"),
	Tnop("Racing Controller"),
	Tnop("Densha de Go! Controller"),
	Tnop("Panther DC/Full Controller"),
};
static const char *maple_device_types[std::size(maple_device_types_src)];

static constexpr const char *maple_ports_src[] = { Tnop("None"), "A", "B", "C", "D", Tnop("All") };
static const char *maple_ports[std::size(maple_ports_src)];

namespace {

class UILanguageChangeHandler
{
public:
	UILanguageChangeHandler() {
		EventManager::listen(Event::LocaleChange, emuEvent);
	}
	~UILanguageChangeHandler() {
		EventManager::unlisten(Event::LocaleChange, emuEvent);
	}

	static void init()
	{
		if (inited)
			return;
		inited = true;
		memcpy(maple_device_types, maple_device_types_src, sizeof(maple_device_types));
		for (auto& label : maple_device_types)
			label = (char *)T(label);
		memcpy(maple_ports, maple_ports_src, sizeof(maple_ports));
		maple_ports[0] = (char *)T(maple_ports[0]);
		maple_ports[5] = (char *)T(maple_ports[5]);
		memcpy(dcButtons, dcButtons_src, sizeof(dcButtons));
		for (auto&  button : dcButtons)
			button.name = (char *)T(button.name);
		memcpy(arcadeButtons, arcadeButtons_src, sizeof(arcadeButtons));
		for (auto&  button : arcadeButtons)
			button.name = (char *)T(button.name);
	}

private:
	static void emuEvent(Event event, void *arg)
	{
		if (inited) {
			inited = false;
			init();
		}
	}
	static bool inited;
};
bool UILanguageChangeHandler::inited;
static UILanguageChangeHandler uiLanguageHandler;

}

static void buttonListener(int port, DreamcastKey key, bool pressed);

static constexpr std::array<int, 2> kMappingSystems { DC_PLATFORM_DREAMCAST, DC_PLATFORM_NAOMI };
static std::array<std::optional<GamepadDevice::MappingDraft>, kMappingSystems.size()> g_mappingDrafts;
static bool g_deletePerGameMapping = false;
static bool g_mappingSaveFailed = false;
static std::vector<std::pair<int, bool>> g_savedPerGameDrafts;

static void ResetControllerMappingRuntime()
{
	if (g_currentGamepadForMapping != nullptr)
	{
		g_currentGamepadForMapping->cancel_detect_input();
		g_currentGamepadForMapping->unlistenButtons(buttonListener);
		g_currentGamepadForMapping.reset();
	}
	g_mapped_codes.clear();
	g_buttonState.clear();
	g_mappingDrafts = {};
	g_deletePerGameMapping = false;
	g_mappingSaveFailed = false;
	g_savedPerGameDrafts.clear();
}

static void CloseDeviceSettingsPopup(const char* name)
{
	const ImGuiID windowId = ImHashStr(name);
	const auto& popups = GImGui->OpenPopupStack;
	for (int level = 0; level < popups.Size; ++level)
	{
		if (popups[level].Window != nullptr && popups[level].Window->ID == windowId)
		{
			ImGui::ClosePopupToLevel(level, true);
			break;
		}
	}
}

static void unmapControl(const std::shared_ptr<InputMapping>& mapping, u32 gamepad_port, DreamcastKey key)
{
	mapping->clear_button(gamepad_port, key);
	mapping->clear_axis(gamepad_port, key);
	g_buttonState.erase(key);
}

static DreamcastKey getOppositeDirectionKey(DreamcastKey key)
{
	switch (key)
	{
	case DC_DPAD_UP: return DC_DPAD_DOWN;
	case DC_DPAD_DOWN: return DC_DPAD_UP;
	case DC_DPAD_LEFT: return DC_DPAD_RIGHT;
	case DC_DPAD_RIGHT: return DC_DPAD_LEFT;
	case DC_DPAD2_UP: return DC_DPAD2_DOWN;
	case DC_DPAD2_DOWN: return DC_DPAD2_UP;
	case DC_DPAD2_LEFT: return DC_DPAD2_RIGHT;
	case DC_DPAD2_RIGHT: return DC_DPAD2_LEFT;
	case DC_AXIS_UP: return DC_AXIS_DOWN;
	case DC_AXIS_DOWN: return DC_AXIS_UP;
	case DC_AXIS_LEFT: return DC_AXIS_RIGHT;
	case DC_AXIS_RIGHT: return DC_AXIS_LEFT;
	case DC_AXIS2_UP: return DC_AXIS2_DOWN;
	case DC_AXIS2_DOWN: return DC_AXIS2_UP;
	case DC_AXIS2_LEFT: return DC_AXIS2_RIGHT;
	case DC_AXIS2_RIGHT: return DC_AXIS2_LEFT;
	case DC_AXIS3_UP: return DC_AXIS3_DOWN;
	case DC_AXIS3_DOWN: return DC_AXIS3_UP;
	case DC_AXIS3_LEFT: return DC_AXIS3_RIGHT;
	case DC_AXIS3_RIGHT: return DC_AXIS3_LEFT;
	default: return EMU_BTN_NONE;
	}
}

static void displayLabelOrCode(const char* label, u32 code, const char* suffix = "")
{
	if (label != nullptr)
		ImGui::Text("%s%s", label, suffix);
	else
		ImGui::Text("[%d]%s", code, suffix);
}

static void detect_input_popup(const MappingEntry* mapping, const std::shared_ptr<InputMapping>& input_mapping)
{
	ImVec2 padding = ScaledVec2(20, 20);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
	ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
	const std::string mapControlTitle = std::string(T("Map Control")) + "###Map Control";
	if (ImGui::BeginPopupModal(mapControlTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text(T("Waiting for control '%s'..."), mapping->name);
		// Gamepad B remains available as an input to map here.
		if (ImGui::Button(T("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			g_currentGamepadForMapping->cancel_detect_input();
			g_mapped_codes.clear();
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		u64 now = getTimeMs();

		if (now >= g_map_start_time)
		{
			bool still_detecting = g_currentGamepadForMapping != nullptr && g_currentGamepadForMapping->is_input_detecting();
			int remaining = still_detecting ? static_cast<int>(5 - (now - g_map_start_time) / 1000) : 0;
			if (remaining < 0)
				remaining = 5;

			if (still_detecting)
				ImGui::Text(T("Time out in %d s"), remaining);

			if (!g_mapped_codes.empty())
			{
				ImGui::Text("%s", T("Current inputs: "));
				ImGui::SameLine();
				bool first = true;
				for (const InputMapping::InputDef& inputDef : g_mapped_codes)
				{
					if (!first)
					{
						ImGui::SameLine();
						ImGui::Text("&");
						ImGui::SameLine();
					}

					const char* name = nullptr;
					if (inputDef.is_button())
						name = g_currentGamepadForMapping->get_button_name(inputDef.code);
					else
						name = g_currentGamepadForMapping->get_axis_name(inputDef.code);

					displayLabelOrCode(name, inputDef.code);
					first = false;
				}

				if (ImGui::Button(T("Confirm")))
					remaining = 0;
			}

			if (remaining <= 0)
			{
				if (input_mapping != nullptr && !g_mapped_codes.empty())
				{
					unmapControl(input_mapping, g_gamepad_port_for_mapping, mapping->key);
					if (g_mapped_codes.size() == 1 && g_mapped_codes.front().is_axis())
					{
						const InputMapping::InputDef& axisInputDef = g_mapped_codes.front();
						const bool positive = (axisInputDef.type == InputMapping::InputDef::InputType::AXIS_POS);
						input_mapping->set_axis(g_gamepad_port_for_mapping, mapping->key, axisInputDef.code, positive);
						const DreamcastKey opposite = getOppositeDirectionKey(mapping->key);
						if (opposite != EMU_BTN_NONE
							&& input_mapping->get_axis_id(g_gamepad_port_for_mapping, axisInputDef.code, !positive) == EMU_BTN_NONE
							&& input_mapping->get_axis_code(g_gamepad_port_for_mapping, opposite).first == static_cast<u32>(-1)
							&& input_mapping->get_button_code(g_gamepad_port_for_mapping, opposite) == static_cast<u32>(-1))
						{
							input_mapping->set_axis(g_gamepad_port_for_mapping, opposite, axisInputDef.code, !positive);
						}
					}
					else
					{
						input_mapping->set_button(g_gamepad_port_for_mapping, mapping->key, InputMapping::ButtonCombo{g_mapped_codes, true});
					}
				}

				if (g_currentGamepadForMapping != nullptr)
					g_currentGamepadForMapping->cancel_detect_input();
				g_mapped_codes.clear();
				ImGui::CloseCurrentPopup();
			}
		}
		ImGui::EndPopup();
	}
}

static void displayMappedControl(const std::shared_ptr<GamepadDevice>& gamepad,
		const std::shared_ptr<InputMapping>& input_mapping, DreamcastKey key)
{
	if (input_mapping == nullptr)
		return;

	InputMapping::ButtonCombo combo = input_mapping->get_button_combo(g_gamepad_port_for_mapping, key);
	if (combo.inputs.empty())
	{
		const std::pair<u32, bool> pair = input_mapping->get_axis_code(g_gamepad_port_for_mapping, key);
		const InputMapping::InputDef inputDef = InputMapping::InputDef::from_axis(pair.first, pair.second);
		if (inputDef.is_valid())
			displayLabelOrCode(gamepad->get_axis_name(inputDef.code), inputDef.code, inputDef.get_suffix());
		return;
	}

	bool first = true;
	for (const InputMapping::InputDef& inputDef : combo.inputs)
	{
		if (!first)
		{
			ImGui::SameLine();
			ImGui::Text("&");
			ImGui::SameLine();
		}

		const char* name = nullptr;
		if (inputDef.is_button())
			name = gamepad->get_button_name(inputDef.code);
		else if (inputDef.is_axis())
			name = gamepad->get_axis_name(inputDef.code);

		displayLabelOrCode(name, inputDef.code, inputDef.get_suffix());
		first = false;
	}

	if (combo.inputs.size() > 1)
	{
		{
			ImguiStyleVar checkboxBorder(ImGuiStyleVar_FrameBorderSize, 1.0f);
			ImguiStyleColor checkboxBorderColor(ImGuiCol_Border, ImGui::GetStyleColorVec4(ImGuiCol_Text));
			ImguiStyleColor checkboxCheckColor(ImGuiCol_CheckMark, ImGui::GetStyleColorVec4(ImGuiCol_Text));
			const bool sequentialChanged = ImGui::Checkbox(T("Sequential"), &(combo.sequential));
#ifdef HOLLYCAST_TEST_HARNESS
			if (test_harness::ui::collectingInspection())
				annotateTestToggle("controls.mapping.sequential", (gamepad->unique_id() + "." + std::to_string(static_cast<int>(key))).c_str(),
					T("Sequential"), combo.sequential);
#endif
			if (sequentialChanged)
				input_mapping->set_button(g_gamepad_port_for_mapping, key, combo);
		}
		ImGui::SameLine();
		ShowFooterHelpMarker(T(
			"When checked, this combo will only activate when all keys are pressed in the given sequence.\n"
			"When not checked, the combo will activate when all keys are pressed in any order."));
	}
}

static float getAxisValue(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey axis)
{
	int port = gamepad->maple_port();
	if (port == -1)
		return 0.f;
	if (port == MAPLE_PORTS)
		port = static_cast<int>(g_gamepad_port_for_mapping);

	float v;
	switch (axis)
	{
	case DC_AXIS_UP: v = -joyy[port] / 32768.f; break;
	case DC_AXIS_DOWN: v = joyy[port] / 32767.f; break;
	case DC_AXIS_LEFT: v = -joyx[port] / 32768.f; break;
	case DC_AXIS_RIGHT: v = joyx[port] / 32767.f; break;
	case DC_AXIS2_UP: v = -joyry[port] / 32768.f; break;
	case DC_AXIS2_DOWN: v = joyry[port] / 32767.f; break;
	case DC_AXIS2_LEFT: v = -joyrx[port] / 32768.f; break;
	case DC_AXIS2_RIGHT: v = joyrx[port] / 32767.f; break;
	case DC_AXIS3_UP: v = -joy3y[port] / 32768.f; break;
	case DC_AXIS3_DOWN: v = joy3y[port] / 32767.f; break;
	case DC_AXIS3_LEFT: v = -joy3x[port] / 32768.f; break;
	case DC_AXIS3_RIGHT: v = joy3x[port] / 32767.f; break;
	case DC_AXIS_LT: v = lt[port] / 65535.f; break;
	case DC_AXIS_RT: v = rt[port] / 65535.f; break;
	case DC_AXIS_LT2: v = lt2[port] / 65535.f; break;
	case DC_AXIS_RT2: v = rt2[port] / 65535.f; break;
	default: v = 0.f; break;
	}
	return std::clamp(v, 0.f, 1.f);
}

static void buttonListener(int port, DreamcastKey key, bool pressed)
{
	if (g_currentGamepadForMapping == nullptr || port == -1)
		return;
	if (g_currentGamepadForMapping->maple_port() == MAPLE_PORTS && port != static_cast<int>(g_gamepad_port_for_mapping))
		return;

	if (pressed)
		g_buttonState.insert(key);
	else
		g_buttonState.erase(key);
}

static bool getButtonState(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey btn)
{
	(void)gamepad;
	return g_buttonState.count(btn) != 0;
}

static void controller_mapping_popup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	fullScreenWindow(true);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	const std::string controllerMappingTitle = std::string(T("Controller Mapping")) + "###Controller Mapping";
	if (!ImGui::BeginPopupModal(controllerMappingTitle.c_str(), nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
		return;

	const ImGuiStyle& style = ImGui::GetStyle();
	const float winWidth = ImGui::GetIO().DisplaySize.x - insetLeft - insetRight
		- (style.WindowBorderSize + style.WindowPadding.x) * 2;
	const float col_width = (winWidth - style.GrabMinSize - style.ItemSpacing.x
		- (ImGui::CalcTextSize(T("Map")).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)
		- (ImGui::CalcTextSize(T("Unmap")).x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)) / 3;

	static int item_current_map_idx = 0;
	if (!g_mappingDrafts[0] && !g_mappingDrafts[1] && game_started)
		item_current_map_idx = settings.platform.isArcade() ? 1 : 0;
	const auto currentDraft = [&]() -> GamepadDevice::MappingDraft& {
		auto& draft = g_mappingDrafts[item_current_map_idx];
		if (!draft)
			draft = gamepad->createMappingDraft(kMappingSystems[item_current_map_idx], !g_deletePerGameMapping);
		return *draft;
	};

	const bool done = ImGui::Button(T("Done"), ScaledVec2(100, 30));
	ImGui::SetItemDefaultFocus();
	ImGui::SameLine();
	const bool cancel = ImGui::Button(T("Cancel"), ScaledVec2(0, 30))
			|| ControllerPopupCancelPressed()
			|| (GImGui->BeginPopupStack.Size == g_controllerPopupDepth && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
	const float cancelWidth = ImGui::GetItemRectSize().x + style.ItemSpacing.x;
	if (done && !cancel)
	{
		g_mappingSaveFailed = false;
		bool changed = false;
		for (size_t i = 0; i < g_mappingDrafts.size(); ++i)
		{
			auto& draft = g_mappingDrafts[i];
			if (draft && draft->mapping != nullptr && draft->mapping->is_dirty())
			{
				if (gamepad->saveMappingDraft(kMappingSystems[i], *draft))
				{
					changed = true;
					if (draft->perGame && g_deletePerGameMapping)
						g_savedPerGameDrafts.emplace_back(kMappingSystems[i], draft->instance);
				}
				else
					g_mappingSaveFailed = true;
			}
		}
		if (!g_mappingSaveFailed && g_deletePerGameMapping)
		{
			g_mappingSaveFailed = !gamepad->deletePerGameMappingsExcept(g_savedPerGameDrafts);
			changed = true;
		}
		if (changed)
			gamepad->reloadEditedMappings();
	}
	if (cancel || (done && !g_mappingSaveFailed))
	{
		ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
		ResetControllerMappingRuntime();
		return;
	}

	float portWidth = 0.0f;
	if (gamepad->maple_port() == MAPLE_PORTS)
	{
		ImGui::SameLine();
		ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding,
			ImVec2(ImGui::GetStyle().FramePadding.x, (SettingsLayoutScaled(30) - ImGui::GetFontSize()) / 2));
		portWidth = ImGui::CalcTextSize("AA").x + ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::GetFontSize();
		ImGui::SetNextItemWidth(portWidth);
		if (ImGui::BeginCombo(T("Port"), kMaplePorts[g_gamepad_port_for_mapping + 1]))
		{
			for (u32 j = 0; j < MAPLE_PORTS; j++)
			{
				bool is_selected = g_gamepad_port_for_mapping == j;
				if (ImGui::Selectable(kMaplePorts[j + 1], &is_selected))
					g_gamepad_port_for_mapping = j;
				if (is_selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		portWidth += ImGui::CalcTextSize(T("Port")).x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x;
	}

	const float comboWidth = ImGui::CalcTextSize(T("Dreamcast Controls")).x + ImGui::GetStyle().ItemSpacing.x
		+ ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 4;
	float gameConfigWidth = 0.0f;
	if (!settings.content.gameId.empty())
	{
		gameConfigWidth = ImGui::CalcTextSize(currentDraft().perGame ? T("Delete Game Config") : T("Make Game Config")).x
			+ ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2;
	}

	const float controlsOffset = ImGui::GetContentRegionAvail().x - comboWidth - gameConfigWidth
		- style.ItemSpacing.x - SettingsLayoutScaled(100) * 2 - portWidth - cancelWidth;
	if (controlsOffset >= style.ItemSpacing.x)
		ImGui::SameLine(0, controlsOffset);
	ImGui::AlignTextToFramePadding();

	if (!settings.content.gameId.empty())
	{
		if (currentDraft().perGame)
		{
			if (ImGui::Button(T("Delete Game Config"), ScaledVec2(0, 30)))
			{
				g_deletePerGameMapping = true;
				g_savedPerGameDrafts.clear();
				for (auto& draft : g_mappingDrafts)
					if (draft && draft->perGame)
						draft.reset();
			}
		}
		else if (ImGui::Button(T("Make Game Config"), ScaledVec2(0, 30)))
		{
			currentDraft().perGame = true;
			currentDraft().mapping->set_dirty();
		}
		ImGui::SameLine();
	}

	if (ImGui::Button(T("Reset..."), ScaledVec2(100, 30)))
		ImGui::OpenPopup(Tnop("Confirm Reset"));

	{
		ImguiStyleVar windowPadding(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
		const std::string confirmResetTitle = std::string(T("Confirm Reset")) + "###Confirm Reset";
		if (ImGui::BeginPopupModal(confirmResetTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::Text("%s", T("Are you sure you want to reset the mappings to default?"));
			static bool hitbox = false;
			if (g_arcade_button_mode)
			{
				ImGui::Text("%s", T("Controller Type:"));
				const bool gamepadSelected = ImGui::RadioButton(T("Gamepad"), !hitbox);
				if (gamepadSelected)
					hitbox = false;
#ifdef HOLLYCAST_TEST_HARNESS
				if (test_harness::ui::collectingInspection())
					test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "controls.mapping.reset.type.gamepad"},
						{"semantic_label", T("Gamepad")}, {"kind", "radio"}, {"value", !hitbox}});
#endif
				ImGui::SameLine();
				const bool arcadeSelected = ImGui::RadioButton(T("Arcade / Hit Box"), hitbox);
				if (arcadeSelected)
					hitbox = true;
#ifdef HOLLYCAST_TEST_HARNESS
				if (test_harness::ui::collectingInspection())
					test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "controls.mapping.reset.type.arcade"},
						{"semantic_label", T("Arcade / Hit Box")}, {"kind", "radio"}, {"value", hitbox}});
#endif
			}
			ImGui::NewLine();
			{
				ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(SettingsLayoutScaled(20), ImGui::GetStyle().ItemSpacing.y));
				ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
				if (ImGui::Button(T("Yes")))
				{
					auto& draft = currentDraft();
					draft.mapping = gamepad->createDefaultMapping(g_arcade_button_mode, !hitbox);
					draft.mapping->set_dirty();
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(T("No")) || ControllerPopupCancelPressed())
					ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	ImGui::SameLine();
	const char* items[] = { T("Dreamcast Controls"), T("Arcade Controls") };

	ImGui::SetNextItemWidth(comboWidth);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(ImGui::GetStyle().FramePadding.x, (SettingsLayoutScaled(30) - ImGui::GetFontSize()) / 2));
	ImGui::Combo("##arcadeMode", &item_current_map_idx, items, IM_ARRAYSIZE(items));
#ifdef HOLLYCAST_TEST_HARNESS
	if (test_harness::ui::collectingInspection())
	{
		test_harness::Json options = test_harness::Json::array();
		for (int i = 0; i < IM_ARRAYSIZE(items); ++i)
			options.push_back({{"value", i}, {"label", items[i]}, {"selected", item_current_map_idx == i}});
		test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "controls.mapping.mode." + gamepad->unique_id()},
			{"semantic_label", "Controller mappings"}, {"kind", "options"}, {"value", item_current_map_idx},
			{"value_label", items[item_current_map_idx]}, {"options", std::move(options)}});
	}
#endif
	ImGui::PopStyleVar();
	g_arcade_button_mode = item_current_map_idx != 0;
	const MappingEntry* systemMapping = g_arcade_button_mode ? arcadeButtons : dcButtons;
	const auto& input_mapping = currentDraft().mapping;
	if (g_mappingSaveFailed)
		ImGui::TextWrapped("%s", T("Some controller mapping changes could not be saved. Check that the mappings folder is writable, then choose Done to retry."));

	char key_id[32];
	ImGui::BeginChild(ImGui::GetID("buttons"), ImVec2(0, 0),
		ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_DragScrolling);
	for (; systemMapping->name != nullptr; systemMapping++)
	{
		if (systemMapping->key == EMU_BTN_NONE)
		{
			ImGui::Columns(1, nullptr, false);
			header(systemMapping->name);
			ImGui::Columns(4, T("bindings"), false);
			ImGui::SetColumnWidth(0, col_width);
			ImGui::SetColumnWidth(1, col_width);
			ImGui::SetColumnWidth(2, col_width);
			continue;
		}

		snprintf(key_id, sizeof(key_id), "key_id%d", systemMapping->key);
		ImguiID mappingId(key_id);
		(void)mappingId;

		const char* game_btn_name = nullptr;
		if (g_arcade_button_mode)
		{
			game_btn_name = GetCurrentGameButtonName(systemMapping->key);
			if (game_btn_name == nullptr)
				game_btn_name = GetCurrentGameAxisName(systemMapping->key);
		}
		if (game_btn_name != nullptr && game_btn_name[0] != '\0')
			ImGui::Text("%s - %s", systemMapping->name, game_btn_name);
		else
			ImGui::Text("%s", systemMapping->name);

		ImGui::NextColumn();
		displayMappedControl(gamepad, input_mapping, systemMapping->key);

		ImGui::NextColumn();
		if (dynamic_cast<KeyboardDevice*>(gamepad.get()) == nullptr && dynamic_cast<Mouse*>(gamepad.get()) == nullptr)
		{
			if ((systemMapping->key & DC_BTN_GROUP_MASK) == DC_AXIS_STICKS
				|| (systemMapping->key & DC_BTN_GROUP_MASK) == DC_AXIS_TRIGGERS)
			{
				ImguiStyleColor plotColor(ImGuiCol_PlotHistogram, ImGui::GetStyle().Colors[ImGuiCol_HeaderActive]);
				const float v = getAxisValue(gamepad, systemMapping->key);
				char s[32];
				snprintf(s, sizeof(s), "%.0f%%", v * 100.f);
				ImGui::ProgressBar(v, ImVec2(-1, 0), s);
			}
			else if (getButtonState(gamepad, systemMapping->key))
			{
				ImGui::PushFont(settingsIconFont);
				ImGui::Text(ICON_FA_CIRCLE_DOT);
				ImGui::PopFont();
			}
		}

		ImGui::NextColumn();
		if (ImGui::Button(T("Map")))
		{
			g_map_start_time = getTimeMs() + 300;
			ImGui::OpenPopup(Tnop("Map Control"));
			g_mapped_codes.clear();
			g_buttonState.erase(systemMapping->key);

			const bool detectCombo = (systemMapping->key & DC_BTN_GROUP_MASK) == EMU_BUTTONS;
			gamepad->detectInput(detectCombo, [](u32 code, bool analog, bool positive) {
				if (analog)
					g_mapped_codes.insert_back(InputMapping::InputDef::from_axis(code, positive));
				else
					g_mapped_codes.insert_back(InputMapping::InputDef::from_button(code));
			});
		}
		detect_input_popup(systemMapping, input_mapping);
		ImGui::SameLine();
		if (ImGui::Button(T("Unmap")))
			unmapControl(input_mapping, g_gamepad_port_for_mapping, systemMapping->key);
		ImGui::NextColumn();
	}
	ImGui::Columns(1, nullptr, false);
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
	error_popup();
	ImGui::EndPopup();
}

static bool isSharedVmuSlotActive(int bus, int slot)
{
	if (config::MapleMainDevices[bus] == MDT_None)
		return false;
	return config::MapleExpansionDevices[bus][slot] == MDT_SegaVMU;
}

static std::string normalizeVmuFileName(std::string name)
{
	if (!hostfs::hasVmuExtension(name))
		name += ".bin";
	return name;
}

static bool isLikelyPerGameA1VmuFileName(const std::string& fileName)
{
	constexpr std::string_view suffix = hostfs::perGameVmuSuffix;
	return fileName.size() >= suffix.size()
		&& fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix.data()) == 0;
}

static void setConfiguredVmuSlotFileName(int bus, int slot, const std::string& fileName)
{
	config::MapleVmuSlotFileNames[bus][slot].get() =
		fileName == hostfs::defaultVmuFileName(hostfs::vmuSlotLabel(bus, slot)) ? "" : fileName;
}

static bool isManualVmuSlotEditable(int bus, int slot, bool perGameEnabled)
{
	return isSharedVmuSlotActive(bus, slot) && !(perGameEnabled && bus == 0 && slot == 0);
}

static void applyVmuSlotSelection(int bus, int slot, const std::string& fileName)
{
	// Slot selection stores only the file name; getVmuPath resolves the active VMU folder.
	setConfiguredVmuSlotFileName(bus, slot, fileName);
	g_mapleDevicesChangedInSettings = true;
}

static std::string getVmuCardManagerFolder()
{
	const std::string customVmuPath = config::VMUPath.get();
	return customVmuPath.empty() ? get_writable_data_path("") : customVmuPath;
}

static bool getVmuCardManagerFilePath(const std::string& fileName, std::string& fullPath, std::string& error)
{
	if (!hostfs::isConfiguredVmuFileNameValid(fileName))
	{
		error = T("Invalid file name.");
		return false;
	}

	try {
		const std::string customVmuPath = config::VMUPath.get();
		fullPath = customVmuPath.empty()
			? get_writable_data_path(fileName)
			: hostfs::storage().getSubPath(customVmuPath, fileName);
	} catch (const hostfs::StorageException&) {
		error = T("Invalid VMU folder.");
		return false;
	}
	return true;
}

static std::string makeVmuCopyFileName(const std::string& fileName)
{
	std::string base = fileName;
	std::string extension = ".bin";
	const size_t dot = fileName.find_last_of('.');
	if (dot != std::string::npos)
	{
		base = fileName.substr(0, dot);
		extension = fileName.substr(dot);
	}

	for (int i = 1; i < 1000; i++)
	{
		std::string candidate = base + (i == 1 ? "_copy" : "_copy" + std::to_string(i)) + extension;
		std::string path;
		std::string error;
		if (!getVmuCardManagerFilePath(candidate, path, error) || !hostfs::storage().exists(path))
			return candidate;
	}

	return base + "_copy" + extension;
}

static bool createBlankVmuFile(const std::string& fileName, std::string& error)
{
	std::string fullPath;
	if (!getVmuCardManagerFilePath(fileName, fullPath, error))
		return false;

	if (hostfs::storage().exists(fullPath))
	{
		error = T("A file with that name already exists.");
		return false;
	}

	FILE *f = nowide::fopen(fullPath.c_str(), "wb");
	if (f == nullptr)
	{
		error = T("Failed to create file.");
		return false;
	}

	std::array<u8, 128_KB> vmuImage {};
	if (!buildDefaultVmuImage(vmuImage.data(), vmuImage.size()))
	{
		std::fclose(f);
		nowide::remove(fullPath.c_str());
		error = T("Failed to build default VMU image.");
		return false;
	}
	if (std::fwrite(vmuImage.data(), 1, vmuImage.size(), f) != vmuImage.size())
	{
		std::fclose(f);
		nowide::remove(fullPath.c_str());
		error = T("Failed to write VMU image.");
		return false;
	}
	std::fclose(f);
	return true;
}

static bool copyVmuCardFile(const std::string& sourceName, const std::string& copyName, std::string& error)
{
	std::string sourcePath;
	if (!getVmuCardManagerFilePath(sourceName, sourcePath, error))
		return false;

	std::string copyPath;
	if (!getVmuCardManagerFilePath(copyName, copyPath, error))
		return false;

	if (hostfs::storage().exists(copyPath))
	{
		error = T("A file with that name already exists.");
		return false;
	}

	{
		std::unique_ptr<FILE, decltype(&fclose)> source(nowide::fopen(sourcePath.c_str(), "rb"), &fclose);
		if (source == nullptr)
		{
			error = T("Failed to open source VMU.");
			return false;
		}

		std::unique_ptr<FILE, decltype(&fclose)> copy(nowide::fopen(copyPath.c_str(), "wb"), &fclose);
		if (copy == nullptr)
		{
			error = T("Failed to create copy.");
			return false;
		}

		std::array<u8, 16 * 1024> buffer {};
		while (true)
		{
			const size_t bytesRead = std::fread(buffer.data(), 1, buffer.size(), source.get());
			if (bytesRead > 0 && std::fwrite(buffer.data(), 1, bytesRead, copy.get()) != bytesRead)
			{
				error = T("Failed to write VMU copy.");
				break;
			}
			if (bytesRead < buffer.size())
			{
				if (std::ferror(source.get()))
					error = T("Failed to read source VMU.");
				break;
			}
		}
	}

	if (!error.empty())
	{
		nowide::remove(copyPath.c_str());
		return false;
	}
	return true;
}

static bool isVmuCardFile(const hostfs::FileInfo& info)
{
	if (info.isDirectory)
		return false;

	size_t fileSize = info.size;
	if (fileSize == 0)
	{
		try {
			fileSize = hostfs::storage().getFileInfo(info.path).size;
		} catch (const hostfs::StorageException&) {
			return false;
		}
	}
	return fileSize == 128_KB && hostfs::isConfiguredVmuFileNameValid(info.name);
}

static void listVmuCardFiles(std::vector<hostfs::FileInfo>& out)
{
	out.clear();
	try {
		const std::string dataDir = getVmuCardManagerFolder();
		for (const auto& entry : hostfs::storage().listContent(dataDir)) {
			if (isVmuCardFile(entry))
				out.push_back(entry);
		}
	} catch (const hostfs::StorageException&) {
	}

	std::sort(out.begin(), out.end(), [](const hostfs::FileInfo& a, const hostfs::FileInfo& b) {
		return a.name < b.name;
	});
}

static void findAssignedVmuSlot(const std::string& fileName, bool perGameEnabled, int& sourceBus, int& sourceSlot);

static bool renameVmuCardFile(const std::string& oldName, const std::string& newName, bool perGameEnabled,
		std::string& error)
{
	std::string oldPath;
	if (!getVmuCardManagerFilePath(oldName, oldPath, error))
		return false;

	std::string newPath;
	if (!getVmuCardManagerFilePath(newName, newPath, error))
		return false;
	if (hostfs::storage().exists(newPath))
	{
		error = T("A file with that name already exists.");
		return false;
	}

	int sourceBus = -1;
	int sourceSlot = -1;
	findAssignedVmuSlot(oldName, perGameEnabled, sourceBus, sourceSlot);

	int targetBus = -1;
	int targetSlot = -1;
	findAssignedVmuSlot(newName, perGameEnabled, targetBus, targetSlot);

	if (nowide::rename(oldPath.c_str(), newPath.c_str()) != 0)
	{
		error = T("Rename failed.");
		return false;
	}

	// Keep rename consistent with Insert: one active VMU slot owns one file name.
	if (sourceBus >= 0)
	{
		if (targetBus >= 0 && (targetBus != sourceBus || targetSlot != sourceSlot))
			applyVmuSlotSelection(targetBus, targetSlot, oldName);
		applyVmuSlotSelection(sourceBus, sourceSlot, newName);
	}
	return true;
}

static void findAssignedVmuSlot(const std::string& fileName, bool perGameEnabled, int& sourceBus, int& sourceSlot)
{
	sourceBus = -1;
	sourceSlot = -1;
	for (int bus = 0; bus < MAPLE_PORTS; bus++)
	{
		for (int slot = 0; slot < 2; slot++)
		{
			if (isManualVmuSlotEditable(bus, slot, perGameEnabled)
					&& hostfs::sharedVmuFileName(hostfs::vmuSlotLabel(bus, slot)) == fileName)
			{
				sourceBus = bus;
				sourceSlot = slot;
				return;
			}
		}
	}
}

static void selectVmuCardAndRefresh(const std::string& fileName, std::string& selectedVmuName, bool& refreshVmuList)
{
	selectedVmuName = fileName;
	refreshVmuList = true;
}

static void renderVmuCardManager()
{
	static std::string selectedVmuName;
	static std::string createVmuName = "new_vmu.bin";
	static std::string copyVmuName;
	static std::string renameVmuName;
	static std::string vmuOpError;
	static std::vector<hostfs::FileInfo> cachedVmuFiles;
	static bool refreshVmuList = true;

	const bool perGameEnabled = static_cast<bool>(config::PerGameVmu);

	ImGui::TextDisabled("%s", config::VMUPath.get().empty() ? T("Memory Cards (Data Folder)") : T("Memory Cards (VMU Folder)"));
	if (perGameEnabled)
		ImGui::TextDisabled("%s", T("Per Game VMU manages A1 automatically; other shared slots remain editable."));

	if (refreshVmuList)
	{
		listVmuCardFiles(cachedVmuFiles);
		refreshVmuList = false;
	}

	int selectedIndex = -1;
	for (int i = 0; i < static_cast<int>(cachedVmuFiles.size()); i++)
	{
		if (cachedVmuFiles[i].name == selectedVmuName)
		{
			selectedIndex = i;
			break;
		}
	}
	const bool hasSelection = selectedIndex >= 0 && selectedIndex < static_cast<int>(cachedVmuFiles.size());
	const std::string selectedName = hasSelection ? cachedVmuFiles[selectedIndex].name : "";

	int selectedBus = -1;
	int selectedSlot = -1;
	if (hasSelection)
		findAssignedVmuSlot(selectedName, perGameEnabled, selectedBus, selectedSlot);

	const bool selectedLooksPerGameA1 = perGameEnabled && hasSelection && isLikelyPerGameA1VmuFileName(selectedName);
	const bool selectedIsAssigned = selectedBus >= 0;
	const bool selectedIsSlotDefault = selectedIsAssigned
		&& selectedName == hostfs::defaultVmuFileName(hostfs::vmuSlotLabel(selectedBus, selectedSlot));
	const bool canEjectSelection = selectedIsAssigned && !selectedIsSlotDefault;

	if (ImGui::Button(T("Refresh")))
	{
		vmuOpError.clear();
		refreshVmuList = true;
	}
	ImGui::SameLine();
	if (ImGui::Button(T("Create New Card")))
	{
		vmuOpError.clear();
		ImGui::OpenPopup(T("Create VMU Card"));
	}
	ImGui::SameLine();
	{
		DisabledScope renameDisabled(!hasSelection);
		if (ImGui::Button(T("Rename")))
		{
			vmuOpError.clear();
			renameVmuName = selectedName;
			ImGui::OpenPopup(T("Rename VMU Card"));
		}
	}
	ImGui::SameLine();
	{
		DisabledScope copyDisabled(!hasSelection);
		if (ImGui::Button(T("Copy")))
		{
			vmuOpError.clear();
			copyVmuName = makeVmuCopyFileName(selectedName);
			ImGui::OpenPopup(T("Copy VMU Card"));
		}
	}
	ImGui::SameLine();
	{
		DisabledScope insertDisabled(!hasSelection || selectedLooksPerGameA1);
		if (ImGui::Button(T("Insert")))
		{
			vmuOpError.clear();
			ImGui::OpenPopup(T("Insert VMU Card"));
		}
	}
	if (selectedLooksPerGameA1 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("%s", T("Copy this Per Game A1 VMU before inserting it into another slot."));
	ImGui::SameLine();
	{
		DisabledScope ejectDisabled(!canEjectSelection);
		if (ImGui::Button(T("Eject")))
		{
			vmuOpError.clear();
			applyVmuSlotSelection(selectedBus, selectedSlot, hostfs::defaultVmuFileName(hostfs::vmuSlotLabel(selectedBus, selectedSlot)));
			refreshVmuList = true;
		}
	}

	auto slotLabelsForFile = [&](const std::string& fileName) {
		std::string labels;
		for (int bus = 0; bus < MAPLE_PORTS; bus++)
		{
			for (int slot = 0; slot < 2; slot++)
			{
				if (!isManualVmuSlotEditable(bus, slot, perGameEnabled)
						|| hostfs::sharedVmuFileName(hostfs::vmuSlotLabel(bus, slot)) != fileName) {
					continue;
				}
				if (!labels.empty())
					labels += ", ";
				labels += hostfs::vmuSlotLabel(bus, slot);
			}
		}
		return labels;
	};

	const float listHeight = SettingsLayoutScaled(220.0f);
	ImGui::BeginChild("VmuCardManager", ImVec2(0.0f, listHeight), true,
		ImGuiWindowFlags_DragScrolling);
	if (ImGui::BeginTable("VmuCardTable", 2,
			ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoSavedSettings))
	{
		ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(T("Slot"), ImGuiTableColumnFlags_WidthFixed, SettingsLayoutScaled(90.0f));
		ImGui::TableHeadersRow();

		for (int i = 0; i < static_cast<int>(cachedVmuFiles.size()); i++)
		{
			const hostfs::FileInfo& info = cachedVmuFiles[i];
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);

			const bool isSelected = i == selectedIndex;
			if (ImGui::Selectable(info.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
				selectedVmuName = info.name;

			ImGui::TableSetColumnIndex(1);
			const std::string labels = slotLabelsForFile(info.name);
			ImGui::TextUnformatted(labels.empty() ? "-" : labels.c_str());
		}
		ImGui::EndTable();
	}
	// Match the Settings panels: an Android finger drag starting over a card row
	// scrolls this vertical list instead of selecting the row beneath the finger.
	scrollWhenDraggingOnVoid();
	windowDragScroll(false);
	ImGui::EndChild();

	if (ImGui::BeginPopupModal(T("Create VMU Card"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::InputText(T("File name"), &createVmuName);
		if (!vmuOpError.empty())
			ImGui::TextColored(SettingsErrorColor(), "%s", vmuOpError.c_str());

		if (ImGui::Button(T("Create")))
		{
			const std::string name = normalizeVmuFileName(createVmuName);
			std::string err;
			if (createBlankVmuFile(name, err))
			{
				selectVmuCardAndRefresh(name, selectedVmuName, refreshVmuList);
				ImGui::CloseCurrentPopup();
			}
			else
			{
				vmuOpError = err;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("Cancel")) || ControllerPopupCancelPressed())
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(T("Copy VMU Card"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::InputText(T("New file name"), &copyVmuName);
		if (!vmuOpError.empty())
			ImGui::TextColored(SettingsErrorColor(), "%s", vmuOpError.c_str());

		if (ImGui::Button(T("Copy")))
		{
			if (!hasSelection)
			{
				ImGui::CloseCurrentPopup();
			}
			else
			{
				const std::string newName = normalizeVmuFileName(copyVmuName);
				if (!hostfs::isConfiguredVmuFileNameValid(newName))
				{
					vmuOpError = T("Invalid file name.");
				}
				else
				{
					std::string err;
					if (copyVmuCardFile(selectedName, newName, err))
					{
						selectVmuCardAndRefresh(newName, selectedVmuName, refreshVmuList);
						ImGui::CloseCurrentPopup();
					}
					else
					{
						vmuOpError = err;
					}
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("Cancel")) || ControllerPopupCancelPressed())
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(T("Rename VMU Card"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const bool showPerGameRenameWarning = perGameEnabled
			&& hasSelection
			&& isLikelyPerGameA1VmuFileName(selectedName);
		if (showPerGameRenameWarning)
		{
			ImVec4 warningColor = CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered], "settings.feedback");
			ImGui::TextColored(warningColor, "%s %s", ICON_FA_TRIANGLE_EXCLAMATION, T("Per Game VMU Rename Warning"));
			ImGui::TextWrapped("%s", T(
				"This looks like an auto-named Per Game VMU for A1.\n"
				"Renaming it can stop Hollycast from auto-loading it while Per Game VMUs is enabled.\n"
				"To restore automatic pickup later, rename it back to its original file name."));
			ImGui::Separator();
		}
		ImGui::InputText(T("New name"), &renameVmuName);
		if (!vmuOpError.empty())
			ImGui::TextColored(SettingsErrorColor(), "%s", vmuOpError.c_str());

		if (ImGui::Button(T("Apply")))
		{
			if (!hasSelection)
			{
				ImGui::CloseCurrentPopup();
			}
			else
			{
				const std::string oldName = cachedVmuFiles[selectedIndex].name;
				const std::string newName = normalizeVmuFileName(renameVmuName);
				if (newName == oldName)
				{
					ImGui::CloseCurrentPopup();
				}
				else if (!hostfs::isConfiguredVmuFileNameValid(newName))
				{
					vmuOpError = T("Invalid file name.");
				}
				else
				{
					std::string err;
					if (renameVmuCardFile(oldName, newName, perGameEnabled, err))
					{
						selectVmuCardAndRefresh(newName, selectedVmuName, refreshVmuList);
						ImGui::CloseCurrentPopup();
					}
					else
					{
						vmuOpError = err;
					}
				}
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(T("Cancel")) || ControllerPopupCancelPressed())
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (ImGui::BeginPopupModal(T("Insert VMU Card"), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		if (!hasSelection)
		{
			ImGui::TextUnformatted(T("No VMU selected."));
		}
		else
		{
			const std::string sourceName = cachedVmuFiles[selectedIndex].name;
			ImGui::Text(T("Insert \"%s\" into:"), sourceName.c_str());
			if (!vmuOpError.empty())
				ImGui::TextColored(SettingsErrorColor(), "%s", vmuOpError.c_str());

			if (perGameEnabled && isLikelyPerGameA1VmuFileName(sourceName))
			{
				ImGui::TextWrapped("%s", T(
					"This looks like an auto-named Per Game VMU for A1.\n"
					"Copy it first, then insert the copy into another slot."));
			}
			else
			{
				bool anySlot = false;
				for (int bus = 0; bus < MAPLE_PORTS; bus++)
				{
					for (int slot = 0; slot < 2; slot++)
					{
						if (!isSharedVmuSlotActive(bus, slot))
							continue;

						const std::string label = hostfs::vmuSlotLabel(bus, slot);
						if (perGameEnabled && bus == 0 && slot == 0)
						{
							const std::string lockedLabel = label + " (" + T("Per Game VMU") + ")";
							DisabledScope disabled(true);
							ImGui::Selectable(lockedLabel.c_str());
							continue;
						}

						anySlot = true;
						if (ImGui::Selectable(label.c_str()))
						{
							int sourceBus = -1;
							int sourceSlot = -1;
							findAssignedVmuSlot(sourceName, perGameEnabled, sourceBus, sourceSlot);

							if (sourceBus == bus && sourceSlot == slot)
							{
								ImGui::CloseCurrentPopup();
								continue;
							}

							// Keep one active VMU slot per file so two emulated cards never write the same save.
							const std::string targetName = hostfs::sharedVmuFileName(hostfs::vmuSlotLabel(bus, slot));
							if (sourceBus >= 0)
								applyVmuSlotSelection(sourceBus, sourceSlot, targetName);
							applyVmuSlotSelection(bus, slot, sourceName);
							refreshVmuList = true;
							ImGui::CloseCurrentPopup();
						}
					}
				}
				if (!anySlot)
					ImGui::TextDisabled("%s", T("No active shared VMU slots are available."));
			}
		}

		ImGui::Spacing();
		if (ImGui::Button(T("Close")) || ControllerPopupCancelPressed())
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
}

void renderControlsTab()
{
	uiLanguageHandler.init();
	if (g_currentGamepadForMapping != nullptr && !g_currentGamepadForMapping->is_registered())
	{
		CloseDeviceSettingsPopup("Controller Mapping");
		ResetControllerMappingRuntime();
	}
	if (g_currentGamepadForSettings != nullptr && !g_currentGamepadForSettings->is_registered())
	{
		CloseDeviceSettingsPopup("Gamepad Settings");
		g_currentGamepadForSettings.reset();
	}

	ScopedTwoLineRowStyle controlsRowStyle(12.0f, true, 0.5f);


	if (RenderCollapsingHeader("PhysicalDevicesSection", ICON_FA_GAMEPAD, T("Physical Devices"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::PushFont(settingsTitleFont);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(4, 3));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 4));
		ImVec4 deviceTableBorderColor = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		deviceTableBorderColor.w *= 0.35f;
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, deviceTableBorderColor);
		if (ImGui::BeginTable("physicalDevices", 5,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings
				| ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV))
		{
			ImGui::TableSetupColumn(T("System"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T("Name"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("Status"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T("Actions"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T("Port"), ImGuiTableColumnFlags_WidthFixed);

			const float portComboWidth = ImGui::CalcTextSize("None").x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
			const ImVec2 deviceButtonSize(0.0f, SettingsLayoutScaled(24.0f));
			const ImVec4 gray = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];

			ImGui::TableNextRow(ImGuiTableRowFlags_None, SettingsLayoutScaled(24.0f));
			ImGui::TableSetColumnIndex(0);
			ImGui::TextColored(gray, "%s", T("System"));

			ImGui::TableSetColumnIndex(1);
			ImGui::TextColored(gray, "%s", T("Name"));

			ImGui::TableSetColumnIndex(2);
			ImGui::TextColored(gray, "%s", T("Status"));

			ImGui::TableSetColumnIndex(3);
			ImGui::TextColored(gray, "%s", T("Actions"));

			ImGui::TableSetColumnIndex(4);
			ImGui::TextColored(gray, "%s", T("Port"));

			// Iterate through all detected gamepads
			for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
			{
				std::shared_ptr<GamepadDevice> gamepad = GamepadDevice::GetGamepad(i);
				if (!gamepad)
					continue;
				char gamepad_row_id[32];
				snprintf(gamepad_row_id, sizeof(gamepad_row_id), "gamepad_row_%d", i);
				ImguiID gamepadRowId(gamepad_row_id);
				(void)gamepadRowId;

				ImGui::TableNextRow(ImGuiTableRowFlags_None, SettingsLayoutScaled(24.0f));
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s", gamepad->api_name().c_str());

				ImGui::TableSetColumnIndex(1);
				ImGui::Text("%s", gamepad->name().c_str());

				ImGui::TableSetColumnIndex(2);
#if defined(USE_DREAMLINK_DEVICES)
				DreamLinkSDLGamepad* dreamLinkGamepad = dynamic_cast<DreamLinkSDLGamepad*>(gamepad.get());
				if (dreamLinkGamepad != nullptr) {
					ImGui::Text("%s", dreamLinkGamepad->dreamLinkStatus());
				}
#endif

				ImGui::TableSetColumnIndex(4);
				char port_name[32];
				snprintf(port_name, sizeof(port_name), "##mapleport%d", i);
				ImguiID _(port_name);
				ImGui::SetNextItemWidth(portComboWidth);

				// Port selection combo (None, A, B, C, D, All)
				const char* current_port = kMaplePorts[gamepad->maple_port() + 1];
#ifdef HOLLYCAST_TEST_HARNESS
				const bool mappingOnlyTestDevice = gamepad->remappable()
					&& gamepad->api_name() == test_harness::ControllerApiName
					&& gamepad->unique_id() == "hollycast-test-mapping-device";
				if (mappingOnlyTestDevice)
					ImGui::BeginDisabled(true);
#endif
				const bool portPopupOpen = ImGui::BeginCombo(port_name, current_port);
#ifdef HOLLYCAST_TEST_HARNESS
				if (test_harness::ui::collectingInspection())
				{
					const auto portItemId = test_harness::ui::lastItemId();
					if (portItemId != 0)
					{
						using test_harness::Json;
						Json options = Json::array();
						for (int port = -1; port <= 4; ++port)
							options.push_back({{"value", port}, {"label", kMaplePorts[port + 1]},
								{"selected", gamepad->maple_port() == port}});
						const std::string semanticId = "controls.physical.port." + gamepad->unique_id();
						const std::string semanticLabel = std::string(gamepad->name()) + " Port";
						test_harness::ui::annotateItem(portItemId, {{"semantic_id", semanticId},
							{"semantic_label", semanticLabel}, {"kind", "options"}, {"value", gamepad->maple_port()},
							{"value_label", current_port}, {"options", std::move(options)},
							{"requires_apply", !mappingOnlyTestDevice}});
					}
				}
#endif
#ifdef HOLLYCAST_TEST_HARNESS
				if (mappingOnlyTestDevice)
					ImGui::EndDisabled();
#endif
				if (portPopupOpen)
				{
					for (int j = -1; j < IM_ARRAYSIZE(kMaplePorts) - 1; j++)
					{
						bool is_selected = gamepad->maple_port() == j;
						if (ImGui::Selectable(kMaplePorts[j + 1], &is_selected))
						{
							gamepad->set_maple_port(j);
							g_mapleDevicesChangedInSettings = true;
						}
						if (is_selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}

				ImGui::TableSetColumnIndex(3);
				ImGui::SameLine(0, SettingsLayoutScaled(8.0f));

				// Map button for controller mapping
				if (gamepad->remappable())
				{
					const bool mapClicked = ImGui::Button(T("Map"), deviceButtonSize);
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
					{
						const std::string semanticId = "controls.device.mapping." + gamepad->unique_id();
						const std::string semanticLabel = std::string(gamepad->name()) + " Mapping";
						test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", semanticId},
							{"semantic_label", semanticLabel}, {"kind", "button"}});
					}
#endif
					if (mapClicked)
					{
						ResetControllerMappingRuntime();
						g_currentGamepadForMapping = gamepad;
						g_currentGamepadForMapping->listenButtons(buttonListener);
						g_gamepad_port_for_mapping = 0;
						ImGui::OpenPopup(Tnop("Controller Mapping"));
					}
				}

				// Virtual controllers open the live editor; physical controllers keep
				// the rumble, deadzone, and saturation popup.
				if (gamepad->is_rumble_enabled() || gamepad->has_analog_stick() || gamepad->is_virtual_gamepad())
				{
					ImGui::SameLine(0, SettingsLayoutScaled(16.0f));
					const bool settingsClicked = ImGui::Button(T("Settings"), deviceButtonSize);
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
					{
						const std::string semanticId = "controls.device.settings." + gamepad->unique_id();
						const std::string semanticLabel = std::string(gamepad->name()) + " Settings";
						test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", semanticId},
							{"semantic_label", semanticLabel}, {"kind", "button"}});
					}
#endif
					if (settingsClicked)
					{
						if (gamepad->is_virtual_gamepad())
						{
							g_currentGamepadForSettings.reset();
							vgamepad::startEditing();
							vgamepad::pauseEditing();
							gui_setState(GuiState::VJoyEditCommands);
						}
						else
						{
							g_currentGamepadForSettings = gamepad;
							ImGui::OpenPopup(Tnop("Gamepad Settings"));
						}
					}
				}

				// Render the gamepad settings popup for this gamepad
				if (g_currentGamepadForSettings == gamepad)
				{
					gamepadSettingsPopup(gamepad);
					if (!ImGui::IsPopupOpen(Tnop("Gamepad Settings")))
						g_currentGamepadForSettings.reset();
				}

				// Render controller mapping popup for the selected gamepad
				if (g_currentGamepadForMapping == gamepad)
				{
					controller_mapping_popup(gamepad);
					if (!ImGui::IsPopupOpen(Tnop("Controller Mapping")))
						ResetControllerMappingRuntime();
				}
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		ImGui::PopFont();
	}

	ImGui::Spacing();

#ifdef USE_HIKARU
	// Platform alone stays Hikaru after unload. Gate cabinet controls on the
	// published loaded descriptor so another title never exposes these options.
	const auto* hikaruGame = hikaru::frontend::loadedGame();
	if (hikaruGame && RenderCollapsingHeader("HikaruCompatibilityBindings", ICON_FA_GAMEPAD,
		T("Game compatibility controls"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextUnformatted(hikaruGame->title);
		ImGui::TextWrapped("%s", T("Enabled gamepad controls use a built-in preset. Disable them to use your saved mapping. Wheels keep their mapped axes. L3 inserts a coin, R3 opens cabinet test, and Back opens the menu."));
		auto* regionOption = hikaruGame == &hikaru::StarWarsRacerGame ? &config::HikaruStarWarsRacerRegion
			: hikaruGame == &hikaru::SegaNascarGame ? &config::HikaruNascarRegion
			: hikaruGame == &hikaru::AirTrixGame ? &config::HikaruAirTrixRegion
			: hikaruGame == &hikaru::BraveFirefightersGame ? &config::HikaruBraveRegion
			: hikaruGame == &hikaru::VirtualOn4Game ? &config::HikaruVirtualOn4Region : &config::HikaruPlanetRegion;
		const char* regionLabels[] = {T("Keep saved region"), T("Japan"), T("USA (English)"), T("Export")};
		int regionSelection = std::clamp(regionOption->get(), -1, 2) + 1;
		SettingsUI::PopupConfig regionCfg {};
		regionCfg.type = SettingsUI::PopupType::Options;
		regionCfg.options.label = T("Cabinet region");
		regionCfg.options.icon = ICON_FA_GLOBE;
		regionCfg.options.popupID = Tnop("HikaruCabinetRegionPopup");
		regionCfg.options.options = regionLabels;
		regionCfg.options.optionCount = 4;
		regionCfg.options.currentValue = &regionSelection;
		regionCfg.options.valueWidth = 220.0f;
		regionCfg.options.onChange = [regionOption](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex > 3)
				return false;
			if (regionOption->get() != selectedIndex - 1)
			{
				regionOption->set(selectedIndex - 1);
				hikaru::frontend::requestCabinetRegion();
			}
			return true;
		};
		RenderGeneralPopupSettingRow(Tnop("HikaruCabinetRegionSetting"),
			T("Choose a region for this game. Changing it restarts the cabinet; calibration and saved records are retained. A backup is kept before the first change."), regionCfg);
		ImGui::TextWrapped("%s", T("USA selects the English cabinet configuration. Export uses the game's export language options. New cabinets must finish creating their settings before the region can apply."));
		if (hikaruGame == &hikaru::StarWarsRacerGame) {
			RenderGeneralToggleSettingRow("HikaruStarWarsRacerGamepad", ICON_FA_GAMEPAD,
				T("Gamepad pod controls"), T("Use triggers and shoulders while both thumbs operate the pod levers"),
				bool(config::HikaruStarWarsRacerGamepad),
				[](bool enabled) { config::HikaruStarWarsRacerGamepad.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("Disable to use only the individually mapped cabinet buttons. The two analog levers keep their assignments."));
			ImGui::TextWrapped("%s", T("Push the left and right sticks forward to advance their respective pod levers. A is boost, B is brake, and X/Y are the left/right buttons."));
			ImGui::TextWrapped("%s", T("With the default arcade gamepad layout, this option also maps LT to boost, RT to brake, and the left/right shoulders or D-pad to the corresponding left/right buttons. Coin and Start use their normal arcade bindings."));
		} else if (hikaruGame == &hikaru::SegaNascarGame) {
			RenderGeneralToggleSettingRow("HikaruNascarGamepad", ICON_FA_GAMEPAD,
				T("Gamepad driving controls"), T("Keep the selected gear engaged and allow D-pad steering"),
				bool(config::HikaruNascarGamepad),
				[](bool enabled) { config::HikaruNascarGamepad.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("Disable for a physical shifter whose gear buttons remain held. Analog steering and pedals always keep their cabinet assignments."));
			ImGui::TextWrapped("%s", T("Standard cabinet: left stick steers, RT is gas, LT is brake. With the preset enabled, A/B/X/Y select gears 1/2/3/4, right shoulder changes view, and left shoulder selects neutral. L3 inserts a coin and Start begins the race."));
			ImGui::TextWrapped("%s", T("Map Steering wheel, Gas pedal and Brake pedal above. Use Game Test > Input Assignments for calibration; select Automatic transmission in the game if you do not want to shift manually."));
		} else if (hikaruGame == &hikaru::AirTrixGame) {
			RenderGeneralToggleSettingRow("HikaruAirTrixDpadBoard", ICON_FA_GAMEPAD,
				T("D-pad board steering"), T("Use the D-pad for board yaw and roll, alongside the analog stick"),
				bool(config::HikaruAirTrixDpadBoard),
				[](bool enabled) { config::HikaruAirTrixDpadBoard.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("When disabled, D-pad up/down operate the original nose/tail kick switches. Action buttons always provide separate kicks."));
			ImGui::TextWrapped("%s", T("Map Tail kick, Nose kick, Cabinet left/right, Coin, Yaw and Roll using Controller Mapping above. With the default arcade gamepad layout: A is tail kick, B is nose kick, X/Y are cabinet left/right, and the right shoulder inserts a coin."));
			ImGui::TextWrapped("%s", T("Tricks combine board motion and kick timing. If steering is ignored, calibrate both axes in Game Test Mode > Calibration > Volume Calibration, then return to the center and save."));
		} else if (hikaruGame == &hikaru::BraveFirefightersGame) {
			RenderGeneralToggleSettingRow("HikaruBraveGamepad", ICON_FA_GAMEPAD,
				T("Gamepad hose controls"), T("Use the built-in gamepad layout for this cabinet"),
				bool(config::HikaruBraveGamepad),
				[](bool enabled) { config::HikaruBraveGamepad.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("Disable to use your saved controller mapping."));
			RenderGeneralToggleSettingRow("HikaruBraveMouseAim", ICON_FA_CROSSHAIRS,
				T("Mouse aim (Brave Firefighters)"), T("Pointer aims the hose; left sprays, right is fog, middle is Start, wheel up inserts a coin"),
				bool(config::HikaruBraveMouseAim),
				[](bool enabled) { config::HikaruBraveMouseAim.set(enabled); },
				T("Uses the pointer position as the hose aim, like touch. Existing keyboard and gamepad bindings keep working."));
			ImGui::TextWrapped("%s", T("Controller Mapping provides Spray, Fog, Horizontal aim and Vertical aim for this cabinet."));
		} else if (hikaruGame == &hikaru::VirtualOn4Game) {
			RenderGeneralToggleSettingRow("HikaruVirtualOn4TwinStick", ICON_FA_GAMEPAD,
				T("Single-controller bindings"), T("Use both sticks and triggers of controller A for one cabinet"),
				bool(config::HikaruVirtualOn4TwinStick),
				[](bool enabled) { config::HikaruVirtualOn4TwinStick.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("When disabled, controllers A and B each provide one lever. These are the two levers of one player, not linked cabinets."));
			ImGui::TextWrapped("%s", T("Left/right sticks move the two levers. LT/RT fire. X/Y activate left/right turbo, L3 inserts a coin, and Start begins the game. Right shoulder opens personal information."));
		} else if (hikaruGame == &hikaru::PlanetHarriersGame) {
			RenderGeneralToggleSettingRow("HikaruPlanetDpadStick", ICON_FA_GAMEPAD,
				T("D-pad flight controls"), T("Use the D-pad for full-range flight movement, alongside the analog stick"),
				bool(config::HikaruPlanetDpadStick),
				[](bool enabled) { config::HikaruPlanetDpadStick.set(enabled); GamepadDevice::RefreshGamepadPresets(); },
				T("Disable to steer only with the mapped analog stick. Bullet, Missile, Bomb and View buttons keep their existing bindings."));
			ImGui::TextWrapped("%s", T("Controller Mapping provides Bullet, Missile, Bomb, View left/right and Stick horizontal/vertical for this cabinet."));
		}
		ImGui::Spacing();
	}
#endif

	if (RenderCollapsingHeader("Mouse&RawInputSection", ICON_FA_CROSSHAIRS, T("Mouse & Raw Input"), ImGuiTreeNodeFlags_DefaultOpen))
	{

		{
			// Temp storage for popup slider
			static int sensitivityTemp = config::MouseSensitivity.get();

			SettingsUI::PopupSliderConfig sensitivityConfig {};
			sensitivityConfig.label = T("Sensitivity");
			sensitivityConfig.icon = ICON_FA_BULLSEYE;
			sensitivityConfig.tooltip = T("Mouse sensitivity for light gun and mouse emulation");
			sensitivityConfig.popupID = Tnop("SensitivityPopup");
			sensitivityConfig.description = T("Adjust the mouse sensitivity for light gun and mouse emulation. "
			                               "Higher values make the cursor move faster.");
			sensitivityConfig.currentValue = &sensitivityTemp;
			sensitivityConfig.minValue = 1;
			sensitivityConfig.maxValue = 500;
			sensitivityConfig.defaultValue = 100;
			sensitivityConfig.format = "%d";
			sensitivityConfig.applyButtonText = T("Apply");
			sensitivityConfig.onApply = nullptr;
			sensitivityConfig.onValueChange = []() {
				config::MouseSensitivity.set(sensitivityTemp);
			};
			sensitivityConfig.iconSize = 20.0f;
			sensitivityConfig.iconSpacing = 8.0f;
			sensitivityConfig.valueWidth = 220.0f;
			sensitivityConfig.sliderWidth = 320.0f;
			sensitivityConfig.showApplyFlag = nullptr;
			sensitivityConfig.hasPendingChanges = false;

			SettingsUI::PopupConfig sensitivityPopupCfg {};
			sensitivityPopupCfg.type = SettingsUI::PopupType::Slider;
			sensitivityPopupCfg.slider = sensitivityConfig;

			RenderGeneralPopupSettingRow(
				Tnop("SensitivitySetting"),
				T("Mouse/light gun sensitivity."),
				sensitivityPopupCfg,
				T(
					"Sensitivity\n"
					"Adjusts mouse and light gun sensitivity.\n"
					"Higher values make the cursor move faster.\n\n"
					"Tip: tune this in actual gameplay (not just menus) and keep it consistent across devices when possible."
				));
		}

#if defined(_WIN32) && !defined(TARGET_UWP)
		RenderGeneralToggleSettingRow(
			"UseRawInput",
			ICON_FA_CROSSHAIRS,
			T("Use Raw Input"),
			T("Supports multiple pointing devices (mice, light guns) and keyboards"),
			static_cast<bool>(config::UseRawInput),
			[](bool enabled) { config::UseRawInput.set(enabled); },
			T(
				"Use Raw Input\n"
				"Enables raw input so multiple pointing devices (mice, light guns) and keyboards can be handled more accurately.\n"
				"Recommended if you use light guns or multiple input devices."
			));
#endif
	}

	ImGui::Spacing();

	if (RenderCollapsingHeader("DreamcastDevicesSection", ICON_FA_PLUG, T("Dreamcast Devices"), ImGuiTreeNodeFlags_DefaultOpen))
	{

		// Helper lambda to get device name
		auto maple_device_name = [](MapleDeviceType type) -> const char* {
			switch (type)
			{
			case MDT_SegaController: return maple_device_types[1];
			case MDT_LightGun: return maple_device_types[2];
			case MDT_Keyboard: return maple_device_types[3];
			case MDT_Mouse: return maple_device_types[4];
			case MDT_TwinStick: return maple_device_types[5];
			case MDT_AsciiStick: return maple_device_types[6];
			case MDT_MaracasController: return maple_device_types[7];
			case MDT_FishingController: return maple_device_types[8];
			case MDT_PopnMusicController: return maple_device_types[9];
			case MDT_RacingController: return maple_device_types[10];
			case MDT_DenshaDeGoController: return maple_device_types[11];
			case MDT_SegaControllerXL: return maple_device_types[12];
			case MDT_None: default: return maple_device_types[0];
			}
		};

		// Helper lambda for device type conversion
		auto maple_device_type_from_index = [](int idx) -> MapleDeviceType {
			switch (idx)
			{
			case 1: return MDT_SegaController;
			case 2: return MDT_LightGun;
			case 3: return MDT_Keyboard;
			case 4: return MDT_Mouse;
			case 5: return MDT_TwinStick;
			case 6: return MDT_AsciiStick;
			case 7: return MDT_MaracasController;
			case 8: return MDT_FishingController;
			case 9: return MDT_PopnMusicController;
			case 10: return MDT_RacingController;
			case 11: return MDT_DenshaDeGoController;
			case 12: return MDT_SegaControllerXL;
			case 0: default: return MDT_None;
			}
		};

		bool is_there_any_xhair = false;

		ImGui::PushFont(settingsTitleFont);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(4, 3));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 4));
		ImVec4 deviceTableBorderColor = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		deviceTableBorderColor.w *= 0.35f;
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, deviceTableBorderColor);
		if (ImGui::BeginTable("dreamcastDevices", 4,
				ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings
				| ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersInnerV,
				ImVec2(0, 0), SettingsLayoutScaled(8)))
		{
			const float comboWidthPadding = ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
			float mainComboWidth = comboWidthPadding;
			for (int i = 0; i < IM_ARRAYSIZE(maple_device_types); i++)
			{
				mainComboWidth = std::max(
					ImGui::CalcTextSize(maple_device_types[i]).x + comboWidthPadding,
					mainComboWidth
				);
			}

			// Externally managed expansion device types
			static constexpr int MDT_DreamPotato = MDT_Count;
			static constexpr int MDT_DreamLink = MDT_Count + 1;

			struct MapleExpansionDeviceDesc
			{
				int deviceType;
				const char* name;
			};

			const std::list<MapleExpansionDeviceDesc> static_maple_expansion_device_types = {
				{MDT_None, T("None")},
				{MDT_SegaVMU, T("Sega VMU")},
				{MDT_PurupuruPack, T("Vibration Pack")},
				{MDT_Microphone, T("Microphone")},
				{MDT_DreamPotato, T("DreamPotato")},
			};

			float expComboWidth = comboWidthPadding;
			for (const auto& devIter : static_maple_expansion_device_types)
			{
				expComboWidth = std::max(
					ImGui::CalcTextSize(devIter.name).x + comboWidthPadding,
					expComboWidth
				);
			}

			// DreamLink device names for main device
			const char* dream_link_names[MAPLE_PORTS]{};
			const char* dream_link_ext_labels[MAPLE_PORTS]{};
			for (int bus = 0; bus < MAPLE_PORTS; bus++)
			{
				auto link = MapleLinkRegistry::GetMapleLink(bus, MAPLE_MAIN_DEV_IDX); // Registered controller, if any
				if (link && (link->dreamlink->getIssueDescription() == nullptr))
				{
					dream_link_names[bus] = link->dreamlink->getName();
					mainComboWidth = std::max(
						ImGui::CalcTextSize(dream_link_names[bus]).x + comboWidthPadding,
						mainComboWidth
					);

					dream_link_ext_labels[bus] = link->dreamlink->getProductName();
					expComboWidth = std::max(
						ImGui::CalcTextSize(dream_link_ext_labels[bus]).x + comboWidthPadding,
						expComboWidth
					);
				}
				else
				{
					dream_link_names[bus] = "";
					dream_link_ext_labels[bus] = "";
				}
			}

			// Settings 3-22: Dreamcast Device Ports (4 ports x device + expansion)
			for (int bus = 0; bus < MAPLE_PORTS; bus++)
			{
				const char* dream_link_name = dream_link_names[bus];
				const bool has_dream_link = (*dream_link_name != '\0');
				const char* selected_name = nullptr;

				if (has_dream_link)
					selected_name = dream_link_name;
				else
					selected_name = maple_device_name(config::MapleMainDevices[bus]);

				ImGui::TableNextRow(ImGuiTableRowFlags_None, SettingsLayoutScaled(24.0f));
				ImGui::TableSetColumnIndex(0);
				ImGui::Text(T("Port %c"), bus + 'A');

				ImGui::TableSetColumnIndex(1);
				char device_name[32];
				snprintf(device_name, sizeof(device_name), "##device%d", bus);
				float w = ImGui::CalcItemWidth() / 3;
				ImGui::PushItemWidth(w);
				ImGui::SetNextItemWidth(mainComboWidth);

				if (has_dream_link)
				{
					// Using real hardware for this - disable selection
					ImGui::BeginDisabled();
				}

				if (ImGui::BeginCombo(device_name, selected_name, ImGuiComboFlags_None))
				{
#if defined(__ANDROID__)
					// BeginCombo has no window-flags argument. Opt this popup into
					// the existing touch-drag handling before submitting its rows.
					ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_DragScrolling;
#endif
					for (int i = 0; i < IM_ARRAYSIZE(maple_device_types); i++)
					{
						bool is_selected = config::MapleMainDevices[bus] == maple_device_type_from_index(i);
						if (ImGui::Selectable(maple_device_types[i], &is_selected))
						{
							config::MapleMainDevices[bus] = maple_device_type_from_index(i);
							g_mapleDevicesChangedInSettings = true;
						}
						if (is_selected)
							ImGui::SetItemDefaultFocus();
					}
#if defined(__ANDROID__)
					scrollWhenDraggingOnVoid();
					windowDragScroll(false);
#endif
					ImGui::EndCombo();
				}

				std::list<MapleExpansionDeviceDesc> maple_expansion_device_types = static_maple_expansion_device_types;
				int port_count = 0;
				if (has_dream_link)
				{
					// Using real hardware for this - done with disable section
					ImGui::EndDisabled();

					port_count = 2;
					maple_expansion_device_types.push_back({MDT_DreamLink, dream_link_ext_labels[bus]});
				}
				else
				{
					port_count = maple_getPortCount(config::MapleMainDevices[bus], MaplePortType::user_accessible);
				}

				for (int port = 0; port < port_count; port++)
				{
					const bool port_has_dream_link = has_dream_link && (MapleLinkRegistry::GetMapleLink(bus, port) != std::nullopt);

					ImGui::TableSetColumnIndex(2 + port);
					snprintf(device_name, sizeof(device_name), "##device%d.%d", bus, port + 1);
					ImguiID _(device_name);
					ImGui::SetNextItemWidth(expComboWidth);
					int subtype = config::MapleExpansionDevices[bus][port];
					if (subtype == MDT_SegaVMU && config::NetworkExpansionDevices[bus][port] == 1) {
						subtype = MDT_DreamPotato;
					}
					else if (port_has_dream_link && (config::DreamLinkSelect[bus][port])) {
						subtype = MDT_DreamLink;
					}

					auto selectedDevIter = std::find_if(
						maple_expansion_device_types.begin(),
						maple_expansion_device_types.end(),
						[subtype](const MapleExpansionDeviceDesc& dev){return subtype == dev.deviceType;}
					);
					if (selectedDevIter == maple_expansion_device_types.end())
						selectedDevIter = maple_expansion_device_types.begin(); // Use None as default

					if (ImGui::BeginCombo(device_name, selectedDevIter->name, ImGuiComboFlags_None))
					{
#if defined(__ANDROID__)
						// Expansion-device lists use the same vertical touch scrolling.
						ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_DragScrolling;
#endif
						for (const auto& devIter : maple_expansion_device_types)
						{
							bool is_selected = (selectedDevIter->deviceType == devIter.deviceType);
							if (ImGui::Selectable(devIter.name, &is_selected))
							{
								subtype = devIter.deviceType;
								if (subtype == MDT_DreamLink) {
									config::DreamLinkSelect[bus][port] = true;
								}
								else if (port_has_dream_link) {
									config::DreamLinkSelect[bus][port] = false;
								}

								if (subtype == MDT_DreamPotato) {
									config::MapleExpansionDevices[bus][port] = MDT_SegaVMU;
									config::NetworkExpansionDevices[bus][port] = 1;
								}
								else {
									if (subtype != MDT_DreamLink) {
										config::MapleExpansionDevices[bus][port] = (MapleDeviceType)subtype;
									}
									config::NetworkExpansionDevices[bus][port] = 0;
								}

								g_mapleDevicesChangedInSettings = true;
							}
							if (is_selected)
								ImGui::SetItemDefaultFocus();
						}
#if defined(__ANDROID__)
						scrollWhenDraggingOnVoid();
						windowDragScroll(false);
#endif
						ImGui::EndCombo();
					}
				}

				// Light gun crosshair color
				if (config::MapleMainDevices[bus] == MDT_LightGun)
				{
					ImGui::TableSetColumnIndex(3);
					snprintf(device_name, sizeof(device_name), "##device%d.xhair", bus);
					ImguiID _(device_name);

					u32 color = config::CrosshairColor[bus];
					float xhairColor[4] {
						(color & 0xff) / 255.f,
						((color >> 8) & 0xff) / 255.f,
						((color >> 16) & 0xff) / 255.f,
						((color >> 24) & 0xff) / 255.f
					};
					bool enabled = color != 0;

					// Toggle switch (clickable row)
					float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
					ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, SettingsLayoutScaled(8.0f));
					bool rowClicked = ImGui::Selectable("##crosshair_row", false,
						ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
						ImVec2(0, rowHeight));
					ImGui::PopStyleVar();

					if (rowClicked)
					{
						enabled = !enabled;
						if (enabled)
						{
							config::CrosshairColor[bus] = (u8)(std::round(xhairColor[0] * 255.f))
									| ((u8)(std::round(xhairColor[1] * 255.f)) << 8)
									| ((u8)(std::round(xhairColor[2] * 255.f)) << 16)
									| ((u8)(std::round(xhairColor[3] * 255.f)) << 24);
							if (config::CrosshairColor[bus] == 0)
								config::CrosshairColor[bus] = 0xC0FFFFFF;
						}
						else
						{
							config::CrosshairColor[bus] = 0;
						}
					}
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
						annotateTestToggle("controls.crosshair", std::to_string(bus).c_str(), T("Crosshair"), enabled);
#endif

					ImGui::SameLine(0, 0);

					// Icon
					SettingIcon(ICON_FA_CROSSHAIRS, SettingsLayoutSize(16.0f, 16.0f));
					ImGui::SameLine(0, SettingsLayoutScaled(6.0f));

					// Label
					ImGui::Text("%s", T("Crosshair"));

					// Toggle switch
					ImGui::SameLine(ImGui::GetContentRegionAvail().x - SettingsLayoutScaled(40));
					float toggleHeight = SettingsLayoutScaled(20.0f);
					float verticalOffset = (rowHeight - toggleHeight) * 0.5f;
					ImVec2 cursorPos = ImGui::GetCursorPos();
					ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y + verticalOffset));
					RenderToggleSwitchVisual(enabled);

					// Color picker on same line
					ImGui::SameLine(0, SettingsLayoutScaled(16));
					bool colorChanged = ImGui::ColorEdit4("##crosshair_color", xhairColor,
						ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf
						| ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoLabel);
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
						annotateTestColor("controls.crosshair.color", std::to_string(bus).c_str(), T("Crosshair color"),
							"##crosshair_color", xhairColor);
#endif

					if (colorChanged)
					{
						if (enabled)
						{
							config::CrosshairColor[bus] = (u8)(std::round(xhairColor[0] * 255.f))
									| ((u8)(std::round(xhairColor[1] * 255.f)) << 8)
									| ((u8)(std::round(xhairColor[2] * 255.f)) << 16)
									| ((u8)(std::round(xhairColor[3] * 255.f)) << 24);
							if (config::CrosshairColor[bus] == 0)
								config::CrosshairColor[bus] = 0xC0FFFFFF;
						}
					}

					is_there_any_xhair |= enabled;
				}

				ImGui::PopItemWidth();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(2);
		ImGui::PopFont();

		// Setting 23: Crosshair Size (10-100)
		{
			DisabledScope scope(!is_there_any_xhair);

			// Temp storage for popup slider
			static int crosshairSizeTemp = config::CrosshairSize.get();

			SettingsUI::PopupSliderConfig crosshairConfig {};
			crosshairConfig.label = T("Crosshair Size");
			crosshairConfig.icon = ICON_FA_CROSSHAIRS;
			crosshairConfig.tooltip = T("Adjust the size of the on-screen crosshair");
			crosshairConfig.popupID = Tnop("CrosshairSizePopup");
			crosshairConfig.description = T("Adjust the size of the on-screen crosshair for light guns. "
											"Only available when a light gun device is connected.");
			crosshairConfig.currentValue = &crosshairSizeTemp;
			crosshairConfig.minValue = 10;
			crosshairConfig.maxValue = 100;
			crosshairConfig.defaultValue = 50;
			crosshairConfig.format = "%d";
			crosshairConfig.applyButtonText = T("Apply");
			crosshairConfig.onApply = nullptr;
			crosshairConfig.onValueChange = []() {
				config::CrosshairSize.set(crosshairSizeTemp);
			};
			crosshairConfig.iconSize = 20.0f;
			crosshairConfig.iconSpacing = 8.0f;
			crosshairConfig.valueWidth = 220.0f;
			crosshairConfig.sliderWidth = 320.0f;
			crosshairConfig.showApplyFlag = nullptr;
			crosshairConfig.hasPendingChanges = false;

			SettingsUI::PopupConfig crosshairPopupCfg {};
			crosshairPopupCfg.type = SettingsUI::PopupType::Slider;
			crosshairPopupCfg.slider = crosshairConfig;

			RenderGeneralPopupSettingRow(
				Tnop("CrosshairSizeSetting"),
				T("Crosshair size for light guns."),
				crosshairPopupCfg,
				T(
					"Crosshair Size\n"
					"Adjusts the size of the on-screen crosshair for light guns.\n"
					"Only available when a light gun device is connected.\n\n"
					"Choose a size that is visible without covering targets."
				),
				!is_there_any_xhair);
		}
	}

	ImGui::Spacing();
	if (RenderCollapsingHeader("VMUSettingsSection", ICON_FA_MICROCHIP, T("VMU Settings"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"PerGameVmu",
			ICON_FA_MICROCHIP,
			T("Per Game VMU A1"),
			T("When enabled, each game has its own VMU on port 1 of controller A"),
			static_cast<bool>(config::PerGameVmu),
			[](bool enabled) { config::PerGameVmu.set(enabled); },
			T(
				"Per Game VMU\n"
				"When enabled, each game has its own VMU on port 1 of controller A.\n"
				"Useful to prevent save-file conflicts between games."
			));

		renderVmuCardManager();

#ifdef USE_DREAMLINK_DEVICES
		RenderGeneralToggleSettingRow(
			"UsePhysicalVmuMemory",
			ICON_FA_MEMORY,
			T("Use External VMU Storage"),
			T("Enables read and write access to physical/external VMU storage via DreamPicoPort or DreamPotato. "
				"VMUs may appear to reconnect after loading state."),
			static_cast<bool>(config::UsePhysicalVmuMemory),
			[](bool enabled) { config::UsePhysicalVmuMemory.set(enabled); },
			T("Use External VMU Storage\n"
			"Enables read and write access to physical/external VMU storage via DreamPicoPort or DreamPotato. "
				"VMUs may appear to reconnect after loading state."),
			game_started);
#endif

#ifdef DREAMPOTATO_INTEGRATED_MODE
		RenderGeneralToggleSettingRow(
			"DreamPotatoIntegratedMode",
			ICON_FA_PLUG,
			T("DreamPotato Integrated Mode"),
			T("Automatically launch DreamPotato (VMU emulator) using the standard VMU file for the given slot."),
			static_cast<bool>(config::DreamPotatoIntegratedMode),
			[](bool enabled) { config::DreamPotatoIntegratedMode.set(enabled); },
			T("DreamPotato Integrated Mode\n"
				"Automatically launch DreamPotato (VMU emulator) using the standard VMU file for the given slot.\n"
				"Applies to all slots which use a DreamPotato device.\n"
				"When this is disabled, DreamPotato devices will connect to a manually-launched standalone instance instead."),
			game_started);

		if (config::DreamPotatoIntegratedMode && hostfs::getDreamPotatoPath().empty())
		{
			ImVec4 warningColor = CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered], "settings.feedback");
			ImGui::TextColored(warningColor, "%s", T("(!) DreamPotato executable not found. Please set DreamPotato Path in General > Custom Paths."));
		}
#endif
	}
}

} // namespace SettingsNew
