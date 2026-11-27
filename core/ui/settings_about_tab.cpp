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
#ifdef TARGET_MAC
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char **environ;
#endif
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

#ifdef TARGET_IPHONE
extern const char *getIosJitStatus();
#endif

namespace SettingsNew {

static constexpr char kHollycastAsciiTextPath[] = "picture/hollycast_ascii.txt";

static const std::string& GetHollycastAsciiText()
{
	static const std::string text = [] {
		size_t dataSize = 0;
		std::unique_ptr<u8[]> data = resource::load(kHollycastAsciiTextPath, dataSize);
		if (data == nullptr || dataSize == 0)
			return std::string{};
		return std::string(reinterpret_cast<const char*>(data.get()), dataSize);
	}();
	return text;
}

static void RenderHollycastAsciiText()
{
	const std::string& text = GetHollycastAsciiText();
	ImFont* font = aboutAsciiFont != nullptr ? aboutAsciiFont : ImGui::GetFont();
	const float availableWidth = ImGui::GetContentRegionAvail().x - 2.0f;
	if (text.empty() || font == nullptr || availableWidth <= 0.0f)
		return;

	size_t maxColumns = 0;
	size_t currentColumns = 0;
	size_t lineCount = 0;
	for (char character : text)
	{
		if (character == '\n')
		{
			maxColumns = std::max(maxColumns, currentColumns);
			currentColumns = 0;
			lineCount++;
		}
		else if (character != '\r')
		{
			currentColumns++;
		}
	}
	if (text.back() != '\n')
	{
		maxColumns = std::max(maxColumns, currentColumns);
		lineCount++;
	}
	if (maxColumns == 0 || lineCount == 0)
		return;

	const float referenceSize = font->LegacySize;
	const char space = ' ';
	const float columnWidth = font->CalcTextSizeA(referenceSize, FLT_MAX, 0.0f, &space, &space + 1).x;
	if (referenceSize <= 0.0f || columnWidth <= 0.0f)
		return;

	// Follow Settings UI scaling, but never allow a row to exceed the content
	// width. No wrapping is used because it would corrupt the ASCII artwork.
#if defined(__ANDROID__)
	constexpr float AsciiFontScale = 0.25f;
	const float desiredFontSize = SettingsPreviewFontSize(font) * AsciiFontScale;
#else
	// Desktop artwork needs a larger lower bound to preserve the original glyph
	// detail. Start at the previous mapping's size at UI scale 90 (effective
	// scale 150). Each subsequent UI scale step adds half its normal amount
	// to the artwork only; values below 50 cannot lower this baseline.
	constexpr float AsciiFontScale = 0.375f;
	constexpr float MinimumUiScale = 50.0f;
	constexpr float MinimumAsciiUiScale = 150.0f;
	constexpr float AsciiUiScaleStep = 0.5f;
	const float uiScale = static_cast<float>(config::UIScaling);
	const float effectiveUiScale = MinimumAsciiUiScale
			+ std::max(0.0f, uiScale - MinimumUiScale) * AsciiUiScaleStep;
	const float desiredFontSize = SettingsPreviewFontSize(font) * AsciiFontScale
			* effectiveUiScale / std::max(uiScale, 1.0f);
#endif
	const float fittingFontSize = referenceSize * availableWidth / (columnWidth * maxColumns);
	const float fontSize = std::min(desiredFontSize, fittingFontSize);
	if (fontSize <= 0.0f)
		return;

	ImVec2 position = ImGui::GetCursorScreenPos();
#if !defined(__ANDROID__)
	// Center the complete monospaced row without modifying the supplied text.
	const float artworkWidth = columnWidth * maxColumns * fontSize / referenceSize;
	position.x += std::max(0.0f, (ImGui::GetContentRegionAvail().x - artworkWidth) * 0.5f);
#endif
	// Normal text leading separates the ASCII rows into visible horizontal
	// bands. Draw the original lines closer together without reflowing them.
	constexpr float AsciiLineSpacing = 0.75f;
	const float lineAdvance = fontSize * AsciiLineSpacing;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
	size_t lineStart = 0;
	size_t row = 0;
	while (lineStart < text.size())
	{
		const size_t newline = text.find('\n', lineStart);
		const size_t lineEnd = newline == std::string::npos ? text.size() : newline;
		drawList->AddText(font, fontSize,
			ImVec2(position.x, position.y + static_cast<float>(row) * lineAdvance), color,
			text.data() + lineStart, text.data() + lineEnd, 0.0f);
		if (newline == std::string::npos)
			break;
		lineStart = newline + 1;
		++row;
	}
	// Reserve the full last row too, so the next section cannot overlap it.
	ImGui::Dummy(ImVec2(0.0f, lineAdvance * static_cast<float>(lineCount - 1) + fontSize));
}

void renderAboutTab()
{
	// Use TextDisabled for the title (theme-aware)

	// Center content for better appearance
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

	ImGui::Spacing();
	RenderHollycastAsciiText();

	ImGui::Spacing();
	ImGui::Spacing();

	// Version Information
	if (RenderCollapsingHeader("VersionInformationSection", ICON_FA_TAG, T("Version Information"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text(T("Version: %s"), app_updates::detail::versionLabel(HOLLYCAST_UPDATE_CHANNEL, GIT_HASH).c_str());
		ImGui::Text(T("Git Hash: %s"), GIT_HASH);
		ImGui::Text(T("Build Date: %s"), BUILD_DATE);
		ImGui::Spacing();
	}

	// Platform Information
	if (RenderCollapsingHeader("PlatformSection", ICON_FA_COMPUTER, T("Platform"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text(T("CPU: %s"),
	#if HOST_CPU == CPU_X86
		T("x86")
	#elif HOST_CPU == CPU_ARM
		T("ARM")
#elif HOST_CPU == CPU_X64
		T("x86_64")
#elif HOST_CPU == CPU_ARM64
		T("ARM64")
	#else
		T("Unknown")
	#endif
		);
		ImGui::Text(T("Operating System: %s"),
	#ifdef __ANDROID__
		T("Android")
	#elif defined(__unix__)
		T("Linux")
#elif defined(__APPLE__)
#ifdef TARGET_IPHONE
		T("iOS")
#else
		T("macOS")
#endif
#elif defined(TARGET_UWP)
		T("Windows Universal Platform")
#elif defined(_WIN32)
		T("Windows")
#elif defined(__SWITCH__)
		T("Nintendo Switch")
	#else
		T("Unknown")
	#endif
		);
#ifdef TARGET_IPHONE
		ImGui::Text(T("JIT Status: %s"), ::getIosJitStatus());
#endif
		ImGui::Spacing();
	}

	// Graphics Information
	{
		std::string graphicsTitle = T("Graphics");
		if (isOpenGL(config::RendererType))
			graphicsTitle = T("Graphics - OpenGL");
		else if (isVulkan(config::RendererType))
			graphicsTitle = T("Graphics - Vulkan");
		else if (isMetal(config::RendererType))
			graphicsTitle = T("Graphics - Metal");
		else if (isDirectX(config::RendererType))
			graphicsTitle = T("Graphics - DirectX");

		if (RenderCollapsingHeader("GraphicsSection", ICON_FA_DISPLAY, graphicsTitle.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
		{
			ImGui::Text(T("Driver: %s"), GraphicsContext::Instance()->getDriverName().c_str());
			ImGui::Text(T("Version: %s"), GraphicsContext::Instance()->getDriverVersion().c_str());

#if defined(__ANDROID__) && HOST_CPU == CPU_ARM64 && USE_VULKAN
			if (isVulkan(config::RendererType))
			{
				ImGui::Spacing();
				if (config::CustomGpuDriver)
				{
					std::string name, description, vendor, version;
					if (getCustomGpuDriverInfo(name, description, vendor, version))
					{
						ImGui::Text("%s", T("Custom Driver:"));
						ImGui::Indent();
						ImGui::Text("%s - %s", name.c_str(), description.c_str());
						ImGui::Text("%s - %s", vendor.c_str(), version.c_str());
						ImGui::Unindent();
					}
				}
			}
#endif
			ImGui::Spacing();
		}
	}

	// Project Description
	const std::string aboutHeader = std::string(T("About")) + "##Section";
	if (ImGui::CollapsingHeader(aboutHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T(
			"Hollycast is a multi-platform emulator for Sega Dreamcast, "
			"Naomi, Naomi 2, and Atomiswave arcade systems."
		));
		ImGui::TextWrapped("%s", T(
			"Based on Flycast, providing accurate emulation with enhancements "
			"for modern systems."
		));
		ImGui::Spacing();
	}

	// Links Section
	if (RenderCollapsingHeader("LinksSection", ICON_FA_LINK, T("Links"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Website (colored text to look like a link)
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
		ImGui::Text("%s", T("Website"));
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextDisabled("https://github.com/nexus382/Hollycast#readme");

		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered]);
		ImGui::Text("%s", T("Source Code"));
		ImGui::PopStyleColor();
		ImGui::SameLine();
		ImGui::TextDisabled("https://github.com/nexus382/Hollycast");
		ImGui::Spacing();
	}

	// License Information
	if (RenderCollapsingHeader("LicenseSection", ICON_FA_FILE_CONTRACT, T("License"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T(
			"Copyright (C) 2019-2025 flyinghead and contributors. "
			"Portions Copyright 2026 The Hollycast Authors"
		));
		ImGui::TextWrapped("%s", T(
			"Hollycast is free software; you can redistribute it and/or modify "
			"it under the terms of the GNU General Public License as published by "
			"the Free Software Foundation; either version 2 of the License, or "
			"(at your option) any later version."
		));
		ImGui::Spacing();
		const struct { const char* title; const char* path; } bundledLicenses[] = {
			{"Noto Sans CJK (SIL Open Font License)", "fonts/NotoSansCJK-LICENSE.txt"},
			{"Noto Sans Devanagari (SIL Open Font License)", "fonts/NotoSansDevanagari-LICENSE.txt"},
			{"HarfBuzz", "fonts/HarfBuzz-LICENSE.txt"},
			{"ReShadeFX", "licenses/reshadefx/LICENSE.md"},
			{"SPIRV-Cross", "licenses/spirv-cross/LICENSE"},
		};
		for (const auto& bundledLicense : bundledLicenses)
		{
			if (!ImGui::TreeNode(bundledLicense.title))
				continue;
			size_t licenseSize = 0;
			auto license = resource::load(bundledLicense.path, licenseSize);
			if (license)
			{
				const std::string text(reinterpret_cast<const char*>(license.get()), licenseSize);
				ImGui::TextWrapped("%s", text.c_str());
			}
			ImGui::TreePop();
		}
	}

	// Credits
	if (RenderCollapsingHeader("CreditsSection", ICON_FA_USERS, T("Credits"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T(
			"This emulator is based on the work of many talented developers "
			"who have contributed to the project and its dependencies."
		));
		ImGui::TextWrapped("%s", T(
			"Special thanks to all contributors who make this project possible."
		));
	}

	ImGui::PopStyleVar();
}


} // namespace SettingsNew
