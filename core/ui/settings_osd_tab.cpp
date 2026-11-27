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

#ifdef HAVE_PHONE_VMU
void renderPhoneVmuSettings()
{
	if (!RenderCollapsingHeader("PhoneVmuSection", ICON_FA_MOBILE_SCREEN, T("Phone VMU"), ImGuiTreeNodeFlags_DefaultOpen))
		return;

	RenderGeneralToggleSettingRow("ConnectPhone", ICON_FA_MOBILE_SCREEN,
		T("Connect Phone"), T("Use your phone as a VMU display and play VMU mini-games."),
		phone_vmu::status().running, [](bool enabled) {
			if (enabled)
			{
				phone_vmu::setPairing(config::PhoneVmuToken.get(), config::PhoneVmuPort.get());
				phone_vmu::start();
				// Persist a first-time generated token so the address survives restarts.
				if (config::PhoneVmuToken.get() != phone_vmu::pairingToken())
					config::PhoneVmuToken.set(phone_vmu::pairingToken());
			}
			else
				phone_vmu::stop();
		}, T("Connect Phone\nScan the QR code with your phone's camera on the same network. Mirror the LCD or play a VMU mini-game with automatic progress sync. The card is unplugged while the phone uses it; choose Return to Dreamcast to reconnect it. Turn this off to stop sharing."));

	const auto connection = phone_vmu::status();
	if (!connection.error.empty())
		ImGui::TextWrapped("%s: %s", T("Unable to share the VMU display"), connection.error.c_str());
	if (!connection.running || connection.urls.empty())
		return;

	ImGui::TextUnformatted(connection.connected ? T("Phone connected") : T("Waiting for phone"));
	if (connection.lastSyncSlot >= 0 && connection.lastSyncSlot < 8 && !connection.lastSyncTime.empty())
	{
		const char slotLabel[] = { static_cast<char>('A' + connection.lastSyncSlot / 2),
				static_cast<char>('1' + connection.lastSyncSlot % 2), '\0' };
		ImGui::TextWrapped("%s: VMU %s at %s", T("Last phone save synced"), slotLabel, connection.lastSyncTime.c_str());
	}
	ImGui::TextWrapped("%s", T("This address stays the same when Hollycast restarts, so a paired phone can reconnect without a new scan."));
	if (ImGui::Button(T("New pairing address")))
		config::PhoneVmuToken.set(phone_vmu::regeneratePairing());
	ImGui::SameLine();
	ImGui::TextWrapped("%s", T("Disconnects all paired phones. Scan the new code to pair again."));
	// Restoring replaces that VMU, so only offer it while no game is loaded.
	{
		static std::string phoneRestoreMessage;
		namespace fs = std::filesystem;
		const char *ports[] = { "A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2" };
		static std::vector<phone_vmu::CardBackup> newestBackups;
		static double lastBackupScan = -1.0;
		const double now = ImGui::GetTime();
		if (!game_started && (lastBackupScan < 0.0 || now - lastBackupScan >= 1.0))
		{
			std::vector<fs::path> directories;
			for (const char *port : ports)
			{
				const fs::path vmuPath = fs::u8path(hostfs::getVmuPath(port, true));
				const fs::path dir = vmuPath.parent_path().empty() ? fs::path(".") : vmuPath.parent_path();
				if (std::find(directories.begin(), directories.end(), dir) == directories.end())
					directories.push_back(dir);
			}
			newestBackups = phone_vmu::newestBackups(directories);
			lastBackupScan = now;
		}
		for (const phone_vmu::CardBackup& backup : newestBackups)
		{
			const fs::path& vmuPath = backup.card;
			if (game_started)
				continue;
			const std::string label = std::string(T("Restore VMU backup for ")) + vmuPath.filename().u8string();
			if (ImGui::Button(label.c_str()))
			{
				const bool restored = phone_vmu::restoreCard(backup, std::time(nullptr));
				if (restored)
					lastBackupScan = -1.0;
				phoneRestoreMessage = std::string(restored ? T("Restored VMU ") : T("Could not restore VMU "))
						+ vmuPath.filename().u8string() + ".";
			}
		}
		if (!newestBackups.empty() && game_started)
			ImGui::TextWrapped("%s", T("Phone VMU backups found. Stop emulation to restore one."));
		if (!phoneRestoreMessage.empty())
			ImGui::TextWrapped("%s", phoneRestoreMessage.c_str());
	}
	ImGui::TextWrapped("%s", T("Connect your phone to the same Wi-Fi and scan this code. To link two VMUs, open the A1 and A2 addresses below in separate browser panes before starting the mini-games."));
	static int selectedAddress = 0;
	selectedAddress = std::clamp(selectedAddress, 0, static_cast<int>(connection.urls.size()) - 1);
	if (connection.urls.size() > 1)
	{
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo(T("##PhoneVmuAddress"), connection.urls[selectedAddress].c_str()))
		{
			for (int i = 0; i < static_cast<int>(connection.urls.size()); ++i)
				if (ImGui::Selectable(connection.urls[i].c_str(), i == selectedAddress))
					selectedAddress = i;
			ImGui::EndCombo();
		}
		ImGui::TextWrapped("%s", T("If the phone cannot connect, choose the address for your Wi-Fi or Ethernet network."));
	}
	const std::string& url = connection.urls[selectedAddress];
	static std::string cachedUrl;
	static std::unique_ptr<qrcodegen::QrCode> qr;
	if (cachedUrl != url)
	{
		qr = std::make_unique<qrcodegen::QrCode>(qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM));
		cachedUrl = url;
	}
	// Integral module sizes and a four-module quiet zone keep the code scannable
	// at every UI scale, without creating a renderer-specific texture.
	const int modules = qr->getSize() + 8;
	const float cell = std::max(1.f, std::floor(std::min(ImGui::GetContentRegionAvail().x, SettingsLayoutScaled(260.f)) / modules));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 size(modules * cell, modules * cell);
	auto *draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(origin, origin + size, IM_COL32(255, 255, 255, 255));
	for (int y = 0; y < qr->getSize(); ++y)
		for (int x = 0; x < qr->getSize(); ++x)
			if (qr->getModule(x, y))
			{
				const ImVec2 pos = origin + ImVec2((x + 4) * cell, (y + 4) * cell);
				draw->AddRectFilled(pos, pos + ImVec2(cell, cell), IM_COL32(0, 0, 0, 255));
			}
	ImGui::Dummy(size);
	const char *copyLabels[] = { T("COPY A1 URL"), T("COPY A2 URL") };
	for (int i = 0; i < 2; ++i)
	{
		const std::string cardUrl = url + (i == 0 ? "#A1" : "#A2");
		ImGui::TextWrapped("%s", cardUrl.c_str());
		ImGui::PushID(i);
		if (ImGui::Button(copyLabels[i]))
		{
#ifdef __ANDROID__
			os_CopySensitiveTextToClipboard(cardUrl.c_str());
#else
			ImGui::SetClipboardText(cardUrl.c_str());
#endif
		}
		ImGui::PopID();
	}
	ImGui::TextWrapped("%s", T("Sharing stays on when you close settings. Turn off Connect Phone to disconnect all phones. Allow Hollycast through your firewall on private networks if needed."));
}
#endif

int getResourceMonitorMode()
{
	return config::loadInt("rend", "ResourceMonitorMode", 0);
}

void setResourceMonitorMode(int mode)
{
	config::saveInt("rend", "ResourceMonitorMode", mode);
}


void renderOnScreenDisplayTab()
{
	ScopedTwoLineRowStyle osdRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("On Screen Display"));
	ImGui::Separator();

	if (RenderCollapsingHeader("OsdInGameSection", ICON_FA_DISPLAY, T("In-Game Display"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"ShowVMU",
			ICON_FA_GAMEPAD,
			T("Show VMU In-Game"),
			T("Show VMU LCD screens during gameplay."),
			static_cast<bool>(config::FloatVMUs),
			[](bool enabled) { config::FloatVMUs.set(enabled); },
			T("Show VMU In-Game\nDisplays the VMU LCD screens while in-game."));
#ifdef HAVE_MOVABLE_VMUS
		ImGui::PushID("MovableVMUs");
		const bool moveRowActivated = BeginTwoLineSettingRow("##row",
			T("Enable to move VMU screens. Turn off to lock them in place. Use reset to restore their default positions."));
		const ImVec2 moveLineStart = BeginTwoLineSettingRowContent();
		SettingIcon(ICON_FA_GAMEPAD, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(T("Move VMU"));
		ImGui::PopFont();
		const float resetWidth = SettingsLayoutScaled(26.0f);
		const float resetHeight = SettingsLayoutScaled(22.0f);
		const float toggleWidth = SettingsLayoutScaled(50.0f);
		ImGui::SameLine(RightColumnX(toggleWidth + resetWidth + SettingsLayoutScaled(8.0f)));
		const ImVec2 togglePos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(togglePos.x,
			togglePos.y + (TwoLineSettingContentHeight() - SettingsLayoutScaled(24.0f)) * 0.5f));
		RenderToggleSwitchVisual(config::MovableVMUs);
		ImGui::SameLine(RightColumnX(resetWidth));
		ImGui::SetCursorPosY(moveLineStart.y + (TwoLineSettingContentHeight() - resetHeight) * 0.5f);
		const bool resetPressed = SettingsIconButton("##ResetVmuPositions", ICON_FA_ARROWS_ROTATE, ImVec2(resetWidth, resetHeight));
		const bool resetHovered = ImGui::IsItemHovered();
		if (resetHovered || ImGui::IsItemFocused())
			SetSettingsFooterText(T("Reset VMU screens to their default positions."));
		// The reset is a separate action inside the selectable row.
		if (moveRowActivated && !resetHovered && !resetPressed)
		{
			config::VmuLayoutEnabled.set(true);
			config::MovableVMUs.set(!config::MovableVMUs.get());
			if (config::MovableVMUs)
				config::FloatVMUs.set(true);
		}
		if (resetPressed)
			vmu_window::resetPositions();
		RenderTwoLineSettingDescription(moveLineStart, T("Turn off to lock VMU screens in place."));
		EndTwoLineSettingRow();
		ImGui::PopID();
		if ((config::MovableVMUs || config::VmuLayoutEnabled) && config::FloatVMUs)
		{
			SettingsUI::PopupConfig vmuSizeCfg {};
			vmuSizeCfg.type = SettingsUI::PopupType::Slider;
			vmuSizeCfg.slider.label = T("VMU Size");
			vmuSizeCfg.slider.icon = ICON_FA_RULER_HORIZONTAL;
			vmuSizeCfg.slider.popupID = Tnop("VmuWindowSizePopup");
			vmuSizeCfg.slider.description = T("Resize all movable VMU screens live.");
			vmuSizeCfg.slider.currentValue = &config::VmuWindowSize.get();
			vmuSizeCfg.slider.minValue = vmu_window::MinSizePercent;
			vmuSizeCfg.slider.maxValue = vmu_window::MaxSizePercent;
			vmuSizeCfg.slider.defaultValue = vmu_window::DefaultSizePercent;
			vmuSizeCfg.slider.format = "%d%%";
			vmuSizeCfg.slider.valueWidth = 220.0f;
			vmuSizeCfg.slider.sliderWidth = 320.0f;
			RenderGeneralPopupSettingRow(Tnop("VmuWindowSizeSetting"),
				T("Scale VMU screens independently of UI scaling."), vmuSizeCfg,
				T("100% keeps the default VMU size. Changes apply immediately and are saved with your settings."));
			SettingsUI::PopupConfig vmuTransparencyCfg {};
			vmuTransparencyCfg.type = SettingsUI::PopupType::Slider;
			vmuTransparencyCfg.slider.label = T("VMU Transparency");
			vmuTransparencyCfg.slider.icon = ICON_FA_EYE;
			vmuTransparencyCfg.slider.popupID = Tnop("VmuWindowTransparencyPopup");
			vmuTransparencyCfg.slider.description = T("Adjust how transparent VMU screens are.");
			vmuTransparencyCfg.slider.currentValue = &config::VmuWindowTransparency.get();
			vmuTransparencyCfg.slider.minValue = vmu_window::MinTransparencyPercent;
			vmuTransparencyCfg.slider.maxValue = vmu_window::MaxTransparencyPercent;
			vmuTransparencyCfg.slider.defaultValue = vmu_window::DefaultTransparencyPercent;
			vmuTransparencyCfg.slider.format = "%d%%";
			vmuTransparencyCfg.slider.valueWidth = 220.0f;
			vmuTransparencyCfg.slider.sliderWidth = 320.0f;
			RenderGeneralPopupSettingRow(Tnop("VmuWindowTransparencySetting"),
				/* xgettext:no-c-format */ T("0% is solid. 100% is invisible."), vmuTransparencyCfg,
				T("25% keeps the default transparency. Desktop VMUs dim further in menus. Changes apply immediately and are saved with your settings."));
		}
#endif
	}

#ifdef HAVE_PHONE_VMU
	renderPhoneVmuSettings();
#endif

	if (RenderCollapsingHeader("OsdPerformanceSection", ICON_FA_CHART_LINE, T("Performance Overlay"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		const std::array<const char*, 3> overlayModeLabels { T("Off"), T("Minimal"), T("Full") };
		int overlayModeSelection = std::clamp(getResourceMonitorMode(), 0, 2);
		SettingsUI::PopupConfig overlayModeCfg {};
		overlayModeCfg.type = SettingsUI::PopupType::Options;
		overlayModeCfg.options.label = T("Overlay Mode");
		overlayModeCfg.options.icon = ICON_FA_CHART_LINE;
		overlayModeCfg.options.popupID = Tnop("ResourceMonitorModePopup");
		overlayModeCfg.options.options = overlayModeLabels.data();
		overlayModeCfg.options.optionCount = static_cast<int>(overlayModeLabels.size());
		overlayModeCfg.options.currentValue = &overlayModeSelection;
		overlayModeCfg.options.valueWidth = 220.0f;
		overlayModeCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(overlayModeLabels.size()))
				return false;
			setResourceMonitorMode(selectedIndex);
			return true;
		};
		RenderGeneralPopupSettingRow(
			Tnop("ResourceMonitorMode"),
			T("Choose the overlay density. Off can use the lightweight FPS counter; Full uses the metric selections below."),
			overlayModeCfg,
			T("Overlay Mode\nOff can show the lightweight FPS counter below. Minimal always shows a compact rate and CPU view. Full uses the diagnostic selections below, including separate host and Dreamcast timing domains."));

		// The lightweight counter and performance overlay own different OSD
		// layouts. Expose only the FPS control used by the active layout so a
		// setting is never silently ignored or duplicated on screen.
		if (overlayModeSelection == 0)
		{
			RenderGeneralToggleSettingRow(
				"ShowFPS",
				ICON_FA_GAUGE_HIGH,
				T("Show Lightweight FPS Counter"),
				T("Show a compact frame-rate counter without enabling performance monitoring."),
				static_cast<bool>(config::ShowFPS),
				[](bool enabled) { config::ShowFPS.set(enabled); },
				T("Show Lightweight FPS Counter\nAvailable while the Performance Overlay is Off. Minimal includes frame rates automatically, while Full uses the Game FPS and VPS metric below."));
		}
		if (overlayModeSelection == 2)
		{
			struct OverlayMetric
			{
				const char* id;
				const char* label;
				const char* description;
				bool defaultValue;
			};
			const std::array<OverlayMetric, 15> overlayMetrics {{
				{ "OverlayShowFPS", T("Show Game FPS and VPS"), T("FPS counts completed non-render-to-texture PVR scenes: the game's internal rendered-frame rate. VPS counts Dreamcast video blanks. A 30 FPS game normally shows 30 FPS and about 60 VPS on NTSC."), true },
				{ "OverlayShowOutputFPS", T("Show Output FPS"), T("Counts successful frames presented to the screen. Compare it with Game FPS to identify repeated or omitted host output frames; it is display cadence, not the game's internal rendered-frame rate."), false },
				{ "OverlayShowEmulationSpeed", T("Show Emulation Speed"), T("Shows current VPS relative to the active PAL or NTSC target and the configured speed limit. This is overall pace, not a CPU or GPU utilization meter."), true },
				{ "OverlayShowThreadingState", T("Show Threading State"), T("Shows whether SH4 emulation and PVR rendering run on separate host threads. Threaded mode changes which thread can become the limiting host CPU core."), true },
				{ "OverlayShowHostCpuUsage", T("Show Host CPU"), T("Shows Hollycast's CPU use as a share of total capacity across all logical cores. Device shows total CPU use "
				"from all apps. A single SH4 or renderer thread can still fully occupy one core while Hollycast's overall "
				"share remains low on an 8-core device."), true },
				{ "OverlayShowGpuUsage", T("Show Host GPU"), T("Shows timestamped GPU work as a one-second share and milliseconds per timed render. Includes PVR screen renders and direct framebuffer uploads; full framebuffer emulation can perform both per video frame. It is backend GPU execution time, not a whole-device GPU sensor. Vulkan, OpenGL/GLES, DirectX 9, and DirectX 11 support it."), true },
				{ "OverlayShowSh4ExecutionRate", T("Show Dreamcast SH4 Rate"), /* xgettext:no-c-format */ T("Shows emulated SH4 cycles advancing versus the real-time Dreamcast target. Below 100% means Hollycast is not advancing SH4 time at the expected rate. It is not host CPU use or a game's exact SH4 occupancy."), true },
				{ "OverlayShowPvrTimingPressure", T("Show Dreamcast PVR Schedule"), /* xgettext:no-c-format */ T("Shows all emulated PVR renderer-complete delays scheduled during the sample as a share of the active Dreamcast video-frame budgets. Values above 100% mean scheduled render work exceeded one frame of budget on average. It is scheduler pressure, not host GPU use and not a full PVR busy-cycle meter."), true },
				{ "OverlayShowCpuUsage", T("Show Host SH4 Thread"), /* xgettext:no-c-format */ T("Shows host CPU time used by the thread running SH4 emulation. A value near 100% means that one host core is saturated; it does not directly mean the original Dreamcast SH4 is full."), false },
				{ "OverlayShowPvrUsage", T("Show Host PVR Renderer Thread"), T("Shows host CPU time used by the PVR renderer thread or its shared-thread portion. It exposes emulator and graphics-driver preparation cost, not Dreamcast PVR pressure or hardware GPU use."), false },
				{ "OverlayShowResolution", T("Show Resolution"), T("Shows native Dreamcast output, internal rendered size, host viewport, and final screen output. Higher rendered resolution can raise host GPU time without changing Dreamcast SH4 or PVR scheduling."), true },
				{ "OverlayShowFrameTimes", T("Show Frame Times"), T("Shows minimum, average, and maximum intervals between successful host presents. Frame-time spikes expose stutter even when the one-second FPS average looks normal."), true },
				{ "OverlayShowGpuStatistics", T("Show Renderer Statistics"), T("Shows API, PVR primitive batches, vertices, PVR render passes, and texture uploads per second. These are renderer work counters, not VRAM bytes, cache-resident texture count, or host draw/copy/readback counters."), true },
				{ "OverlayShowVram", T("Show Host GPU Memory"), T("Shows the Windows DXGI memory attributed to Hollycast and its current adapter budget when the OS exposes it. It changes with allocations, but is not total GPU VRAM and is unavailable on platforms without an authoritative query."), true },
				{ "OverlayShowRam", T("Show Hollycast RAM"), T("Shows the resident memory currently held by Hollycast according to the operating system. It changes with content, caches, and allocations. Fixed Dreamcast RAM and VRAM capacities are intentionally omitted."), true }
			}};
			for (const OverlayMetric& metric : overlayMetrics)
			{
				RenderGeneralToggleSettingRow(
					metric.id,
					ICON_FA_CHART_LINE,
					metric.label,
					metric.description,
					cfgLoadBool("rend", metric.id, metric.defaultValue),
					[metric](bool enabled) { cfgSaveBool("rend", metric.id, enabled); });
			}

			ImGui::Spacing();
			ImGui::TextDisabled("%s", T("History Graphs"));
			const std::array<OverlayMetric, 3> overlayGraphs {{
				{ "OverlayShowFrameTimeGraph", T("Show Frame-Time Graph"), T("Draw a rolling 152-sample graph of successful-present intervals. Spikes are long output frames."), true },
				{ "OverlayShowHostCpuGraph", T("Show Host CPU Graph"), T("Draw rolling one-second samples. The main line is Hollycast's share of total CPU capacity; the blue line is overall device CPU use from all apps."), false },
				{ "OverlayShowGpuGraph", T("Show Host GPU Graph"), T("Draw rolling one-second GPU timestamp samples. It is available only while the active backend supplies valid GPU timestamp queries."), false }
			}};
			for (const OverlayMetric& graph : overlayGraphs)
			{
				RenderGeneralToggleSettingRow(
					graph.id,
					ICON_FA_CHART_LINE,
					graph.label,
					graph.description,
					cfgLoadBool("rend", graph.id, graph.defaultValue),
					[graph](bool enabled) { cfgSaveBool("rend", graph.id, enabled); });
			}
		}
	}

	if (RenderCollapsingHeader("OsdAppearanceSection", ICON_FA_EYE, T("Appearance"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		static int overlayTextSize = cfgLoadInt("rend", "OverlayTextSize", 16);
		SettingsUI::PopupSliderConfig textSizeCfg {};
		textSizeCfg.label = T("Text Size");
		textSizeCfg.icon = ICON_FA_TEXT_HEIGHT;
		textSizeCfg.popupID = Tnop("OverlayTextSizePopup");
		textSizeCfg.description = T("Set the maximum text size. The overlay shrinks when needed to keep every row and graph on screen.");
		textSizeCfg.currentValue = &overlayTextSize;
		textSizeCfg.minValue = 10;
		textSizeCfg.maxValue = 48;
		textSizeCfg.defaultValue = 16;
		textSizeCfg.format = "%d px";
		textSizeCfg.valueWidth = 220.0f;
		textSizeCfg.sliderWidth = 320.0f;
		textSizeCfg.onValueChange = [&]() { cfgSaveInt("rend", "OverlayTextSize", overlayTextSize); };
		SettingsUI::PopupConfig textSizePopup {};
		textSizePopup.type = SettingsUI::PopupType::Slider;
		textSizePopup.slider = textSizeCfg;
		RenderGeneralPopupSettingRow(Tnop("OverlayTextSizeSetting"), T("Choose the overlay font size."), textSizePopup,
			T("Text Size\nControls the maximum text size. All selected rows and graphs automatically fit inside the screen."));

		// Keep the existing configuration key for compatibility. Its value has
		// always represented opacity: larger percentages make the OSD less transparent.
		static int overlayOpacity = cfgLoadInt("rend", "OverlayTransparency", 85);
		SettingsUI::PopupSliderConfig opacityCfg {};
		opacityCfg.label = T("Opacity");
		opacityCfg.icon = ICON_FA_EYE;
		opacityCfg.popupID = Tnop("OverlayTransparencyPopup");
		opacityCfg.description = T("Set the overlay opacity.");
		opacityCfg.currentValue = &overlayOpacity;
		opacityCfg.minValue = 10;
		opacityCfg.maxValue = 100;
		opacityCfg.defaultValue = 85;
		opacityCfg.format = "%d%%";
		opacityCfg.valueWidth = 220.0f;
		opacityCfg.sliderWidth = 320.0f;
		opacityCfg.onValueChange = [&]() { cfgSaveInt("rend", "OverlayTransparency", overlayOpacity); };
		SettingsUI::PopupConfig opacityPopup {};
		opacityPopup.type = SettingsUI::PopupType::Slider;
		opacityPopup.slider = opacityCfg;
		RenderGeneralPopupSettingRow(Tnop("OverlayTransparencySetting"), T("Set the overlay opacity."), opacityPopup,
			T("Opacity\nControls the overlay text and background opacity."));

		const std::array<const char*, 4> overlayCornerLabels { T("Top Left"), T("Top Right"), T("Bottom Left"), T("Bottom Right") };
		int overlayCorner = std::clamp(config::loadInt("rend", "OverlayCorner", 1), 0, 3);
		SettingsUI::PopupConfig cornerCfg {};
		cornerCfg.type = SettingsUI::PopupType::Options;
		cornerCfg.options.label = T("Screen Corner");
		cornerCfg.options.icon = ICON_FA_LOCATION_DOT;
		cornerCfg.options.popupID = Tnop("OverlayCornerPopup");
		cornerCfg.options.options = overlayCornerLabels.data();
		cornerCfg.options.optionCount = static_cast<int>(overlayCornerLabels.size());
		cornerCfg.options.currentValue = &overlayCorner;
		cornerCfg.options.valueWidth = 220.0f;
		cornerCfg.options.onChange = [&](int selectedIndex) {
			if (selectedIndex < 0 || selectedIndex >= static_cast<int>(overlayCornerLabels.size()))
				return false;
			cfgSaveInt("rend", "OverlayCorner", selectedIndex);
			return true;
		};
		RenderGeneralPopupSettingRow(Tnop("OverlayCornerSetting"), T("Choose where the overlay appears."), cornerCfg,
			T("Screen Corner\n15 pixels from the selected edges, and below the menu bar for the top corners."));

		static int overlayOffsetX = cfgLoadInt("rend", "OverlayOffsetX", 0);
		SettingsUI::PopupSliderConfig offsetXCfg {};
		offsetXCfg.label = T("Horizontal Position");
		offsetXCfg.icon = ICON_FA_ARROWS_LEFT_RIGHT;
		offsetXCfg.popupID = Tnop("OverlayOffsetXPopup");
		offsetXCfg.description = T("Move the complete overlay left or right.");
		offsetXCfg.currentValue = &overlayOffsetX;
		offsetXCfg.minValue = -1000;
		offsetXCfg.maxValue = 1000;
		offsetXCfg.defaultValue = 0;
		offsetXCfg.format = "%+d px";
		offsetXCfg.valueWidth = 220.0f;
		offsetXCfg.sliderWidth = 320.0f;
		offsetXCfg.onValueChange = [&]() { cfgSaveInt("rend", "OverlayOffsetX", overlayOffsetX); };
		SettingsUI::PopupConfig offsetXPopup {};
		offsetXPopup.type = SettingsUI::PopupType::Slider;
		offsetXPopup.slider = offsetXCfg;
		RenderGeneralPopupSettingRow(Tnop("OverlayOffsetXSetting"), T("Adjust the overlay's horizontal position."), offsetXPopup,
			T("Horizontal Position\nZero starts 15 pixels from the selected edge. Negative values move left. Positive values move right. The overlay stays on screen."));

		static int overlayOffsetY = cfgLoadInt("rend", "OverlayOffsetY", 0);
		SettingsUI::PopupSliderConfig offsetYCfg {};
		offsetYCfg.label = T("Vertical Position");
		offsetYCfg.icon = ICON_FA_ARROWS_UP_DOWN;
		offsetYCfg.popupID = Tnop("OverlayOffsetYPopup");
		offsetYCfg.description = T("Move the complete overlay up or down.");
		offsetYCfg.currentValue = &overlayOffsetY;
		offsetYCfg.minValue = -1000;
		offsetYCfg.maxValue = 1000;
		offsetYCfg.defaultValue = 0;
		offsetYCfg.format = "%+d px";
		offsetYCfg.valueWidth = 220.0f;
		offsetYCfg.sliderWidth = 320.0f;
		offsetYCfg.onValueChange = [&]() { cfgSaveInt("rend", "OverlayOffsetY", overlayOffsetY); };
		SettingsUI::PopupConfig offsetYPopup {};
		offsetYPopup.type = SettingsUI::PopupType::Slider;
		offsetYPopup.slider = offsetYCfg;
		RenderGeneralPopupSettingRow(Tnop("OverlayOffsetYSetting"), T("Adjust the overlay's vertical position."), offsetYPopup,
			T("Vertical Position\nZero starts 15 pixels from the selected edge, below the menu bar for top corners. Negative values move up. Positive values move down. The overlay stays on screen and below the menu bar."));

		int packedColor = config::loadInt("rend", "OverlayColor", 0xFFFFFFFF);
		float overlayColor[4] {
			((packedColor >> 0) & 0xFF) / 255.0f,
			((packedColor >> 8) & 0xFF) / 255.0f,
			((packedColor >> 16) & 0xFF) / 255.0f,
			((packedColor >> 24) & 0xFF) / 255.0f
		};
		RenderGeneralTextInputSettingRow(
			"OverlayColor",
			ICON_FA_EYE,
			T("Text Color"),
			T("Choose the overlay text color."),
			[&]() {
				const bool colorChanged = ImGui::ColorEdit4("##OverlayColor", overlayColor, ImGuiColorEditFlags_NoInputs);
#ifdef HOLLYCAST_TEST_HARNESS
				annotateTestColor("settings.overlay", "OverlayColor", T("Text Color"), "##OverlayColor", overlayColor);
#endif
				if (colorChanged)
					cfgSaveInt("rend", "OverlayColor", static_cast<int>(std::round(overlayColor[0] * 255.f))
						| (static_cast<int>(std::round(overlayColor[1] * 255.f)) << 8)
						| (static_cast<int>(std::round(overlayColor[2] * 255.f)) << 16)
						| (static_cast<int>(std::round(overlayColor[3] * 255.f)) << 24));
			},
			T("Text Color\nControls the overlay text color."), false, 220.0f, "color");
	}
}


} // namespace SettingsNew
