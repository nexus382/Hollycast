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

#ifdef USE_GGPO
static void renderGgpoAddress(const std::string& address)
{
	const std::string endpoint = address + ":" + std::to_string(ggpo::SERVER_PORT);
	ImGui::PushID(endpoint.c_str());
	ImGui::TextUnformatted(endpoint.c_str());
	ImGui::SameLine();
	if (ImGui::Button(T("Copy")))
	{
#ifdef __ANDROID__
		os_CopySensitiveTextToClipboard(endpoint.c_str());
#else
		ImGui::SetClipboardText(endpoint.c_str());
#endif
	}
	ImGui::PopID();
}

static void renderGgpoLocalAddresses()
{
	static std::vector<std::string> addresses;
	static double refreshAt = 0;
	const double now = ImGui::GetTime();
	if (now >= refreshAt)
	{
		addresses = ggpo::localIPv4Addresses();
		refreshAt = now + 3;
	}

	ImGui::TextUnformatted(T("Your address to share"));
	ImGui::TextWrapped("%s", T("Same Wi-Fi or home network"));
	if (addresses.empty())
		ImGui::TextDisabled("%s", T("No local IPv4 address found."));
	for (const std::string& address : addresses)
		renderGgpoAddress(address);
}

static void renderGgpoInternetAddress()
{
	if (!ImGui::TreeNode("GGPOInternet", "%s", T("Playing over the internet?")))
		return;

#ifndef FEAT_NO_MINIUPNPC
	static std::future<std::string> lookup;
	static std::string address;
	static bool checked = false;
	if (lookup.valid() && lookup.wait_for(std::chrono::seconds::zero()) == std::future_status::ready)
	{
		address = lookup.get();
		checked = true;
	}
	{
		DisabledScope _(lookup.valid());
		if (ImGui::Button(T("Check for internet address")))
			lookup = std::async(std::launch::async, ggpo::routerPublicIPv4Address);
	}
	if (lookup.valid())
		ImGui::TextDisabled("%s", T("Checking your router..."));
	else if (checked && address.empty())
		ImGui::TextWrapped("%s", T("Your router did not report a public IPv4 address."));
	else if (!address.empty())
	{
		ImGui::TextUnformatted(T("Internet address to share"));
		renderGgpoAddress(address);
	}
#else
	ImGui::TextWrapped("%s", T("Internet address lookup is unavailable on this device."));
#endif
	ImGui::TextWrapped(T("For players on different networks, exchange reachable public IPv4 addresses. Allow UDP port %d on both devices. For Dreamcast, Player 1 also needs TCP port %d for the starting save data. Your router may need port forwarding."),
		ggpo::SERVER_PORT, ggpo::SERVER_PORT + 1);
	ImGui::TreePop();
}
#endif

void renderNetworkTab()
{
	ScopedTwoLineRowStyle networkRowStyle(12.0f, true, 0.5f);


	if (RenderCollapsingHeader("NetworkTypeSection", ICON_FA_GLOBE, T("Network Type"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Network Type Selection - Options Popup
		{
			static int netType = 0;
			if (config::GGPOEnable)
				netType = 1;
			else if (config::NetworkEnable)
				netType = 2;
			else if (config::BattleCableEnable)
				netType = 3;
			else
				netType = 0;

			const char* const networkTypeOptions[] = {
				T("Disabled"),
				T("GGPO"),
				T("Naomi"),
				T("Battle Cable")
			};

			SettingsUI::PopupConfig networkTypeCfg {};
			networkTypeCfg.type = SettingsUI::PopupType::Options;
			networkTypeCfg.options.label = T("Network Type");
			networkTypeCfg.options.icon = ICON_FA_GLOBE;
			networkTypeCfg.options.popupID = Tnop("NetworkTypePopup");
			networkTypeCfg.options.options = networkTypeOptions;
			networkTypeCfg.options.optionCount = IM_ARRAYSIZE(networkTypeOptions);
			networkTypeCfg.options.currentValue = &netType;
			networkTypeCfg.options.valueWidth = 220.0f;
			networkTypeCfg.options.onChange = [](int selectedType) {
				config::GGPOEnable = (selectedType == 1);
				config::NetworkEnable = (selectedType == 2);
				config::BattleCableEnable = (selectedType == 3);
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("NetworkTypeSetting"),
				T("Choose which network feature is active."),
				networkTypeCfg,
				T(
					"Network Type\n"
					"Selects the active network mode:\n"
					"GGPO for rollback netplay, Naomi for Naomi network games, or Battle Cable.\n\n"
					"Only one network type should be enabled at a time."
				));
		}

		// Show configuration section if any network type is enabled
		if (config::GGPOEnable || config::NetworkEnable || config::BattleCableEnable)
		{
			ImGui::Spacing();

			if (RenderCollapsingHeader("ConfigurationSection", ICON_FA_SLIDERS, T("Configuration"), ImGuiTreeNodeFlags_DefaultOpen))
			{
				// GGPO Settings
				if (config::GGPOEnable)
				{
					config::NetworkEnable = false;

					RenderGeneralToggleSettingRow(
						"PlayAsPlayer1",
						ICON_FA_USER,
						T("Play as Player 1"),
						T("Host the game session"),
						static_cast<bool>(config::ActAsServer),
						[](bool enabled) { config::ActAsServer = enabled; },
						T(
							"Play as Player 1\n"
							"When enabled, you host the session (Player 1).\n"
							"Deselect to connect as Player 2."
						));

#ifdef USE_GGPO
					renderGgpoLocalAddresses();
#endif

					RenderGeneralTextInputSettingRow(
						"GGPONetPeerRow",
						ICON_FA_NETWORK_WIRED,
						T("Other player's address"),
						T("Enter the address they shared with you."),
						[]() {
							InputText("##PeerInput", &config::NetworkServer.get(),
								ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
								dnsCharFilter);
						},
						T("Enter the other player's IP address and optional port."));

					ImGui::TextWrapped("%s", T("Exchange addresses, choose Player 1 on one device, then start the same game on both."));
#ifdef USE_GGPO
					renderGgpoInternetAddress();
#endif

					if (config::ActAsServer)
						RenderGeneralToggleSettingRow(
							"GGPOAutoDelay",
							ICON_FA_CLOCK,
							T("Test Connection and Set Delay"),
							T("Measure the connection before play and adjust frame delay based on how often the game waits for peer inputs."),
							static_cast<bool>(config::GGPOAutoDelay),
							[](bool enabled) { config::GGPOAutoDelay = enabled; },
							T("Player 1 controls this option. Each player keeps their saved manual delay for sessions that do not use the test."));

					// Frame Delay
					{
						static int ggpoDelayTemp = config::GGPODelay.get();

						SettingsUI::PopupSliderConfig ggpoDelayCfg {};
						ggpoDelayCfg.label = T("Frame Delay");
						ggpoDelayCfg.icon = ICON_FA_CLOCK;
						ggpoDelayCfg.popupID = Tnop("GGPOFrameDelayPopup");
						ggpoDelayCfg.description = T("Sets Frame Delay, advisable for sessions with ping >100 ms.");
						ggpoDelayCfg.currentValue = &ggpoDelayTemp;
						ggpoDelayCfg.minValue = 0;
						ggpoDelayCfg.maxValue = 20;
						ggpoDelayCfg.defaultValue = 0;
						ggpoDelayCfg.format = "%d";
						ggpoDelayCfg.valueWidth = 220.0f;
						ggpoDelayCfg.sliderWidth = 320.0f;
						ggpoDelayCfg.onValueChange = []() {
							config::GGPODelay.set(ggpoDelayTemp);
						};

						SettingsUI::PopupConfig ggpoDelayPopupCfg {};
						ggpoDelayPopupCfg.type = SettingsUI::PopupType::Slider;
						ggpoDelayPopupCfg.slider = ggpoDelayCfg;

						RenderGeneralPopupSettingRow(
							Tnop("GGPOFrameDelaySetting"),
							T("Extra buffering for higher ping sessions."),
							ggpoDelayPopupCfg,
							T(
								"Frame Delay\n"
								"Adds a fixed frame delay to help stabilize gameplay for higher-latency connections.\n"
								"Often advisable when ping is > 100 ms.\n\n"
								"Too much delay increases input lag; use the lowest value that feels stable."
							));
					}

					// Analog Axes Configuration
					if (RenderCollapsingHeader("AnalogAxesSection", ICON_FA_GAMEPAD, T("Analog Axes"), ImGuiTreeNodeFlags_DefaultOpen))
					{
						{
							const char* axesOptions[] = { T("Disabled"), T("Horizontal"), T("Full") };
							int axesSelection = config::GGPOAnalogAxes.get();
							if (axesSelection < 0 || axesSelection > 2)
								axesSelection = 0;

							SettingsUI::PopupConfig axesCfg {};
							axesCfg.type = SettingsUI::PopupType::Options;
							axesCfg.options.label = T("Left Thumbstick");
							axesCfg.options.icon = ICON_FA_GAMEPAD;
							axesCfg.options.popupID = Tnop("GGPOAnalogAxesPopup");
							axesCfg.options.options = axesOptions;
							axesCfg.options.optionCount = IM_ARRAYSIZE(axesOptions);
							axesCfg.options.currentValue = &axesSelection;
							axesCfg.options.valueWidth = 220.0f;
							axesCfg.options.onChange = [&](int selectedIndex) {
								config::GGPOAnalogAxes.set(selectedIndex);
								return true;
							};

							RenderGeneralPopupSettingRow(
								Tnop("GGPOAnalogAxesSetting"),
								T("Control how the left thumbstick is used in GGPO."),
								axesCfg,
								T(
									"Left Thumbstick (GGPO)\n"
									"Configures how the left thumbstick is mapped during GGPO sessions:\n"
									"Disabled, Horizontal only, or Full analog axes.\n\n"
									"Choose the option that matches the game and your preferred control style."
								));
						}

						RenderGeneralToggleSettingRow(
							"EnableChat",
							ICON_FA_COMMENTS,
							T("Enable Chat"),
							T("Open chat on message received"),
							static_cast<bool>(config::GGPOChat),
							[](bool enabled) { config::GGPOChat.set(enabled); },
							T(
								"Enable Chat\n"
								"Opens the chat window when a message is received during GGPO sessions."
							));

						if (config::GGPOChat)
						{
							RenderGeneralToggleSettingRow(
								"GGPOChatTimeoutToggle",
								ICON_FA_HOURGLASS,
								T("Chat Auto-Close"),
								T("Automatically close chat window"),
								static_cast<bool>(config::GGPOChatTimeoutToggle),
								[](bool enabled) { config::GGPOChatTimeoutToggle = enabled; },
								T(
									"Chat Auto-Close\n"
									"Automatically closes the chat window after a timeout.\n"
									"Useful if you want chat notifications without leaving the overlay open."
								));

							if (config::GGPOChatTimeoutToggle)
							{
								ImGui::PushID("GGPOChatTimeoutRow");
								const bool rowActivated = BeginTwoLineSettingRow(
									"##row",
									T(
										"Chat Window Timeout\n"
										"Controls how long the chat window stays open after receiving a new message.\n\n"
										"Increase this if you want more time to read messages.\n"
										"Decrease it if you only want brief notifications without the overlay staying on screen."
									));
								const ImVec2 line1Start = BeginTwoLineSettingRowContent();

								SettingIcon(ICON_FA_HOURGLASS, SettingsLayoutSize(20.0f, 20.0f));
								ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
								ImGui::PushFont(largeFont);
								ImGui::TextUnformatted(T("Chat Window Timeout (s)"));
								ImGui::PopFont();

								const float inputWidth = SettingsLayoutScaled(220.0f);
								ImGui::SameLine(RightColumnX(inputWidth));
								if (rowActivated)
									ImGui::SetKeyboardFocusHere();
								ImGui::SetNextItemWidth(inputWidth);
								char chatTimeout[256];
								snprintf(chatTimeout, sizeof(chatTimeout), "%d", (int)config::GGPOChatTimeout);
								if (InputText("##ChatTimeoutInput", chatTimeout, sizeof(chatTimeout), ImGuiInputTextFlags_CharsDecimal))
									config::GGPOChatTimeout.set(atoi(chatTimeout));

								RenderTwoLineSettingDescription(line1Start, T("Sets duration that chat window stays open after new message is received"));
								EndTwoLineSettingRow();
								ImGui::PopID();
							}
						}
					}

					RenderGeneralToggleSettingRow(
						"NetworkStats",
						ICON_FA_CHART_BAR,
						T("Network Statistics"),
						T("Display netplay stats on screen"),
						static_cast<bool>(config::NetworkStats),
						[](bool enabled) { config::NetworkStats = enabled; },
						T(
							"Network Statistics\n"
							"Shows a small on-screen overlay with live netplay information.\n"
							"This is useful for diagnosing stutter, desyncs, or “why does this feel laggy?” situations.\n\n"
							"If you are tuning settings for a stable experience, enable this along with the FPS counter and test during real gameplay.\n"
							"Look for stable latency/jitter and minimal dropped or delayed packets."
						));
				}
				// Naomi Network Settings
				else if (config::NetworkEnable)
				{
					int role = 0;
					if (!config::ActAsServer)
						role = config::NaomiSatellite ? 2 : 1;

					const char* const networkRoles[] = {
						T("Server"),
						T("Client"),
						T("Satellite")
					};

					SettingsUI::PopupConfig networkRoleCfg {};
					networkRoleCfg.type = SettingsUI::PopupType::Options;
					networkRoleCfg.options.label = T("Network Role");
					networkRoleCfg.options.icon = ICON_FA_GLOBE;
					networkRoleCfg.options.popupID = Tnop("NetworkRolePopup");
					networkRoleCfg.options.options = networkRoles;
					networkRoleCfg.options.optionCount = IM_ARRAYSIZE(networkRoles);
					networkRoleCfg.options.currentValue = &role;
					networkRoleCfg.options.valueWidth = 220.0f;
					networkRoleCfg.options.onChange = [](int selectedType) {
						switch (selectedType) {
						case 0:
							config::ActAsServer = true;
							config::NaomiSatellite = false;
							break;
						case 1:
							config::ActAsServer = false;
							config::NaomiSatellite = false;
							break;
						case 2:
							config::ActAsServer = false;
							config::NaomiSatellite = true;
							break;
						}
						return true;
					};
					RenderGeneralPopupSettingRow(
						Tnop("NetworkRoleSetting"),
						T("Choose the network role."),
						networkRoleCfg,
						T(
							"Network Role\n"
							"Selects the active network role:\n"
							"Server to create a local server for Naomi network games, client to connect to a server, "
							"satellite to monitor games that support it (Virtual-On Oratorio Tangram and Club Kart)\n\n"
							"If you cannot connect to each other, check firewalls/NAT rules and confirm both players are using the same game/version.\n"
							"Use the Local Port setting if you need to match a specific port or avoid conflicts."
						)
					);

					// Server Address (only when not acting as server)
					if (!config::ActAsServer)
					{
						RenderGeneralTextInputSettingRow(
							"NaomiServerRow",
							ICON_FA_NETWORK_WIRED,
							T("Server"),
							T("The server to connect to. Leave blank to find a server automatically on the default port"),
							[]() {
								InputText("##ServerInput", &config::NetworkServer.get(),
									ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
									dnsCharFilter);
							},
							T(
								"Naomi Netplay: Server Address\n"
								"Enter the host to connect to (for example: a hostname or an IP address).\n"
								"If a port is supported by your setup, it is typically written as `host:port`.\n\n"
								"Leave this field blank to use automatic discovery (when available) on the default port.\n"
								"If you have connection issues, try entering the host explicitly and verify your Local Port and firewall/NAT rules."
							));
					}

					// Local Port
					{
						ImGui::PushID("NaomiLocalPortRow");
						const bool rowActivated = BeginTwoLineSettingRow(
							"##row",
							T(
								"Local Port\n"
								"Sets the local UDP port used for Naomi netplay / Battle Cable connections.\n\n"
								"If you cannot connect, make sure both players are using compatible settings and that your firewall/router is not blocking this port.\n"
								"Only change this if you have a port conflict or you need to match a specific setup."
							));
						const ImVec2 line1Start = BeginTwoLineSettingRowContent();

						SettingIcon(ICON_FA_KEY, SettingsLayoutSize(20.0f, 20.0f));
						ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
						ImGui::PushFont(largeFont);
						ImGui::TextUnformatted(T("Local Port"));
						ImGui::PopFont();

						const float inputWidth = SettingsLayoutScaled(220.0f);
						ImGui::SameLine(RightColumnX(inputWidth));
						if (rowActivated)
							ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(inputWidth);
						char localPort[256];
						snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
						if (InputText("##LocalPortInput", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal))
							config::LocalPort.set(atoi(localPort));

						RenderTwoLineSettingDescription(line1Start, T("The local UDP port to use"));
						EndTwoLineSettingRow();
						ImGui::PopID();
					}
				}
			}
		}
		// Battle Cable Settings
		else if (config::BattleCableEnable)
		{
#if defined(USE_ICE) && defined(USE_WEBSOCKET)
			if (ImGui::BeginTabBar("battleMode", ImGuiTabBarFlags_NoTooltip))
			{
				// Match Code Tab
				const std::string matchCodeTabLabel = std::string(T("Match Code")) + "###Match Code";
				if (ImGui::BeginTabItem(matchCodeTabLabel.c_str()))
				{
					ice::State state = ice::getState();
					ImGuiInputTextFlags textFlags = state == ice::Offline ?
						ImGuiInputTextFlags_CharsNoBlank : ImGuiInputTextFlags_ReadOnly;
					static std::string matchCode;
					const std::string matchCodeLabel = std::string(T("Code")) + "###Match Code";
					InputText(matchCodeLabel.c_str(), &matchCode, textFlags);
					ImGui::SameLine();
					ShowFooterHelpMarker(T("Choose a unique word or number and share it with your opponent"));

					if (state == ice::Offline)
					{
						if (ImGui::Button(T("Connect")) && !matchCode.empty())
							ice::init(matchCode, true);
					}
					else
					{
						if (ImGui::Button(T("Disconnect")))
							try { ice::term(); } catch (...) {}
					}

					// Display connection status
					std::string status;
					switch (state)
					{
					case ice::Offline:
						status = ice::getStatusText();
						break;
					case ice::Online:
						status = T("Waiting at meeting point...");
						break;
					case ice::ChalAccepted:
						status = T("Preparing game...");
						break;
					case ice::Playing:
						status = T("Playing ") + matchCode + " (" + ice::getStatusText() + ")";
						break;
					default:
						break;
					}
					ImGui::TextDisabled("%s", status.c_str());

						RenderGeneralToggleSettingRow(
							"NetworkStatsBattleCable",
							ICON_FA_CHART_BAR,
							T("Network Stats"),
							T("Display network statistics"),
							static_cast<bool>(config::NetworkStats),
							[](bool enabled) { config::NetworkStats = enabled; },
							T(
								"Network Statistics\n"
								"Shows a small on-screen overlay with live connection information.\n\n"
								"Use this to diagnose lag spikes, unstable connections, or dropped packets during Battle Cable sessions.\n"
								"For best results, enable the FPS counter too and test during actual gameplay."
							));

					ImGui::EndTabItem();
				}

				// Manual Tab
				const std::string manualTabLabel = std::string(T("Manual")) + "###Manual";
				if (ImGui::BeginTabItem(manualTabLabel.c_str()))
				{
#endif
					RenderGeneralTextInputSettingRow(
						"BattlePeerRow",
						ICON_FA_NETWORK_WIRED,
						T("Peer"),
						T("The peer to connect to. Leave blank to find a player automatically on the default port"),
						[]() {
							InputText("##BattlePeerInput", &config::NetworkServer.get(),
								ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
								dnsCharFilter);
						},
						T(
							"Battle Cable: Peer Address\n"
							"Enter the opponent to connect to (hostname or IP address).\n"
							"If your setup supports specifying a port, it is typically written as `host:port`.\n\n"
							"Leave this field blank to use automatic discovery (when available) on the default port.\n"
							"If matchmaking fails, try entering the peer explicitly and verify your Local Port and firewall/NAT rules."
						));

					{
						ImGui::PushID("BattleLocalPortRow");
						const bool rowActivated = BeginTwoLineSettingRow(
							"##row",
							T(
								"Local Port\n"
								"Sets the local UDP port used for Battle Cable sessions.\n\n"
								"If you cannot connect, verify firewall/router rules and ensure both peers are using the same port.\n"
								"Leave this at the default unless you have a reason to change it."
							));
						const ImVec2 line1Start = BeginTwoLineSettingRowContent();

						SettingIcon(ICON_FA_KEY, SettingsLayoutSize(20.0f, 20.0f));
						ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
						ImGui::PushFont(largeFont);
						ImGui::TextUnformatted(T("Local Port"));
						ImGui::PopFont();

						const float inputWidth = SettingsLayoutScaled(220.0f);
						ImGui::SameLine(RightColumnX(inputWidth));
						if (rowActivated)
							ImGui::SetKeyboardFocusHere();
						ImGui::SetNextItemWidth(inputWidth);
						char localPort[256];
						snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
						if (InputText("##BattleLocalPortInput", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal))
							config::LocalPort.set(atoi(localPort));

						RenderTwoLineSettingDescription(line1Start, T("The local UDP port to use"));
						EndTwoLineSettingRow();
						ImGui::PopID();
					}
#if defined(USE_ICE) && defined(USE_WEBSOCKET)
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
#endif

			RenderGeneralToggleSettingRow(
				"ActAsMaster",
				ICON_FA_CROWN,
				T("Act as Master"),
				T("Host for Maximum Speed mode"),
				static_cast<bool>(config::ActAsServer),
				[](bool enabled) { config::ActAsServer = enabled; },
				T(
					"Maximum Speed: Master Peer\n"
					"Only used by Maximum Speed mode.\n"
					"Exactly one peer must be the master for the session to work correctly.\n\n"
					"If you run into connection problems, try switching which player is master.\n"
					"In general, the player with the more stable connection and lower latency is a good choice."
				));
		}
	}

	ImGui::Spacing();

	if (RenderCollapsingHeader("NetworkOptionsSection", ICON_FA_GEAR, T("Network Options"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"EnableUPnP",
			ICON_FA_NETWORK_WIRED,
			T("Enable UPnP"),
			T("Automatically configure your network router for netplay"),
			static_cast<bool>(config::EnableUPnP),
			[](bool enabled) { config::EnableUPnP.set(enabled); },
			T(
				"Enable UPnP\n"
				"Attempts to automatically create the required port mappings on your router.\n\n"
				"This can make hosting/connecting easier on home networks, but it depends on your router supporting UPnP and allowing it.\n"
				"If it does not work (or if you prefer not to use UPnP), disable this and use manual port forwarding instead."
			));

		RenderGeneralToggleSettingRow(
			"BroadcastOutputs",
			ICON_FA_TOWER_BROADCAST,
			T("Broadcast Digital Outputs"),
			T("Broadcast audio/video"),
			static_cast<bool>(config::NetworkOutput),
			[](bool enabled) { config::NetworkOutput.set(enabled); },
			T(
				"Broadcast Digital Outputs\n"
				"Exports certain arcade I/O “outputs” over the network (for example, values used by some Naomi driving/force-feedback setups).\n\n"
				"This is not a general gameplay video/audio streaming feature.\n"
				"Enable it only if you are using external tooling or hardware that expects network output data."
			));

		RenderGeneralToggleSettingRow(
			"BroadbandAdapter",
			ICON_FA_SERVER,
			T("Broadband Adapter"),
			T("Emulate the Ethernet Broadband Adapter (BBA) instead of the Modem"),
			static_cast<bool>(config::EmulateBBA),
			[](bool enabled) { config::EmulateBBA.set(enabled); },
			T(
				"Broadband Adapter (BBA)\n"
				"Emulates the Dreamcast Ethernet Broadband Adapter (BBA) instead of the modem.\n\n"
				"Use this for games and features that expect Ethernet networking.\n"
				"If a title expects the modem path (or you are troubleshooting connectivity), try toggling this and re-testing."
			),
			game_started);

		RenderGeneralToggleSettingRow(
			"DCNet",
			ICON_FA_CLOUD,
			T("DCNet Cloud"),
			T("Dreamcast Internet via cloud"),
			static_cast<bool>(config::UseDCNet),
			[](bool enabled) { config::UseDCNet.set(enabled); },
			T(
				"DCNet Cloud\n"
				"Routes Dreamcast networking through a cloud-backed service instead of relying entirely on your local network configuration.\n\n"
				"This can simplify getting “online” in certain scenarios, but it also changes the networking path.\n"
				"If you have trouble connecting, try toggling this setting and re-testing with the same game and the same network options."
			));

		// ISP Username
		std::string& ispUsername = config::ISPUsername.get();
		RenderGeneralTextInputSettingRow(
			"ISPUsernameRow",
			ICON_FA_USER,
			T("ISP Username"),
			T("The ISP user name stored in the console Flash RAM. Used by some online games as the player name. Leave blank to keep the current Flash RAM value"),
			[&]() {
				InputText("##ISPUsernameInput", &ispUsername,
					ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
					[](ImGuiInputTextCallbackData* data) {
						return static_cast<int>(data->EventChar <= ' ' || data->EventChar > '~');
					});
				auto it = std::remove_if(ispUsername.begin(), ispUsername.end(),
					[](char c) { return c <= ' ' || c > '~'; });
				ispUsername.erase(it, ispUsername.end());
			});

#if !defined(NDEBUG) || defined(DEBUGFAST)
		// DNS Server (debug builds only)
		RenderGeneralTextInputSettingRow(
			"DNSServerRow",
			ICON_FA_ADDRESS_BOOK,
			T("DNS Server"),
			T("DNS server name or IP address"),
			[]() {
				InputText("##DNSInput", &config::DNS.get(),
					ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
					dnsCharFilter);
			},
			T(
				"DNS Server\n"
				"Overrides the DNS server used by the emulated network stack.\n\n"
				"Only change this if you know you need a specific DNS provider or you are debugging connectivity.\n"
				"Note: When DCNet Cloud is enabled, DNS behavior may be handled differently and this field may not apply."
			),
			config::UseDCNet);
#endif
	}

#ifdef NAOMI_MULTIBOARD
	// Multiboard Screens
	ImGui::Spacing();

	if (RenderCollapsingHeader("MultiboardSection", ICON_FA_TABLE, T("Multiboard"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		OptionRadioButton<int>(T("1 (Twin, Satellite)"), config::MultiboardSlaves, 1,
			T("One screen configuration (F355 Twin, Derby Owners Club satellite)"));
		ImGui::SameLine();
		OptionRadioButton<int>(T("2+ (Deluxe, Main screen)"), config::MultiboardSlaves, 2,
			T("Two or three screens configuration (Airline Pilot, Derby Owners Club main screen, F355 Deluxe, Sega Strike Fighter)"));
	}
#endif
}


} // namespace SettingsNew
