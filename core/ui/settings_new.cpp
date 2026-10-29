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
#include "custom_theme.h"
#include "gui_util.h"
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
#include "input/dreampotato.h"
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/maplelinkregistry.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/pvr/Renderer_if.h"
#ifdef USE_DREAMLINK_DEVICES
#include "sdl/dreamlink/dreamlinkgamepad.h"
#endif
#include "log/LogManager.h"
#include "hw/mem/addrspace.h"
#include "settings.h"
#include "emulator.h"
#include "gui.h"
#include "boxart/boxart.h"
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
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cfloat>
#include <cctype>
#include <future>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <filesystem>
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

static bool g_applyThemeAfterSettings = false;
static bool g_reloadThemeFontsAfterSettings = false;

struct ThemeEditorState {
	bool open = false;
	bool editing = false;
	bool previewing = false;
	bool fontChanged = false;
	bool fontReloadPending = false;
	std::string filename;
	std::string newFilename = "My Theme.json";
	std::string exportFolder;
	std::string error;
	std::string colorSearch;
	CustomTheme::Theme draft;
};
static ThemeEditorState g_themeEditor;

static std::string themeFilenameWithExtension(std::string filename)
{
	if (filename.size() < 5 || filename.compare(filename.size() - 5, 5, ".json") != 0)
		filename += ".json";
	return filename;
}

static bool completePendingThemeMigration(std::string& error)
{
	const std::string directory = CustomTheme::directory();
	int id = -1;
	std::string filename;
	std::string migrationError;
	if (!CustomTheme::readPendingBuiltinMigration(directory, id, filename, migrationError)) {
		if (migrationError.empty())
			return true;
		error = "Failed to read pending theme migration: " + migrationError;
		return false;
	}
	if (!CustomTheme::markLegacyBuiltinMigrationComplete(directory, id, filename, migrationError)) {
		error = "Failed to record theme migration completion: " + migrationError;
		return false;
	}
	if (CustomTheme::clearPendingBuiltinMigration(directory, migrationError))
		return true;
	error = "Failed to clear pending theme migration: " + migrationError;
	return false;
}

static ImVec4 SettingsErrorColor()
{
	const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
	const ImVec4 error(0.88f, 0.26f, 0.24f, 1.0f);
	constexpr float errorWeight = 0.62f;
	const ImVec4 fallback(accent.x + (error.x - accent.x) * errorWeight,
			accent.y + (error.y - accent.y) * errorWeight,
			accent.z + (error.z - accent.z) * errorWeight, 1.0f);
	return CustomTheme::roleColor("error", fallback, "settings.feedback");
}

static std::string cfgLoadStr(const std::string& section, const std::string& key, const std::string& def)
{
	return config::loadStr(section, key, def);
}

static void cfgSaveStr(const std::string& section, const std::string& key, const std::string& value)
{
	config::saveStr(section, key, value);
}

static bool cfgLoadBool(const std::string& section, const std::string& key, bool def)
{
	return config::loadBool(section, key, def);
}

static void cfgSaveBool(const std::string& section, const std::string& key, bool value)
{
	config::saveBool(section, key, value);
}

static void cfgSaveInt(const std::string& section, const std::string& key, int value)
{
	config::saveInt(section, key, value);
}

static int cfgLoadInt(const std::string& section, const std::string& key, int def)
{
	return config::loadInt(section, key, def);
}

static int getResourceMonitorMode()
{
	return config::loadInt("rend", "ResourceMonitorMode", 0);
}

static void setResourceMonitorMode(int mode)
{
	config::saveInt("rend", "ResourceMonitorMode", mode);
}

#if defined(USE_DREAMLINK_DEVICES)
static void reconnectDreamLinks()
{
	// DreamLink devices are tied into maple reconnect flow; keep this hook so
	// settings exit can request DreamLink-specific reconnection behavior later.
}
#endif

// Static state for gamepad settings popup
static std::shared_ptr<GamepadDevice> g_currentGamepadForSettings;
static float g_twoLineRowExtraHeightPx = 0.0f;
static bool g_twoLineRowSeparatorsEnabled = false;
static float g_twoLineRowSeparatorAlpha = 0.5f;
static float g_twoLineRowStartScreenX = 0.0f;
static float g_twoLineRowEndScreenX = 0.0f;
static float g_twoLineRowEndScreenY = 0.0f;
static bool g_mapleDevicesChangedInSettings = false;
static bool g_scrollToBoxArtSection = false;
static bool g_focusSettingsNavigation = false;
static bool g_focusSettingsContent = false;
static bool g_focusSettingsBack = false;
static bool g_controllerPopupWasOpen = false;
static int g_controllerPopupDepth = 0;
static bool g_controllerEditing = false;
static ImGuiWindow* g_navigationWindow = nullptr;
static ImGuiWindow* g_contentWindow = nullptr;
static ImGuiID g_backButtonId = 0;
enum class ControllerSection { None, Tabs, Content, Back };
static ControllerSection g_controllerSection = ControllerSection::None;
static double g_tabSlideStart = -1.0;
static SettingsTab g_tabSlideTarget = SettingsTab::General;
static int g_tabSlideDirection = 0;
static bool g_tabSlideFocusContent = false;
static constexpr double kTabSlideSeconds = 0.18;
static std::string g_settingsFooterText;
static constexpr float kSettingsFooterHeightPx = 152.0f;

#if defined(__ANDROID__)
struct SettingsAndroidScaleState
{
	float activeUiScale = 0.0f;
	float baseScale = 1.0f;
	float appliedUserScale = 1.0f;
};

static SettingsAndroidScaleState& SettingsAndroidScaleStateCache()
{
	static SettingsAndroidScaleState state;

	const float userScale = uiUserScale();
	if (state.activeUiScale != settings.display.uiScale)
	{
		state.activeUiScale = settings.display.uiScale;
		state.baseScale = settings.display.uiScale / userScale;
		state.appliedUserScale = userScale;
	}

	return state;
}

static float SettingsAndroidBaseScale()
{
	return SettingsAndroidScaleStateCache().baseScale;
}
#endif

static float SettingsTextPreviewScale()
{
#if defined(__ANDROID__)
	const float appliedUserScale = std::max(0.01f, SettingsAndroidScaleStateCache().appliedUserScale);
	// The font atlas contains the globally applied user scale. Compensate for it
	// here so Settings text and geometry share the same bounded Android scale,
	// both while previewing and after Apply rebuilds the atlas.
	return uiUserScale() / appliedUserScale;
#else
	return 1.0f;
#endif
}

static float SettingsPreviewFontSize(ImFont* font)
{
	font = font != nullptr ? font : ImGui::GetFont();
	return font->LegacySize * SettingsTextPreviewScale();
}

static float SettingsLayoutScaled(float px)
{
#if defined(__ANDROID__)
	return px * SettingsAndroidBaseScale() * uiUserScale();
#else
	return uiScaled(px);
#endif
}

static float SettingsFixedLayoutScaled(float px)
{
#if defined(__ANDROID__)
	return SettingsLayoutScaled(px);
#else
	return uiScaled(px);
#endif
}

static float SettingsFixedFooterScaled(float px)
{
#if defined(__ANDROID__)
	return SettingsFixedLayoutScaled(px) * 0.85f;
#else
	return uiScaled(px);
#endif
}

static float SettingsFixedNavigationScaled(float px)
{
#if defined(__ANDROID__)
	return SettingsFixedLayoutScaled(px);
#else
	return uiScaled(px);
#endif
}

static ImVec2 SettingsLayoutSize(float width, float height)
{
	return ImVec2(SettingsLayoutScaled(width), SettingsLayoutScaled(height));
}

static bool UseCompactSettingsLayout()
{
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	return displaySize.x < SettingsLayoutScaled(960.0f) || displaySize.y < SettingsLayoutScaled(720.0f);
}

static float SettingsFooterHeight()
{
#if defined(__ANDROID__)
	const float displayHeight = ImGui::GetIO().DisplaySize.y;
	const float maximumHeight = displayHeight * 0.24f;
	if (!UseCompactSettingsLayout())
		return std::min(SettingsFixedFooterScaled(kSettingsFooterHeightPx), maximumHeight);

	const float minimumHeight = displayHeight * 0.14f;
	return std::clamp(SettingsFixedFooterScaled(112.0f), minimumHeight, maximumHeight);
#else
	if (!UseCompactSettingsLayout())
		return SettingsFixedFooterScaled(kSettingsFooterHeightPx);
	return std::clamp(ImGui::GetIO().DisplaySize.y * 0.20f, SettingsFixedFooterScaled(104.0f), SettingsFixedFooterScaled(136.0f));
#endif
}

static float SettingsFooterGap()
{
	return UseCompactSettingsLayout() ? SettingsLayoutScaled(1.0f) : SettingsLayoutScaled(2.0f);
}

// Visual-only toggle switch rendering (no interaction)
// Renders the toggle switch appearance based on the boolean value
// The parent Selectable handles all user input
static void RenderToggleSwitchVisual(bool value)
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	if (window->SkipItems)
		return;

	const float height = SettingsLayoutScaled(24.0f);
	const float width = SettingsLayoutScaled(48.0f);
	const float radius = height * 0.5f;
	const ImVec2 pos = window->DC.CursorPos;
	const ImVec2 size(width, height);

	const ImRect bb(pos, ImVec2(pos.x + size.x, pos.y + size.y));
	ImGui::ItemSize(size, ImGui::GetStyle().FramePadding.y);

	// Animation
	float anim = value ? 1.0f : 0.0f;

	// Render background
	ImU32 col_bg;
	if (value)
		col_bg = ImGui::GetColorU32(ImGuiCol_ButtonActive);
	else
		col_bg = ImGui::GetColorU32(ImGuiCol_FrameBg);

	ImDrawList* draw_list = ImGui::GetWindowDrawList();
	draw_list->AddRectFilled(bb.Min, bb.Max, col_bg, radius);

	// Render knob - use theme-aware color
	ImVec2 knob_pos;
	knob_pos.x = ImLerp(bb.Min.x + radius, bb.Max.x - radius, anim);
	knob_pos.y = bb.Min.y + radius;

	// Use the theme's text color for a knob that contrasts with both surfaces.
	ImU32 knobColor = ImGui::GetColorU32(ImGuiCol_Text);
	draw_list->AddCircleFilled(knob_pos, radius - 1.0f, knobColor);
}

static float TwoLineSettingContentHeight()
{
	ImFont* primaryFont = largeFont != nullptr ? largeFont : ImGui::GetFont();
	ImFont* secondaryFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	const float primaryHeight = SettingsPreviewFontSize(primaryFont);
	const float secondaryHeight = SettingsPreviewFontSize(secondaryFont);
	return primaryHeight + SettingsLayoutScaled(4.0f) + secondaryHeight;
}

static float TwoLineSettingSecondaryLineOffsetY()
{
	ImFont* primaryFont = largeFont != nullptr ? largeFont : ImGui::GetFont();
	return SettingsPreviewFontSize(primaryFont) + SettingsLayoutScaled(4.0f);
}

static float TwoLineSettingRowHeight()
{
	return TwoLineSettingContentHeight() + SettingsLayoutScaled(g_twoLineRowExtraHeightPx);
}

static float TwoLineSettingContentOffsetY()
{
	return std::max(0.0f, (TwoLineSettingRowHeight() - TwoLineSettingContentHeight()) * 0.5f);
}

static float TwoLineSettingRowGap()
{
	return SettingsLayoutScaled(2.0f);
}

static float RightColumnX(float itemWidth, float rightPadding = 28.0f)
{
	const float rowRightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	return rowRightX - itemWidth - SettingsLayoutScaled(rightPadding);
}

static void RenderScaledIconInRect(const char* icon, const ImVec2& min, const ImVec2& max)
{
	ImFont* font = settingsIconFont != nullptr ? settingsIconFont : ImGui::GetFont();
	const ImVec2 iconSize(max.x - min.x, max.y - min.y);
	float fontSize = std::min(iconSize.x, iconSize.y) * 0.72f;
	ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon);
	if (textSize.x > iconSize.x && textSize.x > 0.0f)
	{
		fontSize *= iconSize.x / textSize.x;
		textSize = font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon);
	}

	const ImVec2 textPos(
		min.x + (iconSize.x - textSize.x) * 0.5f,
		min.y + (iconSize.y - textSize.y) * 0.5f
	);
	ImGui::GetWindowDrawList()->AddText(
		font,
		fontSize,
		textPos,
		ImGui::GetColorU32(ImGuiCol_Text),
		icon
	);
}

static bool SettingsIconButton(const char* id, const char* icon, const ImVec2& size)
{
	const bool pressed = ImGui::Button(id, size);
	RenderScaledIconInRect(icon, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
	return pressed;
}

static const char* GetTabHelpText(SettingsTab tab)
{
	switch (tab)
	{
	case SettingsTab::General:
		return T("System, content, interface, and storage behavior. Highlight or select a setting to see a fuller explanation here.");
	case SettingsTab::Library:
		return T("Library layout, artwork, scraped media, game metadata, and VMU save icons. Highlight or select a setting to see a fuller explanation here.");
	case SettingsTab::Video:
		return T("Graphics quality, renderer behavior, scaling, and performance tuning. Highlight or select a setting to see what it changes and why it matters.");
	case SettingsTab::Shader:
		return T("Choose, order, and adjust display shader layers. Every selected layer must compile before the active stack is replaced.");
	case SettingsTab::OnScreenDisplay:
		return T("In-game information, VMU displays, performance metrics, and overlay appearance. Highlight or select a setting to see what it changes and why it matters.");
	case SettingsTab::Audio:
		return T("Audio backend, latency, mixing, and output behavior. Highlight or select a setting for practical guidance before changing it.");
	case SettingsTab::Controls:
		return T("Controller devices, mappings, sensitivity, and input behavior. Highlight or select a setting to see setup tips and tradeoffs.");
	case SettingsTab::Network:
		return T("Online play, server, match code, and link features. Highlight or select a setting for connection details and recommendations.");
	case SettingsTab::Advanced:
		return T("Power-user options, debugging, underclocking, overlays, and developer tools. Highlight or select a setting for deeper explanations.");
	case SettingsTab::About:
		return T("Project information, links, license details, and credits.");
	case SettingsTab::Count:
		break;
	}
	return T("Highlight or select a setting to see details here.");
}

static void ResetSettingsFooter()
{
	g_settingsFooterText = GetTabHelpText(g_state.currentTab);
}

static void SetSettingsFooterText(const char* text)
{
	if (text != nullptr && text[0] != '\0')
		g_settingsFooterText = text;
}

static bool IsAnySettingsPopupOpen()
{
	return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

void prepareControllerNavigation()
{
	ImGuiContext& g = *GImGui;
	// Capture before NewFrame closes popups: one B press must perform one step.
	g_controllerPopupWasOpen = !g.OpenPopupStack.empty();
	g_controllerPopupDepth = g.OpenPopupStack.Size;
	g_controllerEditing = g.ActiveId != 0;
	g_controllerSection = ControllerSection::None;
	for (ImGuiWindow* window = g.NavWindow; window != nullptr; window = window->ParentWindow)
	{
		if (window == g_navigationWindow)
		{
			g_controllerSection = g.NavId == g_backButtonId ? ControllerSection::Back : ControllerSection::Tabs;
			break;
		}
		if (window == g_contentWindow)
		{
			g_controllerSection = ControllerSection::Content;
			break;
		}
	}
	if (!g_controllerPopupWasOpen && g_controllerSection != ControllerSection::None)
	{
		const ImGuiID owner = ImHashStr("SettingsControllerNavigation");
		ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, owner);
		if (!g_controllerEditing)
			for (ImGuiKey key : {ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
					ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight})
				ImGui::SetKeyOwner(key, owner);
	}
}

static bool ControllerPopupCancelPressed()
{
	// A child popup may already have consumed B in NewFrame. Never cascade
	// that press into the parent dialog as it renders later in the same frame.
	return GImGui->BeginPopupStack.Size == g_controllerPopupDepth
			&& ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

static void FocusCurrentSettingsItem()
{
	ImGuiWindow* window = ImGui::GetCurrentWindow();
	ImGuiContext& g = *GImGui;
	ImGui::FocusWindow(window, ImGuiFocusRequestFlags_RestoreFocusedChild);
	ImGui::SetNavID(
		ImGui::GetItemID(),
		window->DC.NavLayerCurrent,
		g.CurrentFocusScopeId,
		ImGui::WindowRectAbsToRel(window, ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()))
	);
}

static void ShowFooterHelpMarker(const char* desc)
{
	ImGui::TextDisabled("%s", Tnop("(?)"));
	if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
		SetSettingsFooterText(desc);
}

static ImU32 FooterTextWeightColor(const ImVec4& color, float alphaScale)
{
	ImVec4 weightedColor = color;
	weightedColor.w *= alphaScale;
	return ImGui::ColorConvertFloat4ToU32(weightedColor);
}

static void AddWeightedFooterText(ImDrawList* drawList, ImFont* font, float fontSize,
	const ImVec2& pos, const ImVec4& color, const char* text, float wrapWidth)
{
	const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(color);
	drawList->AddText(font, fontSize, pos, textColor, text, nullptr, wrapWidth);

	// Low-DPI and lower-quality panels can make the details footer look too thin.
	// A 33% strength subpixel overdraw gives this box more weight without changing
	// font metrics, wrapping, or the user's theme colors.
	drawList->AddText(font, fontSize, ImVec2(pos.x + 0.6f, pos.y),
		FooterTextWeightColor(color, 0.33f), text, nullptr, wrapWidth);
}

static void RenderSettingsFooterBar()
{
	const float footerHeight = SettingsFooterHeight();
	ImGui::BeginChild("SettingsFooterBar", ImVec2(0.0f, footerHeight), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 footerMin = ImGui::GetWindowPos();
	const ImVec2 footerMax(footerMin.x + ImGui::GetWindowSize().x, footerMin.y + ImGui::GetWindowSize().y);
	ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
	ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive);
	border.w = std::min(border.w + 0.10f, 1.0f);
	accent.w = 1.0f;

	const bool compactLayout = UseCompactSettingsLayout();
	const float padX = compactLayout ? SettingsLayoutScaled(1.0f) : SettingsLayoutScaled(2.0f);
	const float padY = compactLayout ? SettingsLayoutScaled(1.0f) : SettingsLayoutScaled(2.0f);
	ImFont* titleFont = settingsTitleFont != nullptr ? settingsTitleFont : largeFont;
	ImFont* bodyFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
#if defined(__ANDROID__)
	const float footerTextScale = (compactLayout ? 1.0f : 1.15f) * SettingsTextPreviewScale();
#else
	const float footerTextScale = 1.25f;
#endif
	const float titleFontSize = titleFont->LegacySize * (compactLayout ? 1.00f : 1.06f) * footerTextScale;
	const float bodyFontSize = bodyFont->LegacySize * (compactLayout ? 0.89f : 0.93f) * footerTextScale;
	const float textPadX = compactLayout ? SettingsLayoutScaled(10.0f) : SettingsLayoutScaled(12.0f);
	const float textPadY = compactLayout ? SettingsLayoutScaled(8.0f) : SettingsLayoutScaled(10.0f);
	const ImVec2 textBoxMin(footerMin.x + padX, footerMin.y + padY);
	const ImVec2 textBoxMax(footerMax.x - padX, footerMax.y - padY);
	ImVec4 textBoxBg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	textBoxBg.w = std::min(textBoxBg.w + 0.35f, 1.0f);
	drawList->AddRectFilled(textBoxMin, textBoxMax,
		ImGui::ColorConvertFloat4ToU32(textBoxBg), SettingsLayoutScaled(10.0f));
	drawList->AddRect(textBoxMin, textBoxMax,
		ImGui::ColorConvertFloat4ToU32(border), SettingsLayoutScaled(10.0f), 0, 1.0f);
	drawList->AddRectFilled(
		textBoxMin,
		ImVec2(textBoxMax.x, textBoxMin.y + SettingsLayoutScaled(3.0f)),
		ImGui::ColorConvertFloat4ToU32(accent),
		SettingsLayoutScaled(10.0f),
		ImDrawFlags_RoundCornersTop);

	const char* footerText = g_settingsFooterText.empty()
		? T("Highlight or select a setting to see details here.")
		: g_settingsFooterText.c_str();
	const ImVec2 clipMin(textBoxMin.x + textPadX, textBoxMin.y + textPadY);
	const ImVec2 clipMax(textBoxMax.x - textPadX, textBoxMax.y - textPadY);
	const ImVec2 bodyPos(clipMin.x, clipMin.y);
	const float wrapWidth = std::max(0.0f, clipMax.x - clipMin.x);
	const ImVec4 bodyColor = ImGui::GetStyle().Colors[ImGuiCol_Text];
	const char* bodyStart = footerText;
	std::string titleLine;
	if (const char* nl = strchr(footerText, '\n'); nl != nullptr)
	{
		titleLine.assign(footerText, nl);
		bodyStart = nl + 1;
	}
	const auto normalizeFooterBody = [](const char* text) -> std::string
	{
		if (text == nullptr || text[0] == '\0')
			return {};
		std::string out;
		out.reserve(strlen(text));
		int newlineCount = 0;
		for (const char* p = text; *p != '\0'; ++p)
		{
			char c = *p;
			if (c == '\r')
				continue;
			if (c == '\n')
			{
				++newlineCount;
				continue;
			}

			if (newlineCount > 0)
			{
				if (newlineCount >= 2)
				{
					while (!out.empty() && out.back() == ' ')
						out.pop_back();
					if (!out.empty() && out.back() != '\n')
						out.push_back('\n');
					out.push_back('\n');
				}
				else
				{
					if (!out.empty() && out.back() != ' ' && out.back() != '\n')
						out.push_back(' ');
				}
				newlineCount = 0;
			}

			if (c == '\t')
				c = ' ';
			if (c == ' ')
			{
				if (out.empty() || out.back() == ' ' || out.back() == '\n')
					continue;
			}
			out.push_back(c);
		}

		if (newlineCount >= 2)
		{
			while (!out.empty() && out.back() == ' ')
				out.pop_back();
			if (!out.empty() && out.back() != '\n')
				out.push_back('\n');
			out.push_back('\n');
		}
		else if (newlineCount == 1)
		{
			if (!out.empty() && out.back() != ' ' && out.back() != '\n')
				out.push_back(' ');
		}

		while (!out.empty() && out.back() == ' ')
			out.pop_back();
		return out;
	};
	std::string bodyText = normalizeFooterBody(bodyStart);
	const std::string displayTitle = titleLine.empty()
		? std::string()
		: std::string(T("Setting Details: ")) + titleLine;
	const float titleSpacing = compactLayout ? SettingsLayoutScaled(3.0f) : SettingsLayoutScaled(5.0f);
	const float availableHeight = std::max(0.0f, clipMax.y - clipMin.y);
	float finalTitleSize = titleFontSize;
	float finalBodySize = bodyFontSize;
	if (availableHeight > 0.0f && (!displayTitle.empty() || !bodyText.empty()))
	{
		const auto measureFooterTextHeight = [&](float titleSize, float bodySize, const std::string& body) -> float
		{
			float totalHeight = 0.0f;
			if (!displayTitle.empty())
				totalHeight += titleFont->CalcTextSizeA(titleSize, FLT_MAX, wrapWidth, displayTitle.c_str()).y + titleSpacing;
			if (!body.empty())
				totalHeight += bodyFont->CalcTextSizeA(bodySize, FLT_MAX, wrapWidth, body.c_str()).y;
			return totalHeight;
		};
		const auto compactOverflowBody = [](const std::string& text) -> std::string
		{
			std::string out;
			out.reserve(text.size());
			bool pendingSpace = false;
			for (char c : text)
			{
				if (c == '\r' || c == '\n' || c == '\t' || c == ' ')
				{
					pendingSpace = !out.empty();
					continue;
				}
				if (pendingSpace && !out.empty())
				{
					out.push_back(' ');
					pendingSpace = false;
				}
				out.push_back(c);
			}
			return out;
		};

		float totalHeight = measureFooterTextHeight(titleFontSize, bodyFontSize, bodyText);
#if defined(__ANDROID__)
		// Keep paragraph breaks when they fit. At high Android UI scale, compact
		// only overflowing footer copy so sentences use horizontal wrap space
		// instead of being clipped below the fixed details box.
		if (totalHeight > availableHeight)
		{
			std::string compactBodyText = compactOverflowBody(bodyText);
			const float compactHeight = measureFooterTextHeight(titleFontSize, bodyFontSize, compactBodyText);
			if (compactHeight < totalHeight)
			{
				bodyText = std::move(compactBodyText);
				totalHeight = compactHeight;
			}
		}
#endif
		if (totalHeight > availableHeight)
		{
#if defined(__ANDROID__)
			const float scale = std::clamp(availableHeight / totalHeight, 0.70f, 1.0f);
#else
			const float scale = std::clamp(availableHeight / totalHeight, 0.78f, 1.0f);
#endif
			finalTitleSize = titleFontSize * scale;
			finalBodySize = bodyFontSize * scale;
		}
	}

	ImVec2 textPos(std::floor(bodyPos.x), std::floor(bodyPos.y));
	drawList->PushClipRect(clipMin, clipMax, true);
	if (!displayTitle.empty())
	{
		const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_HeaderActive];
		const ImVec4 titleColor2(bodyColor.x + (accent.x - bodyColor.x) * 0.3f,
				bodyColor.y + (accent.y - bodyColor.y) * 0.3f,
				bodyColor.z + (accent.z - bodyColor.z) * 0.3f, 1.0f);
		AddWeightedFooterText(
			drawList,
			titleFont,
			finalTitleSize,
			textPos,
			titleColor2,
			displayTitle.c_str(),
			wrapWidth);
		textPos.y = std::floor(textPos.y + finalTitleSize + titleSpacing);
	}

	if (!bodyText.empty())
	{
		AddWeightedFooterText(
			drawList,
			bodyFont,
			finalBodySize,
			textPos,
			bodyColor,
			bodyText.c_str(),
			wrapWidth);
	}
	drawList->PopClipRect();

	ImGui::EndChild();
}

class ScopedTwoLineRowStyle
{
public:
	ScopedTwoLineRowStyle(float rowHeightExtraPx, bool drawSeparators, float separatorAlpha = 0.5f)
		: m_prevRowHeightExtraPx(g_twoLineRowExtraHeightPx)
		, m_prevDrawSeparators(g_twoLineRowSeparatorsEnabled)
		, m_prevSeparatorAlpha(g_twoLineRowSeparatorAlpha)
	{
		g_twoLineRowExtraHeightPx = rowHeightExtraPx;
		g_twoLineRowSeparatorsEnabled = drawSeparators;
		g_twoLineRowSeparatorAlpha = separatorAlpha;
	}

	~ScopedTwoLineRowStyle()
	{
		g_twoLineRowExtraHeightPx = m_prevRowHeightExtraPx;
		g_twoLineRowSeparatorsEnabled = m_prevDrawSeparators;
		g_twoLineRowSeparatorAlpha = m_prevSeparatorAlpha;
	}

private:
	float m_prevRowHeightExtraPx;
	bool m_prevDrawSeparators;
	float m_prevSeparatorAlpha;
};

static ImVec2 BeginTwoLineSettingRowContent()
{
	ImGui::SameLine(0, 0);
	ImVec2 line1Start = ImGui::GetCursorPos();
	line1Start.y += TwoLineSettingContentOffsetY();
	ImGui::SetCursorPos(line1Start);
	return line1Start;
}

static bool BeginTwoLineSettingRow(const char* rowId, const char* tooltip = nullptr, bool disabled = false)
{
	ImGuiSelectableFlags rowFlags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
	if (disabled)
		rowFlags |= ImGuiSelectableFlags_Disabled;

	ImguiStyleVar rowRounding(ImGuiStyleVar_FrameRounding, SettingsLayoutScaled(8.0f));
	const ImVec2 itemSpacing = ImGui::GetStyle().ItemSpacing;
	ImguiStyleVar rowItemSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(itemSpacing.x, 0.0f));
	const bool clicked = ImGui::Selectable(rowId, false, rowFlags, ImVec2(0.0f, TwoLineSettingRowHeight()));
	g_twoLineRowStartScreenX = ImGui::GetItemRectMin().x;
	g_twoLineRowEndScreenX = ImGui::GetItemRectMax().x;
	g_twoLineRowEndScreenY = ImGui::GetItemRectMax().y;
	if (tooltip != nullptr && (ImGui::IsItemHovered() || ImGui::IsItemFocused()))
		SetSettingsFooterText(tooltip);

	if (g_twoLineRowSeparatorsEnabled)
	{
		const ImVec2 rowMin = ImGui::GetItemRectMin();
		const ImVec2 rowMax = ImGui::GetItemRectMax();
		const float lineInset = SettingsLayoutScaled(6.0f);
		ImVec4 lineColor = ImGui::GetStyleColorVec4(ImGuiCol_Border);
		lineColor.w *= g_twoLineRowSeparatorAlpha;
		ImGui::GetWindowDrawList()->AddLine(
			ImVec2(rowMin.x + lineInset, rowMax.y - 1.0f),
			ImVec2(rowMax.x - lineInset, rowMax.y - 1.0f),
			ImGui::ColorConvertFloat4ToU32(lineColor),
			1.0f
		);
	}

	return clicked && !disabled;
}

static void EndTwoLineSettingRow()
{
	ImGui::SetCursorScreenPos(ImVec2(g_twoLineRowStartScreenX, g_twoLineRowEndScreenY));
	ImguiStyleVar rowGapSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
	ImGui::Dummy(ImVec2(0.0f, TwoLineSettingRowGap()));
}

static void RenderTwoLineSettingDescription(const ImVec2& line1Start, const char* description)
{
	if (description == nullptr || description[0] == '\0')
		return;

	// Keep inline row descriptions compact: if text uses "Title\nBody",
	// show the first body line (or title when no body exists).
	auto compactDescriptionLine = [](const char* text) -> std::string
	{
		if (text == nullptr || text[0] == '\0')
			return {};
		const char* start = text;
		const char* firstNewline = strchr(text, '\n');
		if (firstNewline != nullptr)
		{
			start = firstNewline + 1;
			while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
				++start;
			if (*start == '\0')
				start = text;
		}
		const char* end = strchr(start, '\n');
		if (end == nullptr)
			end = start + strlen(start);
		while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
			--end;
		return std::string(start, end);
	};
	const std::string compactLine = compactDescriptionLine(description);
	if (compactLine.empty())
		return;

	ImGui::SetCursorPos(ImVec2(
		line1Start.x + SettingsLayoutScaled(28.0f),
		line1Start.y + TwoLineSettingSecondaryLineOffsetY()
	));
	ImGui::PushFont(settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont());

	const ImVec4 normalColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	const ImVec4 disabledColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
	const ImVec4 textColor((normalColor.x + disabledColor.x) * 0.5f,
			(normalColor.y + disabledColor.y) * 0.5f,
			(normalColor.z + disabledColor.z) * 0.5f, 1.0f);
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	const float wrapWidth = std::max(SettingsLayoutScaled(80.0f),
		g_twoLineRowEndScreenX - ImGui::GetCursorScreenPos().x - SettingsLayoutScaled(16.0f));
	const ImVec2 textMin = ImGui::GetCursorScreenPos();
	ImGui::PushClipRect(textMin, ImVec2(g_twoLineRowEndScreenX - SettingsLayoutScaled(8.0f), g_twoLineRowEndScreenY - SettingsLayoutScaled(2.0f)), true);
	ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
	ImGui::TextUnformatted(compactLine.c_str());
	ImGui::PopTextWrapPos();
	ImGui::PopClipRect();
	ImGui::PopStyleColor();
	ImGui::PopFont();
}

static const char* GetPopupId(const SettingsUI::PopupConfig& popupCfg)
{
	switch (popupCfg.type)
	{
	case SettingsUI::PopupType::Options:
		return popupCfg.options.popupID;
	case SettingsUI::PopupType::Slider:
		return popupCfg.slider.popupID;
	default:
		return nullptr;
	}
}

static bool RenderGeneralPopupSettingRow(const char* id, const char* description,
	SettingsUI::PopupConfig& popupCfg, const char* tooltip = nullptr, bool disabled = false)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	if (popupCfg.type == SettingsUI::PopupType::Options)
	{
		popupCfg.options.valueClickable = false;
		if ((popupCfg.options.title == nullptr || popupCfg.options.title[0] == '\0')
			&& popupCfg.options.label != nullptr && popupCfg.options.label[0] != '\0')
			popupCfg.options.title = popupCfg.options.label;
		if (popupCfg.options.onOptionHighlight == nullptr)
		{
			popupCfg.options.onOptionHighlight =
				[rowTooltip, label = popupCfg.options.label](int, const char* optionText)
				{
					std::string footerText;
					if (label != nullptr && label[0] != '\0')
					{
						footerText = label;
						if (optionText != nullptr && optionText[0] != '\0')
						{
							footerText += ": ";
							footerText += optionText;
						}
					}
					else if (optionText != nullptr && optionText[0] != '\0')
					{
						footerText = optionText;
					}

					if (rowTooltip != nullptr && rowTooltip[0] != '\0')
					{
						if (!footerText.empty())
							footerText += ". ";
						footerText += rowTooltip;
					}

					if (!footerText.empty())
						SetSettingsFooterText(footerText.c_str());
				};
		}
	}
	else if (popupCfg.type == SettingsUI::PopupType::Slider)
	{
		popupCfg.slider.preferTextEntry = false;
	}

	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	if (rowActivated)
	{
		const char* popupId = GetPopupId(popupCfg);
		if (popupId != nullptr && popupId[0] != '\0')
		{
			if (popupCfg.type == SettingsUI::PopupType::Slider)
				popupCfg.slider.preferTextEntry = ImGui::GetIO().MouseClicked[ImGuiMouseButton_Left];
			ImGui::OpenPopup(popupId);
		}
	}

	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	// Vertically center right-side value text within the full highlighted row.
	// Cursor is already at row content start, so offset from that baseline.
	const float centeredValueOffset = (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f;
	if (popupCfg.type == SettingsUI::PopupType::Options)
		popupCfg.options.valueVerticalOffset = centeredValueOffset;
	else if (popupCfg.type == SettingsUI::PopupType::Slider)
		popupCfg.slider.valueVerticalOffset = centeredValueOffset;

	{
		DisabledScope _(disabled);
		SettingsUI::SettingPopup(popupCfg);
	}
	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
	ImGui::PopID();
	return rowActivated;
}

template <typename OnToggle>
static bool RenderGeneralToggleSettingRow(const char* id, const char* icon, const char* label,
	const char* description, bool value, OnToggle&& onToggle, const char* tooltip = nullptr,
	bool disabled = false)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	if (rowActivated)
	{
		value = !value;
		onToggle(value);
	}

	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	{
		DisabledScope _(disabled);
		SettingIcon(icon, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(label);
		ImGui::PopFont();

		const float toggleWidth = SettingsLayoutScaled(50.0f);
		ImGui::SameLine(RightColumnX(toggleWidth));

		const float toggleHeight = SettingsLayoutScaled(24.0f);
		const float verticalOffset = (TwoLineSettingContentHeight() - toggleHeight) * 0.5f;
		const ImVec2 togglePos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(togglePos.x, togglePos.y + verticalOffset));
		RenderToggleSwitchVisual(value);
	}

	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
	ImGui::PopID();
	return rowActivated;
}

template <typename DrawInput>
static void RenderGeneralTextInputSettingRow(const char* id, const char* icon, const char* label,
	const char* description, DrawInput&& drawInput, const char* tooltip = nullptr, bool disabled = false,
	float inputWidthPx = 220.0f)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	{
		DisabledScope _(disabled);
		SettingIcon(icon, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(label);
		ImGui::PopFont();

		const float inputWidth = SettingsLayoutScaled(inputWidthPx);
		ImGui::SameLine(RightColumnX(inputWidth));
		if (rowActivated)
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(inputWidth);
		drawInput();
	}

	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
	ImGui::PopID();
}

// Physical gamepad settings popup (ported from settings_controls.cpp)
// Handles rumble, deadzone, and saturation.
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
		if (ControllerPopupCancelPressed())
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
		if (ImGui::Button(T("Done"), ScaledVec2(100, 30)))
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
		if (gamepad->is_rumble_enabled())
		{
			header(T("Rumble"));
			int power = gamepad->get_rumble_power();
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			if (ImGui::SliderInt(T("Power"), &power, 0, 100, "%d%%"))
				gamepad->set_rumble_power(power);
			ImGui::SameLine();
			ShowFooterHelpMarker(T("Rumble power"));
		}
		if (gamepad->has_analog_stick())
		{
			header(T("Thumbsticks"));
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			if (ImGui::SliderInt(T("Dead zone"), &deadzone, 0, 100, "%d%%"))
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowFooterHelpMarker(T("Minimum deflection to register as input"));
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(SettingsLayoutScaled(300));
			if (ImGui::SliderInt(T("Saturation"), &saturation, 50, 200, "%d%%"))
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

static void detect_input_popup(const MappingEntry* mapping)
{
	ImVec2 padding = ScaledVec2(20, 20);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
	ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
	const std::string mapControlTitle = std::string(T("Map Control")) + "###Map Control";
	if (ImGui::BeginPopupModal(mapControlTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text(T("Waiting for control '%s'..."), mapping->name);
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
				std::shared_ptr<InputMapping> input_mapping = g_currentGamepadForMapping->get_input_mapping();
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

static void displayMappedControl(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey key)
{
	std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
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
			if (ImGui::Checkbox(T("Sequential"), &(combo.sequential)))
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

	static int map_system = DC_PLATFORM_DREAMCAST;
	static int item_current_map_idx = 0;
	static int last_item_current_map_idx = 2;
	if (g_currentGamepadForMapping == nullptr)
	{
		g_currentGamepadForMapping = gamepad;
		g_currentGamepadForMapping->listenButtons(buttonListener);
	}

	std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
	if (input_mapping == nullptr || ImGui::Button(T("Done"), ScaledVec2(100, 30)))
	{
		ImGui::CloseCurrentPopup();
		gamepad->save_mapping(map_system);
		last_item_current_map_idx = 2;
		ImGui::EndPopup();
		ResetControllerMappingRuntime();
		return;
	}
	ImGui::SetItemDefaultFocus();

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
		gameConfigWidth = ImGui::CalcTextSize(gamepad->isPerGameMapping() ? T("Delete Game Config") : T("Make Game Config")).x
			+ ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2;
	}

	ImGui::SameLine(0, ImGui::GetContentRegionAvail().x - comboWidth - gameConfigWidth
		- ImGui::GetStyle().ItemSpacing.x - SettingsLayoutScaled(100) * 2 - portWidth);
	ImGui::AlignTextToFramePadding();

	if (!settings.content.gameId.empty())
	{
		if (gamepad->isPerGameMapping())
		{
			if (ImGui::Button(T("Delete Game Config"), ScaledVec2(0, 30)))
			{
				gamepad->setPerGameMapping(false);
				if (!gamepad->find_mapping(map_system))
				{
					if (map_system == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
						gamepad->resetMappingToDefault(g_arcade_button_mode, true);
				}
			}
		}
		else
		{
			if (ImGui::Button(T("Make Game Config"), ScaledVec2(0, 30)))
				gamepad->setPerGameMapping(true);
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
				if (ImGui::RadioButton(T("Gamepad"), !hitbox))
					hitbox = false;
				ImGui::SameLine();
				if (ImGui::RadioButton(T("Arcade / Hit Box"), hitbox))
					hitbox = true;
			}
			ImGui::NewLine();
			{
				ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(SettingsLayoutScaled(20), ImGui::GetStyle().ItemSpacing.y));
				ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
				if (ImGui::Button(T("Yes")))
				{
					gamepad->resetMappingToDefault(g_arcade_button_mode, !hitbox);
					gamepad->save_mapping(map_system);
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
	if (last_item_current_map_idx == 2 && game_started)
		item_current_map_idx = settings.platform.isArcade() ? 1 : 0;

	ImGui::SetNextItemWidth(comboWidth);
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(ImGui::GetStyle().FramePadding.x, (SettingsLayoutScaled(30) - ImGui::GetFontSize()) / 2));
	ImGui::Combo("##arcadeMode", &item_current_map_idx, items, IM_ARRAYSIZE(items));
	ImGui::PopStyleVar();
	if (last_item_current_map_idx != 2 && item_current_map_idx != last_item_current_map_idx)
		gamepad->save_mapping(map_system);

	const MappingEntry* systemMapping = dcButtons;
	if (item_current_map_idx == 0)
	{
		g_arcade_button_mode = false;
		map_system = DC_PLATFORM_DREAMCAST;
		systemMapping = dcButtons;
	}
	else
	{
		g_arcade_button_mode = true;
		map_system = DC_PLATFORM_NAOMI;
		systemMapping = arcadeButtons;
	}

	if (item_current_map_idx != last_item_current_map_idx)
	{
		if (!gamepad->find_mapping(map_system))
		{
			if (map_system == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
				gamepad->resetMappingToDefault(g_arcade_button_mode, true);
		}
		input_mapping = gamepad->get_input_mapping();
		last_item_current_map_idx = item_current_map_idx;
	}

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
		displayMappedControl(gamepad, systemMapping->key);

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
		detect_input_popup(systemMapping);
		ImGui::SameLine();
		if (ImGui::Button(T("Unmap")))
		{
			input_mapping = gamepad->get_input_mapping();
			unmapControl(input_mapping, g_gamepad_port_for_mapping, systemMapping->key);
		}
		ImGui::NextColumn();
	}
	ImGui::Columns(1, nullptr, false);
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::EndChild();
	error_popup();
	ImGui::EndPopup();
}

static void RenderGeneralRightValue(const std::string& value, float valueWidthPx, float extraRightOffsetPx = 0.0f, bool disabled = false)
{
	const float valueWidth = SettingsLayoutScaled(valueWidthPx);
	const float extraRight = SettingsLayoutScaled(extraRightOffsetPx);
	const float verticalOffset = (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f;
	std::string clipped = middleEllipsis(value, valueWidth);
	ImFont* valueFont = settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;

	ImGui::SameLine(RightColumnX(valueWidth + extraRight));
	const ImVec2 valuePos = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(valuePos.x, valuePos.y + verticalOffset));
	ImGui::PushFont(valueFont);
	if (disabled)
		ImGui::TextDisabled("%s", clipped.c_str());
	else
		ImGui::TextUnformatted(clipped.c_str());
	ImGui::PopFont();
}

static void RenderGeneralInfoRow(const char* id, const char* icon, const char* label, const char* description,
	const std::string& value, bool disabledValue = false, const char* tooltip = nullptr)
{
	ImGui::PushID(id);
	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	BeginTwoLineSettingRow("##row", rowTooltip, true);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	SettingIcon(icon, SettingsLayoutSize(20.0f, 20.0f));
	ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
	ImGui::PushFont(largeFont);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();
	RenderGeneralRightValue(value, 280.0f, 0.0f, disabledValue);
	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();

	ImGui::PopID();
}

static void manageSinglePath(const char* label, config::Option<std::string, false>& pathOption, const char* helpText,
		bool selectFile = false, const std::string& extension = "")
{
	ImGui::PushID(label);
	const bool rowActivated = BeginTwoLineSettingRow("##row", helpText);
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	SettingIcon(ICON_FA_FOLDER_OPEN, SettingsLayoutSize(20.0f, 20.0f));
	ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
	ImGui::PushFont(largeFont);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();

	bool openPopup = rowActivated;
	const bool hasPath = !pathOption.get().empty();
	if (hasPath)
	{
		const float deleteBtnWidthPx = 26.0f;
		const float deleteBtnHeightPx = 22.0f;
		const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
		RenderGeneralRightValue(pathOption.get(), 280.0f, extraOffsetPx);

		const float buttonWidth = SettingsLayoutScaled(deleteBtnWidthPx);
		const float buttonHeight = SettingsLayoutScaled(deleteBtnHeightPx);
		const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(buttonWidth));
		ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
		if (SettingsIconButton("##ClearPath", ICON_FA_TRASH_CAN, ImVec2(buttonWidth, buttonHeight)))
		{
			pathOption.get().clear();
			openPopup = false;
		}
	}
	else
	{
		RenderGeneralRightValue(T("Set Path"), 280.0f, 0.0f, true);
	}

	RenderTwoLineSettingDescription(line1Start, helpText);
	EndTwoLineSettingRow();
	ImGui::PopID();

	const std::string popupName = std::string(T("Select ")) + label;
	select_file_popup(popupName.c_str(), [&pathOption](bool cancelled, const std::string& selection) {
		if (!cancelled)
			pathOption.get() = selection;
		return true;
	}, selectFile, extension);
#ifdef __ANDROID__
	if (openPopup)
	{
		const char *mimeType = extension == ".xml" ? "application/xml" : "*/*";
		const StoragePopupResult storageResult = select_storage_popup(!selectFile, false, popupName, [&pathOption](bool cancelled, const std::string& selection) {
			if (!cancelled)
			{
				pathOption.get() = selection;
				SaveSettings();
			}
			return true;
		}, selectFile ? mimeType : "");
		if (storageResult == StoragePopupResult::Unsupported)
			ImGui::OpenPopup(popupName.c_str());
	}
#else
	if (openPopup)
		ImGui::OpenPopup(popupName.c_str());
#endif
}

static void managePathList(const char* label, const std::string& singularLabel, std::vector<std::string>& paths, const char* helpText)
{
	ImguiID _(label);
	int to_delete = -1;
	bool openPopup = false;
	char buffer[256];

	snprintf(buffer, sizeof(buffer), T("Add %s"), singularLabel.c_str());
	const std::string addRowLabel = buffer;

	snprintf(buffer, sizeof(buffer), T("Add another %s path"), singularLabel.c_str());
	const std::string addRowDescription = buffer;

	for (u32 i = 0; i < paths.size(); i++)
	{
		ImGui::PushID(static_cast<int>(i));
		const bool rowActivated = BeginTwoLineSettingRow("##row", helpText);
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();
		(void)rowActivated;

		SettingIcon(ICON_FA_FOLDER_OPEN, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		if (i == 0)
			ImGui::TextUnformatted(label);
		else
		{
			std::string rowLabel = singularLabel + " " + std::to_string(i + 1);
			ImGui::TextUnformatted(rowLabel.c_str());
		}
		ImGui::PopFont();

		const float deleteBtnWidthPx = 26.0f;
		const float deleteBtnHeightPx = 22.0f;
		const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
		RenderGeneralRightValue(paths[i], 280.0f, extraOffsetPx);

		const float buttonWidth = SettingsLayoutScaled(deleteBtnWidthPx);
		const float buttonHeight = SettingsLayoutScaled(deleteBtnHeightPx);
		const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(buttonWidth));
		ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
		if (SettingsIconButton("##DeletePath", ICON_FA_TRASH_CAN, ImVec2(buttonWidth, buttonHeight)))
			to_delete = static_cast<int>(i);

		snprintf(buffer, sizeof(buffer), T("Additional %s path"), singularLabel.c_str());
		const std::string additionalDesc = buffer;
		const char* desc = (i == 0) ? helpText : additionalDesc.c_str();
		RenderTwoLineSettingDescription(line1Start, desc);
		EndTwoLineSettingRow();
		ImGui::PopID();
	}

	ImGui::PushID("AddPathRow");
	{
		const bool addRowActivated = BeginTwoLineSettingRow("##row", helpText);
		const ImVec2 line1Start = BeginTwoLineSettingRowContent();

		SettingIcon(ICON_FA_PLUS, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(addRowLabel.c_str());
		ImGui::PopFont();
		RenderGeneralRightValue(T("Select Path"), 280.0f, 0.0f, true);
		RenderTwoLineSettingDescription(line1Start, addRowDescription.c_str());
		EndTwoLineSettingRow();
		openPopup = addRowActivated;
	}
	ImGui::PopID();

	if (to_delete >= 0)
	{
		paths.erase(paths.begin() + to_delete);
		SaveSettings();
	}

	// Handle file selection popup
	snprintf(buffer, sizeof(buffer), T("Select %s"), label);
	const std::string popupTitle = buffer;
	auto* pathsPtr = &paths;
	select_file_popup(popupTitle.c_str(), [pathsPtr](bool cancelled, const std::string& selection) {
		if (!cancelled)
		{
			pathsPtr->push_back(selection);
			SaveSettings();
		}
		return true;
	});
#ifdef __ANDROID__
	if (openPopup)
	{
		const StoragePopupResult storageResult = select_storage_popup(true, false, popupTitle, [pathsPtr](bool cancelled, const std::string& selection) {
			if (!cancelled)
			{
				pathsPtr->push_back(selection);
				SaveSettings();
			}
			return true;
		});
		if (storageResult == StoragePopupResult::Unsupported)
			ImGui::OpenPopup(popupTitle.c_str());
	}
#else
	if (openPopup)
		ImGui::OpenPopup(popupTitle.c_str());
#endif
}


// Global state instance
SettingsUIState g_state;

// Input polling and Android touch dispatch read this flag from other threads.
static std::atomic<bool> g_textureFilterLiveTune{false};

static void applyPendingSettings()
{
	dreampotato::update();
	if (g_mapleDevicesChangedInSettings)
	{
		g_mapleDevicesChangedInSettings = false;
		if (game_started && settings.platform.isConsole())
		{
#if defined(USE_DREAMLINK_DEVICES)
			reconnectDreamLinks();
#endif
			maple_ReconnectDevices();
			reset_vmus();
		}
	}
	SaveSettings();
}

// Preserve the stored renderer IDs so old settings files cannot silently select a different filter.
// ID 14 was Spline36 and is intentionally no longer exposed.
static constexpr std::array<int, 19> sampleTextureFilterValues { 0, 19, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18 };

static std::array<const char*, 19> getSampleTextureFilterLabels()
{
	return {
		T("Disabled"), T("Nearest-Neighbor"), T("Bilinear"), T("Bilinear / No Edge Blending"), T("Jinc2"),
		T("Jinc2 / No Edge Blending"), T("MMPX"), T("MMPX Enhanced"), T("MMPX Advanced"),
		T("Jinc2 Anti-Ring"), T("Jinc2 Sharp Anti-Ring"), T("Jinc2 Mild Anti-Ring"),
		T("Jinc2 Extra Sharp Anti-Ring"), T("Lanczos2 Anti-Ring"), T("Lanczos3 Anti-Ring"),
		T("Robidoux"), T("Robidoux Sharp"), T("Bicubic Catmull-Rom"),
		T("Bicubic Mitchell")
	};
}

static int getSampleTextureFilterOptionIndex(int value)
{
	for (size_t index = 0; index < sampleTextureFilterValues.size(); ++index)
		if (sampleTextureFilterValues[index] == value)
			return static_cast<int>(index);
	return 0;
}

static constexpr std::array<int, 8> maxFilteredTextureSizeValues { 8, 16, 32, 64, 128, 256, 512, 1024 };

static std::array<const char*, 8> getMaxFilteredTextureSizeLabels()
{
	return { T("8x8"), T("16x16"), T("32x32"), T("64x64"), T("128x128"), T("256x256"), T("512x512"), T("1024x1024+") };
}

bool textureFilterLiveTuneActive()
{
	return g_textureFilterLiveTune.load(std::memory_order_relaxed);
}

void stopTextureFilterLiveTune()
{
	g_textureFilterLiveTune.store(false, std::memory_order_relaxed);
}

void renderTextureFilterLiveTuneOverlay()
{
	if (!textureFilterLiveTuneActive())
		return;
	// Recheck the session, but reserve the stateful canPause() check for a pause request.
	if (!game_started || ggpo::active())
	{
		stopTextureFilterLiveTune();
		return;
	}

	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
#ifdef __ANDROID__
	const float panelHeight = std::max(displaySize.y * .42f, uiScaled(250.f));
	ImGui::SetNextWindowPos(ImVec2(0.f, displaySize.y - panelHeight), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(displaySize.x, panelHeight), ImGuiCond_Always);
	flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
#else
	const float margin = uiScaled(18.f);
	const ImVec2 panelSize(std::min(uiScaled(620.f), std::max(1.f, displaySize.x - margin * 2.f)),
			std::min(uiScaled(330.f), std::max(1.f, displaySize.y - uiScaled(96.f))));
	ImGui::SetNextWindowPos(ImVec2(displaySize.x - panelSize.x - margin, uiScaled(48.f)), ImGuiCond_Once);
	ImGui::SetNextWindowSize(panelSize, ImGuiCond_Once);
#endif
	ImGui::SetNextWindowBgAlpha(.94f);

	bool open = true;
	bool returnToSettings = false;
	if (ImGui::Begin(T("Texture Filtering Live Tune###TextureFilteringLiveTune"), &open, flags))
	{
		if (ImGui::Button(T("Back to Video Settings")))
			returnToSettings = true;
		ImGui::SameLine();
		ImGui::TextDisabled("%s", T("live game preview"));
		ImGui::Separator();
		ImGui::TextWrapped("%s", T("Try the filter and its texture-size limit while the game continues behind this panel."));
		ImGui::Spacing();

		const auto filterLabels = getSampleTextureFilterLabels();
		if (config::SampleTextureFilter.get() == 14)
		{
			config::SampleTextureFilter.set(0);
			SaveSettings();
		}
		int filterSelection = getSampleTextureFilterOptionIndex(config::SampleTextureFilter.get());
		if (ImGui::BeginCombo(T("Sample Texture Filter"), filterLabels[filterSelection]))
		{
#if defined(__ANDROID__)
			// Combo popups need the same drag-scroll opt-in as Settings lists,
			// before their selectable rows can hand a finger swipe to the popup.
			ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_DragScrolling;
#endif
			for (int index = 0; index < static_cast<int>(filterLabels.size()); ++index)
			{
				const bool selected = index == filterSelection;
				if (ImGui::Selectable(filterLabels[index], selected))
				{
					config::SampleTextureFilter.set(sampleTextureFilterValues[index]);
					SaveSettings();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
#if defined(__ANDROID__)
			scrollWhenDraggingOnVoid();
			windowDragScroll(false);
#endif
			ImGui::EndCombo();
		}

		int sizeSelection = 0;
		for (size_t index = 0; index < maxFilteredTextureSizeValues.size(); ++index)
			if (maxFilteredTextureSizeValues[index] == config::MaxFilteredTextureSize.get())
			{
				sizeSelection = static_cast<int>(index);
				break;
			}
		const auto sizeLabels = getMaxFilteredTextureSizeLabels();
		if (ImGui::BeginCombo(T("Filter Resolution Limit"), sizeLabels[sizeSelection]))
		{
#if defined(__ANDROID__)
			ImGui::GetCurrentWindow()->Flags |= ImGuiWindowFlags_DragScrolling;
#endif
			for (int index = 0; index < static_cast<int>(sizeLabels.size()); ++index)
			{
				const bool selected = index == sizeSelection;
				if (ImGui::Selectable(sizeLabels[index], selected))
				{
					config::MaxFilteredTextureSize.set(maxFilteredTextureSizeValues[index]);
					SaveSettings();
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
#if defined(__ANDROID__)
			scrollWhenDraggingOnVoid();
			windowDragScroll(false);
#endif
			ImGui::EndCombo();
		}
		ImGui::TextDisabled("%s", T("Filter resolution limit uses the texture width bucket. Larger textures are left on the normal sampler path."));
	}
	ImGui::End();

	if (returnToSettings)
	{
		stopTextureFilterLiveTune();
		gui_open_texture_filter_settings();
	}
	else if (!open)
	{
		stopTextureFilterLiveTune();
	}
}

// Reset state when entering settings screen
void resetState()
{
	g_state.currentTab = SettingsTab::General;
	g_focusSettingsNavigation = true;
	g_focusSettingsContent = g_focusSettingsBack = false;
	g_tabSlideStart = -1.0;
	ResetSettingsFooter();
}

void openTab(SettingsTab tab)
{
	g_state.currentTab = tab;
	g_tabSlideStart = -1.0;
	g_scrollToBoxArtSection = false;
	ResetSettingsFooter();
}

void focusBoxArtSection()
{
	openTab(SettingsTab::Library);
	g_scrollToBoxArtSection = true;
}

// Get display name for each tab
const char* getTabName(SettingsTab tab)
{
	switch (tab)
	{
	case SettingsTab::General:
		return T("General");
	case SettingsTab::Library:
		return T("Library");
	case SettingsTab::Video:
		return T("Video");
	case SettingsTab::Shader:
		return T("Shader");
	case SettingsTab::OnScreenDisplay:
		return T("On Screen Display");
	case SettingsTab::Audio:
		return T("Audio");
	case SettingsTab::Controls:
		return T("Controls");
	case SettingsTab::Network:
		return T("Network");
	case SettingsTab::Advanced:
		return T("Advanced");
	case SettingsTab::About:
		return T("About");
	default:
		return T("Unknown");
	}
}

static float SettingsNavigationWidth(const char* exitLabel)
{
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	const auto& themeLayout = CustomTheme::activeLayout();
#if defined(__ANDROID__)
	const bool compactLayout = UseCompactSettingsLayout();
	const float maxWidth = displaySize.x * (compactLayout ? 0.24f : 0.20f);
	const float minWidth = std::min(maxWidth,
		std::max(displaySize.x * 0.10f, SettingsFixedNavigationScaled(72.0f)));
	const float layoutWidth = displaySize.x * (compactLayout ? 0.14f : 0.16f);

	ImFont* navFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : navFont;
	const float fontSize = SettingsPreviewFontSize(navFont);
	float labelWidth = navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, exitLabel).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ARROW_LEFT).x
			+ SettingsLayoutScaled(8.0f);
	for (int i = 0; i < (int)SettingsTab::Count; i++)
	{
		const char* tabName = getTabName((SettingsTab)i);
		labelWidth = std::max(labelWidth, navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, tabName).x);
	}

	const float themedWidth = themeLayout.settingsSidebarWidth > 0.0f
			? SettingsLayoutScaled(themeLayout.settingsSidebarWidth) : layoutWidth;
	const float desiredWidth = std::max(themedWidth, labelWidth + SettingsLayoutScaled(24.0f));
	return std::clamp(desiredWidth, minWidth, maxWidth);
#else
	const float minWidth = 136.0f;
	const float maxWidth = std::clamp(displaySize.x * 0.20f, minWidth, 310.0f);
	const float layoutWidth = UseCompactSettingsLayout() ? displaySize.x * 0.16f : uiScaled(225.0f);

	ImFont* navFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : navFont;
	const float fontSize = navFont->LegacySize;
	float labelWidth = navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, exitLabel).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ARROW_LEFT).x
			+ uiScaled(8.0f);
	for (int i = 0; i < (int)SettingsTab::Count; i++)
	{
		const char* tabName = getTabName((SettingsTab)i);
		labelWidth = std::max(labelWidth, navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, tabName).x);
	}

	const float themedWidth = themeLayout.settingsSidebarWidth > 0.0f
			? uiScaled(themeLayout.settingsSidebarWidth) : layoutWidth;
	const float desiredWidth = std::max(themedWidth, labelWidth + ImGui::GetStyle().WindowPadding.x * 2.0f);
	return std::clamp(desiredWidth, minWidth, maxWidth);
#endif
}

static bool SettingsBackButton(const char* icon, const char* label)
{
	const bool pressed = ImGui::Button("##SettingsBackButton", ImVec2(-1, 0));
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();

	ImFont* textFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImFont* iconFont = settingsIconFont;
	const float fontSize = ImGui::GetFontSize();
	const ImVec2 textSize = textFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, label);
	const ImVec2 iconSize = iconFont != nullptr ? iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon) : ImVec2();
	const float gap = iconFont != nullptr ? SettingsLayoutScaled(8.0f) : 0.0f;
	const float contentWidth = iconSize.x + gap + textSize.x;
	const float minTextX = min.x + SettingsLayoutScaled(6.0f);
	float x = std::max(minTextX, min.x + (max.x - min.x - contentWidth) * 0.5f);
	const float y = min.y + (max.y - min.y - textSize.y) * 0.5f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
	const ImVec4 clipRect(min.x, min.y, max.x, max.y);
	if (iconFont != nullptr)
	{
		drawList->AddText(iconFont, fontSize, ImVec2(x, y), color, icon, nullptr, 0.0f, &clipRect);
		x += iconSize.x + gap;
	}
	drawList->AddText(textFont, fontSize, ImVec2(x, y), color, label, nullptr, 0.0f, &clipRect);
	return pressed;
}

// Render left navigation rail with tab buttons (225px wide)
// This creates the vertical strip on the left side with navigation items
static void renderNavigationRail(const std::function<void()>& exitSettings, const char* exitLabel)
{
	const float navWidth = SettingsNavigationWidth(exitLabel);
	const float density = CustomTheme::activeLayout().density;
	const float footerGap = SettingsFooterGap();
	const float footerHeight = SettingsFooterHeight();
	const bool focusSelectedTab = g_focusSettingsNavigation;

	// Left navigation rail: 225px wide child window
	// Match the main content column height so the shared footer sits flush below both panels.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y * density));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * density));
	ImGui::BeginChild("NavigationRail", ImVec2(navWidth, -(footerHeight + footerGap)),
	                  ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
	                  ImGuiWindowFlags_DragScrolling);
	ImGui::SetWindowFontScale(SettingsTextPreviewScale());
	g_focusSettingsNavigation = false;

	ImGui::PushFont(settingsTitleFont);
	g_navigationWindow = ImGui::GetCurrentWindow();
	if (SettingsBackButton(ICON_FA_ARROW_LEFT, exitLabel))
		exitSettings();
	g_backButtonId = ImGui::GetItemID();
	if (g_focusSettingsBack)
	{
		FocusCurrentSettingsItem();
		g_focusSettingsBack = false;
	}
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Iterate through all tabs and create navigation buttons
	// Using ImGui::Selectable() which provides proper hover and selection states
	// Colors come from ImGuiCol_Header in the current theme
	for (int i = 0; i < (int)SettingsTab::Count; i++)
	{
		SettingsTab tab = (SettingsTab)i;
		const char* tabName = getTabName(tab);

		// Highlight the currently active tab
		// When selected, ImGui uses ImGuiCol_Header (from current theme)
		bool isSelected = (g_state.currentTab == tab);

		// Create selectable item that fills the navigation rail width
		// Clicking changes the current tab
		if (ImGui::Selectable(tabName, isSelected, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x, 0)))
		{
			g_state.currentTab = tab;
			g_tabSlideStart = -1.0;
			ResetSettingsFooter();
		}
		if (focusSelectedTab && isSelected)
			FocusCurrentSettingsItem();
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText(GetTabHelpText(tab));

		if (!IsAnySettingsPopupOpen()
			&& ImGui::IsItemFocused()
			&& (ImGui::IsKeyPressed(ImGuiKey_Enter, false)
				|| ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)
				|| ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)))
		{
			g_state.currentTab = tab;
			g_focusSettingsContent = true;
			ResetSettingsFooter();
		}
	}

	// Keep touch swipes that begin over a tab row as scrolling. This is the
	// same path used by the content pane, so taps and controller activation
	// retain their existing Selectable behavior.
	scrollWhenDraggingOnVoid();
	windowDragScroll();
	ImGui::PopFont();

	ImGui::EndChild();
	ImGui::PopStyleVar(2);
}

// Render right content area (remaining width for settings content)
// This displays the actual settings for the currently selected tab
static void renderContentArea()
{
	const float density = CustomTheme::activeLayout().density;
	const float footerGap = SettingsFooterGap();
	const float footerHeight = SettingsFooterHeight();
	const bool focusContent = g_focusSettingsContent;

	// Right content area: fills remaining space
	// ImGui::SameLine() positions this after the navigation rail
	// No width specified means it takes all remaining space
	if (focusContent)
		ImGui::SetNextWindowFocus();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y * density));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * density));
	ImGui::BeginChild("ContentArea", ImVec2(0, -(footerHeight + footerGap)),
	                  ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened,
	                  ImGuiWindowFlags_DragScrolling);
	ImGui::SetWindowFontScale(SettingsTextPreviewScale());
	g_focusSettingsContent = false;

	if (focusContent)
		ImGui::SetKeyboardFocusHere();
	g_contentWindow = ImGui::GetCurrentWindow();
	const bool sliding = g_tabSlideStart >= 0.0;
	const int firstVertex = g_contentWindow->DrawList->VtxBuffer.Size;
	// Animation changes presentation only. Input is disabled until the visual
	// position agrees with the regular, stationary hit targets again.
	ImGui::BeginDisabled(sliding);

	// Call the appropriate tab renderer based on current selection
	switch (g_state.currentTab)
	{
	case SettingsTab::General:
		renderGeneralTab();
		break;
	case SettingsTab::Library:
		renderLibraryTab();
		break;
	case SettingsTab::Video:
		renderVideoTab();
		break;
	case SettingsTab::Shader:
		renderShaderTab();
		break;
	case SettingsTab::OnScreenDisplay:
		renderOnScreenDisplayTab();
		break;
	case SettingsTab::Audio:
		renderAudioTab();
		break;
	case SettingsTab::Controls:
		renderControlsTab();
		break;
	case SettingsTab::Network:
		renderNetworkTab();
		break;
	case SettingsTab::Advanced:
		renderAdvancedTab();
		break;
	case SettingsTab::About:
		renderAboutTab();
		break;
	default:
		break;
	}

	ImGui::EndDisabled();
	if (sliding)
	{
		const float progress = std::clamp(float((ImGui::GetTime() - g_tabSlideStart) / kTabSlideSeconds), 0.0f, 1.0f);
		const float phase = progress < 0.5f ? progress * 2.0f : (1.0f - progress) * 2.0f;
		const float eased = phase * phase * (3.0f - 2.0f * phase);
		const float offset = (progress < 0.5f ? -1.0f : 1.0f) * g_tabSlideDirection
				* g_contentWindow->InnerRect.GetWidth() * eased;
		for (ImGuiWindow* window : GImGui->Windows)
		{
			if (
				!window->Active ||
				(window != g_contentWindow && !ImGui::IsWindowChildOf(window, g_contentWindow, false))
			) {
				continue;
			}

			ImDrawList* draw = window->DrawList;
			for (int i = window == g_contentWindow ? firstVertex : 0; i < draw->VtxBuffer.Size; ++i) {
				draw->VtxBuffer[i].pos.x += offset;
			}
			// Nested lists move too, clipped to the stationary content pane.
			if (window != g_contentWindow) {
				for (ImDrawCmd& command : draw->CmdBuffer)
				{
					command.ClipRect.x = std::clamp(command.ClipRect.x + offset, g_contentWindow->InnerClipRect.Min.x, g_contentWindow->InnerClipRect.Max.x);
					command.ClipRect.z = std::clamp(command.ClipRect.z + offset, command.ClipRect.x, g_contentWindow->InnerClipRect.Max.x);
				}
			}
		}
	}

	// The content pane uses drag scrolling, but Android swipes often begin on
	// selectable rows. Keep this after rendering rows so the helper can turn
	// those finger drags into scrolling instead of row activation.
	scrollWhenDraggingOnVoid();
#if defined(__ANDROID__)
	windowDragScroll(false);
#else
	windowDragScroll();
#endif

	if (!IsAnySettingsPopupOpen()
		&& !g_controllerPopupWasOpen
		&& ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
		&& ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		g_focusSettingsNavigation = true;
	}

	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::Dummy(ImVec2(0.0f, footerGap));
	RenderSettingsFooterBar();
}

// This is similar to ImGui::CollapsingHeader, but it has a separate id field plus label parts
static bool RenderCollapsingHeader(
	const char* id,
	const char* labelIcon,
	const char* labelText,
	ImGuiTreeNodeFlags flags = 0
)
{
	// Use a static thread-local buffer to avoid heap allocations every frame
	thread_local std::string labelStr;
	labelStr.clear(); // Keeps the allocated capacity intact

	ImFont* titleFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImGui::PushFont(titleFont);
	if (labelIcon && *labelIcon != '\0')
	{
		ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : ImGui::GetFont();
		const float fontSize = ImGui::GetFontSize();
		const float iconWidth = iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, labelIcon).x;
		const float spaceWidth = std::max(1.0f, titleFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, " ").x);
		const int spacerCount = std::max(2, static_cast<int>(std::ceil((iconWidth + SettingsLayoutScaled(2.0f)) / spaceWidth)));
		labelStr.append(spacerCount, ' ');
	}

	labelStr += labelText;

	if (id && *id != '\0')
	{
		labelStr += "###";
		labelStr += id;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(SettingsLayoutScaled(8.0f), SettingsLayoutScaled(10.0f)));
	bool open = ImGui::CollapsingHeader(labelStr.c_str(), flags);
	if (labelIcon && *labelIcon != '\0')
	{
		ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : ImGui::GetFont();
		const ImVec2 itemMin = ImGui::GetItemRectMin();
		const ImVec2 itemMax = ImGui::GetItemRectMax();
		const float iconFontSize = ImGui::GetFontSize();
		const ImVec2 iconTextSize = iconFont->CalcTextSizeA(iconFontSize, FLT_MAX, -1.0f, labelIcon);
		const ImVec2 iconPos(
			itemMin.x + ImGui::GetTreeNodeToLabelSpacing(),
			itemMin.y + (itemMax.y - itemMin.y - iconTextSize.y) * 0.5f
		);
		ImGui::GetWindowDrawList()->AddText(
			iconFont,
			iconFontSize,
			iconPos,
			ImGui::GetColorU32(ImGuiCol_Text),
			labelIcon);
	}
	ImGui::PopStyleVar();
	ImGui::PopFont();
	return open;
}

// Render General tab with all settings from settings_general.cpp
static void renderSettingsContentTab(SettingsTab tab)
{
	using namespace SettingsUI;
	ScopedTwoLineRowStyle generalRowStyle(12.0f, true, 0.5f);
	if (tab == SettingsTab::General)
	{

	ImGui::TextDisabled("%s", T("General Configuration"));
	ImGui::Separator();
	if (ImGui::Button(T("Ready to Play Checklist")))
		gui_open_setup_checklist();
	ImGui::TextWrapped("%s", T("Review game folders, firmware and controls."));
	ImGui::Spacing();

	// ========================================
	// Language & Region Section
	// ========================================
	if (RenderCollapsingHeader("Language&RegionSection", ICON_FA_GLOBE, T("Language & Region"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		{
			struct UILanguageOption
			{
				const char* label;
				const char* locale;
			};

			const UILanguageOption uiLanguageOptions[] = {
				{ T("System Default"), "" },
				{ Tnop("English"), "en" },
				{ Tnop("Français"), "fr" },
				{ Tnop("Magyar"), "hu" },
				{ Tnop("日本語"), "ja" },
				{ Tnop("Português (Brasil)"), "pt_BR" },
				{ Tnop("Svenska"), "sv" },
				{ Tnop("Türkçe"), "tr" },
				{ Tnop("简体中文"), "zh_CN" },
				{ Tnop("正體中文"), "zh_TW" },
			};

			const char* uiLanguageLabels[IM_ARRAYSIZE(uiLanguageOptions)];
			for (int i = 0; i < IM_ARRAYSIZE(uiLanguageOptions); i++)
				uiLanguageLabels[i] = uiLanguageOptions[i].label;

			int uiLanguageSelection = 0;
			const std::string currentUILanguage = config::UILanguage.get();
			for (int i = 0; i < IM_ARRAYSIZE(uiLanguageOptions); i++)
			{
				if (currentUILanguage == uiLanguageOptions[i].locale)
				{
					uiLanguageSelection = i;
					break;
				}
			}

			SettingsUI::PopupConfig uiLanguageCfg {};
			uiLanguageCfg.type = SettingsUI::PopupType::Options;
			uiLanguageCfg.options.label = T("UI Language");
			uiLanguageCfg.options.icon = ICON_FA_LANGUAGE;
			uiLanguageCfg.options.popupID = Tnop("UILanguagePopup");
			uiLanguageCfg.options.options = uiLanguageLabels;
			uiLanguageCfg.options.optionCount = IM_ARRAYSIZE(uiLanguageLabels);
			uiLanguageCfg.options.currentValue = &uiLanguageSelection;
			uiLanguageCfg.options.valueWidth = 220.0f;
			uiLanguageCfg.options.onChange = [&](int selectedIndex) {
				if (selectedIndex < 0 || selectedIndex >= IM_ARRAYSIZE(uiLanguageOptions))
					return true;
				config::UILanguage = uiLanguageOptions[selectedIndex].locale;
				config::UILanguage.save();
				i18n::reloadLanguage();
				mainui_reinit();
				return true;
			};
			RenderGeneralPopupSettingRow(
				Tnop("UILanguageSetting"),
				T("Language used by Hollycast menus and settings."),
				uiLanguageCfg,
				T(
					"UI Language\n"
					"Changes the language used by Hollycast menus, settings, notifications, and other app UI.\n"
					"System Default follows your device language when a translation is available.\n\n"
					"Game text is controlled separately by Dreamcast Language below."
				)
			);
		}

		// 0 -> JP, 1 -> EN, 2 -> DE, 3 -> FR, 4 -> SP, 5 -> IT, 6 -> default
		const char* languages[] = { T("Japanese"), T("English"), T("German"), T("French"), T("Spanish"), T("Italian"), T("Default") };
		SettingsUI::PopupConfig languageCfg {};
		languageCfg.type = SettingsUI::PopupType::Options;
		languageCfg.options.label = T("Dreamcast Language");
		languageCfg.options.icon = ICON_FA_LANGUAGE;
		languageCfg.options.popupID = Tnop("LanguagePopup");
		languageCfg.options.options = languages;
		languageCfg.options.optionCount = IM_ARRAYSIZE(languages);
		languageCfg.options.currentValue = &config::Language.get();
		languageCfg.options.disabled = settings.platform.isArcade();
		languageCfg.options.disabledPrefix = T("(auto) ");
		languageCfg.options.valueWidth = 220.0f;
			RenderGeneralPopupSettingRow(
				Tnop("LanguageSetting"),
				T("Dreamcast BIOS language (affects game text/menus)."),
				languageCfg,
				T(
					"Language\n"
					"Sets the Dreamcast BIOS language that games read for menus and text.\n"
					"Most games will follow this setting; some may ignore it.\n\n"
					"Arcade platforms typically use automatic region/language behavior."
				)
			);

		const char* broadcast[] = { Tnop("NTSC"), Tnop("PAL"), Tnop("PAL/M"), Tnop("PAL/N"), T("Default") };
		SettingsUI::PopupConfig broadcastCfg {};
		broadcastCfg.type = SettingsUI::PopupType::Options;
		broadcastCfg.options.label = T("Broadcast");
		broadcastCfg.options.icon = ICON_FA_TV;
		broadcastCfg.options.popupID = Tnop("BroadcastPopup");
		broadcastCfg.options.options = broadcast;
		broadcastCfg.options.optionCount = IM_ARRAYSIZE(broadcast);
		broadcastCfg.options.currentValue = &config::Broadcast.get();
		broadcastCfg.options.disabled = settings.platform.isArcade();
		broadcastCfg.options.disabledPrefix = T("(auto) ");
		broadcastCfg.options.valueWidth = 220.0f;
			RenderGeneralPopupSettingRow(
				Tnop("BroadcastSetting"),
				T("TV broadcast standard for non-VGA output modes."),
				broadcastCfg,
				T(
					"Broadcast\n"
					"Controls the TV broadcast standard used for non-VGA video modes.\n"
					"NTSC is typically 60 Hz; PAL is typically 50 Hz.\n\n"
					"If you see timing issues or a game expects a specific region video mode, try changing this."
				));

		const char* consoleRegion[] = { T("Japan"), T("USA"), T("Europe"), T("Default") };
		const char* arcadeRegion[] = { T("Japan"), T("USA"), T("Export"), T("Korea") };
		const char* const* region = settings.platform.isArcade() ? arcadeRegion : consoleRegion;

		SettingsUI::PopupConfig regionCfg {};
		regionCfg.type = SettingsUI::PopupType::Options;
		regionCfg.options.label = T("Region");
		regionCfg.options.icon = ICON_FA_EARTH_AMERICAS;
		regionCfg.options.popupID = Tnop("RegionPopup");
		regionCfg.options.options = region;
		regionCfg.options.optionCount = 4;
		regionCfg.options.currentValue = &config::Region.get();
		regionCfg.options.valueWidth = 220.0f;
			RenderGeneralPopupSettingRow(
				Tnop("RegionSetting"),
				T("BIOS region (can affect compatibility and defaults)."),
				regionCfg,
				T(
					"Region\n"
					"Sets the emulated BIOS region.\n"
					"This can affect game compatibility, default language choices, and region-locked behavior in some titles.\n\n"
					"If a game refuses to boot or behaves like the wrong region, try changing this."
				)
			);

		const char* cable[] = { T("VGA"), T("RGB Component"), T("TV Composite") };
		const bool disabled = config::Cable.isReadOnly() || settings.platform.isArcade();

		SettingsUI::PopupConfig cableCfg {};
		cableCfg.type = SettingsUI::PopupType::Options;
		cableCfg.options.label = T("Cable");
		cableCfg.options.icon = ICON_FA_PLUG;
		cableCfg.options.popupID = Tnop("CablePopup");
		cableCfg.options.options = cable;
		cableCfg.options.optionCount = IM_ARRAYSIZE(cable);
		cableCfg.options.currentValue = &config::Cable.get();
		cableCfg.options.disabled = disabled;
		cableCfg.options.disabledPrefix = T("(auto) ");
		cableCfg.options.valueWidth = 220.0f;
		cableCfg.options.valueToString = [](int storageValue) -> const char* {
			switch (storageValue)
			{
			case 0:
			case 1:
				return T("VGA");
			case 2:
				return T("RGB Component");
			case 3:
				return T("TV Composite");
			default:
				return T("VGA");
			}
		};
		cableCfg.options.storageIndexMap = [](int displayIndex) -> int {
			return displayIndex == 0 ? 0 : displayIndex + 1;
		};
			RenderGeneralPopupSettingRow(
				Tnop("CableSetting"),
				T("Emulated video cable type (affects game video modes)."),
				cableCfg,
				T(
					"Cable\n"
					"Controls the emulated Dreamcast video cable type.\n"
					"Games can change behavior depending on whether they detect VGA vs. TV output.\n\n"
					"If a game has missing effects, wrong colors, or odd mode selection, this is worth trying."
				),
				disabled
			);
	}

	// ========================================
	// Content Paths Section
	// ========================================
#if !defined(TARGET_IPHONE)
	if (RenderCollapsingHeader("ContentPathsSection", ICON_FA_FOLDER, T("Content Paths"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		int to_delete = -1;
		bool addContentRequested = false;
		for (u32 i = 0; i < config::ContentPath.get().size(); i++)
		{
			ImGui::PushID(static_cast<int>(i));
				BeginTwoLineSettingRow(
					"##row",
					T(
						"Content Paths\n"
						"These are the folders where your games are stored.\n"
						"Add one or more folders and use Rescan/Refresh to update the library view."
					)
				);
			const ImVec2 line1Start = BeginTwoLineSettingRowContent();

			SettingIcon(ICON_FA_FOLDER_OPEN, SettingsLayoutSize(20.0f, 20.0f));
			ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
			ImGui::PushFont(largeFont);
			if (i == 0)
			{
				ImGui::TextUnformatted(T("Content Folders"));
			}
			else
			{
				char buffer[128] = {};
				snprintf(buffer, sizeof(buffer), T("Content Folder %u"), i + 1);
				ImGui::TextUnformatted(buffer);
			}
			ImGui::PopFont();

			const float deleteBtnWidthPx = 26.0f;
			const float deleteBtnHeightPx = 22.0f;
			const float extraOffsetPx = deleteBtnWidthPx + 8.0f;
			RenderGeneralRightValue(config::ContentPath.get()[i], 280.0f, extraOffsetPx);

			const float buttonWidth = SettingsLayoutScaled(deleteBtnWidthPx);
			const float buttonHeight = SettingsLayoutScaled(deleteBtnHeightPx);
			const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;
			ImGui::SameLine(RightColumnX(buttonWidth));
			ImVec2 buttonPos = ImGui::GetCursorPos();
			ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
			if (SettingsIconButton("##DeleteContentPath", ICON_FA_TRASH_CAN, ImVec2(buttonWidth, buttonHeight)))
				to_delete = static_cast<int>(i);

			const char* desc = (i == 0) ? T("The folders where your games are stored") : T("Additional content folder");
			RenderTwoLineSettingDescription(line1Start, desc);
			EndTwoLineSettingRow();

			ImGui::PopID();
		}

		{
			ImGui::PushID("ContentAddRow");
				const bool addRowActivated = BeginTwoLineSettingRow(
					"##row",
					T(
						"Content Paths\n"
						"Adds another folder to scan for games.\n"
						"If you have performance issues when scanning, prefer a smaller folder or fewer subfolders."
					)
				);
			const ImVec2 line1Start = BeginTwoLineSettingRowContent();
			SettingIcon(ICON_FA_PLUS, SettingsLayoutSize(20.0f, 20.0f));
			ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
			ImGui::PushFont(largeFont);
			ImGui::TextUnformatted(T("Add Content Folder"));
			ImGui::PopFont();

			const float refreshBtnWidthPx = 26.0f;
			const float refreshBtnHeightPx = 22.0f;
			const float extraOffsetPx = refreshBtnWidthPx + 8.0f;
			RenderGeneralRightValue(T("Select Path"), 280.0f, extraOffsetPx, true);

			const float refreshButtonWidth = SettingsLayoutScaled(refreshBtnWidthPx);
			const float refreshButtonHeight = SettingsLayoutScaled(refreshBtnHeightPx);
			const float refreshVerticalOffset = (TwoLineSettingContentHeight() - refreshButtonHeight) * 0.5f;
			ImGui::SameLine(RightColumnX(refreshButtonWidth));
			ImVec2 refreshButtonPos = ImGui::GetCursorPos();
			ImGui::SetCursorPos(ImVec2(refreshButtonPos.x, refreshButtonPos.y + refreshVerticalOffset));
			const bool refreshPressed = SettingsIconButton("##RescanContentBtn", ICON_FA_ARROWS_ROTATE, ImVec2(refreshButtonWidth, refreshButtonHeight));
			if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
				SetSettingsFooterText(T("Refresh the current content library view."));

			RenderTwoLineSettingDescription(line1Start, T("Add another content folder"));
			EndTwoLineSettingRow();
			addContentRequested = addRowActivated;
			if (refreshPressed)
				gui_refresh_files();
			ImGui::PopID();
		}

		// Keep this call every frame so the popup can render while open.
		addContentPath(addContentRequested);

		if (to_delete >= 0)
		{
			scanner.stop();
			config::ContentPath.get().erase(config::ContentPath.get().begin() + to_delete);
			gui_refresh_files();
		}
	}

	#if defined(__linux__) && !defined(__ANDROID__)
		if (RenderCollapsingHeader("DataFolderSection", ICON_FA_FLOPPY_DISK, T("Data Folder"), ImGuiTreeNodeFlags_DefaultOpen))
		{
			RenderGeneralInfoRow(
				"DataFolderPathRow",
				ICON_FA_FLOPPY_DISK,
				T("Data Folder"),
				T("The folder containing BIOS files, as well as saved VMUs and states"),
				get_writable_data_path(""),
				false,
				T(
					"Data Folder\n"
					"This is Hollycast's writable data directory on this platform.\n\n"
					"It typically contains:\n"
					"- Dreamcast BIOS setup files (`dc_boot.bin` and `dc_flash.bin`)\n"
					"- Save data (VMU files) and save states\n\n"
						"If Hollycast cannot find a BIOS, double-check that your BIOS files are placed in the expected location and that their filenames match what Flycast looks for.\n"
						"Custom Paths below can also override where some of these files are stored."
				)
			);
		}
	#else
		if (RenderCollapsingHeader("HomeDirectorySection", ICON_FA_HOUSE, T("Home Directory"), ImGuiTreeNodeFlags_DefaultOpen))
		{
			RenderGeneralInfoRow(
				"HomeFolderPathRow",
				ICON_FA_HOUSE,
				T("Home Directory"),
				T("The folder where Hollycast saves configuration files and VMUs. BIOS files should be in a subfolder named \"data\""),
				get_writable_config_path(""),
				false,
				T(
					"Home Directory\n"
					"This is Hollycast's main configuration directory.\n\n"
					"It typically contains:\n"
					"- Configuration files\n"
					"- VMU saves and other user data\n\n"
					"On platforms that use a `data` subfolder, BIOS/Flash files should be placed under `data` inside this directory.\n"
					"If you change custom paths below, Hollycast may store some files elsewhere, but this directory remains the base for core configuration."
				)
			);

			ImguiStyleVar _homePadding(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
#ifdef __ANDROID__
			{
				DisabledScope _(!config::UseSafFilePicker);
				if (ImGui::Button(T("Import")))
					hostfs::importHomeDirectory();
				ImGui::SameLine();
				if (ImGui::Button(T("Export")))
					hostfs::exportHomeDirectory();
			}
#endif
	#ifdef TARGET_MAC
			if (ImGui::Button(T("Reveal in Finder")))
			{
				char temp[512];
				snprintf(temp, sizeof(temp), T("open \"%s\""), get_writable_config_path("").c_str());
				system(temp);
			}
	#endif
		}
	#endif // !linux
	ImGui::Spacing();
#else // TARGET_IPHONE
	{
			ImGui::PushID("IphoneRescanContentRow");
			const bool rescanRowActivated = BeginTwoLineSettingRow(
				"##row",
				T(
					"Rescan Content\n"
					"Rescans all configured content folders and rebuilds the game list.\n\n"
					"This can take a moment if you have many games or slow storage.\n"
					"If you just added/removed files and they are not showing up, this is the first thing to try."
				)
			);
			const ImVec2 line1Start = BeginTwoLineSettingRowContent();
		SettingIcon(ICON_FA_ARROWS_ROTATE, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(T("Rescan Content"));
		ImGui::PopFont();

		const float refreshBtnWidthPx = 26.0f;
		const float refreshBtnHeightPx = 22.0f;
		const float refreshButtonWidth = SettingsLayoutScaled(refreshBtnWidthPx);
		const float refreshButtonHeight = SettingsLayoutScaled(refreshBtnHeightPx);
		const float refreshVerticalOffset = (TwoLineSettingContentHeight() - refreshButtonHeight) * 0.5f;

		ImGui::SameLine(RightColumnX(refreshButtonWidth));
		ImVec2 refreshButtonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(refreshButtonPos.x, refreshButtonPos.y + refreshVerticalOffset));
		const bool refreshPressed = SettingsIconButton("##IphoneRescanContentBtn", ICON_FA_ARROWS_ROTATE, ImVec2(refreshButtonWidth, refreshButtonHeight));
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText(T("Refresh the current content library view."));

		RenderTwoLineSettingDescription(line1Start, T("Rescan all content folders"));
		EndTwoLineSettingRow();
		if (rescanRowActivated || refreshPressed)
			gui_refresh_files();
		ImGui::PopID();
	}
#endif
	ImGui::Spacing();

	// ========================================
	// UI Settings Section
	// ========================================
	if (RenderCollapsingHeader("UISettingsSection", ICON_FA_GEAR, T("UI Settings"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto& builtins = CustomTheme::builtins();
		const int builtinCount = static_cast<int>(builtins.size());
		static std::vector<std::string> customFiles;
		static std::string themeError;
		static std::string themeNotice;
		static double nextThemeScan = 0;
		ImGui::PushID("UIThemeSetting");
		const bool themePopupOpen = ImGui::IsPopupOpen("UIThemePopup");
		ImGui::PopID();
		if (!themePopupOpen && ImGui::GetTime() >= nextThemeScan) {
			customFiles = CustomTheme::list();
			nextThemeScan = ImGui::GetTime() + 2.0;
		}
		std::vector<std::string> labels;
		std::vector<const char*> themes;
		for (const auto& builtin : builtins) themes.push_back(T(builtin.name));
		for (const auto& file : customFiles)
			labels.push_back(file.substr(0, file.size() - 5) + " (Custom)");
		int selection = config::UITheme.get();
		if (selection == CustomTheme::CustomThemeId) {
			auto selected = std::find(customFiles.begin(), customFiles.end(), config::UICustomTheme.get());
			if (selected != customFiles.end())
				selection = builtinCount + static_cast<int>(selected - customFiles.begin());
			else {
				selection = builtinCount + static_cast<int>(labels.size());
				labels.push_back(config::UICustomTheme.get() + " (Missing)");
			}
		} else if (selection < 0 || selection >= builtinCount) {
			selection = 0;
		}
		for (const auto& label : labels) themes.push_back(label.c_str());
		bool applyThemeAfterPopup = false;
		SettingsUI::PopupConfig themeCfg {};
		themeCfg.type = SettingsUI::PopupType::Options;
		themeCfg.options.label = T("UI Theme");
		themeCfg.options.icon = ICON_FA_PALETTE;
		themeCfg.options.popupID = Tnop("UIThemePopup");
		themeCfg.options.options = themes.data();
		themeCfg.options.optionCount = static_cast<int>(themes.size());
		themeCfg.options.currentValue = &selection;
		themeCfg.options.valueWidth = 220.0f;
		themeCfg.options.onChange = [&](int selected) {
			themeError.clear();
			themeNotice.clear();
			const bool selectingBuiltin = selected >= 0 && selected < builtinCount;
			const bool selectingCustom = selected >= builtinCount && selected - builtinCount < static_cast<int>(customFiles.size());
			if (!selectingBuiltin && !selectingCustom)
				return true;
			CustomTheme::Theme candidate;
			if (selectingBuiltin) {
				CustomTheme::loadBuiltinFromDirectory(CustomTheme::directory(), builtins[selected].id, candidate, themeError);
			} else {
				const auto& filename = customFiles[selected - builtinCount];
				if (!CustomTheme::load(filename, candidate, themeError)) {
					themeError = filename + ": " + themeError;
					return true;
				}
			}
			std::string migrationError;
			if (!completePendingThemeMigration(migrationError)) {
				themeError = migrationError;
				return true;
			}
			if (selectingBuiltin) {
				config::UITheme = builtins[selected].id;
			} else {
				const auto& filename = customFiles[selected - builtinCount];
				config::UITheme = CustomTheme::CustomThemeId;
				config::UICustomTheme = filename;
			}
			applyThemeAfterPopup = true;
			g_reloadThemeFontsAfterSettings = true;
			return true;
		};

		RenderGeneralPopupSettingRow(
			Tnop("UIThemeSetting"),
			T("Choose the UI theme colors."),
			themeCfg,
			T("UI Theme\nChanges the overall look and contrast of the UI. Built-in themes are bundled presets.\n"
			  "Use Create Theme to copy the selected preset or custom theme and customize your own version.\n"
			  "Custom themes are saved in the themes folder in the configuration directory.")
		);
		if (applyThemeAfterPopup)
			g_applyThemeAfterSettings = true;
		const std::string themeDirectory = CustomTheme::directory();
		RenderGeneralInfoRow("CustomThemeFolder", ICON_FA_FOLDER_OPEN, T("Custom Theme Folder"),
			T("Add JSON theme files here, then select them above."), themeDirectory, false, themeDirectory.c_str());
		if (!themeError.empty())
			RenderGeneralInfoRow("CustomThemeError", ICON_FA_TRIANGLE_EXCLAMATION, T("Theme Error"),
				T("The theme file could not be loaded. Built-in themes use their bundled defaults."), themeError, false, themeError.c_str());
		if (!themeNotice.empty())
			RenderGeneralInfoRow("CustomThemeNotice", ICON_FA_CHECK, T("Theme Import"),
				T("The theme and its referenced assets are ready to use."), themeNotice, false, themeNotice.c_str());

		// Theme authoring stays in settings so users can create a custom theme
		// without editing JSON. The draft is previewed in the normal pre-frame
		// theme reload path; the selected theme/config is untouched until Save.
		auto startThemeEditor = [&](bool editSelected) {
			g_themeEditor = ThemeEditorState{};
			g_themeEditor.open = true;
			g_themeEditor.editing = editSelected;
			g_themeEditor.exportFolder = CustomTheme::directory();
			g_themeEditor.newFilename = editSelected ? config::UICustomTheme.get() : "My Theme.json";
			if (config::UITheme == CustomTheme::CustomThemeId) {
				const std::string selectedFilename = config::UICustomTheme.get();
				if (!CustomTheme::load(selectedFilename, g_themeEditor.draft, g_themeEditor.error)) {
					themeError = g_themeEditor.error;
					g_themeEditor.open = false;
					return;
				}
				if (editSelected)
					g_themeEditor.filename = selectedFilename;
			} else {
				g_themeEditor.draft.base = config::UITheme.get() >= 0 && config::UITheme.get() < builtinCount
						? config::UITheme.get() : 0;
				g_themeEditor.filename.clear();
				g_themeEditor.editing = false;
				g_themeEditor.error.clear();
			}
			g_themeEditor.draft.assets.directory = CustomTheme::directory();
			ImGui::OpenPopup("ThemeEditorPopup");
		};
		if (ImGui::Button(T("Create Theme"))) startThemeEditor(false);
		ImGui::SameLine();
		const bool canEditCustom = config::UITheme == CustomTheme::CustomThemeId
				&& std::find(customFiles.begin(), customFiles.end(), config::UICustomTheme.get()) != customFiles.end();
		if (!canEditCustom) ImGui::BeginDisabled();
		if (ImGui::Button(T("Edit Selected Theme")) && canEditCustom) startThemeEditor(true);
		if (!canEditCustom) ImGui::EndDisabled();
		ImGui::SameLine();
		const char* importTitle = "Import Theme JSON";
		if (ImGui::Button(T("Import Theme…")))
			ImGui::OpenPopup(importTitle);
		select_file_popup(importTitle, [&](bool cancelled, const std::string& source) {
			if (!cancelled) {
				themeNotice.clear();
				std::string imported;
				if (CustomTheme::importFile(source, imported, themeError)) {
					customFiles = CustomTheme::list();
					themeNotice = "Imported " + imported;
				}
			}
			return true;
		}, true, "json");

		if (g_themeEditor.open) {
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, std::min(uiScaled(760.0f), display.x - uiScaled(24.0f))),
					std::max(1.0f, std::min(uiScaled(650.0f), display.y - uiScaled(24.0f)))));
		}
		const std::string editorTitle = std::string(T(g_themeEditor.editing ? "Edit theme" : "Create theme")) + "###ThemeEditorPopup";
		if (g_themeEditor.open && ImGui::BeginPopupModal(editorTitle.c_str(), nullptr,
				ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
			ImGui::TextDisabled("%s", T("Changes preview as you edit. Cancel restores the previous theme."));
			ImGui::Spacing();
			const auto& builtinList = CustomTheme::builtins();
			int baseIndex = g_themeEditor.draft.base;
			std::vector<const char*> baseNames;
			for (const auto& item : builtinList) baseNames.push_back(T(item.name));
			if (g_themeEditor.editing) {
				ImGui::Text("%s: %s", T("Theme"), g_themeEditor.filename.c_str());
			} else {
				ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.56f));
				ImGui::InputText(T("Theme name"), &g_themeEditor.newFilename);
			}
			ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.56f));
			if (ImGui::Combo(T("Base theme"), &baseIndex, baseNames.data(), static_cast<int>(baseNames.size()))) {
				g_themeEditor.draft.base = baseIndex;
				g_themeEditor.previewing = true;
			}
			ImGui::Spacing();
			const ImGuiStyle& editorStyle = ImGui::GetStyle();
			const float editorActionHeight = ImGui::GetTextLineHeight() + editorStyle.FramePadding.y * 2.0f;
			const bool stackThemeActions = ImGui::GetContentRegionAvail().x < uiScaled(220.0f);
			const float editorActionsHeight = editorActionHeight * (stackThemeActions ? 2.0f : 1.0f)
					+ (stackThemeActions ? editorStyle.ItemSpacing.y : 0.0f);
			// Keep enough space for the bounded error panel, separator and all action
			// rows before sizing the tab child. This also covers errors created during
			// the current frame (for example, by Export).
			const float editorFooterReserve = editorActionsHeight + uiScaled(72.0f) + editorStyle.ItemSpacing.y * 2.0f + 1.0f;
			ImGuiStyle previewStyle{};
			CustomTheme::Theme baseTheme;
			std::string baseError;
			if (CustomTheme::loadBundled(g_themeEditor.draft.base, baseTheme, baseError))
				CustomTheme::applyStyle(baseTheme, previewStyle);
			CustomTheme::applyStyle(g_themeEditor.draft, previewStyle);
			ImGui::TextDisabled("%s", T("Preview"));
			ImGui::SameLine();
			const int previewColors[] = {ImGuiCol_WindowBg, ImGuiCol_ChildBg, ImGuiCol_FrameBg, ImGuiCol_ButtonActive, ImGuiCol_Text};
			for (int i = 0; i < IM_ARRAYSIZE(previewColors); ++i) {
				if (i != 0) ImGui::SameLine(0, uiScaled(4.0f));
				ImGui::PushID(i);
				ImGui::ColorButton("##preview", previewStyle.Colors[previewColors[i]],
						ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoPicker,
						ImVec2(uiScaled(30.0f), uiScaled(18.0f)));
				ImGui::PopID();
			}
			ImGui::Spacing();
			if (ImGui::BeginTabBar("ThemeEditorTabs")) {
				if (ImGui::BeginTabItem(T("Colors"))) {
					const float editorHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - editorFooterReserve);
					if (ImGui::BeginChild("ThemeColorsList", ImVec2(0, editorHeight), ImGuiChildFlags_Borders)) {
						if (ImGui::CollapsingHeader(T("Key colors"), ImGuiTreeNodeFlags_DefaultOpen)) {
							ImGui::TextWrapped("%s", T("Choose which colors to override. Unchecked colors use the theme defaults."));
							const char* roles[] = {"accent", "success", "warning", "error"};
							const char* labels[] = {T("Accent"), T("Success"), T("Warning"), T("Error")};
							const ImVec4 suggestions[] = {previewStyle.Colors[ImGuiCol_ButtonActive], ImVec4(0.20f, 0.76f, 0.38f, 1.f),
									ImVec4(0.94f, 0.71f, 0.25f, 1.f), ImVec4(0.88f, 0.26f, 0.24f, 1.f)};
							for (int i = 0; i < IM_ARRAYSIZE(roles); ++i) {
								const char* role = roles[i];
								ImGui::PushID(role);
								auto found = g_themeEditor.draft.roles.find(role);
								bool enabled = found != g_themeEditor.draft.roles.end();
								if (ImGui::Checkbox(labels[i], &enabled)) {
									if (enabled) {
										auto inherited = baseTheme.roles.find(role);
										g_themeEditor.draft.roles[role] = inherited != baseTheme.roles.end() ? inherited->second : suggestions[i];
									} else g_themeEditor.draft.roles.erase(role);
									g_themeEditor.previewing = true;
								}
								found = g_themeEditor.draft.roles.find(role);
								if (found != g_themeEditor.draft.roles.end()) {
									ImVec4 value = found->second;
									if (ImGui::ColorEdit4("##color", &value.x, ImGuiColorEditFlags_AlphaBar)) {
										found->second = value;
										g_themeEditor.previewing = true;
									}
								} else {
									ImGui::SameLine();
									ImGui::TextDisabled("%s", T("Using theme default"));
								}
								ImGui::PopID();
							}
						}
						if (ImGui::CollapsingHeader(T("Advanced interface colors"))) {
							ImGui::SetNextItemWidth(-FLT_MIN);
							ImGui::InputTextWithHint("##colorSearch", T("Search colors"), &g_themeEditor.colorSearch);
							std::string query = g_themeEditor.colorSearch;
							std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							for (int i = 0; i < ImGuiCol_COUNT; ++i) {
								const char* name = ImGui::GetStyleColorName(i);
								std::string searchable = name;
								std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
								if (!query.empty() && searchable.find(query) == std::string::npos) continue;
								ImVec4 value = previewStyle.Colors[i];
								if (ImGui::ColorEdit4(name, &value.x, ImGuiColorEditFlags_AlphaBar)) {
									auto found = std::find_if(g_themeEditor.draft.colors.begin(), g_themeEditor.draft.colors.end(), [i](const auto& c) { return c.first == i; });
									if (found == g_themeEditor.draft.colors.end()) g_themeEditor.draft.colors.emplace_back(i, value); else found->second = value;
									g_themeEditor.previewing = true;
								}
							}
						}
						if (ImGui::CollapsingHeader(T("Widget-specific colors"))) {
							const char* widgetKeys[] = {"media.status", "settings.feedback"};
							const char* roleNames[] = {"accent", "success", "warning", "error"};
							for (const char* widget : widgetKeys) {
								ImGui::TextUnformatted(widget);
								for (const char* role : roleNames) {
									auto& roles = g_themeEditor.draft.widgetRoles[widget];
									auto found = roles.find(role);
									bool enabled = found != roles.end();
									ImGui::PushID(widget);
									ImGui::PushID(role);
									if (ImGui::Checkbox(role, &enabled)) {
										if (enabled) {
											auto global = g_themeEditor.draft.roles.find(role);
											auto inherited = baseTheme.roles.find(role);
											roles[role] = global != g_themeEditor.draft.roles.end() ? global->second
												: inherited != baseTheme.roles.end() ? inherited->second : ImGui::GetStyle().Colors[ImGuiCol_Text];
										} else roles.erase(role);
										g_themeEditor.previewing = true;
									}
									found = roles.find(role);
									if (found != roles.end()) {
										ImVec4 value = found->second;
										if (ImGui::ColorEdit4("##color", &value.x, ImGuiColorEditFlags_AlphaBar)) { found->second = value; g_themeEditor.previewing = true; }
									}
									ImGui::PopID();
									ImGui::PopID();
								}
							}
						}
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(T("Style & layout"))) {
					const float editorHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - editorFooterReserve);
					if (ImGui::BeginChild("ThemeStyleList", ImVec2(0, editorHeight), ImGuiChildFlags_Borders)) {
					auto scalar = [&](const char* name, float ImGuiStyle::*member, float max) {
						float value = previewStyle.*member;
						const float upper = std::strstr(name, "BorderSize") != nullptr ? 20.f : max;
						if (ImGui::SliderFloat(name, &value, 0.f, upper, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
							auto it = std::find_if(g_themeEditor.draft.scalars.begin(), g_themeEditor.draft.scalars.end(), [&](const auto& v) { return v.first == member; });
							if (it == g_themeEditor.draft.scalars.end()) g_themeEditor.draft.scalars.emplace_back(member, value); else it->second = value;
							g_themeEditor.previewing = true;
						}
					};
					auto vector = [&](const char* name, ImVec2 ImGuiStyle::*member, float max) {
						ImVec2 value = previewStyle.*member;
						if (ImGui::DragFloat2(name, &value.x, 0.25f, 0.f, max, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
							auto it = std::find_if(g_themeEditor.draft.vectors.begin(), g_themeEditor.draft.vectors.end(), [&](const auto& v) { return v.first == member; });
							if (it == g_themeEditor.draft.vectors.end()) g_themeEditor.draft.vectors.emplace_back(member, value); else it->second = value;
							g_themeEditor.previewing = true;
						}
					};
						ImGui::TextDisabled("%s", T("Spacing, rounding, padding and control proportions"));
					const std::pair<const char*, float ImGuiStyle::*> scalars[] = {
						{"Alpha", &ImGuiStyle::Alpha}, {"DisabledAlpha", &ImGuiStyle::DisabledAlpha},
						{"WindowRounding", &ImGuiStyle::WindowRounding}, {"WindowBorderSize", &ImGuiStyle::WindowBorderSize},
						{"ChildRounding", &ImGuiStyle::ChildRounding}, {"PopupRounding", &ImGuiStyle::PopupRounding},
						{"FrameRounding", &ImGuiStyle::FrameRounding}, {"FrameBorderSize", &ImGuiStyle::FrameBorderSize},
						{"ScrollbarSize", &ImGuiStyle::ScrollbarSize}, {"ScrollbarRounding", &ImGuiStyle::ScrollbarRounding},
						{"GrabMinSize", &ImGuiStyle::GrabMinSize}, {"GrabRounding", &ImGuiStyle::GrabRounding}, {"TabRounding", &ImGuiStyle::TabRounding}
					};
					for (const auto& item : scalars) {
						if (item.second == &ImGuiStyle::Alpha) {
							float value = previewStyle.*item.second;
							if (ImGui::SliderFloat(item.first, &value, 0.2f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) {
								auto it = std::find_if(g_themeEditor.draft.scalars.begin(), g_themeEditor.draft.scalars.end(), [&](const auto& v) { return v.first == item.second; });
								if (it == g_themeEditor.draft.scalars.end()) g_themeEditor.draft.scalars.emplace_back(item.second, value); else it->second = value;
								g_themeEditor.previewing = true;
							}
						} else {
							scalar(item.first, item.second, item.second == &ImGuiStyle::DisabledAlpha ? 1.f : 40.f);
						}
					}
					vector("WindowPadding", &ImGuiStyle::WindowPadding, 50.f);
					vector("FramePadding", &ImGuiStyle::FramePadding, 40.f);
					vector("ItemSpacing", &ImGuiStyle::ItemSpacing, 50.f);
					vector("ItemInnerSpacing", &ImGuiStyle::ItemInnerSpacing, 50.f);
					vector("CellPadding", &ImGuiStyle::CellPadding, 50.f);
					vector("ButtonTextAlign", &ImGuiStyle::ButtonTextAlign, 1.f);
					ImGui::Separator();
						ImGui::TextDisabled("%s", T("Hollycast layout"));
					auto layoutSlider = [&](const char* name, float& value, float minimum, float maximum, float fallback, bool& present) {
						float shown = present ? value : fallback;
						if (ImGui::SliderFloat(name, &shown, minimum, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp)) { value = shown; present = true; g_themeEditor.previewing = true; }
					};
					layoutSlider("Density", g_themeEditor.draft.layout.density, 0.75f, 1.5f, 1.f, g_themeEditor.draft.layout.hasDensity);
					layoutSlider("Settings sidebar width", g_themeEditor.draft.layout.settingsSidebarWidth, 136.f, 310.f, 225.f, g_themeEditor.draft.layout.hasSettingsSidebarWidth);
					layoutSlider("Library cover spacing", g_themeEditor.draft.layout.libraryCoverSpacing, 0.f, 48.f, 8.f, g_themeEditor.draft.layout.hasLibraryCoverSpacing);
					layoutSlider("Artwork corner radius", g_themeEditor.draft.layout.artworkCornerRadius, 0.f, 32.f, 0.f, g_themeEditor.draft.layout.hasArtworkCornerRadius);
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem(T("Fonts & assets"))) {
					const float editorHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - editorFooterReserve);
					if (ImGui::BeginChild("ThemeAssetsList", ImVec2(0, editorHeight), ImGuiChildFlags_Borders)) {
					ImGui::TextWrapped("%s", T("Use paths relative to the custom theme folder. Copy files there first; import and export copy referenced files automatically."));
					ImGui::Spacing();
					auto assetInput = [&](const char* label, std::string& value, bool font = false) {
						const bool changed = ImGui::InputText(label, &value);
						const bool editCommitted = font && ImGui::IsItemDeactivatedAfterEdit();
						if (changed) { g_themeEditor.previewing = true; g_themeEditor.fontChanged |= font; }
						if (editCommitted) { g_themeEditor.previewing = true; g_themeEditor.fontReloadPending = true; }
					};
					assetInput(T("Body font path"), g_themeEditor.draft.assets.bodyFont, true);
					assetInput(T("Bold font path"), g_themeEditor.draft.assets.boldFont, true);
					assetInput(T("Background image path"), g_themeEditor.draft.assets.background);
					std::string settingsIconPath;
					auto settingsIcon = g_themeEditor.draft.assets.icons.find("settings");
					if (settingsIcon != g_themeEditor.draft.assets.icons.end()) settingsIconPath = settingsIcon->second;
					if (ImGui::InputText(T("Settings icon path"), &settingsIconPath)) {
						if (settingsIconPath.empty()) g_themeEditor.draft.assets.icons.erase("settings");
						else g_themeEditor.draft.assets.icons["settings"] = settingsIconPath;
						g_themeEditor.previewing = true;
					}
					float opacity = g_themeEditor.draft.assets.backgroundOpacity;
					if (ImGui::SliderFloat(T("Background opacity"), &opacity, 0.f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp)) { g_themeEditor.draft.assets.backgroundOpacity = opacity; g_themeEditor.previewing = true; }
					const char* fits[] = {"cover", "contain", "stretch"};
					int fit = g_themeEditor.draft.assets.backgroundFit == "contain" ? 1 : g_themeEditor.draft.assets.backgroundFit == "stretch" ? 2 : 0;
					if (ImGui::Combo(T("Background fit"), &fit, fits, IM_ARRAYSIZE(fits))) { g_themeEditor.draft.assets.backgroundFit = fits[fit]; g_themeEditor.previewing = true; }
					ImGui::InputText(T("Export folder"), &g_themeEditor.exportFolder);
					if (ImGui::Button(T("Export theme and assets"))) {
						const std::string exportName = themeFilenameWithExtension(g_themeEditor.editing ? g_themeEditor.filename : g_themeEditor.newFilename);
						if (CustomTheme::exportToDirectory(g_themeEditor.draft, exportName, g_themeEditor.exportFolder, g_themeEditor.error))
							g_themeEditor.error = "Exported " + exportName + " and its referenced assets.";
					}
					}
					ImGui::EndChild();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			if (g_themeEditor.previewing) {
				g_themeEditor.previewing = false;
				// A zero ImGuiStyle::Alpha makes the whole application invisible. Clamp
				// only the preview copy so existing themes retain their saved values.
				CustomTheme::Theme previewTheme = g_themeEditor.draft;
				for (auto& scalar : previewTheme.scalars) {
					if (scalar.first == &ImGuiStyle::Alpha)
						scalar.second = std::isfinite(scalar.second) ? std::max(0.2f, scalar.second) : 1.f;
				}
				setThemePreview(&previewTheme);
				g_applyThemeAfterSettings = true;
				g_reloadThemeFontsAfterSettings |= g_themeEditor.fontReloadPending;
				g_themeEditor.fontReloadPending = false;
			}
			ImGui::Separator();
			if (!g_themeEditor.error.empty()) {
				const float availableHeight = std::max(0.0f, ImGui::GetContentRegionAvail().y - editorActionsHeight
						- editorStyle.ItemSpacing.y * 2.0f - 1.0f);
				const float errorHeight = std::min(uiScaled(72.0f), availableHeight);
				if (errorHeight > 0.0f) {
					if (ImGui::BeginChild("ThemeEditorError", ImVec2(0, errorHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
						ImGui::TextWrapped("%s", g_themeEditor.error.c_str());
					ImGui::EndChild();
				}
			}
			const float availableWidth = ImGui::GetContentRegionAvail().x;
			const float desiredButtonWidth = std::max(uiScaled(96.0f), std::max(ImGui::CalcTextSize(T("Save theme")).x,
					ImGui::CalcTextSize(T("Cancel")).x) + ImGui::GetStyle().FramePadding.x * 2.0f);
			const bool stackButtons = stackThemeActions;
			const float buttonWidth = stackButtons ? availableWidth
					: std::min(desiredButtonWidth, (availableWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
			if (!stackButtons)
				ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - (buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x)));
			const bool cancelTheme = ImGui::Button(T("Cancel"), ImVec2(buttonWidth, 0)) || ControllerPopupCancelPressed();
			if (!stackButtons) ImGui::SameLine();
			const bool saveTheme = ImGui::Button(T("Save theme"), ImVec2(buttonWidth, 0));
			if (saveTheme && !cancelTheme) {
				const std::string filename = themeFilenameWithExtension(g_themeEditor.editing ? g_themeEditor.filename : g_themeEditor.newFilename);
				g_themeEditor.draft.assets.directory = CustomTheme::directory();
				std::string validationError;
				CustomTheme::serialize(g_themeEditor.draft, validationError);
				std::string migrationError;
				if (!validationError.empty()) {
					g_themeEditor.error = validationError;
				} else if (CustomTheme::save(filename, g_themeEditor.draft, g_themeEditor.editing, g_themeEditor.error)) {
					customFiles = CustomTheme::list();
					if (!completePendingThemeMigration(migrationError)) {
						g_themeEditor.error = "Saved " + filename + ", but could not select it. " + migrationError
								+ ". Close this editor, resolve the migration error, then select this theme from UI Theme.";
					} else {
						config::UITheme = CustomTheme::CustomThemeId;
						config::UICustomTheme = filename;
						setThemePreview(nullptr);
						g_applyThemeAfterSettings = true;
						g_reloadThemeFontsAfterSettings = true;
						g_themeEditor.open = false;
						ImGui::CloseCurrentPopup();
					}
				}
			}
			if (cancelTheme) {
				setThemePreview(nullptr);
				g_applyThemeAfterSettings = true;
				g_reloadThemeFontsAfterSettings |= g_themeEditor.fontChanged;
				g_themeEditor.open = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (g_themeEditor.open && !ImGui::IsPopupOpen("ThemeEditorPopup")) {
			setThemePreview(nullptr);
			g_applyThemeAfterSettings = true;
			g_reloadThemeFontsAfterSettings |= g_themeEditor.fontChanged;
			g_themeEditor.open = false;
		}

#if !defined(TARGET_MAC)
		const char* menuBarModes[] = { T("Auto"), T("On"), T("Off") };
		SettingsUI::PopupConfig menuBarCfg {};
		menuBarCfg.type = SettingsUI::PopupType::Options;
		menuBarCfg.options.label = T("Top Menu Bar");
		menuBarCfg.options.icon = ICON_FA_BARS;
		menuBarCfg.options.popupID = Tnop("MenuBarVisibilityPopup");
		menuBarCfg.options.options = menuBarModes;
		menuBarCfg.options.optionCount = IM_ARRAYSIZE(menuBarModes);
		menuBarCfg.options.currentValue = &config::MenuBarVisibility.get();
		menuBarCfg.options.valueWidth = 220.0f;
		RenderGeneralPopupSettingRow(
			Tnop("MenuBarVisibilitySetting"),
			T("Choose when the top menu bar is shown."),
			menuBarCfg,
			T("Top Menu Bar\n"
				"On keeps the bar visible. Off keeps it hidden.\n"
				"Auto follows the normal mouse and touch reveal behavior.")
		);
#endif

		static bool showApplyButtonForUIScaling = false;
		if (uiUserScaleUpdated && !showApplyButtonForUIScaling)
			showApplyButtonForUIScaling = true;

		SettingsUI::PopupConfig uiScalingCfg {};
		uiScalingCfg.type = SettingsUI::PopupType::Slider;
		uiScalingCfg.slider.label = T("UI Scaling");
		uiScalingCfg.slider.icon = ICON_FA_RULER_HORIZONTAL;
		uiScalingCfg.slider.popupID = Tnop("UIScalingPopup");
		uiScalingCfg.slider.description = T("Adjust UI element sizes");
		uiScalingCfg.slider.currentValue = &config::UIScaling.get();
		uiScalingCfg.slider.minValue = 50;
		uiScalingCfg.slider.maxValue = 200;
		uiScalingCfg.slider.format = "%d%%";
		uiScalingCfg.slider.valueWidth = 220.0f;
		uiScalingCfg.slider.showApplyFlag = &showApplyButtonForUIScaling;
		uiScalingCfg.slider.onApply = [&]() {
			mainui_reinit();
			uiUserScaleUpdated = false;
			showApplyButtonForUIScaling = false;
		};
		uiScalingCfg.slider.onCancel = [&]() {
			// The stored scale is restored before this runs. Rebuild any preview
			// state too, so reopening the popup uses the original scale throughout.
			mainui_reinit();
			uiUserScaleUpdated = false;
			showApplyButtonForUIScaling = false;
		};

		RenderGeneralPopupSettingRow(
			Tnop("UIScalingSetting"),
			T("Scale the UI to fit your screen and viewing distance."),
			uiScalingCfg,
			T(
				"UI Scaling\n"
				"Adjusts UI element sizes.\n"
				"Useful on high-DPI displays, TVs, or small screens.\n\n"
				"After applying, the UI will reinitialize to apply the new scale."
			)
		);

		RenderGeneralToggleSettingRow(
			"HideLegacyNaomiRoms",
			ICON_FA_LOCK,
			T("Hide Legacy Naomi Roms"),
			T("Hide legacy Naomi file types in the content browser."),
			static_cast<bool>(config::HideLegacyNaomiRoms),
			[](bool enabled) {
				config::HideLegacyNaomiRoms = enabled;
				gui_refresh_files();
			},
			T(
				"Hide Legacy Naomi ROMs\n"
				"Hides `.bin`, `.dat`, and `.lst` files from the content browser.\n"
				"Useful if you have mixed Naomi sets and only want to see launchable content."
			)
		);
	}
#ifdef __ANDROID__
	RenderGeneralToggleSettingRow(
		"UseSafFilePicker",
		ICON_FA_MOBILE_SCREEN_BUTTON,
		T("Use SAF File Picker"),
		T("Android storage picker integration."),
		static_cast<bool>(config::UseSafFilePicker),
		[](bool enabled) { config::UseSafFilePicker = enabled; },
		T(
			"Use SAF File Picker\n"
			"Uses Android's Storage Access Framework (SAF) for file browsing.\n"
			"This can improve compatibility with scoped storage and content providers on newer Android versions."
		)
	);
#endif

	}

	if (tab == SettingsTab::Library)
	{
	// ========================================
	// Library Section
	// ========================================
	if (RenderCollapsingHeader("BoxArtSection", ICON_FA_IMAGE, T("Box Art"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (g_scrollToBoxArtSection)
		{
			ImGui::SetScrollHereY(0.0f);
			g_scrollToBoxArtSection = false;
		}

		const char* boxartSources[] = { T("Original Box Art"), T("Physical Media"), T("Custom Boxart") };
		SettingsUI::PopupConfig boxartSourceCfg {};
		boxartSourceCfg.type = SettingsUI::PopupType::Options;
		boxartSourceCfg.options.label = T("Box Art Source");
		boxartSourceCfg.options.icon = ICON_FA_IMAGE;
		boxartSourceCfg.options.popupID = Tnop("BoxArtSourcePopup");
		boxartSourceCfg.options.options = boxartSources;
		boxartSourceCfg.options.optionCount = IM_ARRAYSIZE(boxartSources);
		boxartSourceCfg.options.currentValue = &config::BoxartSourceMode.get();
		boxartSourceCfg.options.valueWidth = 220.0f;
		boxartSourceCfg.options.onChange = [](int) {
			gui_refresh_custom_boxart(false);
			return true;
		};

		RenderGeneralPopupSettingRow(
			Tnop("BoxartSourceSetting"),
			T("Choose the base artwork source for each game."),
			boxartSourceCfg,
			T(
				"Box Art Source\n"
				"Choose whether to use the default scraped artwork, physical-disc artwork pulled from the current ROM, or custom box art files you provide.\n\n"
				"If you have a custom Scraper folder for ES-DE, you can use its `media` folder and subfolders for the full custom-artwork experience."
			)
		);

		const bool usingCustomBoxart = config::BoxartSourceMode.get() == static_cast<int>(BoxartSourceMode::CustomThenScraped);
		ImGui::Indent();

		const char* libraryImageSources[] = {
			T("Current Artwork"),
			T("VMU Save Icon"),
			T("VMU Save Icon, then Current Artwork"),
			T("Current Artwork, then VMU Save Icon"),
		};
		SettingsUI::PopupConfig libraryImageSourceCfg {};
		libraryImageSourceCfg.type = SettingsUI::PopupType::Options;
		libraryImageSourceCfg.options.label = T("VMU Icon Options");
		libraryImageSourceCfg.options.icon = ICON_FA_IMAGE;
		libraryImageSourceCfg.options.popupID = Tnop("LibraryImageSourcePopup");
		libraryImageSourceCfg.options.options = libraryImageSources;
		libraryImageSourceCfg.options.optionCount = IM_ARRAYSIZE(libraryImageSources);
		libraryImageSourceCfg.options.currentValue = &config::LibraryImageSource.get();
		libraryImageSourceCfg.options.valueWidth = 240.0f;
		libraryImageSourceCfg.options.onChange = [](int) { return true; };
		RenderGeneralPopupSettingRow(
			Tnop("LibraryImageSource"),
			T("Choose whether cached VMU icons appear in the library."),
			libraryImageSourceCfg,
			T("VMU Icon Options\n"
			"Choose whether to exclude or use VMU save icons as cover art. When enabled, Hollycast captures the icon after you create a VMU save for a game, then makes it available for the library.\n"
			"Choose how those icons are prioritized alongside your other artwork options."));

		if (usingCustomBoxart)
		{
			ImGui::PushID("CustomBoxartFolderRow");
			const bool rowActivated = BeginTwoLineSettingRow(
				"##row",
				T(
					"Custom Boxart Folder\n"
					"Choose the folder that contains your already-scraped game media.\n\n"
					"For regular custom media, add an image to the custom folder using the same name as the ROM. For example, if the ROM is `Sonic Adventure.chd`, the image file should be `Sonic Adventure.png` or `Sonic Adventure.jpg`.\n\n"
					"For existing ES-DE media folders, or for the full custom-artwork experience, choose the system media folder, such as `downloaded_media/dreamcast`. Hollycast reads supported subfolders within it, including `covers`, `miximages`, `physicalmedia`, `marquees`, `titlescreens`, `screenshots`, `fanart`, and `manuals`. This lets you reuse ES-DE scraped media without copying or renaming files.\n\n"
					"Image/manual file names should match your ROM names. Use Refresh after adding or changing media files."
				)
			);
			const ImVec2 line1Start = BeginTwoLineSettingRowContent();

			SettingIcon(ICON_FA_FOLDER_OPEN, SettingsLayoutSize(20.0f, 20.0f));
			ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
			ImGui::PushFont(largeFont);
			ImGui::TextUnformatted(T("Custom Boxart Folder"));
			ImGui::PopFont();

			bool openPopup = rowActivated;
			bool refreshPressed = false;
			const bool hasPath = !config::BoxartPath.get().empty();
			if (hasPath)
			{
				const float buttonWidthPx = 26.0f;
				const float buttonHeightPx = 22.0f;
				const float buttonSpacingPx = 8.0f;
				const float extraOffsetPx = buttonWidthPx * 2.0f + buttonSpacingPx;
				RenderGeneralRightValue(config::BoxartPath.get(), 280.0f, extraOffsetPx);

				const float buttonWidth = SettingsLayoutScaled(buttonWidthPx);
				const float buttonHeight = SettingsLayoutScaled(buttonHeightPx);
				const float buttonSpacing = SettingsLayoutScaled(buttonSpacingPx);
				const float verticalOffset = (TwoLineSettingContentHeight() - buttonHeight) * 0.5f;

				ImGui::SameLine(RightColumnX(buttonWidth * 2.0f + buttonSpacing));
				ImVec2 buttonPos = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y + verticalOffset));
				refreshPressed = SettingsIconButton("##BoxartRefreshBtn", ICON_FA_ARROWS_ROTATE, ImVec2(buttonWidth, buttonHeight));
				if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
					SetSettingsFooterText(T("Refresh custom media after adding, removing, or renaming files in the selected Custom Boxart Folder."));

				ImGui::SameLine(0, buttonSpacing);
				ImVec2 deletePos = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(deletePos.x, buttonPos.y + verticalOffset));
				if (SettingsIconButton("##ClearBoxartPath", ICON_FA_TRASH_CAN, ImVec2(buttonWidth, buttonHeight)))
				{
					config::BoxartPath.get().clear();
					openPopup = false;
				}
			}
			else
			{
				RenderGeneralRightValue(T("Set Path"), 280.0f, 0.0f, true);
			}

			RenderTwoLineSettingDescription(line1Start, T("Folder containing custom box art images (png/jpg). File names should match game names"));
			EndTwoLineSettingRow();
			ImGui::PopID();

			const std::string popupName = T("Select Custom Boxart Folder");
#ifdef __ANDROID__
			const auto isRawAndroidStoragePath = [](const std::string& path) {
				return path.find("/storage/") == 0 || path.find("/sdcard/") == 0;
			};
			if (refreshPressed && isRawAndroidStoragePath(config::BoxartPath.get()))
			{
				openPopup = true;
				refreshPressed = false;
			}
#else
			select_file_popup(popupName.c_str(), [](bool cancelled, const std::string& selection) {
				if (!cancelled)
					config::BoxartPath.get() = selection;
				return true;
			}, false, "");
#endif
#ifdef __ANDROID__
			if (openPopup)
			{
				const StoragePopupResult storageResult = select_storage_popup(true, false, popupName, [](bool cancelled, const std::string& selection) {
					if (!cancelled)
					{
						config::BoxartPath.get() = selection;
						SaveSettings();
					}
					return true;
				});
				if (storageResult == StoragePopupResult::Unsupported)
					ImGui::OpenPopup(popupName.c_str());
			}
#else
			if (openPopup)
				ImGui::OpenPopup(popupName.c_str());
#endif

			static std::string lastBoxartPath = config::BoxartPath.get();
			if (lastBoxartPath != config::BoxartPath.get())
			{
				gui_refresh_custom_boxart(true);
				lastBoxartPath = config::BoxartPath.get();
			}
			if (refreshPressed)
				gui_refresh_custom_boxart(true);

			const char* libraryCoverMediaSources[] = {
				T("Current Artwork"),
				T("Mix Image"),
				T("Cover"),
				T("Case / 3D Box"),
				T("Screenshot"),
				T("Marquee"),
				T("Physical Media"),
				T("Fan Art"),
				T("Title Screen"),
			};
			SettingsUI::PopupConfig libraryCoverMediaCfg {};
			libraryCoverMediaCfg.type = SettingsUI::PopupType::Options;
			libraryCoverMediaCfg.options.label = T("Custom Boxart Source");
			libraryCoverMediaCfg.options.icon = ICON_FA_IMAGE;
			libraryCoverMediaCfg.options.popupID = Tnop("LibraryCoverMediaPopup");
			libraryCoverMediaCfg.options.options = libraryCoverMediaSources;
			libraryCoverMediaCfg.options.optionCount = IM_ARRAYSIZE(libraryCoverMediaSources);
			libraryCoverMediaCfg.options.currentValue = &config::LibraryCoverMedia.get();
			libraryCoverMediaCfg.options.valueWidth = 240.0f;
			libraryCoverMediaCfg.options.onChange = [](int) { return true; };
			RenderGeneralPopupSettingRow(
				Tnop("LibraryCoverMedia"),
				T("Choose which custom media category becomes the main library image."),
				libraryCoverMediaCfg,
				T("Custom Boxart Source\n"
				"Choose Current Artwork when your Custom Boxart Folder contains one properly named image for each game.\n\n"
				"The other options are for the full media-folder setup. They let you choose which of the multiple images available for each title becomes its visible library cover art.\n\n"
				"Other media can still appear in the game hover-information panel when the matching folders and files are present."));
		}

		ImGui::Unindent();

		ImGui::PushID("BoxartDatabaseReviewRow");
		const bool refreshCacheRowActivated = BeginTwoLineSettingRow(
			"##row",
			T(
				"Boxart Database Review\n"
				"Checks the boxart database and artwork files without deleting your artwork."
			)
		);
		const ImVec2 refreshCacheLine1Start = BeginTwoLineSettingRowContent();

		SettingIcon(ICON_FA_ARROWS_ROTATE, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(T("Boxart Database Review"));
		ImGui::PopFont();

		RenderTwoLineSettingDescription(refreshCacheLine1Start, T("Verifies every DB entry has a usable artwork file, then rebuilds from existing JSON and image files."));
		EndTwoLineSettingRow();
		ImGui::PopID();

		if (refreshCacheRowActivated)
			gui_refresh_boxart_cache();

		const char* libraryDisplayStyles[] = { T("Classic"), T("List") };
		SettingsUI::PopupConfig libraryDisplayStyleCfg {};
		libraryDisplayStyleCfg.type = SettingsUI::PopupType::Options;
		libraryDisplayStyleCfg.options.label = T("Library Display Style");
		libraryDisplayStyleCfg.options.icon = ICON_FA_LIST;
		libraryDisplayStyleCfg.options.popupID = Tnop("LibraryDisplayStylePopup");
		libraryDisplayStyleCfg.options.options = libraryDisplayStyles;
		libraryDisplayStyleCfg.options.optionCount = IM_ARRAYSIZE(libraryDisplayStyles);
		libraryDisplayStyleCfg.options.currentValue = &config::LibraryDisplayStyle.get();
		libraryDisplayStyleCfg.options.valueWidth = 220.0f;
		libraryDisplayStyleCfg.options.onChange = [](int) { return true; };
		RenderGeneralPopupSettingRow(
			Tnop("LibraryDisplayStyle"),
			T("Choose how the game library is displayed."),
			libraryDisplayStyleCfg,
			T("Library Display Style\n"
			"Selects the visual layout for the game library.\n"
			"Classic keeps the existing gallery/list behavior.\n"
			"List uses a compact row/table layout with title and metadata columns."));

		const char* vmuIconModes[] = { T("Static"), T("Active / Animated") };
		SettingsUI::PopupConfig vmuIconModeCfg {};
		vmuIconModeCfg.type = SettingsUI::PopupType::Options;
		vmuIconModeCfg.options.label = T("VMU Icon Mode");
		vmuIconModeCfg.options.icon = ICON_FA_BATTERY_FULL;
		vmuIconModeCfg.options.popupID = Tnop("VMUIconModePopup");
		vmuIconModeCfg.options.options = vmuIconModes;
		vmuIconModeCfg.options.optionCount = IM_ARRAYSIZE(vmuIconModes);
		vmuIconModeCfg.options.currentValue = &config::VmuIconMode.get();
		vmuIconModeCfg.options.valueWidth = 220.0f;
		vmuIconModeCfg.options.onChange = [](int) { return true; };
		RenderGeneralPopupSettingRow(
			Tnop("VMUIconMode"),
			T("Set VMU icon playback behavior."),
			vmuIconModeCfg,
			T("VMU Icon Mode\n"
			"Controls how cached VMU icons play after they have been captured from saves.\n"
			"Static shows one frame; Active / Animated plays the BIOS-style icon frames."));

		RenderGeneralToggleSettingRow(
			"BoxartDisplayMode",
			ICON_FA_IMAGE,
			T("Box Art Game List"),
			T("Display game cover art in the game list."),
			static_cast<bool>(config::BoxartDisplayMode),
			[](bool enabled) { config::BoxartDisplayMode = enabled; },
			T(
				"Box Art Game List\n"
				"Displays cover art tiles in the content list.\n"
				"Disable if you prefer a faster, more compact list or want to reduce UI clutter."
			)
		);

		const bool physicalOnly = config::BoxartSourceMode.get() == static_cast<int>(BoxartSourceMode::PhysicalMediaOnly);
		RenderGeneralToggleSettingRow(
			"FetchBoxart",
			ICON_FA_DOWNLOAD,
			T("Fetch Box Art"),
			T("Fetch cover images from TheGamesDB.net."),
			static_cast<bool>(config::FetchBoxart),
			[](bool enabled) { config::FetchBoxart = enabled; },
			T(
				"Fetch Box Art\n"
				"Downloads cover images from TheGamesDB.net.\n"
				"Disable if you only want physical media images, custom artwork, or to avoid network fetching."
			),
			physicalOnly
		);

#if !defined(TARGET_IPHONE)
		manageSinglePath(T("Gamelist XML"), config::GameListPath,
			T(
				"Gamelist XML\n"
				"Choose the exact `gamelist.xml` file that belongs to your Dreamcast ROM set.\n\n"
				"This file provides the game info used by the library hover/profile screen: description, developer, genre, players, release date, and manual path.\n\n"
				"For ES-DE, this is usually in a separate folder from the images, for example `gamelists/dreamcast/gamelist.xml`.\n"
				"Hollycast will not guess this path. Set it here if you want game descriptions and details."
			),
			true,
			"xml"
		);
		ImGui::Spacing();
#endif
	}
	}

	if (tab == SettingsTab::General)
	{
	// ========================================
	// Automatic Save States Section
	// ========================================
	if (RenderCollapsingHeader("AutomaticSaveStatesSection", ICON_FA_FLOPPY_DISK, T("Automatic Save States"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"AutoLoadState",
			ICON_FA_FOLDER_OPEN,
			T("Auto-Load on Start"),
			T("Automatically loads the hidden Auto-Save slot on game boot."),
			static_cast<bool>(config::AutoLoadState),
			[](bool enabled) { config::AutoLoadState = enabled; },
			T(
				"Auto-Load on Start\n"
				"When enabled, on game boot, if the hidden Auto-Save slot has an Auto-Save, "
				"it will automatically load this slot on game boot."
			)
		);

		RenderGeneralToggleSettingRow(
			"AutoSaveState",
			ICON_FA_FLOPPY_DISK,
			T("Auto-Save on Exit"),
			T("Automatically saves to the hidden Auto-Save slot on game exit."),
			static_cast<bool>(config::AutoSaveState),
			[](bool enabled) { config::AutoSaveState = enabled; },
			T(
				"Auto-Save on Exit\n"
				"When enabled, leaving a game saves to the hidden Auto-Save slot without asking first."
			)
		);

		RenderGeneralToggleSettingRow(
			"SaveProtection",
			ICON_FA_SHIELD_HALVED,
			T("Save Protection"),
			T("Disabling this will remove the safeguards that help prevent lost game progress from not saving."),
			static_cast<bool>(config::SaveProtection),
			[](bool enabled) { config::SaveProtection = enabled; },
			T(
				"Save Protection\n"
				"This is the assurance that you will never be able to leave a game without being prompted "
				"to save right then, ensuring you do not accidentally leave without saving your data."
			)
		);

		RenderGeneralToggleSettingRow(
			"NaomiFreePlay",
			ICON_FA_TROPHY,
			T("Naomi Free Play"),
			T("Configure Naomi games in Free Play mode."),
			static_cast<bool>(config::ForceFreePlay),
			[](bool enabled) { config::ForceFreePlay = enabled; },
			T(
				"Naomi Free Play\n"
				"Forces supported Naomi titles into Free Play mode.\n"
				"Useful if you don't want to manage credits/coin input."
			)
		);

#if USE_DISCORD
		RenderGeneralToggleSettingRow(
			"discord_presence",
			ICON_FA_COMMENT,
			T("Discord Presence"),
			T("Show which game you are playing on Discord"),
			static_cast<bool>(config::DiscordPresence),
			[](bool enabled) { config::DiscordPresence = enabled; },
			T(
				"Discord Presence\n"
				"Shows which game you are playing on Discord.\n"
				"Disable if you prefer not to share activity status."
			)
		);
#endif

		ImGui::Spacing();
	}

	// ========================================
	// RetroAchievements Section
	// ========================================
#ifdef USE_RACHIEVEMENTS
	if (RenderCollapsingHeader("RetroAchievementsSection", ICON_FA_TROPHY, T("Retro Achievements"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"EnableAchievements",
			ICON_FA_TROPHY,
			T("Enable RetroAchievements"),
			T("Track your game achievements using RetroAchievements.org"),
			static_cast<bool>(config::EnableAchievements),
			[](bool enabled) { config::EnableAchievements = enabled; },
			T(
				"Enable RetroAchievements\n"
				"Enables RetroAchievements support and achievement tracking.\n"
				"Requires a RetroAchievements account and a supported title."
			)
		);

		if (config::EnableAchievements)
		{
			ImGui::Indent();

			RenderGeneralToggleSettingRow(
				"ShowAchievementPopups",
				ICON_FA_BELL,
				T("Show Achievement Popups"),
				T("Show on-screen RetroAchievements notifications while playing."),
				static_cast<bool>(config::ShowAchievementPopups),
				[](bool enabled) { config::ShowAchievementPopups = enabled; },
				T(
					"Show Achievement Popups\n"
					"Controls achievement, progress, challenge, and leaderboard notifications shown over gameplay."
				)
			);

			ImGui::Spacing();

			RenderGeneralToggleSettingRow(
				"AchievementsHardcoreMode",
				ICON_FA_FIRE,
				T("Hardcore Mode"),
				T("Enable RetroAchievements hardcore mode. Using cheats and loading a state are not allowed."),
				static_cast<bool>(config::AchievementsHardcoreMode),
				[](bool enabled) { config::AchievementsHardcoreMode = enabled; },
				T(
					"Hardcore Mode\n"
					"RetroAchievements Hardcore Mode disables features that can invalidate achievements.\n"
					"Using cheats and loading a state are not allowed."
				)
			);

			ImGui::Spacing();

			// Username display and login/logout
			if (achievements::isLoggedOn())
			{
				ImGui::Text(T("User: %s"), config::AchievementsUserName.get().c_str());
				const std::string logoutText = std::string(T("Logout")) + "##RA";
				ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(logoutText.c_str()).x - ImGui::GetStyle().FramePadding.x * 2);
				if (ImGui::Button(logoutText.c_str()))
					achievements::logout();
			}
			else
			{
				static char password[256];
				static std::future<void> futureLogin;

				RenderGeneralTextInputSettingRow(
					"RAUsernameRow",
					ICON_FA_USER,
					T("Username"),
					T("RetroAchievements account username"),
					[]() {
						InputText("##UsernameInput", &config::AchievementsUserName.get(), ImGuiInputTextFlags_None);
					},
					T(
						"RetroAchievements Username\n"
						"Enter your RetroAchievements account username used for login."
					)
				);

				RenderGeneralTextInputSettingRow(
					"RAPasswordRow",
					ICON_FA_LOCK,
					T("Password"),
					T("RetroAchievements account password"),
					[&]() {
						InputText("##PasswordInput", password, sizeof(password), ImGuiInputTextFlags_Password);
					},
					T(
						"RetroAchievements Password\n"
						"Enter your RetroAchievements account password used for login."
					)
				);

				if (futureLogin.valid())
				{
					if (futureLogin.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout)
					{
						ImGui::Text("%s", T("Authenticating..."));
					}
					else
					{
						try
						{
							futureLogin.get();
						}
						catch (const FlycastException& e)
						{
							gui_error(e.what());
						}
					}
				}

				{
					DisabledScope _(config::AchievementsUserName.get().empty() || password[0] == '\0');
					const std::string loginText = std::string(T("Login")) + "##RA";
					if (ImGui::Button(loginText.c_str()) && !futureLogin.valid())
					{
						achievements::init();
						futureLogin = achievements::login(config::AchievementsUserName.get().c_str(), password);
						memset(password, 0, sizeof(password));
					}
				}
			}

			ImGui::Unindent();
		}
	}
	ImGui::Spacing();
#endif

	// ========================================
	// Custom Paths Section
	// ========================================
#if !defined(TARGET_IPHONE)
	if (RenderCollapsingHeader("CustomPathsSection", ICON_FA_SLIDERS, T("Custom Paths")))
	{

		managePathList(T("BIOS Folders"), T("BIOS Folder"), config::BiosPath.get(),
			T(
				"BIOS Folders\n"
				"Folders that Hollycast searches for Dreamcast and arcade BIOS files.\n\n"
				"Place `dc_boot.bin` and `dc_flash.bin` in Hollycast's data folder, unless you have specified a custom BIOS folder here.\n"
				"If you keep multiple BIOS sets, you can add multiple folders and Hollycast will search all of them.\n"
				"For troubleshooting, keep your BIOS files in a single known-good folder to avoid confusion."
			)
		);
		ImGui::Spacing();

#if !defined(__ANDROID__)
		manageSinglePath(T("VMU Folder"), config::VMUPath,
			T(
				"VMU Folder\n"
				"Where VMU memory card files (`.bin`) are stored.\n\n"
				"If you change this, Hollycast will look for VMUs in the new location.\n"
				"Move/copy your existing VMU files if you want to keep your saves.\n"
				"Use this to keep saves on a specific drive or to share saves between installations."
			)
		);
		ImGui::Spacing();

#ifdef DREAMPOTATO_INTEGRATED_MODE
	manageSinglePath("DreamPotato Path", config::DreamPotatoFolderPath,
		"DreamPotato Path\n"
		"Path of the folder containing the DreamPotato (VMU emulator) executable.\n\n"
		"Set this if you want to use a custom version of DreamPotato with integrated mode.");
	ImGui::Spacing();
#endif

		managePathList(T("Savestate Folders"), T("Savestate Folder"), config::SavestatePath.get(),
			T(
				"Savestate Folders\n"
				"Folders used for save states.\n\n"
				"The first folder is used when creating new save states.\n"
				"All listed folders are searched when loading, which is useful if you keep states organized across different locations.\n"
				"Save states are not always portable between very different Hollycast versions; if a state fails to load, try recreating it on your current build."
			)
		);
		ImGui::Spacing();

		manageSinglePath(T("Game Save Folder"), config::SavePath,
			T(
				"Game Save Folder\n"
				"Folder for non-VMU save data (for example: arcade NVRAM and other persistent game data).\n\n"
				"If you change this path, move/copy your existing save files if you want to keep progress.\n"
				"This is separate from save states, and separate from VMU files."
			)
		);
		ImGui::Spacing();
#endif

		managePathList(T("Texture Pack Folders"), T("Texture Pack Folder"), config::TexturePath.get(),
			T(
				"Texture Pack Folders\n"
				"Folders that Hollycast searches for custom texture packs.\n\n"
				"Common layouts include `textures/<gameId>/...` or `<gameId>` inside a `textures` subfolder.\n"
				"After installing a pack, enable Custom Textures in the Video tab and restart the game.\n"
				"If you notice stutter while textures stream in, consider using Preload Textures (more RAM/VRAM required)."
			)
		);
		ImGui::Spacing();

#if !defined(__ANDROID__)
		manageSinglePath("Texture Dump Folder", config::TextureDumpPath,
			T(
				"Texture Dump Folder\n"
				"Where Hollycast saves dumped textures when Texture Dumping is enabled.\n\n"
				"Game-specific subfolders are created automatically.\n"
				"Dumping can generate a large number of files quickly and consume significant disk space.\n"
				"Enable dumping only when needed, then disable it after you are done."
			)
		);
		ImGui::Spacing();

		managePathList(T("Controller Mapping Folders"), T("Controller Mapping Folder"), config::MappingsPath.get(),
			T("Folders containing controller mapping files (.cfg). The emulator also looks in Home Folder/mappings. Per-game mappings are suffixed with _<gameId>.cfg")
		);
		ImGui::Spacing();

		managePathList(T("Cheat Folders"), T("Cheat Folder"), config::CheatPath.get(),
			T(
				"Cheat Folders\n"
				"Folders containing cheat files (`.cht` / `.txt`) named using the game's ID.\n\n"
				"Hollycast can auto-load matching cheat files when present.\n"
				"If cheats are not loading, verify the filename matches the game ID and that the cheat file format is correct.\n"
				"Use cheats carefully: they can crash games or cause unexpected behavior."
			)
		);
		ImGui::Spacing();
#endif  // !ANDROID
	}
#endif  // !IPHONE
	}
}

void renderGeneralTab()
{
	renderSettingsContentTab(SettingsTab::General);
}

void renderLibraryTab()
{
	renderSettingsContentTab(SettingsTab::Library);
}

void renderVideoTab()
{
	ScopedTwoLineRowStyle videoRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("Video Configuration"));
	ImGui::Separator();

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
	}

	constexpr int apiCount = 0
#ifdef USE_VULKAN
		+ 1
#endif
#ifdef USE_DX9
		+ 1
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
			std::array<const char*, 4> apiLabels {};
			std::array<int, 4> apiValues {};
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
			apiLabels[apiOptionCount] = "DirectX 9";
			apiValues[apiOptionCount++] = 2;
#endif
#ifdef USE_DX11
			apiLabels[apiOptionCount] = "DirectX 11";
			apiValues[apiOptionCount++] = 3;
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
			const bool has_per_pixel = GraphicsContext::Instance()->hasPerPixel();
			int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;

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
						T(
							"Per Pixel\n"
							"Highest performance requirements. This matches the Dreamcast's sorting quality, but only about 15% of games need it and show a meaningful difference versus the middle sorting option.\n"
							"Use only when needed (or if you have the resource overhead). It can almost double resource requirements.\n"
							"If you need full speed at a target resolution, you may have to lower resolution when using Per Pixel sorting."
						)
					);
					break;
				case 0:
					SetSettingsFooterText(
						T(
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
				T(
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
			if ((config::Widescreen || config::WidescreenGameHacks)
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
			stretchCfg.description = T("Stretches the image sideways to fill your display. Use 132% for 16:9 correction.");
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
				T("Fine-tune horizontal scaling (often 132% for 16:9 correction)."),
				stretchPopupCfg,
				T(
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
					? T("Pixel Buffer Size\n512 MB reserves the least memory and is the fastest place to start. Use this if you want to minimize VRAM/RAM usage, but increase it if you see missing or incorrect transparency in heavier scenes.")
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
			T("Reserved memory for Per-Pixel transparency rendering."),
			pixelBufferCfg,
			T(
				"Pixel Buffer Size\n"
				"Controls how much memory is reserved for Per-Pixel (OIT) transparency rendering.\n"
				"If you increase Internal Resolution a lot or see missing or incorrect transparency in complex scenes, you may need a larger buffer.\n"
				"Lower values reduce memory usage, but can limit transparency accuracy in heavier scenes."
			)
		);

		static int perPixelLayersTemp = config::PerPixelLayers.get();
		if (!ImGui::IsPopupOpen("PerPixelMaximumLayersPopup"))
			perPixelLayersTemp = config::PerPixelLayers.get();

		SettingsUI::PopupSliderConfig perPixelLayersCfg {};
		perPixelLayersCfg.label = T("Maximum Layers");
		perPixelLayersCfg.icon = ICON_FA_LAYER_GROUP;
		perPixelLayersCfg.popupID = Tnop("PerPixelMaximumLayersPopup");
		perPixelLayersCfg.description =
			T(
				"Controls how many transparent layers can be resolved per pixel in complex scenes.\n"
				"Increase this if you see missing transparency or incorrect layering.\n"
				"Lower values reduce memory pressure and can improve performance."
			);
		perPixelLayersCfg.currentValue = &perPixelLayersTemp;
		perPixelLayersCfg.minValue = 8;
		perPixelLayersCfg.maxValue = 128;
		perPixelLayersCfg.format = "%d";
		perPixelLayersCfg.valueWidth = 220.0f;
		perPixelLayersCfg.sliderWidth = 320.0f;
		perPixelLayersCfg.onValueChange = [&]() {
			config::PerPixelLayers.set(perPixelLayersTemp);
		};

		SettingsUI::PopupConfig perPixelLayersPopupCfg {};
		perPixelLayersPopupCfg.type = SettingsUI::PopupType::Slider;
		perPixelLayersPopupCfg.slider = perPixelLayersCfg;

		RenderGeneralPopupSettingRow(
			Tnop("PerPixelMaximumLayersSetting"),
			T("Maximum transparent layers resolved per pixel."),
			perPixelLayersPopupCfg,
			T(
				"Per-Pixel Maximum Layers\n"
				"Limits how many transparent layers can be resolved per pixel in complex scenes.\n"
				"Increase this if you see missing transparency or incorrect layering.\n"
				"Decrease it to improve performance and reduce memory pressure."
			)
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
				"Uses the sample-filter path with nearest-neighbor sampling only. This affects eligible textures up to the selected size limit and leaves Flycast's global sampler override at Default."
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
		if (ImGui::Button(T("Texture Filtering Live Tune"), ScaledVec2(260.f, 34.f)))
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

	// Update renderer type based on selections
	switch (renderApi)
	{
	case 0:
		config::RendererType = perPixel ? RenderType::OpenGL_OIT : RenderType::OpenGL;
		break;
	case 1:
		config::RendererType = perPixel ? RenderType::Vulkan_OIT : RenderType::Vulkan;
		break;
	case 2:
		config::RendererType = RenderType::DirectX9;
		break;
		case 3:
			config::RendererType = perPixel ? RenderType::DirectX11_OIT : RenderType::DirectX11;
			break;
	}
}

struct ShaderBrowserEntry
{
	std::string path;
	std::string displayPath;
	std::string catalogKey;
	std::string searchText;
	postprocess::ShaderFormat format = postprocess::ShaderFormat::Unknown;
};

struct ShaderBrowserMetadata
{
	std::string name;
	std::string description;
	std::string author;
	std::string performance;
	bool mobileRecommended = false;
};

static std::vector<ShaderBrowserEntry> g_shaderBrowserEntries;
static std::unordered_map<std::string, ShaderBrowserMetadata> g_shaderBrowserMetadata;
static std::string g_shaderBrowserDirectory;
static std::string g_shaderBrowserError;
static std::string g_shaderCatalogWarning;
static std::string g_shaderFilter;
static bool g_shaderBrowserInitialized = false;
static std::vector<size_t> g_shaderVisibleEntries;
static std::string g_shaderAppliedFilter;
static uint64_t g_shaderBrowserGeneration = 0;
static uint64_t g_shaderVisibleGeneration = 0;

struct ShaderScanResult
{
	std::string directory;
	std::vector<ShaderBrowserEntry> entries;
	std::string error;
};

static std::future<ShaderScanResult> g_shaderScanFuture;
static bool g_shaderScanPending = false;
static std::string g_shaderPendingDirectory;

struct ShaderPreviewState
{
	bool initialized = false;
	std::vector<std::string> paths;
	std::string technique;
	std::string overrides;
	postprocess::ShaderChain chain;
	std::vector<postprocess::ShaderParameter> defaults;
	std::string error;
	uint64_t activationStatusGeneration = 0;
	bool activationPending = false;
};

static ShaderPreviewState g_shaderPreview;
static std::atomic_bool g_shaderLiveTune { false };

static std::string defaultShaderDirectory()
{
	return get_writable_data_path("shaders/");
}

static std::string configuredShaderDirectory()
{
	return config::PostProcessShaderDirectory.get().empty()
			? defaultShaderDirectory() : config::PostProcessShaderDirectory.get();
}

static std::string lowercaseAscii(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
		return static_cast<char>(std::tolower(character));
	});
	return value;
}

static std::string shaderCatalogKey(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	while (path.rfind("./", 0) == 0)
		path.erase(0, 2);
#ifdef _WIN32
	path = lowercaseAscii(std::move(path));
#endif
	return path;
}

static void loadShaderCatalog(const std::string& directory)
{
	constexpr size_t MaxCatalogBytes = 1024 * 1024;
	g_shaderBrowserMetadata.clear();
	g_shaderCatalogWarning.clear();
	const std::string catalogPath = hostfs::storage().getSubPath(directory, "shader-catalog.json");
	if (!hostfs::storage().exists(catalogPath))
		return;

	try {
		std::unique_ptr<hostfs::File> file(hostfs::storage().openFile(catalogPath, "rb"));
		if (file == nullptr || file->size() < 0 || static_cast<uint64_t>(file->size()) > MaxCatalogBytes) {
			g_shaderCatalogWarning = T("shader-catalog.json is unavailable or exceeds the 1 MiB safety limit.");
			return;
		}
		std::string text(static_cast<size_t>(file->size()), '\0');
		if (!text.empty() && file->read(text.data(), 1, text.size()) != text.size()) {
			g_shaderCatalogWarning = T("shader-catalog.json could not be read.");
			return;
		}
		const nlohmann::json catalog = nlohmann::json::parse(text, nullptr, false);
		if (catalog.is_discarded() || !catalog.is_object() || !catalog.contains("shaders")
				|| !catalog["shaders"].is_object()) {
			g_shaderCatalogWarning = T("shader-catalog.json is malformed.");
			return;
		}
		for (auto entry = catalog["shaders"].begin(); entry != catalog["shaders"].end(); ++entry) {
			if (!entry.value().is_object())
				continue;
			ShaderBrowserMetadata metadata;
			metadata.name = entry.value().value("name", std::string());
			metadata.description = entry.value().value("description", std::string());
			metadata.author = entry.value().value("author", std::string());
			metadata.performance = entry.value().value("performance", std::string());
			metadata.mobileRecommended = entry.value().value("mobile_recommended", false);
			g_shaderBrowserMetadata.emplace(shaderCatalogKey(entry.key()), std::move(metadata));
		}
	}
	catch (const std::exception& exception) {
		g_shaderCatalogWarning = exception.what();
	}
	for (ShaderBrowserEntry& entry : g_shaderBrowserEntries) {
		entry.searchText = lowercaseAscii(entry.displayPath);
		if (const auto metadata = g_shaderBrowserMetadata.find(entry.catalogKey);
				metadata != g_shaderBrowserMetadata.end())
			entry.searchText += '\n' + lowercaseAscii(metadata->second.name) + '\n'
					+ lowercaseAscii(metadata->second.description) + '\n' + lowercaseAscii(metadata->second.author);
	}
}

static std::string shaderDisplayPath(const std::string& root, const std::string& path)
{
	std::string normalizedRoot = root;
	std::string normalizedPath = path;
	std::replace(normalizedRoot.begin(), normalizedRoot.end(), '\\', '/');
	std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');
	while (!normalizedRoot.empty() && normalizedRoot.back() == '/')
		normalizedRoot.pop_back();
#ifdef _WIN32
	const bool insideRoot = lowercaseAscii(normalizedPath).compare(0, normalizedRoot.size(), lowercaseAscii(normalizedRoot)) == 0;
#else
	const bool insideRoot = normalizedPath.compare(0, normalizedRoot.size(), normalizedRoot) == 0;
#endif
	if (insideRoot && normalizedPath.size() > normalizedRoot.size()
			&& normalizedPath[normalizedRoot.size()] == '/')
		return normalizedPath.substr(normalizedRoot.size() + 1);
	return normalizedPath;
}

static ShaderScanResult scanShaderDirectory(std::string directory)
{
	constexpr size_t MaxDirectories = 4096;
	constexpr size_t MaxFiles = 16384;
	constexpr size_t MaxDepth = 64;
	ShaderScanResult result;
	result.directory = std::move(directory);
	try {
		std::vector<std::pair<std::string, size_t>> pendingDirectories {
			{ result.directory, 0 }
		};
		std::unordered_set<std::string> visitedDirectories;
		while (!pendingDirectories.empty() && result.entries.size() < MaxFiles
				&& visitedDirectories.size() < MaxDirectories) {
			auto [directory, depth] = std::move(pendingDirectories.back());
			pendingDirectories.pop_back();
			if (!visitedDirectories.emplace(directory).second)
				continue;
			for (const hostfs::FileInfo& entry : hostfs::storage().listContent(directory)) {
				if (entry.isDirectory) {
					if (depth < MaxDepth)
						pendingDirectories.emplace_back(entry.path, depth + 1);
					continue;
				}
				const postprocess::ShaderFormat format = postprocess::detectFormat(entry.path);
				if (format == postprocess::ShaderFormat::Unknown)
					continue;
				const std::string displayPath = shaderDisplayPath(result.directory, entry.path);
				result.entries.push_back({ entry.path, displayPath, shaderCatalogKey(displayPath),
						lowercaseAscii(displayPath), format });
				if (result.entries.size() == MaxFiles)
					break;
			}
		}
		if (!pendingDirectories.empty())
			result.error = "The shader scan reached its safety limit. Choose a smaller shader folder.";
		std::sort(result.entries.begin(), result.entries.end(), [](const ShaderBrowserEntry& left,
				const ShaderBrowserEntry& right) {
			return left.searchText < right.searchText;
		});
	}
	catch (const std::exception& exception) {
		result.error = exception.what();
	}
	return result;
}

static void startShaderBrowserScan(const std::string& directory)
{
	g_shaderBrowserDirectory = directory;
	g_shaderBrowserInitialized = true;
	g_shaderBrowserEntries.clear();
	g_shaderVisibleEntries.clear();
	g_shaderBrowserError.clear();
	g_shaderCatalogWarning.clear();
	if (config::PostProcessShaderDirectory.get().empty()
			&& !hostfs::storage().exists(directory) && !make_directory(directory)) {
		g_shaderBrowserError = T("The default Data/shaders folder could not be created.");
		return;
	}
	if (!hostfs::storage().exists(directory)) {
		g_shaderBrowserError = T("The selected shader folder does not exist or is unavailable.");
		return;
	}
	g_shaderScanFuture = std::async(std::launch::async, [directory] {
		return scanShaderDirectory(directory);
	});
}

static void refreshShaderBrowser()
{
	const std::string directory = configuredShaderDirectory();
	if (g_shaderScanFuture.valid()) {
		g_shaderScanPending = true;
		g_shaderPendingDirectory = directory;
		return;
	}
	startShaderBrowserScan(directory);
}

static void pollShaderBrowserScan()
{
	if (!g_shaderScanFuture.valid()
			|| g_shaderScanFuture.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
		return;
	ShaderScanResult result;
	try {
		result = g_shaderScanFuture.get();
	}
	catch (const std::exception& exception) {
		result.directory = g_shaderBrowserDirectory;
		result.error = exception.what();
	}
	if (g_shaderScanPending) {
		g_shaderScanPending = false;
		startShaderBrowserScan(g_shaderPendingDirectory);
		g_shaderPendingDirectory.clear();
		return;
	}
	if (result.directory != g_shaderBrowserDirectory)
		return;
	g_shaderBrowserEntries = std::move(result.entries);
	g_shaderBrowserError = std::move(result.error);
	loadShaderCatalog(g_shaderBrowserDirectory);
	++g_shaderBrowserGeneration;
}

static bool rebuildShaderPreview()
{
	g_shaderPreview.initialized = true;
	g_shaderPreview.paths = postprocess::configuredShaderPaths();
	g_shaderPreview.technique = config::PostProcessShaderTechnique.get();
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	g_shaderPreview.chain = {};
	g_shaderPreview.defaults.clear();
	g_shaderPreview.error.clear();
	if (g_shaderPreview.paths.empty())
		return true;

	postprocess::ShaderChain candidate;
	if (!postprocess::loadShaderChain(g_shaderPreview.paths, candidate, g_shaderPreview.error)
			|| !postprocess::preflightShaderAssets(candidate, g_shaderPreview.error))
		return false;
	g_shaderPreview.defaults = candidate.parameters;
	postprocess::applyShaderChainParameterValues(candidate, g_shaderPreview.overrides);
	g_shaderPreview.chain = std::move(candidate);
	return true;
}

static void ensureShaderPreview()
{
	const std::vector<std::string> paths = postprocess::configuredShaderPaths();
	if (!g_shaderPreview.initialized || paths != g_shaderPreview.paths
			|| config::PostProcessShaderTechnique.get() != g_shaderPreview.technique
			|| config::PostProcessShaderParameters.get() != g_shaderPreview.overrides)
		rebuildShaderPreview();
}

static void requestShaderActivation()
{
	// The renderer publishes a new status after it has processed this request.
	// Keeping the status generation avoids treating the previous stack as the
	// newly selected one while the game is paused between rendered frames.
	g_shaderPreview.activationStatusGeneration = postprocess::shaderChainStatusGeneration();
	g_shaderPreview.activationPending = true;
	postprocess::requestShaderChainReload();
}

static bool shaderActivationPending()
{
	if (g_shaderPreview.activationPending
			&& postprocess::shaderChainStatusGeneration() != g_shaderPreview.activationStatusGeneration)
		g_shaderPreview.activationPending = false;
	return g_shaderPreview.activationPending;
}

static bool previewMatchesActiveShader()
{
	if (shaderActivationPending() || g_shaderPreview.paths.empty())
		return false;
	const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
	return status.error.empty() && !status.activePath.empty()
			&& status.activePath == postprocess::shaderSelectionLabel(g_shaderPreview.paths)
			&& status.parameters.size() == g_shaderPreview.chain.parameters.size();
}

static void shaderTextWrapped(const ImVec4& color, const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	ImGui::PushStyleColor(ImGuiCol_Text, color);
	ImGui::TextWrappedV(format, arguments);
	ImGui::PopStyleColor();
	va_end(arguments);
}

static bool setPreviewParameter(const std::string& name, const float *values, uint32_t componentCount)
{
	if (values == nullptr || componentCount == 0 || componentCount > 16)
		return false;
	auto parameter = std::find_if(g_shaderPreview.chain.parameters.begin(), g_shaderPreview.chain.parameters.end(),
			[&](const postprocess::ShaderParameter& candidate) {
				return postprocess::shaderParameterKey(candidate) == name;
			});
	if (parameter == g_shaderPreview.chain.parameters.end() || parameter->componentCount != componentCount)
		return false;
	for (uint32_t component = 0; component < componentCount; ++component) {
		const float minimum = componentCount == 1 ? parameter->minimum : parameter->minimumValues[component];
		const float maximum = componentCount == 1 ? parameter->maximum : parameter->maximumValues[component];
		if (!std::isfinite(values[component])
				|| (minimum < maximum && (values[component] < minimum || values[component] > maximum)))
			return false;
	}

	std::copy_n(values, componentCount, parameter->values.begin());
	parameter->value = parameter->values[0];
	config::PostProcessShaderParameters.get() =
			postprocess::serializeShaderChainParameterValues(g_shaderPreview.chain.parameters);
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	if (previewMatchesActiveShader()) {
		if (componentCount == 1)
			postprocess::setShaderChainParameter(name, values[0]);
		else
			postprocess::setShaderChainParameterComponents(name, values, componentCount);
	}
	return true;
}

static void resetShaderParameters()
{
	if (g_shaderPreview.defaults.size() != g_shaderPreview.chain.parameters.size())
		return;
	g_shaderPreview.chain.parameters = g_shaderPreview.defaults;
	config::PostProcessShaderParameters.get().clear();
	g_shaderPreview.overrides.clear();
	if (previewMatchesActiveShader()) {
		for (const postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
			if (parameter.componentCount == 1)
				postprocess::setShaderChainParameter(postprocess::shaderParameterKey(parameter), parameter.values[0]);
			else
				postprocess::setShaderChainParameterComponents(postprocess::shaderParameterKey(parameter),
						parameter.values.data(), parameter.componentCount);
		}
	}
	SaveSettings();
}

static void renderShaderActivationStatus()
{
	if (g_shaderPreview.paths.empty()) {
		ImGui::TextDisabled("%s", T("No display shader selected."));
		return;
	}
	if (!g_shaderPreview.error.empty()) {
		shaderTextWrapped(ImVec4(1.f, .35f, .35f, 1.f), T("Invalid: %s"), g_shaderPreview.error.c_str());
		return;
	}
	if (shaderActivationPending()) {
		shaderTextWrapped(ImVec4(1.f, .75f, .25f, 1.f), "%s",
				g_shaderLiveTune ? T("Loading on the current graphics API...")
						: T("Ready. Start Live Tune or return to the game to activate it."));
		return;
	}

	const postprocess::ShaderChainStatus status = postprocess::getShaderChainStatus();
	if (!status.error.empty())
		shaderTextWrapped(ImVec4(1.f, .35f, .35f, 1.f), T("Rejected: %s"), status.error.c_str());
	else if (status.activePath == postprocess::shaderSelectionLabel(g_shaderPreview.paths))
		shaderTextWrapped(ImVec4(.35f, 1.f, .55f, 1.f), T("Active: %s"), status.activePath.c_str());
	else
		shaderTextWrapped(ImVec4(1.f, .75f, .25f, 1.f), "%s", T("Parsed successfully; waiting for gameplay to activate."));
}

static void applyShaderTechniqueSelection(std::string technique)
{
	const std::string previousTechnique = config::PostProcessShaderTechnique.get();
	ShaderPreviewState previousPreview = std::move(g_shaderPreview);
	config::PostProcessShaderTechnique.get() = std::move(technique);
	if (!rebuildShaderPreview()) {
		// Keep the working technique and its controls when a consumer is chosen
		// without the producer layer it requires.
		const std::string error = g_shaderPreview.error;
		config::PostProcessShaderTechnique.get() = previousTechnique;
		g_shaderPreview = std::move(previousPreview);
		gui_error(error);
		return;
	}
	requestShaderActivation();
	SaveSettings();
}

static void renderShaderParameterControls()
{
	for (postprocess::ShaderTechniqueGroup& group : g_shaderPreview.chain.techniqueGroups) {
		if (group.techniques.size() < 2)
			continue;
		const int selectedTechnique = group.selectedTechnique >= 0
				&& group.selectedTechnique < static_cast<int>(group.techniques.size())
				? group.selectedTechnique : 0;
		ImGui::PushID(static_cast<int>(group.layerIndex));
		std::string selection;
		if (ImGui::BeginCombo(group.label.c_str(), group.techniques[selectedTechnique].c_str())) {
			for (size_t index = 0; index < group.techniques.size(); ++index) {
				const bool selected = static_cast<int>(index) == selectedTechnique;
				if (ImGui::Selectable(group.techniques[index].c_str(), selected) && !selected) {
					const int previousIndex = group.selectedTechnique;
					group.selectedTechnique = static_cast<int>(index);
					selection = postprocess::serializeShaderTechniqueSelections(g_shaderPreview.chain.techniqueGroups);
					group.selectedTechnique = previousIndex;
				}
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::PopID();
		if (!selection.empty()) {
			applyShaderTechniqueSelection(std::move(selection));
			break;
		}
	}
	if (g_shaderPreview.chain.techniqueGroups.empty() && g_shaderPreview.chain.techniques.size() > 1) {
		const int selectedTechnique = g_shaderPreview.chain.selectedTechnique >= 0
				&& g_shaderPreview.chain.selectedTechnique < static_cast<int>(g_shaderPreview.chain.techniques.size())
				? g_shaderPreview.chain.selectedTechnique : 0;
		std::string selection;
		if (ImGui::BeginCombo(T("Technique"), g_shaderPreview.chain.techniques[selectedTechnique].c_str())) {
			for (size_t index = 0; index < g_shaderPreview.chain.techniques.size(); ++index) {
				const bool selected = static_cast<int>(index) == selectedTechnique;
				if (ImGui::Selectable(g_shaderPreview.chain.techniques[index].c_str(), selected) && !selected)
					selection = g_shaderPreview.chain.techniques[index];
				if (selected)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		if (!selection.empty())
			applyShaderTechniqueSelection(std::move(selection));
	}
	if (g_shaderPreview.chain.parameters.empty()) {
		ImGui::TextDisabled("%s", T("This shader stack exposes no adjustable parameters."));
		return;
	}
	if (ImGui::SmallButton(T("Reset All Parameters")))
		resetShaderParameters();
	ImGui::Separator();

	for (postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
		if (!parameter.hostSource.empty())
			continue;
		ImGui::PushID(postprocess::shaderParameterKey(parameter).c_str());
		if (!parameter.key.empty())
			ImGui::TextDisabled(T("Layer %u"), parameter.layerIndex + 1);
		bool changed = false;
		bool saveAfterEdit = false;
		if (parameter.componentCount <= 1) {
			float value = parameter.value;
			if (parameter.type == postprocess::SlangParameterType::Bool) {
				ImGui::TextWrapped("%s", parameter.label.c_str());
				bool enabled = value != 0.f;
				changed = ImGui::Checkbox(T("Enabled"), &enabled);
				value = enabled ? 1.f : 0.f;
				saveAfterEdit = changed;
			}
			else if (!parameter.choiceLabels.empty()) {
				const int selected = static_cast<int>(std::clamp(std::round(static_cast<double>(value)),
						0.0, static_cast<double>(parameter.choiceLabels.size() - 1)));
				ImGui::TextWrapped("%s", parameter.label.c_str());
				ImGui::SetNextItemWidth(-1.f);
				if (ImGui::BeginCombo("##choice", parameter.choiceLabels[selected].c_str())) {
					for (size_t index = 0; index < parameter.choiceLabels.size(); ++index) {
						const bool isSelected = static_cast<int>(index) == selected;
						if (ImGui::Selectable(parameter.choiceLabels[index].c_str(), isSelected) && !isSelected) {
							value = static_cast<float>(index);
							changed = true;
							saveAfterEdit = true;
						}
						if (isSelected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
			}
			else if (parameter.minimum < parameter.maximum) {
				const double range = static_cast<double>(parameter.maximum) - parameter.minimum;
				if (parameter.type == postprocess::SlangParameterType::Int) {
					// Shader annotations are external data. Bound the slider before
					// converting to int so large ranges cannot overflow the UI.
					const double minimum = std::ceil(static_cast<double>(parameter.minimum));
					const double maximum = std::max(minimum, std::floor(static_cast<double>(parameter.maximum)));
					const double step = parameter.step > 0.f ? std::max(1.0, std::round(static_cast<double>(parameter.step)))
							: std::max(1.0, std::ceil((maximum - minimum) / 100.0));
					const int stepCount = static_cast<int>(std::clamp(std::ceil((maximum - minimum) / step), 1.0, 1000000.0));
					int stepIndex = static_cast<int>(std::clamp(std::round((value - minimum) / step), 0.0, static_cast<double>(stepCount)));
					ImGui::TextWrapped("%s: %.0f", parameter.label.c_str(), static_cast<double>(value));
					ImGui::SetNextItemWidth(-1.f);
					changed = ImGui::SliderInt("##value", &stepIndex, 0, stepCount, "");
					value = static_cast<float>(std::min(maximum, minimum + stepIndex * step));
				}
				else {
					const double step = parameter.step > 0.f ? parameter.step : range / 100.0;
					const int stepCount = static_cast<int>(std::clamp(std::ceil(range / step), 1.0, 1000000.0));
					int stepIndex = static_cast<int>(std::clamp(std::round((value - parameter.minimum) / step), 0.0, static_cast<double>(stepCount)));
					ImGui::TextWrapped("%s: %.3f", parameter.label.c_str(), value);
					ImGui::SetNextItemWidth(-1.f);
					changed = ImGui::SliderInt("##value", &stepIndex, 0, stepCount, "");
					value = static_cast<float>(std::clamp(parameter.minimum + stepIndex * step,
							static_cast<double>(parameter.minimum), static_cast<double>(parameter.maximum)));
				}
				saveAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
			}
			else
				ImGui::TextDisabled("%s: %.3f", parameter.label.c_str(), parameter.value);
			if (changed)
				setPreviewParameter(postprocess::shaderParameterKey(parameter), &value, 1);
		}
		else {
			ImGui::TextWrapped("%s", parameter.label.c_str());
			float values[16] {};
			std::copy(parameter.values.begin(), parameter.values.end(), values);
			for (uint32_t component = 0; component < parameter.componentCount; ++component) {
				ImGui::PushID(static_cast<int>(component));
				const float minimum = parameter.minimumValues[component];
				const float maximum = parameter.maximumValues[component];
				const float step = parameter.stepValues[component] > 0.f ? parameter.stepValues[component] : 0.01f;
				ImGui::TextDisabled("%u", component + 1);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(-1.f);
				changed = ImGui::DragFloat("##component", &values[component],
						step,
						minimum < maximum ? minimum : 0.f, minimum < maximum ? maximum : 0.f) || changed;
				if (ImGui::IsItemEdited() && minimum < maximum)
					values[component] = std::clamp(minimum + std::round((values[component] - minimum) / step) * step,
							minimum, maximum);
				saveAfterEdit = ImGui::IsItemDeactivatedAfterEdit() || saveAfterEdit;
				ImGui::PopID();
			}
			if (changed)
				setPreviewParameter(postprocess::shaderParameterKey(parameter), values, parameter.componentCount);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::BeginTooltip();
			ImGui::Text("%s", parameter.label.c_str());
			ImGui::TextDisabled("%s", parameter.name.c_str());
			ImGui::EndTooltip();
		}
		if (saveAfterEdit)
			SaveSettings();
		ImGui::PopID();
	}
}

static void applyShaderSelection(const std::vector<std::string>& paths,
		std::vector<size_t> previousLayers = {})
{
	const std::vector<std::string> previousPaths = g_shaderPreview.paths;
	const std::vector<postprocess::ShaderParameter> previousParameters = g_shaderPreview.chain.parameters;
	// Explicit indices preserve identity when two layers use the same effect.
	// Browser changes match each old occurrence once; new layers start at defaults.
	if (previousLayers.size() != paths.size()) {
		previousLayers.assign(paths.size(), previousPaths.size());
		std::vector<bool> used(previousPaths.size(), false);
		for (size_t layer = 0; layer < paths.size(); ++layer)
			for (size_t previous = 0; previous < previousPaths.size(); ++previous)
				if (!used[previous] && previousPaths[previous] == paths[layer]) {
					previousLayers[layer] = previous;
					used[previous] = true;
					break;
				}
	}
	std::vector<postprocess::ShaderTechniqueGroup> previousTechniques = g_shaderPreview.chain.techniqueGroups;
	if (previousTechniques.empty() && previousPaths.size() == 1 && !g_shaderPreview.chain.techniques.empty())
		previousTechniques.push_back({ 0, {}, g_shaderPreview.chain.techniques, g_shaderPreview.chain.selectedTechnique });
	std::vector<postprocess::ShaderTechniqueGroup> techniques;
	for (size_t layer = 0; layer < paths.size(); ++layer)
		for (const auto& previous : previousTechniques)
			if (previous.layerIndex == previousLayers[layer]) {
				techniques.push_back(previous);
				techniques.back().layerIndex = layer;
				break;
			}
	const auto previousConfiguredPaths = config::PostProcessShaders.get();
	const std::string previousSinglePath = config::PostProcessShader.get();
	const std::string previousTechnique = config::PostProcessShaderTechnique.get();
	const std::string previousOverrides = config::PostProcessShaderParameters.get();
	ShaderPreviewState previousPreview = std::move(g_shaderPreview);
	config::PostProcessShaders.get() = paths;
	// Keep old single-path configurations interoperable with older builds.
	config::PostProcessShader.get() = paths.size() == 1 ? paths.front() : std::string();
	if (paths.size() == 1 && !techniques.empty() && techniques.front().selectedTechnique >= 0
			&& techniques.front().selectedTechnique < static_cast<int>(techniques.front().techniques.size()))
		config::PostProcessShaderTechnique.get() = techniques.front().techniques[techniques.front().selectedTechnique];
	else
		config::PostProcessShaderTechnique.get() = postprocess::serializeShaderTechniqueSelections(techniques);
	config::PostProcessShaderParameters.get().clear();
	if (!rebuildShaderPreview()) {
		// An invalid ordering must not erase settings from the working stack.
		const std::string error = g_shaderPreview.error;
		config::PostProcessShaders.get() = previousConfiguredPaths;
		config::PostProcessShader.get() = previousSinglePath;
		config::PostProcessShaderTechnique.get() = previousTechnique;
		config::PostProcessShaderParameters.get() = previousOverrides;
		g_shaderPreview = std::move(previousPreview);
		gui_error(error);
		return;
	}
	for (postprocess::ShaderParameter& parameter : g_shaderPreview.chain.parameters) {
		if (!parameter.hostSource.empty())
			continue;
		if (parameter.layerIndex >= previousLayers.size())
			continue;
		const size_t previousLayer = previousLayers[parameter.layerIndex];
		const auto previous = std::find_if(previousParameters.begin(), previousParameters.end(), [&](const auto& value) {
			return value.layerIndex == previousLayer && value.name == parameter.name
					&& value.type == parameter.type && value.componentCount == parameter.componentCount;
		});
		if (previous == previousParameters.end())
			continue;
		for (uint32_t component = 0; component < parameter.componentCount; ++component) {
			const float minimum = parameter.componentCount == 1 ? parameter.minimum : parameter.minimumValues[component];
			const float maximum = parameter.componentCount == 1 ? parameter.maximum : parameter.maximumValues[component];
			parameter.values[component] = minimum < maximum
					? std::clamp(previous->values[component], minimum, maximum) : previous->values[component];
		}
		parameter.value = parameter.values[0];
	}
	config::PostProcessShaderParameters.get() = postprocess::serializeShaderChainParameterValues(g_shaderPreview.chain.parameters);
	g_shaderPreview.overrides = config::PostProcessShaderParameters.get();
	requestShaderActivation();
	SaveSettings();
}

static std::string selectedShaderLabel(const std::string& path)
{
	const auto entry = std::find_if(g_shaderBrowserEntries.begin(), g_shaderBrowserEntries.end(),
			[&](const ShaderBrowserEntry& value) { return value.path == path; });
	if (entry == g_shaderBrowserEntries.end())
		return path;
	const auto metadata = g_shaderBrowserMetadata.find(entry->catalogKey);
	return metadata != g_shaderBrowserMetadata.end() && !metadata->second.name.empty()
			? metadata->second.name : entry->displayPath;
}

void renderShaderTab()
{
	ImGui::TextDisabled("%s", T("Shader Configuration"));
	ImGui::Separator();
	ensureShaderPreview();

	const std::string directory = configuredShaderDirectory();
	if (!g_shaderBrowserInitialized || directory != g_shaderBrowserDirectory)
		refreshShaderBrowser();
	pollShaderBrowserScan();

	if (RenderCollapsingHeader("ShaderLibrarySection", ICON_FA_FOLDER_OPEN, T("Shader Library"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T("Hollycast scans this folder recursively for GLSL, GLSLP, Slang, Slang presets, and supported ReShade FX files."));
		ImGui::TextWrapped(T("Folder: %s"), directory.c_str());

		const char *folderPopup = T("Select Shader Folder");
		const auto folderSelected = [](bool cancelled, const std::string& selection) {
			if (!cancelled && !selection.empty()) {
				config::PostProcessShaderDirectory.get() = selection;
				refreshShaderBrowser();
			}
			return true;
		};
		const bool changeFolder = ImGui::Button(T("Change Folder"));
#ifdef __ANDROID__
		if (changeFolder && select_storage_popup(true, false, folderPopup, folderSelected)
				== StoragePopupResult::Unsupported)
			ImGui::OpenPopup(folderPopup);
#else
		if (changeFolder)
			ImGui::OpenPopup(folderPopup);
#endif
		ImGui::SameLine();
		if (ImGui::Button(T("Refresh List")))
			refreshShaderBrowser();
		if (!config::PostProcessShaderDirectory.get().empty()) {
			ImGui::SameLine();
			if (ImGui::Button(T("Use Default Folder"))) {
				config::PostProcessShaderDirectory.get().clear();
				refreshShaderBrowser();
			}
		}
		select_file_popup(folderPopup, folderSelected, false);

		std::vector<std::string> selectedPaths = postprocess::configuredShaderPaths();
		ImGui::Text(T("Recognized: %u    Selected layers: %u / %u"),
				static_cast<unsigned>(g_shaderBrowserEntries.size()), static_cast<unsigned>(selectedPaths.size()),
				static_cast<unsigned>(postprocess::MaxShaderLayers));
		if (!g_shaderBrowserError.empty())
			ImGui::TextColored(ImVec4(1.f, .55f, .25f, 1.f), "%s", g_shaderBrowserError.c_str());
		if (!g_shaderCatalogWarning.empty())
			ImGui::TextColored(ImVec4(1.f, .75f, .25f, 1.f), "%s", g_shaderCatalogWarning.c_str());
		if (g_shaderScanFuture.valid())
			ImGui::TextDisabled("%s", T("Scanning shader folder..."));

		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##ShaderFilter", T("Filter recognized shaders..."), &g_shaderFilter);
		if (g_shaderAppliedFilter != g_shaderFilter || g_shaderVisibleGeneration != g_shaderBrowserGeneration) {
			g_shaderAppliedFilter = g_shaderFilter;
			g_shaderVisibleGeneration = g_shaderBrowserGeneration;
			g_shaderVisibleEntries.clear();
			g_shaderVisibleEntries.reserve(g_shaderBrowserEntries.size());
			const std::string loweredFilter = lowercaseAscii(g_shaderFilter);
			for (size_t index = 0; index < g_shaderBrowserEntries.size(); ++index)
				if (loweredFilter.empty()
						|| g_shaderBrowserEntries[index].searchText.find(loweredFilter) != std::string::npos)
					g_shaderVisibleEntries.push_back(index);
		}

		ImGui::BeginChild("RecognizedShaderList", ImVec2(0.f, SettingsLayoutScaled(260.f)),
				ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
		if (g_shaderVisibleEntries.empty())
			ImGui::TextDisabled("%s", g_shaderBrowserEntries.empty()
					? T("No recognized shaders were found in this folder.") : T("No shaders match the current filter."));
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(g_shaderVisibleEntries.size()));
		while (clipper.Step()) {
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
				const ShaderBrowserEntry& entry = g_shaderBrowserEntries[g_shaderVisibleEntries[row]];
				ImGui::PushID(entry.path.c_str());
				const auto selected = std::find(selectedPaths.begin(), selectedPaths.end(), entry.path);
				bool enabled = selected != selectedPaths.end();
				const bool canEnable = enabled || selectedPaths.size() < postprocess::MaxShaderLayers;
				if (!canEnable)
					ImGui::BeginDisabled();
				if (ImGui::Checkbox("##Selected", &enabled)) {
					if (enabled)
						selectedPaths.push_back(entry.path);
					else
						selectedPaths.erase(std::remove(selectedPaths.begin(), selectedPaths.end(), entry.path), selectedPaths.end());
					applyShaderSelection(selectedPaths);
				}
				if (!canEnable)
					ImGui::EndDisabled();
				ImGui::SameLine();
				const auto metadata = g_shaderBrowserMetadata.find(entry.catalogKey);
				const bool hasDisplayName = metadata != g_shaderBrowserMetadata.end() && !metadata->second.name.empty();
				ImGui::TextUnformatted(hasDisplayName ? metadata->second.name.c_str() : entry.displayPath.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", postprocess::formatName(entry.format));
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					if (hasDisplayName)
						ImGui::TextUnformatted(metadata->second.name.c_str());
					ImGui::TextWrapped("%s", entry.displayPath.c_str());
					ImGui::TextDisabled(T("Format: %s"), postprocess::formatName(entry.format));
					if (metadata != g_shaderBrowserMetadata.end()) {
						if (!metadata->second.description.empty())
							ImGui::TextWrapped("%s", metadata->second.description.c_str());
						if (!metadata->second.author.empty())
							ImGui::TextDisabled(T("Author: %s"), metadata->second.author.c_str());
						if (!metadata->second.performance.empty())
							ImGui::TextDisabled(T("Performance: %s"), metadata->second.performance.c_str());
						if (metadata->second.mobileRecommended)
							ImGui::TextColored(ImVec4(.35f, 1.f, .55f, 1.f), "%s", T("Recommended for mobile GPUs"));
					}
					ImGui::Separator();
					ImGui::TextDisabled("%s", enabled
							? T("Selected. Stack compatibility is shown below.")
							: T("Graphics API compatibility is checked when selected and activated."));
					ImGui::EndTooltip();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	std::vector<std::string> selectedPaths = postprocess::configuredShaderPaths();
	if (RenderCollapsingHeader("ShaderLayersSection", ICON_FA_WAND_MAGIC, T("Active Shader Layers"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextWrapped("%s", T("Layers run from top to bottom. A preset remains one selectable layer even when it contains several internal passes."));
		if (selectedPaths.size() > 1)
			ImGui::TextDisabled("%s", T("Each layer keeps its own parameter settings."));
		if (selectedPaths.empty())
			ImGui::TextDisabled("%s", T("No shader layers selected."));
		else if (ImGui::BeginTable("SelectedShaderLayers", 4,
				ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, SettingsLayoutScaled(28.f));
			ImGui::TableSetupColumn(T("Shader"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(T("Order"), ImGuiTableColumnFlags_WidthFixed);
			ImGui::TableSetupColumn(T("Remove"), ImGuiTableColumnFlags_WidthFixed);
			int moveFrom = -1;
			int moveTo = -1;
			int removeIndex = -1;
			int duplicateIndex = -1;
			for (size_t index = 0; index < selectedPaths.size(); ++index) {
				ImGui::PushID(static_cast<int>(index));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%u", static_cast<unsigned>(index + 1));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextWrapped("%s", selectedShaderLabel(selectedPaths[index]).c_str());
				ImGui::TableSetColumnIndex(2);
				if (index == 0)
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Up"))) {
					moveFrom = static_cast<int>(index);
					moveTo = static_cast<int>(index - 1);
				}
				if (index == 0)
					ImGui::EndDisabled();
				ImGui::SameLine();
				if (index + 1 == selectedPaths.size())
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Down"))) {
					moveFrom = static_cast<int>(index);
					moveTo = static_cast<int>(index + 1);
				}
				if (index + 1 == selectedPaths.size())
					ImGui::EndDisabled();
				ImGui::TableSetColumnIndex(3);
				if (selectedPaths.size() >= postprocess::MaxShaderLayers)
					ImGui::BeginDisabled();
				if (ImGui::SmallButton(T("Duplicate")))
					duplicateIndex = static_cast<int>(index);
				if (selectedPaths.size() >= postprocess::MaxShaderLayers)
					ImGui::EndDisabled();
				ImGui::SameLine();
				if (ImGui::SmallButton(T("Remove")))
					removeIndex = static_cast<int>(index);
				ImGui::PopID();
			}
			ImGui::EndTable();
			std::vector<size_t> previousLayers(selectedPaths.size());
			for (size_t layer = 0; layer < previousLayers.size(); ++layer)
				previousLayers[layer] = layer;
			if (removeIndex >= 0) {
				selectedPaths.erase(selectedPaths.begin() + removeIndex);
				previousLayers.erase(previousLayers.begin() + removeIndex);
				applyShaderSelection(selectedPaths, previousLayers);
			}
			else if (moveFrom >= 0 && moveTo >= 0) {
				std::swap(selectedPaths[moveFrom], selectedPaths[moveTo]);
				std::swap(previousLayers[moveFrom], previousLayers[moveTo]);
				applyShaderSelection(selectedPaths, previousLayers);
			}
			else if (duplicateIndex >= 0) {
				selectedPaths.insert(selectedPaths.begin() + duplicateIndex + 1, selectedPaths[duplicateIndex]);
				previousLayers.insert(previousLayers.begin() + duplicateIndex + 1, duplicateIndex);
				applyShaderSelection(selectedPaths, previousLayers);
			}
		}

		if (!selectedPaths.empty()) {
			if (ImGui::Button(T("Reload Active Stack"))) {
				requestShaderActivation();
				rebuildShaderPreview();
			}
			ImGui::SameLine();
			if (ImGui::Button(T("Clear All")))
				applyShaderSelection({});
			ImGui::SameLine();
			const bool canLiveTune = game_started && !ggpo::active() && (!emu.running() || achievements::canPause())
					&& g_shaderPreview.error.empty();
			if (!canLiveTune)
				ImGui::BeginDisabled();
			if (ImGui::Button(T("Live Tune"))) {
				applyPendingSettings();
				GamepadDevice::load_system_mappings();
				stopTextureFilterLiveTune();
				g_shaderLiveTune = true;
				gui_resume_game();
			}
			if (!canLiveTune)
				ImGui::EndDisabled();
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("%s", !game_started ? T("Start a game to tune shader parameters live.")
						: T("The selected stack must parse successfully and gameplay must be pausable."));
		}
	}

	if (RenderCollapsingHeader("ShaderStatusSection", ICON_FA_SLIDERS, T("Status and Parameters"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		renderShaderActivationStatus();
		renderShaderParameterControls();
	}
}

void renderShaderLiveTuneOverlay()
{
	if (!g_shaderLiveTune)
		return;
	if (!game_started || ggpo::active()) {
		closeShaderLiveTune();
		return;
	}
	ensureShaderPreview();

	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
#ifdef __ANDROID__
	const float panelHeight = std::max(displaySize.y * .48f, uiScaled(260.f));
	ImGui::SetNextWindowPos(ImVec2(0.f, displaySize.y - panelHeight), ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(displaySize.x, panelHeight), ImGuiCond_Always);
	flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
#else
	const float margin = uiScaled(18.f);
	const ImVec2 maximumSize(std::max(1.f, displaySize.x - margin * 2.f),
			std::max(1.f, displaySize.y - uiScaled(96.f)));
	const ImVec2 panelSize(std::min(uiScaled(820.f), maximumSize.x),
			std::min(uiScaled(440.f), maximumSize.y));
	const ImVec2 minimumSize(std::min(uiScaled(560.f), maximumSize.x),
			std::min(uiScaled(260.f), maximumSize.y));
	ImGui::SetNextWindowPos(ImVec2(displaySize.x - panelSize.x - margin, uiScaled(48.f)), ImGuiCond_Once);
	ImGui::SetNextWindowSize(panelSize, ImGuiCond_Once);
	ImGui::SetNextWindowSizeConstraints(minimumSize, maximumSize);
#endif
	ImGui::SetNextWindowBgAlpha(.94f);
	bool open = true;
	bool returnToSettings = false;
	if (ImGui::Begin(T("Shader Live Tune###ShaderLiveTune"), &open, flags)) {
		if (ImGui::Button(T("Back to Shader Page")))
			returnToSettings = true;
		ImGui::SameLine();
		ImGui::TextDisabled(T("%u layer(s)"), static_cast<unsigned>(g_shaderPreview.paths.size()));
		ImGui::Separator();
		renderShaderActivationStatus();
		ImGui::Separator();
		renderShaderParameterControls();
	}
	ImGui::End();

	if (returnToSettings) {
		g_shaderLiveTune = false;
		gui_open_shader_settings();
	}
	else if (!open)
		g_shaderLiveTune = false;
}

bool liveTuneActive()
{
	return textureFilterLiveTuneActive() || g_shaderLiveTune.load(std::memory_order_relaxed);
}

void closeShaderLiveTune()
{
	g_shaderLiveTune = false;
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
				T("0% is solid. 100% is invisible."), vmuTransparencyCfg,
				T("25% keeps the default transparency. Desktop VMUs dim further in menus. Changes apply immediately and are saved with your settings."));
		}
#endif
	}

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
				{ "OverlayShowHostCpuUsage", T("Show Host CPU"), T("Hollycast is the emulator process share of all logical CPU cores. Device is total CPU use from all apps. A single SH4 or renderer thread can still fill one core while Hollycast is low on an 8-core device."), true },
				{ "OverlayShowGpuUsage", T("Show Host GPU"), T("Shows timestamped GPU work as a one-second share and milliseconds per timed render. Includes PVR screen renders and direct framebuffer uploads; full framebuffer emulation can perform both per video frame. It is backend GPU execution time, not a whole-device GPU sensor. Vulkan, OpenGL/GLES, DirectX 9, and DirectX 11 support it."), true },
				{ "OverlayShowSh4ExecutionRate", T("Show Dreamcast SH4 Rate"), T("Shows emulated SH4 cycles advancing versus the real-time Dreamcast target. Below 100% means Hollycast is not advancing SH4 time at the expected rate. It is not host CPU use or a game's exact SH4 occupancy."), true },
				{ "OverlayShowPvrTimingPressure", T("Show Dreamcast PVR Schedule"), T("Shows all emulated PVR renderer-complete delays scheduled during the sample as a share of the active Dreamcast video-frame budgets. Values above 100% mean scheduled render work exceeded one frame of budget on average. It is scheduler pressure, not host GPU use and not a full PVR busy-cycle meter."), true },
				{ "OverlayShowCpuUsage", T("Show Host SH4 Thread"), T("Shows host CPU time used by the thread running SH4 emulation. A value near 100% means that one host core is saturated; it does not directly mean the original Dreamcast SH4 is full."), false },
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
				if (ImGui::ColorEdit4("##OverlayColor", overlayColor, ImGuiColorEditFlags_NoInputs))
					cfgSaveInt("rend", "OverlayColor", static_cast<int>(std::round(overlayColor[0] * 255.f))
						| (static_cast<int>(std::round(overlayColor[1] * 255.f)) << 8)
						| (static_cast<int>(std::round(overlayColor[2] * 255.f)) << 16)
						| (static_cast<int>(std::round(overlayColor[3] * 255.f)) << 24));
			},
			T("Text Color\nControls the overlay text color."));
	}
}

void renderAudioTab()
{
	ScopedTwoLineRowStyle audioRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("Audio Configuration"));
	ImGui::Separator();

	// Audio Playback Section
	if (RenderCollapsingHeader("PlaybackSection", ICON_FA_VOLUME_HIGH, T("Playback"), ImGuiTreeNodeFlags_DefaultOpen))
	{

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
		// Audio Latency Section
#ifdef __ANDROID__
		// Automatic Latency (Android only)
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

	// Manual Latency Control
	if (!config::AutoLatency
		|| (config::AudioBackend.get() != "auto" && config::AudioBackend.get() != "android"))
	{
		static int latencyTemp = (int)roundf(config::AudioBufferSize.get() * 1000.f / 44100.f);
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
			latencyTemp = (int)roundf(openingAudioBufferSize * 1000.f / 44100.f);
		};
		latencyConfig.onCancel = []() {
			// Milliseconds round the sample count. Restore the exact original
			// samples rather than round-tripping through the displayed value.
			config::AudioBufferSize.set(openingAudioBufferSize);
		};
		latencyConfig.onValueChange = []() {
			config::AudioBufferSize.set((int)roundf(latencyTemp * 44100.f / 1000.f));
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

static std::string vmuSlotLabel(int bus, int slot)
{
	std::string label;
	label.push_back(static_cast<char>('A' + bus));
	label.push_back(static_cast<char>('1' + slot));
	return label;
}

static std::string defaultVmuFileNameForSlot(int bus, int slot)
{
	return "vmu_save_" + vmuSlotLabel(bus, slot) + ".bin";
}

static bool isSharedVmuSlotActive(int bus, int slot)
{
	if (config::MapleMainDevices[bus] == MDT_None)
		return false;
	return config::MapleExpansionDevices[bus][slot] == MDT_SegaVMU;
}

static std::string normalizeVmuFileName(std::string name)
{
	if (name.find('.') == std::string::npos)
		return name + ".bin";

	std::string lower = name;
	string_tolower(lower);
	const bool hasVmuExtension = lower.size() >= 4
		&& (lower.compare(lower.size() - 4, 4, ".bin") == 0
			|| lower.compare(lower.size() - 4, 4, ".vmu") == 0);
	if (!hasVmuExtension)
		name += ".bin";
	return name;
}

static bool isLikelyPerGameA1VmuFileName(const std::string& fileName)
{
	static constexpr std::string_view suffix = "_vmu_save_A1.bin";
	return fileName.size() >= suffix.size()
		&& fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix.data()) == 0;
}

static void setConfiguredVmuSlotFileName(int bus, int slot, const std::string& fileName)
{
	config::MapleVmuSlotFileNames[bus][slot].get() =
		fileName == defaultVmuFileNameForSlot(bus, slot) ? "" : fileName;
}

static bool isManualVmuSlotEditable(int bus, int slot, bool perGameEnabled)
{
	return isSharedVmuSlotActive(bus, slot) && !(perGameEnabled && bus == 0 && slot == 0);
}

static std::string currentSlotVmuFileName(int bus, int slot)
{
	const std::string configuredName = config::MapleVmuSlotFileNames[bus][slot].get();
	if (hostfs::isConfiguredVmuFileNameValid(configuredName))
		return configuredName;
	return defaultVmuFileNameForSlot(bus, slot);
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
					&& currentSlotVmuFileName(bus, slot) == fileName)
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
		&& selectedName == defaultVmuFileNameForSlot(selectedBus, selectedSlot);
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
			applyVmuSlotSelection(selectedBus, selectedSlot, defaultVmuFileNameForSlot(selectedBus, selectedSlot));
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
						|| currentSlotVmuFileName(bus, slot) != fileName) {
					continue;
				}
				if (!labels.empty())
					labels += ", ";
				labels += vmuSlotLabel(bus, slot);
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

						const std::string label = vmuSlotLabel(bus, slot);
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
							const std::string targetName = currentSlotVmuFileName(bus, slot);
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

	ScopedTwoLineRowStyle controlsRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("Controls Configuration"));
	ImGui::Separator();

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
				DreamLinkGamepad* dreamLinkGamepad = dynamic_cast<DreamLinkGamepad*>(gamepad.get());
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
				if (ImGui::BeginCombo(port_name, current_port))
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
				if (gamepad->remappable() && ImGui::Button(T("Map"), deviceButtonSize))
				{
					ResetControllerMappingRuntime();
					g_currentGamepadForMapping = gamepad;
					g_currentGamepadForMapping->listenButtons(buttonListener);
					g_gamepad_port_for_mapping = 0;
					ImGui::OpenPopup(T("Controller Mapping"));
				}

				// Virtual controllers open the live editor; physical controllers keep
				// the rumble, deadzone, and saturation popup.
				if (gamepad->is_rumble_enabled() || gamepad->has_analog_stick() || gamepad->is_virtual_gamepad())
				{
					ImGui::SameLine(0, SettingsLayoutScaled(16.0f));
					if (ImGui::Button(T("Settings"), deviceButtonSize))
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
							ImGui::OpenPopup(T("Gamepad Settings"));
						}
					}
				}

				// Render the gamepad settings popup for this gamepad
				if (g_currentGamepadForSettings == gamepad)
				{
					gamepadSettingsPopup(gamepad);
					if (!ImGui::IsPopupOpen(T("Gamepad Settings")))
						g_currentGamepadForSettings.reset();
				}

				// Render controller mapping popup for the selected gamepad
				if (g_currentGamepadForMapping == gamepad)
				{
					controller_mapping_popup(gamepad);
					if (!ImGui::IsPopupOpen(T("Controller Mapping")))
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

void renderNetworkTab()
{
	ScopedTwoLineRowStyle networkRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("Network Configuration"));
	ImGui::Separator();

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

					// Peer Address
					RenderGeneralTextInputSettingRow(
						"GGPONetPeerRow",
						ICON_FA_NETWORK_WIRED,
						T("Peer"),
						T("Your peer IP address and optional port"),
						[]() {
							InputText("##PeerInput", &config::NetworkServer.get(),
								ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
								dnsCharFilter);
						},
						T(
							"GGPO Peer\n"
							"Enter your peer's IP address (and optional port).\n"
							"Use this when connecting directly without matchmaking."
						));

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

void renderAdvancedTab()
{
	ScopedTwoLineRowStyle advancedRowStyle(12.0f, true, 0.5f);

	ImGui::TextDisabled("%s", T("Advanced Configuration"));
	ImGui::Separator();

	// CPU & Emulation Section
	if (RenderCollapsingHeader("CPU&EmulationSection", ICON_FA_MICROCHIP, T("CPU & Emulation"), ImGuiTreeNodeFlags_DefaultOpen))
	{
#if FEAT_SHREC != DYNAREC_NONE
		// CPU Mode
		{
			const char* cpuModeLabels[] = { T("Dynarec"), T("Interpreter") };
			int cpuModeSelection = config::DynarecEnabled.get() ? 0 : 1;

			SettingsUI::PopupConfig cpuModeCfg {};
			cpuModeCfg.type = SettingsUI::PopupType::Options;
			cpuModeCfg.options.label = T("CPU Mode");
			cpuModeCfg.options.icon = ICON_FA_MICROCHIP;
			cpuModeCfg.options.popupID = Tnop("CpuModePopup");
			cpuModeCfg.options.options = cpuModeLabels;
			cpuModeCfg.options.optionCount = IM_ARRAYSIZE(cpuModeLabels);
			cpuModeCfg.options.currentValue = &cpuModeSelection;
			cpuModeCfg.options.valueWidth = 220.0f;
			cpuModeCfg.options.onOptionHighlight = [](int idx, const char*)
			{
				const char* kHelp[] = {
					T(
						"CPU Mode: Dynarec\n"
						"Recommended for almost everyone.\n"
						"Uses the dynamic recompiler (JIT) for the best performance.\n\n"
						"If a specific game is crashing or behaving incorrectly, switching to the Interpreter can be a useful compatibility test."
					),
					T(
						"CPU Mode: Interpreter\n"
						"Highest compatibility, lowest performance.\n"
						"Runs the SH4 in interpreter mode which is much slower than Dynarec.\n\n"
						"Use this mainly for troubleshooting hard-to-reproduce bugs, or for rare titles that misbehave under Dynarec."
					)
				};
				if (idx >= 0 && idx < (int)std::size(kHelp))
					SetSettingsFooterText(kHelp[idx]);
			};
				cpuModeCfg.options.onChange = [&](int selectedIndex) {
					config::DynarecEnabled.set(selectedIndex == 0);
					return true;
				};

			RenderGeneralPopupSettingRow(
				Tnop("CpuModeSetting"),
				T("Use the dynamic recompiler for speed, or interpreter for compatibility."),
				cpuModeCfg,
				T(
					"CPU Mode\n"
					"Controls how Hollycast emulates the SH4 CPU.\n\n"
					"Dynarec is dramatically faster and is the normal choice for gameplay.\n"
					"Interpreter is much slower, but can be helpful for compatibility testing.\n\n"
					"If you are troubleshooting a game-specific issue, switch modes and re-test the exact same scene to compare behavior."
				));
		}

		// SH4 Clock
		{
			static int sh4ClockTemp = config::Sh4Clock.get();

			SettingsUI::PopupSliderConfig sh4ClockCfg {};
			sh4ClockCfg.label = T("SH4 Clock");
			sh4ClockCfg.icon = ICON_FA_GAUGE_HIGH;
			sh4ClockCfg.popupID = Tnop("Sh4ClockPopup");
			sh4ClockCfg.description = T(
				"Adjusts the emulated SH4 CPU clock. Default is 200 MHz.\n"
				"Higher values may reduce CPU-side frame drops in some games, but can also break timing.\n"
				"Lower values can reduce CPU load, but may cause slowdowns or audio/video instability.");
			sh4ClockCfg.currentValue = &sh4ClockTemp;
			sh4ClockCfg.minValue = 100;
			sh4ClockCfg.maxValue = 300;
			sh4ClockCfg.defaultValue = 200;
			sh4ClockCfg.format = "%d MHz";
			sh4ClockCfg.valueWidth = 220.0f;
			sh4ClockCfg.sliderWidth = 320.0f;
			sh4ClockCfg.onValueChange = []() {
				config::Sh4Clock.set(sh4ClockTemp);
			};

			SettingsUI::PopupConfig sh4ClockPopupCfg {};
			sh4ClockPopupCfg.type = SettingsUI::PopupType::Slider;
			sh4ClockPopupCfg.slider = sh4ClockCfg;

			RenderGeneralPopupSettingRow(
				Tnop("Sh4ClockSetting"),
				T("Over/Underclock the main SH4 CPU."),
				sh4ClockPopupCfg,
				T(
					"SH4 Clock\n"
					"Overclocks/underclocks the emulated Dreamcast CPU.\n\n"
					"Default is 200 MHz.\n"
					"Increasing the clock can help some CPU-limited games maintain full speed, but it can also introduce timing problems, bugs, or instability.\n"
					"Decreasing the clock can lower CPU requirements, but may reduce performance or cause glitches.\n\n"
					"If you change this, re-test gameplay with the FPS counter enabled and keep adjustments small."
				));
		}

		// Fast-Forward Speed Limit
		{
			static int fastForwardLimitTemp = config::FastForwardSpeedLimit.get();

			SettingsUI::PopupSliderConfig fastForwardLimitCfg {};
			fastForwardLimitCfg.label = T("Fast-Forward Speed");
			fastForwardLimitCfg.icon = ICON_FA_FORWARD_FAST;
			fastForwardLimitCfg.popupID = Tnop("FastForwardSpeedLimitPopup");
			fastForwardLimitCfg.description = T(
				"Sets a cap for Fast-Forward speed.\n"
				"Values below Infinite apply frame pacing while Fast-Forward is active.\n"
				"Infinite preserves the current uncapped behavior."
			);
			fastForwardLimitCfg.currentValue = &fastForwardLimitTemp;
			fastForwardLimitCfg.minValue = 2;
			fastForwardLimitCfg.maxValue = 300;
			fastForwardLimitCfg.defaultValue = 300;
			fastForwardLimitCfg.format = "%d%%";
			fastForwardLimitCfg.valueWidth = 220.0f;
			fastForwardLimitCfg.sliderWidth = 320.0f;
			fastForwardLimitCfg.valueFormatter = [](int value) {
				if (value >= 300)
					return std::string("Infinite");
				char text[16];
				snprintf(text, sizeof(text), "%d%%", value);
				return std::string(text);
			};
			fastForwardLimitCfg.onValueChange = []() {
				config::FastForwardSpeedLimit.set(fastForwardLimitTemp);
			};

			SettingsUI::PopupConfig fastForwardLimitPopupCfg {};
			fastForwardLimitPopupCfg.type = SettingsUI::PopupType::Slider;
			fastForwardLimitPopupCfg.slider = fastForwardLimitCfg;

			RenderGeneralPopupSettingRow(
				Tnop("FastForwardSpeedLimitSetting"),
				T("Cap Fast-Forward speed or leave it Infinite."),
				fastForwardLimitPopupCfg,
				T(
					"Fast-Forward Speed\n"
					"Controls the Fast-Forward cap while Fast-Forward is active.\n\n"
					"Set this below Infinite if you want a controlled speed-up instead of running as fast as possible.\n"
					"Infinite keeps today's uncapped Fast-Forward behavior.\n\n"
					"Use moderate values first, then raise or lower based on how smooth gameplay and audio feel."
				)
			);
		}

		RenderGeneralToggleSettingRow(
			"FastForwardAudio",
			ICON_FA_VOLUME_HIGH,
			T("Fast-Forward Audio"),
			T("Play audio during Fast-Forward"),
			static_cast<bool>(config::FastForwardAudio),
			[](bool enabled) { config::FastForwardAudio.set(enabled); },
			T(
				"Fast-Forward Audio\n"
				"Controls whether audio plays while Fast-Forward is active.\n\n"
				"When enabled, Fast-Forward audio plays at 35% volume to keep it usable without being too harsh.\n"
				"When disabled, Fast-Forward keeps the current muted-audio behavior."
			)
		);
	#endif

		// HLE BIOS
		RenderGeneralToggleSettingRow(
			"HLEBIOS",
			ICON_FA_MICROCHIP,
			T("HLE BIOS"),
			T("High Level Emulation BIOS"),
			static_cast<bool>(config::UseReios),
			[](bool enabled) { config::UseReios.set(enabled); },
			T(
				"HLE BIOS\n"
				"Uses a High Level Emulation BIOS implementation instead of a real BIOS.\n\n"
				"This can reduce CPU overhead and simplifies setup, but it can be less accurate than using a real BIOS.\n"
				"If you encounter game-specific boot issues or unusual behavior, try disabling HLE BIOS (and using a real BIOS if available) for maximum compatibility."
			));

		// Multi-threaded emulation
		RenderGeneralToggleSettingRow(
			"MultiThreaded",
			ICON_FA_GEAR,
			T("Multi-threading"),
			T("Use additional CPU threads"),
			static_cast<bool>(config::ThreadedRendering),
			[](bool enabled) { config::ThreadedRendering.set(enabled); },
			T(
				"Multi-threading\n"
				"Allows Hollycast to use more than one CPU thread for certain emulation/rendering work.\n\n"
				"On modern multi-core devices this can improve performance or reduce stutter.\n"
				"On some drivers/devices it can introduce instability or odd timing issues.\n\n"
				"If you see new crashes, visual glitches, or inconsistent frame pacing after enabling it, try turning it off and re-testing."
			));

		ImGui::Spacing();

		// Dreamcast 32MB RAM Mod (with warning)
		RenderGeneralToggleSettingRow(
			"ExtraRAM",
			ICON_FA_SD_CARD,
			T("32MB RAM"),
			T("Extended memory (32MB)"),
			static_cast<bool>(config::RamMod32MB),
			[](bool enabled) { config::RamMod32MB.set(enabled); },
			T(
				"32MB RAM (Memory Mod)\n"
				"Enables an extended-memory mode (not standard Dreamcast hardware).\n\n"
				"Only use this if a specific title/homebrew explicitly requires it.\n"
				"Because it changes the memory map, it can break games, cause graphical corruption, or lead to crashes.\n\n"
				"If you are unsure, leave this disabled."
			),
			game_started);
	}

	// Debugging Section
	if (RenderCollapsingHeader("DebuggingSection", ICON_FA_BUG, T("Debugging"), ImGuiTreeNodeFlags_DefaultOpen))
	{
#if !defined(__ANDROID__) && !defined(GDB_SERVER)
		// Serial Console - 2x Row Pattern
		RenderGeneralToggleSettingRow(
			"SerialConsole",
			ICON_FA_TERMINAL,
			T("Serial Console"),
			T("Serial console output"),
			static_cast<bool>(config::SerialConsole),
			[](bool enabled) { config::SerialConsole.set(enabled); },
			T(
				"Serial Console\n"
				"Enables serial console output for debugging.\n\n"
				"Most users should leave this off.\n"
				"Enable it only if you are debugging, capturing logs, or using tools that rely on serial output."
			));
#endif

	#ifdef GDB_SERVER
		// GDB Server - 2x Row
		RenderGeneralToggleSettingRow(
			"GDBWait",
			ICON_FA_BUG,
			T("GDB Server"),
			T("GDB debug server"),
			static_cast<bool>(config::GDBWait),
			[](bool enabled) { config::GDBWait.set(enabled); },
			T(
				"GDB Server\n"
				"Starts a GDB debug server for developers.\n\n"
				"When enabled, Hollycast can pause startup and wait for a debugger to attach.\n"
				"Leave this disabled unless you are actively debugging."
			));

		// GDB Server options (shown when enabled)
		if (config::GDBWait.get())
		{
			ImGui::Indent();

			// Wait for connection - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"GDBWaitForConnection",
				ICON_FA_LINK,
				T("Wait for Connection"),
				T("Wait for debugger to connect"),
				static_cast<bool>(config::GDBWaitForConnection),
				[](bool enabled) { config::GDBWaitForConnection.set(enabled); },
				T(
					"Wait for Connection\n"
					"Delays emulation until the debugger connects to the GDB server.\n\n"
					"Useful when you need to catch very early boot issues.\n"
					"Disable this for normal play."
				));

			{
				ImGui::PushID("GDBPortRow");
				const bool rowActivated = BeginTwoLineSettingRow(
					"##row",
					T(
						"GDB Port\n"
						"Sets the TCP port used by the built-in GDB debug server.\n\n"
						"Default is 3263.\n"
						"Only change this if you have a port conflict or you need to match your debugging tool configuration."
					));
				const ImVec2 line1Start = BeginTwoLineSettingRowContent();

				SettingIcon(ICON_FA_LINK, SettingsLayoutSize(20.0f, 20.0f));
				ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
				ImGui::PushFont(largeFont);
				ImGui::TextUnformatted(T("GDB Port"));
				ImGui::PopFont();

				const float inputWidth = SettingsLayoutScaled(220.0f);
				ImGui::SameLine(RightColumnX(inputWidth));
				if (rowActivated)
					ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(inputWidth);
				static int gdbport = config::GDBPort;
				if (ImGui::InputInt("##GDBPortInput", &gdbport))
					config::GDBPort = gdbport;

				RenderTwoLineSettingDescription(line1Start, T("Default port is 3263"));
				EndTwoLineSettingRow();
				ImGui::PopID();
			}
			ImGui::Unindent();
		}
	#endif

		// Log to File - 2x Row Pattern
		// Android writes this log to its app-writable data directory.
		const bool logToFileValue = cfgLoadBool("log", "LogToFile", false);
		RenderGeneralToggleSettingRow(
			"LogToFile",
			ICON_FA_FILE,
			T("Log to File"),
			T("Save log output to file"),
			logToFileValue,
			[](bool enabled) { cfgSaveBool("log", "LogToFile", enabled); },
			T(
				"Log to File\n"
				"Saves log output to a file on disk.\n\n"
				"This is helpful when you need to share logs for troubleshooting, but it can increase I/O and may reduce performance on slower storage.\n"
				"If you enable this, try to reproduce the issue, then disable it again to avoid unnecessary disk usage."
			));
	}

	// Logging Section (Debug builds only)
#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER
	if (RenderCollapsingHeader("LoggingSection", ICON_FA_FILE_LINES, T("Logging"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		LogManager *logManager = LogManager::GetInstance();

		// Log Verbosity
		const char *levels[] = { T("Notice"), T("Error"), T("Warning"), T("Info"), T("Debug") };
		{
			int logVerbositySelection = logManager->GetLogLevel() - 1;
			if (logVerbositySelection < 0 || logVerbositySelection >= (int)std::size(levels))
				logVerbositySelection = 0;

			SettingsUI::PopupConfig logVerbosityCfg {};
			logVerbosityCfg.type = SettingsUI::PopupType::Options;
			logVerbosityCfg.options.label = T("Log Verbosity");
			logVerbosityCfg.options.icon = ICON_FA_SORT;
			logVerbosityCfg.options.popupID = Tnop("LogVerbosityPopup");
			logVerbosityCfg.options.options = levels;
			logVerbosityCfg.options.optionCount = (int)std::size(levels);
			logVerbosityCfg.options.currentValue = &logVerbositySelection;
			logVerbosityCfg.options.valueWidth = 220.0f;
			logVerbosityCfg.options.onOptionHighlight = [](int idx, const char*)
			{
				const char* kHelp[] = {
					T(
						"Log Verbosity: Notice\n"
						"Minimal information.\n"
						"Best for normal usage when you only want important notices."
					),
					T(
						"Log Verbosity: Error\n"
						"Only errors.\n"
						"Useful when you want to reduce log noise and focus on failures."
					),
					T(
						"Log Verbosity: Warning\n"
						"Errors and warnings.\n"
						"A good balance for general troubleshooting with moderate verbosity."
					),
					T(
						"Log Verbosity: Info\n"
						"Adds informational messages.\n"
						"Helpful when investigating subsystems without going full debug spam."
					),
					T(
						"Log Verbosity: Debug\n"
						"Most verbose.\n"
						"Can be very noisy and may impact performance; use temporarily when debugging."
					)
				};
				if (idx >= 0 && idx < (int)std::size(kHelp))
					SetSettingsFooterText(kHelp[idx]);
			};
			logVerbosityCfg.options.onChange = [&](int selectedIndex) {
				LogManager::GetInstance()->SetLogLevel((LogTypes::LOG_LEVELS)(selectedIndex + 1));
				cfgSaveInt("log", "Verbosity", selectedIndex + 1);
				return true;
			};

			RenderGeneralPopupSettingRow(
				Tnop("LogVerbositySetting"),
				T("Control the verbosity of log output"),
				logVerbosityCfg,
				T(
					"Log Verbosity\n"
					"Controls how much information Hollycast writes to the log.\n\n"
					"Lower verbosity is cleaner and faster.\n"
					"Higher verbosity is useful when diagnosing problems, but can generate a lot of output (and can impact performance in extreme cases).\n\n"
					"For troubleshooting, start with Warning or Info, then use Debug only if you need deeper detail."
				));
		}

		// Log Categories (collapsible with 2-column toggle grid)
		const std::string logCategoriesHeader = std::string(T("Log Categories")) + "###Log Categories";
		if (ImGui::CollapsingHeader(logCategoriesHeader.c_str()))
		{
			ImGui::Indent();
			int columnCount = 0;
			for (LogTypes::LOG_TYPE type = LogTypes::AICA; type < LogTypes::NUMBER_OF_LOGS; type = (LogTypes::LOG_TYPE)(type + 1))
			{
				if (columnCount % 2 == 0)
					ImGui::Columns(2, "LogCategories", false);

				bool enabled = logManager->IsEnabled(type, logManager->GetLogLevel());
				std::string shortName = logManager->GetShortName(type);
				std::string fullName = logManager->GetFullName(type);

				// Use 2-row toggle pattern for each category
				ImGui::PushID(shortName.c_str());
				float rowHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;

				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, SettingsLayoutScaled(8.0f));
				bool rowClicked = ImGui::Selectable(("##" + shortName + "_row").c_str(), false,
					ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
					ImVec2(0, rowHeight));
				ImGui::PopStyleVar();

				if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
					SetSettingsFooterText(fullName.c_str());

				if (rowClicked && logManager->GetLogLevel() > LogTypes::LWARNING)
				{
					enabled = !enabled;
					logManager->SetEnable(type, enabled);
					cfgSaveBool("log", shortName.c_str(), enabled);
				}

				ImGui::SameLine(0, 0);
				ImVec2 line1Start = ImGui::GetCursorPos();

				ImGui::PushFont(largeFont);
				ImGui::Text("%s", shortName.c_str());
				ImGui::PopFont();

				float toggleWidth = SettingsLayoutScaled(50.0f);
				ImGui::SameLine(RightColumnX(toggleWidth));

				float toggleHeight = SettingsLayoutScaled(24.0f);
				float verticalOffset = (rowHeight - toggleHeight) * 0.5f;
				ImVec2 cursorPos = ImGui::GetCursorPos();
				ImGui::SetCursorPos(ImVec2(cursorPos.x, cursorPos.y + verticalOffset));

				RenderToggleSwitchVisual(enabled);

				ImGui::SetCursorPos(ImVec2(line1Start.x + SettingsLayoutScaled(28.0f), line1Start.y + ImGui::GetTextLineHeightWithSpacing()));
				ImGui::TextDisabled("%s", fullName.c_str());

				ImGui::PopID();

				columnCount++;
				if (columnCount % 2 == 0)
				{
					ImGui::Columns(1);
					ImGui::Spacing();
				}
			}
			if (columnCount % 2 != 0)
				ImGui::Columns(1);
			ImGui::Unindent();
		}

		// Log Server (network logging)
		SettingIcon(ICON_FA_SERVER, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::Text("%s", T("Log Server"));
		ImGui::PopFont();
		ImGui::SameLine();
			InputText("", &config::LogServer.get(),
				ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
				dnsCharFilter);
		ImGui::SameLine();
		ShowFooterHelpMarker(T("Log to this hostname[:port] with UDP. Default port is 31667."));
	}
#endif

	// Profiling Section
#if FC_PROFILER
	ImGui::Spacing();
	if (RenderCollapsingHeader("ProfilingSection", ICON_FA_CHART_LINE, T("Profiling"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"ProfilerEnabled",
			ICON_FA_CHART_LINE,
			T("Enable Profiler"),
			T("Performance profiling"),
			static_cast<bool>(config::ProfilerEnabled),
			[](bool enabled) { config::ProfilerEnabled.set(enabled); },
			T(
				"Profiler\n"
				"Enables performance profiling tools for developers and power users.\n\n"
				"This can add overhead. Leave it off unless you are actively profiling performance."
			));
		if (config::ProfilerEnabled)
		{
			ImGui::Indent();

			// Display in GUI - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"ProfilerDrawToGUI",
				ICON_FA_DESKTOP,
				T("Display in GUI"),
				T("Show profiler overlay"),
				static_cast<bool>(config::ProfilerDrawToGUI),
				[](bool enabled) { config::ProfilerDrawToGUI.set(enabled); },
				T(
					"Profiler: Display in GUI\n"
					"Draws profiler output as an in-game overlay.\n\n"
					"Useful for quickly spotting spikes or expensive subsystems while reproducing a performance issue."
				));

			// Output to terminal - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"ProfilerOutputTTY",
				ICON_FA_TERMINAL,
				T("Output to Terminal"),
				T("Write profiler to terminal"),
				static_cast<bool>(config::ProfilerOutputTTY),
				[](bool enabled) { config::ProfilerOutputTTY.set(enabled); },
				T(
					"Profiler: Output to Terminal\n"
					"Writes profiler output to the terminal/console.\n\n"
					"Useful when capturing data during automated runs or when you want to copy/paste profiling output."
				));

			ImGui::Unindent();
		}
	}
#endif

	// Experimental Features Section (with warnings)
	ImGui::Spacing();
	const std::string experimentalFeaturesHeader = std::string(T("Experimental Features")) + "##Section";
	if (ImGui::CollapsingHeader(experimentalFeaturesHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Texture Dumping (with warning)
		ImVec4 warningColor = CustomTheme::roleColor("warning", ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered], "settings.feedback");
		ImGui::TextColored(warningColor, "%s", T("(!) Texture Dumping"));
		ImGui::Indent();
		RenderGeneralToggleSettingRow(
			"DumpTextures",
			ICON_FA_DOWNLOAD,
			T("Texture Dumping"),
			T("Dump textures to files"),
			static_cast<bool>(config::DumpTextures),
			[](bool enabled) { config::DumpTextures.set(enabled); },
			T(
				"Texture Dumping\n"
				"Saves textures to disk as they are used.\n\n"
				"This is mainly for creating or debugging custom texture packs.\n"
				"It can create a large number of files quickly and may reduce performance due to disk I/O.\n\n"
				"Enable it only when you specifically need a dump, then disable it again."
			));
		if (config::DumpTextures)
		{
			ImGui::Indent();

			// Dump Replaced Textures - 2x Row Pattern
			RenderGeneralToggleSettingRow(
				"DumpReplacedTextures",
				ICON_FA_COPY,
				T("Dump Replaced"),
				T("Dump replaced textures too"),
				static_cast<bool>(config::DumpReplacedTextures),
				[](bool enabled) { config::DumpReplacedTextures.set(enabled); },
				T(
					"Dump Replaced Textures\n"
					"Also dumps textures even when they are being replaced by a custom texture pack.\n\n"
					"This is useful when comparing originals vs replacements, but it increases disk usage even more.\n"
					"Leave this off unless you are actively working on textures."
				)
			);

			RenderGeneralToggleSettingRow(
				"DiscardVideoAndAnimatedTextures",
				ICON_FA_COPY,
				T("Discard Video and Animated Textures"),
				T("Skip dumping video (YUV) and already updated textures"),
				static_cast<bool>(config::DumpUniqueTextures),
				[](bool enabled) { config::DumpUniqueTextures.set(enabled); },
				T("Skip dumping video (YUV) and already updated textures")
			);

			ImGui::Unindent();
		}
		ImGui::Unindent();
	}

	// Developer Tools Section
	ImGui::Spacing();
	const std::string developerToolsHeader = std::string(T("Developer Tools")) + "##Section";
	if (ImGui::CollapsingHeader(developerToolsHeader.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
	{
#ifdef USE_LUA
		// Lua Scripting
		ImGui::Text("%s", T("Lua Scripting"));
		const std::string luaFilenameLabel = std::string(T("Lua Filename")) + "###Lua Filename";
		InputText(luaFilenameLabel.c_str(), &config::LuaFileName.get(), ImGuiInputTextFlags_CharsNoBlank);
		ImGui::SameLine();
		ShowFooterHelpMarker(T("Specify lua filename to use. Should be located in Hollycast config folder. Defaults to flycast.lua when empty."));
#endif

#ifdef SENTRY_UPLOAD
		// Crash Reporting - 2x Row Pattern
		RenderGeneralToggleSettingRow(
			"CrashReporting",
			ICON_FA_STETHOSCOPE,
			T("Crash Reports"),
			T("Auto-upload crash reports"),
			static_cast<bool>(config::UploadCrashLogs),
			[](bool enabled) { config::UploadCrashLogs.set(enabled); },
			T(
				"Crash Reports\n"
				"Automatically uploads crash reports to help developers diagnose and fix issues.\n\n"
				"Crash reports typically include technical details about the crash and environment.\n"
				"Leave this enabled if you want to help improve stability; disable it if you prefer not to send crash data."
			));
	#endif

#if defined(GDB_SERVER)
		// Virtual Memory Addresses (debug builds)
		if (config::GDB)
		{
			ImGui::Spacing();
			ImGui::Text("%s", T("Virtual Memory Addresses"));
			ImGui::Separator();
			void *ram_base, *ram, *vram, *aram;
			addrspace::getAddress(&ram_base, &ram, &vram, &aram);

			ImGui::Text(T("Base Address: %p"), ram_base);

			if (ram == nullptr) {
				ImVec4 gray = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
				ImGui::TextColored(gray, "%s", T("RAM addresses are not available until the emulation is started"));
			} else {
				ImGui::Columns(3, "virtualMemoryAddress", false);
				ImGui::Text(T("RAM: %p"), ram);
				ImGui::NextColumn();
				ImGui::Text(T("VRAM64: %p"), vram);
				ImGui::NextColumn();
				ImGui::Text(T("ARAM: %p"), aram);
				ImGui::Columns(1, nullptr, false);
			}
		}
#endif
	}
}

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
	ImGui::TextDisabled("%s", T("About Hollycast"));
	ImGui::Separator();

	// Center content for better appearance
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

	ImGui::Spacing();
	RenderHollycastAsciiText();

	ImGui::Spacing();
	ImGui::Spacing();

	// Version Information
	if (RenderCollapsingHeader("VersionInformationSection", ICON_FA_TAG, T("Version Information"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text(T("Version: %s"), GIT_VERSION);
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
		extern const char *getIosJitStatus();
		ImGui::Text(T("JIT Status: %s"), getIosJitStatus());
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
		if (ImGui::TreeNode("Noto Sans CJK (SIL Open Font License)"))
		{
			size_t licenseSize = 0;
			auto license = resource::load("fonts/NotoSansCJK-LICENSE.txt", licenseSize);
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

// Main entry point - renders the entire settings UI
void renderSettingsNew()
{
	if (!g_controllerPopupWasOpen && !IsAnySettingsPopupOpen()
			&& g_controllerSection != ControllerSection::None)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
		{
			ImGui::ClearActiveID();
			ImGui::NavMoveRequestCancel();
			if (g_tabSlideStart >= 0.0)
				g_state.currentTab = g_tabSlideTarget;
			g_tabSlideStart = -1.0;
			g_focusSettingsContent = false;
			g_focusSettingsNavigation = g_controllerSection == ControllerSection::Content;
			g_focusSettingsBack = !g_focusSettingsNavigation;
		}
		// The navigation rail is for choosing a specific tab with A. Left and
		// right only change tabs after entering that tab's content pane.
		else if (!g_controllerEditing && g_controllerSection == ControllerSection::Content)
		{
			const bool left = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)
					|| ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true);
			const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)
					|| ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true);
			if (left != right)
			{
				ImGui::NavMoveRequestCancel();
				if (g_tabSlideStart >= 0.0)
					g_state.currentTab = g_tabSlideTarget;
				g_tabSlideDirection = right ? 1 : -1;
				g_tabSlideTarget = static_cast<SettingsTab>((static_cast<int>(g_state.currentTab)
						+ g_tabSlideDirection + static_cast<int>(SettingsTab::Count)) % static_cast<int>(SettingsTab::Count));
				g_tabSlideStart = ImGui::GetTime();
				g_tabSlideFocusContent = g_controllerSection == ControllerSection::Content;
				g_focusSettingsContent = false;
			}
		}
	}
	if (g_tabSlideStart >= 0.0)
	{
		const double elapsed = ImGui::GetTime() - g_tabSlideStart;
		if (elapsed >= kTabSlideSeconds * 0.5 && g_state.currentTab != g_tabSlideTarget)
		{
			g_state.currentTab = g_tabSlideTarget;
			if (g_contentWindow != nullptr)
				ImGui::SetScrollY(g_contentWindow, 0.0f);
			ResetSettingsFooter();
		}
		if (elapsed >= kTabSlideSeconds)
		{
			g_tabSlideStart = -1.0;
			g_focusSettingsContent = g_tabSlideFocusContent;
			g_focusSettingsNavigation = !g_focusSettingsContent;
		}
	}
	if (g_settingsFooterText.empty())
		ResetSettingsFooter();

#if defined(__ANDROID__)
	// Keep the Settings page internally consistent with its bounded scale without
	// changing the global style used by the library, menus, or virtual controls.
	const ImGuiStyle settingsStyleBackup = ImGui::GetStyle();
	ImGui::GetStyle().ScaleAllSizes(SettingsTextPreviewScale());
#endif

	// Set up full-screen window
	fullScreenWindow(false);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

	// Main settings window
	const std::string settingsWindowTitle = std::string(T("Settings")) + "###Settings";
	if (ImGui::Begin(settingsWindowTitle.c_str(), nullptr, ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		const bool pushedSettingsFont = settingsTitleFont != nullptr;
		if (pushedSettingsFont)
			ImGui::PushFont(settingsTitleFont);

		const std::function<void()> exitSettings = [&]() {
			applyPendingSettings();

			if (game_started)
				gui_setState(GuiState::Commands);
			else
				gui_setState(GuiState::Main);
		};

		const char* backLabel = game_started ? T("Back to Game") : T("Library");

		// Split into left navigation rail and right content area
		renderNavigationRail(exitSettings, backLabel);
		ImGui::SameLine();
		renderContentArea();

		if (pushedSettingsFont)
			ImGui::PopFont();
	}
	ImGui::End();
	ImGui::PopStyleVar();

#if defined(__ANDROID__)
	ImGui::GetStyle() = settingsStyleBackup;
#endif
	if (g_applyThemeAfterSettings) {
		g_applyThemeAfterSettings = false;
		applyCurrentTheme();
		if (g_reloadThemeFontsAfterSettings) {
			g_reloadThemeFontsAfterSettings = false;
			gui_requestStyleReload();
		}
	}
}

} // namespace SettingsNew
