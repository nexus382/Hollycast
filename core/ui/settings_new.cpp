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

namespace SettingsNew {

static bool g_applyThemeAfterSettings = false;
static bool g_reloadThemeFontsAfterSettings = false;

#ifdef HOLLYCAST_TEST_HARNESS
void annotateTestToggle(const char* prefix, const char* id, const char* label, bool value)
{
	if (!test_harness::ui::collectingInspection())
		return;
	const auto itemId = test_harness::ui::lastItemId();
	if (itemId != 0)
		test_harness::ui::annotateItem(itemId, {{"semantic_id", std::string(prefix) + "." + id},
			{"semantic_label", label ? label : ""}, {"kind", "toggle"}, {"value", value}});
}

void annotateTestFloat(const char* prefix, const char* id, const char* label,
		float value, float minimum, float maximum)
{
	if (!test_harness::ui::collectingInspection())
		return;
	const auto itemId = test_harness::ui::lastItemId();
	if (itemId != 0)
		test_harness::ui::annotateItem(itemId, {{"semantic_id", std::string(prefix) + "." + id},
			{"semantic_label", label ? label : ""}, {"kind", "slider"}, {"value_type", "float32"}, {"value", value},
			{"range", {{"min", minimum}, {"max", maximum}}}});
}

void annotateTestInteger(const char* prefix, const char* id, const char* label,
		int value, int minimum, int maximum)
{
	if (!test_harness::ui::collectingInspection())
		return;
	const auto itemId = test_harness::ui::lastItemId();
	if (itemId != 0)
		test_harness::ui::annotateItem(itemId, {{"semantic_id", std::string(prefix) + "." + id},
			{"semantic_label", label ? label : ""}, {"kind", "slider"}, {"value", value},
			{"range", {{"min", minimum}, {"max", maximum}}}});
}

void annotateTestVector2(const char* prefix, const char* id, const char* label,
		const char* widgetLabel, const ImVec2& value, float minimum, float maximum)
{
	if (!test_harness::ui::collectingInspection())
		return;
	const ImGuiID groupId = ImGui::GetID(widgetLabel);
	const std::string semanticId = std::string(prefix) + "." + id;
	for (int component = 0; component < 2; ++component)
	{
		const ImGuiID componentId = ImGui::GetIDWithSeed(component, groupId);
		const ImGuiID itemId = ImGui::GetIDWithSeed("", nullptr, componentId);
		const char axis = component == 0 ? 'X' : 'Y';
		const std::string componentLabel = std::string(label ? label : "") + " " + axis;
		test_harness::ui::annotateItem(itemId, {{"semantic_id", semanticId + (component == 0 ? ".x" : ".y")},
			{"semantic_label", componentLabel}, {"kind", "slider"}, {"value_type", "float32"}, {"value", component == 0 ? value.x : value.y},
			{"range", {{"min", minimum}, {"max", maximum}}}});
	}
}

void annotateTestColor(const char* prefix, const char* id, const char* label,
		const char* widgetLabel, const float value[4])
{
	if (!test_harness::ui::collectingInspection())
		return;
	const ImGuiID colorEditId = ImGui::GetID(widgetLabel);
	const ImGuiID colorButtonId = ImGui::GetIDWithSeed("##ColorButton", nullptr, colorEditId);
	test_harness::ui::annotateItem(colorButtonId, {{"semantic_id", std::string(prefix) + "." + id},
		{"semantic_label", label ? label : ""}, {"kind", "color"},
		{"value", {value[0], value[1], value[2], value[3]}}});
}

void annotateTestText(const char* prefix, const char* id, const char* label)
{
	if (!test_harness::ui::collectingInspection())
		return;
	const auto itemId = test_harness::ui::lastItemId();
	if (itemId != 0)
		test_harness::ui::annotateItem(itemId, {{"semantic_id", std::string(prefix) + "." + id},
			{"semantic_label", label ? label : ""}, {"kind", "text"}});
}
#endif

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

ImVec4 SettingsErrorColor()
{
	const ImVec4 accent = ImGui::GetStyle().Colors[ImGuiCol_ButtonActive];
	const ImVec4 error(0.88f, 0.26f, 0.24f, 1.0f);
	constexpr float errorWeight = 0.62f;
	const ImVec4 fallback(accent.x + (error.x - accent.x) * errorWeight,
			accent.y + (error.y - accent.y) * errorWeight,
			accent.z + (error.z - accent.z) * errorWeight, 1.0f);
	return CustomTheme::roleColor("error", fallback, "settings.feedback");
}

static ImVec4 SettingsAccentColor()
{
	return CustomTheme::roleColor("accent", ImVec4(0.93f, 0.51f, 0.39f, 1.0f), "settings.accent");
}

static ImVec4 SettingsSuccessColor()
{
	return CustomTheme::roleColor("success", ImVec4(0.36f, 0.77f, 0.46f, 1.0f), "settings.feedback");
}

static ImVec4 SettingsWarningColor()
{
	return CustomTheme::roleColor("warning", ImVec4(0.93f, 0.64f, 0.36f, 1.0f), "settings.feedback");
}

std::string cfgLoadStr(const std::string& section, const std::string& key, const std::string& def)
{
	return config::loadStr(section, key, def);
}

void cfgSaveStr(const std::string& section, const std::string& key, const std::string& value)
{
	config::saveStr(section, key, value);
}

bool cfgLoadBool(const std::string& section, const std::string& key, bool def)
{
	return config::loadBool(section, key, def);
}

void cfgSaveBool(const std::string& section, const std::string& key, bool value)
{
	config::saveBool(section, key, value);
}

void cfgSaveInt(const std::string& section, const std::string& key, int value)
{
	config::saveInt(section, key, value);
}

int cfgLoadInt(const std::string& section, const std::string& key, int def)
{
	return config::loadInt(section, key, def);
}


#if defined(USE_DREAMLINK_DEVICES)
static void reconnectDreamLinks()
{
	// DreamLink devices are tied into maple reconnect flow; keep this hook so
	// settings exit can request DreamLink-specific reconnection behavior later.
}
#endif

// Static state for gamepad settings popup
float g_twoLineRowExtraHeightPx = 0.0f;
bool g_twoLineRowSeparatorsEnabled = false;
float g_twoLineRowSeparatorAlpha = 0.5f;
static float g_twoLineRowStartScreenX = 0.0f;
static float g_twoLineRowEndScreenX = 0.0f;
static float g_twoLineRowEndScreenY = 0.0f;
bool g_mapleDevicesChangedInSettings = false;
static bool g_scrollToBoxArtSection = false;
static bool g_resetSettingsContentScroll = false;
static bool g_focusSettingsNavigation = false;
static bool g_focusSettingsContent = false;
static bool g_focusSettingsBack = false;
static bool g_controllerPopupWasOpen = false;
int g_controllerPopupDepth = 0;
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
static constexpr float kSettingsFooterHeightPx = 42.0f;

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

float SettingsPreviewFontSize(ImFont* font)
{
	font = font != nullptr ? font : ImGui::GetFont();
	return font->LegacySize * SettingsTextPreviewScale();
}

float SettingsLayoutScaled(float px)
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

ImVec2 SettingsLayoutSize(float width, float height)
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
	return std::clamp(SettingsFixedFooterScaled(kSettingsFooterHeightPx), minimumHeight, maximumHeight);
#else
	return SettingsFixedFooterScaled(kSettingsFooterHeightPx);
#endif
}

static float SettingsFooterGap()
{
	return UseCompactSettingsLayout() ? SettingsLayoutScaled(1.0f) : SettingsLayoutScaled(2.0f);
}

// Visual-only toggle switch rendering (no interaction)
// Renders the toggle switch appearance based on the boolean value
// The parent Selectable handles all user input
void RenderToggleSwitchVisual(bool value)
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

float TwoLineSettingContentHeight()
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

float RightColumnX(float itemWidth, float rightPadding)
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

bool SettingsIconButton(const char* id, const char* icon, const ImVec2& size)
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

static void SelectSettingsTab(SettingsTab tab)
{
	if (g_state.currentTab != tab)
	{
		g_state.currentTab = tab;
		g_resetSettingsContentScroll = true;
	}
	ResetSettingsFooter();
}

void SetSettingsFooterText(const char* text)
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
	SettingsUI::PrepareSliderPopupNavigation();
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

static void ScrollSettingsFromController()
{
	ImGuiContext& g = *GImGui;
	ImGuiWindow* focused = g.NavWindow;
	if (focused == nullptr || focused->ScrollMax.y > 0.f || g.NavWindowingTarget != nullptr
			|| !(g.IO.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) || !(g.IO.BackendFlags & ImGuiBackendFlags_HasGamepad))
		return;
	const float vertical = ImGui::GetKeyData(ImGuiKey_GamepadLStickDown)->AnalogValue
			- ImGui::GetKeyData(ImGuiKey_GamepadLStickUp)->AnalogValue;
	if (vertical == 0.f)
		return;

	// ImGui scrolls the focused window itself. A non-scrolling card inside
	// a pane should use the same left-stick motion on its nearest scrollable parent.
	for (ImGuiWindow* window = focused; (window->Flags & ImGuiWindowFlags_ChildWindow)
			&& !(window->Flags & ImGuiWindowFlags_Popup);)
	{
		window = window->ParentWindow;
		if (window->ScrollMax.y > 0.f && !(window->Flags & ImGuiWindowFlags_NoNavInputs))
		{
			const float speed = std::round(focused->FontRefSize * 100.f * g.IO.DeltaTime);
			ImGui::SetScrollY(window, std::trunc(window->Scroll.y + vertical * speed));
			break;
		}
	}
}

bool ControllerPopupCancelPressed()
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

void ShowFooterHelpMarker(const char* desc)
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

static void ResetGeneralPageSettings()
{
	// Every preference on the General page returns to its declared default.
	// Content folders are user data, not a preference, so they are kept.
	config::UILanguage = "";
	config::UILanguage.save();
	config::Language = 1;	// English
	config::Region = 1;		// USA
	config::Broadcast = 0;	// NTSC
	config::Cable = 3;		// TV Composite
	config::UITheme = 0;
	config::UICustomTheme = "";
	config::MenuBarVisibility = static_cast<int>(config::MenuBarMode::Auto);
	config::UIScaling = 100;
	config::HideLegacyNaomiRoms = true;
#if defined(__ANDROID__) && !defined(LIBRETRO)
	config::UseSafFilePicker = true;
#endif
	SaveSettings();
	i18n::reloadLanguage();
	mainui_reinit();
	gui_refresh_files();
	g_applyThemeAfterSettings = true;
}

static void RenderSettingsFooterBar()
{
	const float footerHeight = SettingsFooterHeight();
	ImGui::BeginChild("SettingsFooterBar", ImVec2(0.0f, footerHeight), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImVec2 footerMin = ImGui::GetWindowPos();
	const float footerWidth = ImGui::GetWindowSize().x;
	const float padX = SettingsLayoutScaled(14.0f);
	const bool isGeneral = g_state.currentTab == SettingsTab::General;
	ImVec4 border = ImGui::GetStyleColorVec4(ImGuiCol_Border);
	drawList->AddLine(footerMin, ImVec2(footerMin.x + footerWidth, footerMin.y),
		ImGui::ColorConvertFloat4ToU32(border), 1.0f);

	// The General page can be returned to its defaults from here.
	float resetWidth = 0.0f;
	const float resetHeight = footerHeight - SettingsLayoutScaled(10.0f);
	if (isGeneral)
	{
		ImFont* textFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
		ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : textFont;
		const float fontSize = ImGui::GetFontSize();
		resetWidth = textFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, T("Reset this page")).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ROTATE_LEFT).x
			+ SettingsLayoutScaled(26.0f);
		ImGui::SetCursorPos(ImVec2(footerWidth - padX - resetWidth, (footerHeight - resetHeight) * 0.5f));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
		const bool resetPressed = ImGui::Button("##ResetThisPage", ImVec2(resetWidth, resetHeight));
		ImGui::PopStyleColor(3);
		const ImVec2 btnMin = ImGui::GetItemRectMin();
		const ImVec2 btnMax = ImGui::GetItemRectMax();
		const bool highlighted = ImGui::IsItemHovered() || ImGui::IsItemFocused();
		const ImU32 color = ImGui::GetColorU32(highlighted ? ImGuiCol_Text : ImGuiCol_TextDisabled);
		const ImVec4 clipRect(btnMin.x, btnMin.y, btnMax.x, btnMax.y);
		float x = btnMin.x + SettingsLayoutScaled(8.0f);
		const float y = btnMin.y + (btnMax.y - btnMin.y - ImGui::GetTextLineHeight()) * 0.5f;
		drawList->AddText(iconFont, fontSize, ImVec2(x, y), color, ICON_FA_ROTATE_LEFT, nullptr, 0.0f, &clipRect);
		x += iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ROTATE_LEFT).x + SettingsLayoutScaled(8.0f);
		drawList->AddText(textFont, fontSize, ImVec2(x, y), color, T("Reset this page"), nullptr, 0.0f, &clipRect);
		if (resetPressed)
			ResetGeneralPageSettings();
	}

	// Autosave note on the left.
	const float textY = (footerHeight - ImGui::GetTextLineHeight()) * 0.5f;
	ImGui::SetCursorPos(ImVec2(padX, textY));
	if (settingsIconFont != nullptr)
	{
		ImGui::PushFont(settingsIconFont);
		ImGui::PushStyleColor(ImGuiCol_Text, SettingsSuccessColor());
		ImGui::TextUnformatted(ICON_FA_CIRCLE_CHECK);
		ImGui::PopStyleColor();
		ImGui::PopFont();
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
	}
	ImGui::TextUnformatted(T("Changes save automatically"));

	// Context help: the first line of the current setting's explanation.
	std::string helpTitle = g_settingsFooterText;
	if (const size_t newline = helpTitle.find('\n'); newline != std::string::npos)
		helpTitle.resize(newline);
	if (!helpTitle.empty())
	{
		ImFont* font = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
		const float fontSize = SettingsPreviewFontSize(font);
		const float autosaveWidth = (settingsIconFont != nullptr
				? settingsIconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_CIRCLE_CHECK).x + SettingsLayoutScaled(8.0f)
				: 0.0f)
			+ font->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, T("Changes save automatically")).x;
		const float helpX = footerMin.x + padX + autosaveWidth + SettingsLayoutScaled(28.0f);
		const float helpMaxX = footerMin.x + footerWidth - padX
			- (isGeneral ? resetWidth + SettingsLayoutScaled(16.0f) : 0.0f);
		if (helpMaxX > helpX)
		{
			drawList->PushClipRect(ImVec2(helpX, footerMin.y), ImVec2(helpMaxX, footerMin.y + footerHeight), true);
			AddWeightedFooterText(drawList, font, fontSize,
				ImVec2(helpX, footerMin.y + textY), ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				helpTitle.c_str(), 0.0f);
			drawList->PopClipRect();
		}
	}

	ImGui::EndChild();
}

ImVec2 BeginTwoLineSettingRowContent()
{
	ImGui::SameLine(0, 0);
	ImVec2 line1Start = ImGui::GetCursorPos();
	line1Start.y += TwoLineSettingContentOffsetY();
	ImGui::SetCursorPos(line1Start);
	return line1Start;
}

bool BeginTwoLineSettingRow(const char* rowId, const char* tooltip, bool disabled)
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

void EndTwoLineSettingRow()
{
	ImGui::SetCursorScreenPos(ImVec2(g_twoLineRowStartScreenX, g_twoLineRowEndScreenY));
	ImguiStyleVar rowGapSpacing(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 0.0f));
	ImGui::Dummy(ImVec2(0.0f, TwoLineSettingRowGap()));
}

void RenderTwoLineSettingDescription(const ImVec2& line1Start, const char* description)
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

bool RenderGeneralPopupSettingRow(const char* id, const char* description,
	SettingsUI::PopupConfig& popupCfg, const char* tooltip, bool disabled,
	bool infoMarker)
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
#ifdef HOLLYCAST_TEST_HARNESS
	const std::uint32_t rowItemId = test_harness::ui::collectingInspection() ? test_harness::ui::lastItemId() : 0;
#endif
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
#ifdef HOLLYCAST_TEST_HARNESS
	if (rowItemId != 0)
	{
		using test_harness::Json;
		Json metadata = {{"semantic_id", id ? id : ""}, {"semantic_label", popupCfg.type == SettingsUI::PopupType::Options
			? (popupCfg.options.label ? popupCfg.options.label : "")
			: (popupCfg.slider.label ? popupCfg.slider.label : "")}};
		if (popupCfg.type == SettingsUI::PopupType::Options && popupCfg.options.currentValue != nullptr)
		{
			const auto& options = popupCfg.options;
			const int currentValue = *options.currentValue;
			metadata["kind"] = options.multiSelect ? "multi_select" : "options";
			metadata["value"] = currentValue;
			metadata["requires_apply"] = options.multiSelect;
			Json entries = Json::array();
			const int optionCount = std::min(options.optionCount, 128);
			for (int i = 0; i < optionCount && options.options != nullptr; ++i)
			{
				const int storageValue = options.storageIndexMap ? options.storageIndexMap(i) : i;
				const bool selected = options.multiSelect
					? storageValue >= 0 && storageValue < 31 && (currentValue & (1 << storageValue)) != 0
					: currentValue == storageValue;
				entries.push_back({{"value", storageValue}, {"label", options.options[i] ? options.options[i] : ""},
					{"selected", selected}});
			}
			metadata["options"] = std::move(entries);
			if (options.valueToString)
			{
				const char* valueLabel = options.valueToString(currentValue);
				if (valueLabel != nullptr)
					metadata["value_label"] = valueLabel;
			}
			else if (!options.multiSelect && options.options != nullptr)
			{
				for (int i = 0; i < optionCount; ++i)
				{
					const int storageValue = options.storageIndexMap ? options.storageIndexMap(i) : i;
					if (storageValue == currentValue && options.options[i] != nullptr)
					{
						metadata["value_label"] = options.options[i];
						break;
					}
				}
			}
		}
		else if (popupCfg.type == SettingsUI::PopupType::Slider && popupCfg.slider.currentValue != nullptr)
		{
			const auto& slider = popupCfg.slider;
			const int currentValue = *slider.currentValue;
			metadata["kind"] = "slider";
			metadata["value"] = currentValue;
			metadata["range"] = {{"min", slider.minValue}, {"max", slider.maxValue}};
			metadata["requires_apply"] = slider.onApply != nullptr;
			if (slider.valueFormatter)
				metadata["value_label"] = slider.valueFormatter(currentValue);
			else
			{
				char valueLabel[128];
				std::snprintf(valueLabel, sizeof(valueLabel), slider.format ? slider.format : "%d", currentValue);
				metadata["value_label"] = valueLabel;
			}
		}
		test_harness::ui::annotateItem(rowItemId, metadata);
	}
#endif
	if (infoMarker && settingsIconFont != nullptr && largeFont != nullptr
			&& popupCfg.type == SettingsUI::PopupType::Options && popupCfg.options.label != nullptr)
	{
		// Small info glyph after the label; the row tooltip carries the text.
		const float fontSize = SettingsPreviewFontSize(largeFont);
		const float labelWidth = largeFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, popupCfg.options.label).x;
		const ImVec2 windowPos = ImGui::GetWindowPos();
		const float iconSize = ImGui::GetFontSize() * 0.85f;
		const ImVec2 iconPos(
			windowPos.x + line1Start.x + uiScaled(20.0f + 8.0f) + labelWidth + SettingsLayoutScaled(7.0f),
			windowPos.y + line1Start.y + (fontSize - iconSize) * 0.5f - SettingsLayoutScaled(7.0f));
		ImGui::GetWindowDrawList()->AddText(settingsIconFont, iconSize, iconPos,
			ImGui::GetColorU32(ImGuiCol_TextDisabled), ICON_FA_CIRCLE_INFO);
	}
	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
	ImGui::PopID();
	return rowActivated;
}





// Physical gamepad settings popup (ported from settings_controls.cpp)
// Handles rumble, deadzone, and saturation.
static void RenderGeneralRightValue(const std::string& value, float valueWidthPx, float extraRightOffsetPx = 0.0f, bool disabled = false)
{
	const float valueWidth = SettingsLayoutScaled(valueWidthPx);
	const float extraRight = SettingsLayoutScaled(extraRightOffsetPx);
	ImFont* valueFont = settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont;
	// Measure and center values with the same font used to draw them.
	ImGui::PushFont(valueFont);
	const float verticalOffset = (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f;
	const std::string clipped = middleEllipsis(value, valueWidth);

	ImGui::SameLine(RightColumnX(valueWidth + extraRight));
	const ImVec2 valuePos = ImGui::GetCursorPos();
	ImGui::SetCursorPos(ImVec2(valuePos.x, valuePos.y + verticalOffset));
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
#ifdef HOLLYCAST_TEST_HARNESS
	const std::uint32_t rowItemId = test_harness::ui::collectingInspection() ? test_harness::ui::lastItemId() : 0;
#endif
	const ImVec2 line1Start = BeginTwoLineSettingRowContent();

	SettingIcon(icon, SettingsLayoutSize(20.0f, 20.0f));
	ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
	ImGui::PushFont(largeFont);
	ImGui::TextUnformatted(label);
	ImGui::PopFont();
	RenderGeneralRightValue(value, 280.0f, 0.0f, disabledValue);
	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
#ifdef HOLLYCAST_TEST_HARNESS
	if (rowItemId != 0)
		test_harness::ui::annotateItem(rowItemId, {{"semantic_id", id ? id : ""},
			{"semantic_label", label ? label : ""}, {"kind", "info"}, {"value", value}});
#endif

	ImGui::PopID();
}

static void manageSinglePath(const char* label, config::Option<std::string, false>& pathOption, const char* helpText,
		bool selectFile = false, const std::string& extension = "")
{
	ImGui::PushID(label);
	const bool rowActivated = BeginTwoLineSettingRow("##row", helpText);
#ifdef HOLLYCAST_TEST_HARNESS
	const std::uint32_t rowItemId = test_harness::ui::collectingInspection() ? test_harness::ui::lastItemId() : 0;
#endif
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
		const bool clearPath = SettingsIconButton("##ClearPath", ICON_FA_TRASH_CAN, ImVec2(buttonWidth, buttonHeight));
#ifdef HOLLYCAST_TEST_HARNESS
		if (test_harness::ui::collectingInspection())
			test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", std::string(label) + ".clear"},
				{"semantic_label", std::string("Clear ") + label}, {"kind", "button"}});
#endif
		if (clearPath)
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
#ifdef HOLLYCAST_TEST_HARNESS
	if (rowItemId != 0)
		test_harness::ui::annotateItem(rowItemId, {{"semantic_id", label ? label : ""},
			{"semantic_label", label ? label : ""}, {"kind", "path"}, {"value", pathOption.get()}});
#endif
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
std::atomic<bool> g_textureFilterLiveTune{false};

void applyPendingSettings()
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








std::array<const char*, 19> getSampleTextureFilterLabels()
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

int getSampleTextureFilterOptionIndex(int value)
{
	for (size_t index = 0; index < sampleTextureFilterValues.size(); ++index)
		if (sampleTextureFilterValues[index] == value)
			return static_cast<int>(index);
	return 0;
}

std::array<const char*, 8> getMaxFilteredTextureSizeLabels()
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
	SelectSettingsTab(SettingsTab::General);
	g_focusSettingsNavigation = true;
	g_focusSettingsContent = g_focusSettingsBack = false;
	g_tabSlideStart = -1.0;
}

void openTab(SettingsTab tab)
{
	SelectSettingsTab(tab);
	g_tabSlideStart = -1.0;
	g_scrollToBoxArtSection = false;
}

void focusBoxArtSection()
{
	openTab(SettingsTab::Library);
	g_scrollToBoxArtSection = true;
}

// One descriptor per tab: rail label, rail icon, and page subtitle.
// Order must match the SettingsTab enum.
struct SettingsTabInfo
{
	const char* name;
	const char* icon;
	const char* subtitle;
};
static const SettingsTabInfo kSettingsTabs[] = {
	{ "General", ICON_FA_GEAR, "Make Hollycast feel right for you." },
	{ "Game library", ICON_FA_FOLDER, "Your games, artwork and library layout." },
	{ "Video", ICON_FA_DISPLAY, "Graphics quality, scaling and performance." },
	{ "Shader", ICON_FA_WAND_MAGIC, "Shader Configuration" },
	{ "On-screen display", ICON_FA_RECTANGLE_LIST, "Overlays, VMU screens and in-game information." },
	{ "Audio", ICON_FA_VOLUME_HIGH, "Sound output, mixing and latency." },
	{ "Controls", ICON_FA_GAMEPAD, "Controllers, mappings and input behavior." },
	{ "Network", ICON_FA_SHARE_NODES, "Online play, servers and link features." },
	{ "Advanced", ICON_FA_SLIDERS, "Power-user and developer options." },
	{ "About", ICON_FA_CIRCLE_INFO, "Version, credits and project information." },
};
static const SettingsTabInfo& GetTabInfo(SettingsTab tab)
{
	static const SettingsTabInfo unknown{ "Unknown", ICON_FA_GEAR, "" };
	const int index = static_cast<int>(tab);
	if (index < 0 || index >= static_cast<int>(std::size(kSettingsTabs)))
		return unknown;
	return kSettingsTabs[index];
}

// Get display name for each tab
const char* getTabName(SettingsTab tab)
{
	return T(GetTabInfo(tab).name);
}

static const char* SettingsTabIcon(SettingsTab tab);

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
	const float minWidth = uiScaled(208.0f);
	const float maxWidth = std::clamp(displaySize.x * 0.22f, minWidth, uiScaled(330.0f));
	const float layoutWidth = UseCompactSettingsLayout() ? displaySize.x * 0.16f : uiScaled(245.0f);

	ImFont* navFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : navFont;
	const float fontSize = navFont->LegacySize;
	float labelWidth = navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, exitLabel).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ARROW_LEFT).x
			+ uiScaled(8.0f);
	for (int i = 0; i < (int)SettingsTab::Count; i++)
	{
		const char* tabName = getTabName((SettingsTab)i);
		// Rows carry an icon and gap before the label.
		labelWidth = std::max(labelWidth, navFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, tabName).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, SettingsTabIcon((SettingsTab)i)).x
			+ uiScaled(14.0f));
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

static const char* SettingsTabIcon(SettingsTab tab)
{
	return GetTabInfo(tab).icon;
}

static const char* GetTabSubtitle(SettingsTab tab)
{
	return T(GetTabInfo(tab).subtitle);
}

// Shared card surface: bordered, auto-sized to its rows.
static bool BeginSettingsCardSurface(const char* id)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, SettingsLayoutScaled(10.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
		ImVec2(SettingsLayoutScaled(18.0f), SettingsLayoutScaled(14.0f)));
	return ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
		ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_NavFlattened);
}

static void EndSettingsCardSurface()
{
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
}

// Card with an icon, title, and optional subtitle header.
static bool BeginSettingsCard(const char* id, const char* icon, const char* title, const char* subtitle = nullptr)
{
	const bool open = BeginSettingsCardSurface(id);
	if (open)
	{
		const float headerX = ImGui::GetCursorPosX();
		float titleX = headerX;
		if (icon != nullptr && settingsIconFont != nullptr)
		{
			const float iconFontSize = SettingsPreviewFontSize(settingsIconFont) * 1.15f;
			const float iconWidth = settingsIconFont->CalcTextSizeA(iconFontSize, FLT_MAX, -1.0f, icon).x;
			ImGui::PushFont(settingsIconFont, iconFontSize);
			ImGui::TextUnformatted(icon);
			ImGui::PopFont();
			ImGui::SameLine(0, SettingsLayoutScaled(10.0f));
			titleX = headerX + iconWidth + SettingsLayoutScaled(10.0f);
		}
		if (largeFont != nullptr)
			ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(title);
		if (largeFont != nullptr)
			ImGui::PopFont();
		if (subtitle != nullptr && subtitle[0] != '\0')
		{
			ImGui::SetCursorPosX(titleX);
			ImGui::TextDisabled("%s", subtitle);
		}
		ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing();
	}
	return open;
}

static void EndSettingsCard()
{
	EndSettingsCardSurface();
	ImGui::Spacing();
}

// Small filled note shown at the bottom of a card.
static void RenderSettingsInfoBanner(const char* id, const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, SettingsLayoutScaled(8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
		ImVec2(SettingsLayoutScaled(12.0f), SettingsLayoutScaled(9.0f)));
	if (ImGui::BeginChild(id, ImVec2(0.0f, 0.0f),
			ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize))
	{
		if (settingsIconFont != nullptr)
		{
			ImGui::PushFont(settingsIconFont);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(ICON_FA_CIRCLE_INFO);
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::SameLine(0, SettingsLayoutScaled(10.0f));
		}
		ImGui::TextWrapped("%s", text);
	}
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

// Button whose icon and label are drawn centered over the button rect.
static bool SettingsLabelButton(const char* id, const char* icon, const char* label, const ImVec2& size,
	bool iconAfter = false)
{
	const bool pressed = ImGui::Button(id, size);
	const ImVec2 min = ImGui::GetItemRectMin();
	const ImVec2 max = ImGui::GetItemRectMax();
	ImFont* textFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
	ImFont* iconFont = settingsIconFont;
	const float fontSize = ImGui::GetFontSize();
	const ImVec2 textSize = textFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, label);
	const ImVec2 iconSize = iconFont != nullptr
		? iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, icon) : ImVec2();
	const float gap = iconFont != nullptr ? SettingsLayoutScaled(8.0f) : 0.0f;
	float x = min.x + (max.x - min.x - (iconSize.x + gap + textSize.x)) * 0.5f;
	const float y = min.y + (max.y - min.y - textSize.y) * 0.5f;
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
	const ImVec4 clipRect(min.x, min.y, max.x, max.y);
	if (iconFont != nullptr && !iconAfter)
	{
		drawList->AddText(iconFont, fontSize, ImVec2(x, y), color, icon, nullptr, 0.0f, &clipRect);
		x += iconSize.x + gap;
	}
	drawList->AddText(textFont, fontSize, ImVec2(x, y), color, label, nullptr, 0.0f, &clipRect);
	if (iconFont != nullptr && iconAfter)
		drawList->AddText(iconFont, fontSize, ImVec2(x + textSize.x + gap, y), color, icon, nullptr, 0.0f, &clipRect);
	return pressed;
}

static char g_settingsSearchBuffer[128] = "";
static bool g_focusSettingsSearch = false;

static std::string SettingsLowercase(const std::string& text)
{
	std::string lowered = text;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return lowered;
}

// Extra search terms per tab, indexed like kSettingsTabs.
static const char* kSettingsTabKeywords[] = {
	"language region broadcast cable interface theme content folder data home ui system tv",
	"library games boxart artwork cover metadata scan vmu folders scrape",
	"video graphics resolution render texture quality preset scaling fps display",
	"shader post processing effects filter layers preview parameters",
	"osd overlay vmu screen notification display performance connect phone wifi remote",
	"audio sound volume latency mixing output speakers",
	"controls controller gamepad mapping keyboard buttons sensitivity input",
	"network online server netplay match link dcnet",
	"advanced debug underclock developer experimental power rewind",
	"about version credits license links",
};

static bool SettingsTabMatchesQuery(SettingsTab tab, const std::string& loweredQuery)
{
	if (loweredQuery.empty())
		return false;
	if (SettingsLowercase(getTabName(tab)).find(loweredQuery) != std::string::npos)
		return true;
	const int index = static_cast<int>(tab);
	return index >= 0 && index < static_cast<int>(std::size(kSettingsTabKeywords))
		&& std::string(kSettingsTabKeywords[index]).find(loweredQuery) != std::string::npos;
}

static std::string NormalizedSettingsQuery(const char* rawQuery)
{
	std::string query = SettingsLowercase(rawQuery != nullptr ? rawQuery : "");
	const size_t first = query.find_first_not_of(" \t");
	if (first == std::string::npos)
		return {};
	return query.substr(first, query.find_last_not_of(" \t") - first + 1);
}

static SettingsTab FindSettingsTabForQuery(const char* rawQuery)
{
	const std::string query = NormalizedSettingsQuery(rawQuery);
	if (query.empty())
		return SettingsTab::Count;
	for (int i = 0; i < (int)SettingsTab::Count; i++)
		if (SettingsTabMatchesQuery((SettingsTab)i, query))
			return (SettingsTab)i;
	return SettingsTab::Count;
}

static void RenderSettingsContentHeader()
{
	const SettingsTab tab = g_state.currentTab;
	if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)
			&& ImGui::IsKeyPressed(ImGuiKey_K, false))
		g_focusSettingsSearch = true;

	const float startX = ImGui::GetCursorPosX();
	const float availWidth = ImGui::GetContentRegionAvail().x;
	const ImVec2 headerTop = ImGui::GetCursorPos();

	if (largeFont != nullptr)
		ImGui::PushFont(largeFont, SettingsLayoutScaled(30.0f));
	ImGui::TextUnformatted(getTabName(tab));
	if (largeFont != nullptr)
		ImGui::PopFont();
	const float titleBottomY = ImGui::GetCursorPosY();

	// Search box on the title row. Enter jumps to the matching page.
	const float searchWidth = std::clamp(SettingsLayoutScaled(320.0f),
		SettingsLayoutScaled(170.0f), availWidth * 0.45f);
	ImGui::SetCursorPos(ImVec2(startX + availWidth - searchWidth, headerTop.y + SettingsLayoutScaled(2.0f)));
	if (g_focusSettingsSearch)
	{
		ImGui::SetKeyboardFocusHere();
		g_focusSettingsSearch = false;
	}
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, SettingsLayoutScaled(8.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		ImVec2(SettingsLayoutScaled(32.0f), SettingsLayoutScaled(8.0f)));
	ImGui::SetNextItemWidth(searchWidth);
	if (ImGui::InputTextWithHint("##SettingsSearch", T("Search settings"), g_settingsSearchBuffer,
			sizeof(g_settingsSearchBuffer), ImGuiInputTextFlags_EnterReturnsTrue))
	{
		const SettingsTab match = FindSettingsTabForQuery(g_settingsSearchBuffer);
		if (match != SettingsTab::Count)
			openTab(match);
		else
			SetSettingsFooterText(T("No settings page matches that search."));
	}
	ImGui::PopStyleVar(2);
	{
		const ImVec2 boxMin = ImGui::GetItemRectMin();
		const ImVec2 boxMax = ImGui::GetItemRectMax();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 dimColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);
		if (settingsIconFont != nullptr)
		{
			const float iconSize = ImGui::GetFontSize();
			const ImVec2 glyph = settingsIconFont->CalcTextSizeA(iconSize, FLT_MAX, -1.0f, ICON_FA_MAGNIFYING_GLASS);
			drawList->AddText(settingsIconFont, iconSize,
				ImVec2(boxMin.x + SettingsLayoutScaled(10.0f), boxMin.y + (boxMax.y - boxMin.y - glyph.y) * 0.5f),
				dimColor, ICON_FA_MAGNIFYING_GLASS);
		}
#if defined(__APPLE__)
		const char* shortcutLabel = "Cmd K";
#else
		const char* shortcutLabel = "Ctrl K";
#endif
		ImFont* badgeFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
		const float badgeFontSize = ImGui::GetFontSize() * 0.85f;
		const ImVec2 badgeText = badgeFont->CalcTextSizeA(badgeFontSize, FLT_MAX, -1.0f, shortcutLabel);
		const ImVec2 badgeMin(boxMax.x - badgeText.x - SettingsLayoutScaled(18.0f),
			boxMin.y + SettingsLayoutScaled(5.0f));
		const ImVec2 badgeMax(boxMax.x - SettingsLayoutScaled(6.0f), boxMax.y - SettingsLayoutScaled(5.0f));
		drawList->AddRectFilled(badgeMin, badgeMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered),
			SettingsLayoutScaled(5.0f));
		drawList->AddText(badgeFont, badgeFontSize,
			ImVec2(badgeMin.x + SettingsLayoutScaled(6.0f), badgeMin.y + (badgeMax.y - badgeMin.y - badgeText.y) * 0.5f),
			dimColor, shortcutLabel);
		if (ImGui::IsItemHovered() || ImGui::IsItemFocused())
			SetSettingsFooterText(T("Search settings: type a setting name and press Enter to open its page."));
	}

	ImGui::SetCursorPos(ImVec2(startX, titleBottomY + SettingsLayoutScaled(4.0f)));
	ImGui::TextDisabled("%s", GetTabSubtitle(tab));
	ImGui::Spacing();

	// While typing, offer the matching pages directly; Enter opens the first.
	const std::string query = NormalizedSettingsQuery(g_settingsSearchBuffer);
	if (!query.empty())
	{
		bool anyMatch = false;
		for (int i = 0; i < (int)SettingsTab::Count; i++)
		{
			const SettingsTab candidate = (SettingsTab)i;
			if (!SettingsTabMatchesQuery(candidate, query))
				continue;
			if (anyMatch)
				ImGui::SameLine();
			anyMatch = true;
			if (ImGui::SmallButton(getTabName(candidate)))
			{
				g_settingsSearchBuffer[0] = '\0';
				openTab(candidate);
			}
		}
		if (anyMatch)
			ImGui::Spacing();
	}
	ImGui::Spacing();
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
	ImGui::Spacing();
	ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	ImGui::TextUnformatted(T("SETTINGS"));
	ImGui::PopStyleColor();
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
		// Clicking changes the current tab. The active page gets the accent
		// tint and edge bar from the design; other rows keep theme colors.
		const char* tabIcon = SettingsTabIcon(tab);
		std::string tabLabel;
		if (settingsIconFont != nullptr && settingsTitleFont != nullptr)
		{
			const float fontSize = ImGui::GetFontSize();
			const float iconWidth = settingsIconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, tabIcon).x;
			const float spaceWidth = std::max(1.0f, settingsTitleFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, " ").x);
			const int spacerCount = std::max(2, static_cast<int>(std::ceil((iconWidth + SettingsLayoutScaled(10.0f)) / spaceWidth)));
			tabLabel.append(spacerCount, ' ');
		}
		tabLabel += tabName;
		if (isSelected)
		{
			const ImVec4 accent = SettingsAccentColor();
			ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(accent.x, accent.y, accent.z, 0.35f));
			ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accent.x, accent.y, accent.z, 0.45f));
			ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(accent.x, accent.y, accent.z, 0.55f));
		}
		const bool tabClicked = ImGui::Selectable(tabLabel.c_str(), isSelected, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x, 0));
#ifdef HOLLYCAST_TEST_HARNESS
		const std::uint32_t tabItemId = test_harness::ui::collectingInspection() ? test_harness::ui::lastItemId() : 0;
#endif
		if (isSelected)
			ImGui::PopStyleColor(3);
		if (settingsIconFont != nullptr)
		{
			const ImVec2 itemMin = ImGui::GetItemRectMin();
			const ImVec2 itemMax = ImGui::GetItemRectMax();
			const float fontSize = ImGui::GetFontSize();
			const ImVec2 iconTextSize = settingsIconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, tabIcon);
			ImGui::GetWindowDrawList()->AddText(settingsIconFont, fontSize,
				ImVec2(itemMin.x + ImGui::GetStyle().FramePadding.x,
					itemMin.y + (itemMax.y - itemMin.y - iconTextSize.y) * 0.5f),
				ImGui::GetColorU32(isSelected ? ImGuiCol_Text : ImGuiCol_TextDisabled), tabIcon);
			if (isSelected)
			{
				ImGui::GetWindowDrawList()->AddRectFilled(
					ImVec2(itemMin.x, itemMin.y + SettingsLayoutScaled(4.0f)),
					ImVec2(itemMin.x + SettingsLayoutScaled(3.0f), itemMax.y - SettingsLayoutScaled(4.0f)),
					ImGui::ColorConvertFloat4ToU32(SettingsAccentColor()), SettingsLayoutScaled(2.0f));
			}
		}
		if (tabClicked)
			openTab(tab);
#ifdef HOLLYCAST_TEST_HARNESS
		if (tabItemId != 0)
			test_harness::ui::annotateItem(tabItemId, {{"semantic_id", "settings.tab." + std::to_string(i)},
				{"semantic_label", tabName ? tabName : ""}, {"kind", "tab"}, {"value", g_state.currentTab == tab}});
#endif
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
			openTab(tab);
			g_focusSettingsContent = true;
		}
	}

	// Product line pinned to the bottom of the rail.
	{
		const float railHeight = ImGui::GetWindowSize().y;
		const float labelHeight = ImGui::GetTextLineHeight();
		const float bottomY = railHeight - labelHeight - ImGui::GetStyle().WindowPadding.y;
		if (bottomY > ImGui::GetCursorPosY() + SettingsLayoutScaled(16.0f))
		{
			ImGui::SetCursorPosY(bottomY);
			ImGui::TextDisabled("%s", T("Dreamcast emulator"));
		}
	}

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
	if (g_resetSettingsContentScroll)
	{
		ImGui::SetNextWindowScroll(ImVec2(-1.f, 0.f));
		g_resetSettingsContentScroll = false;
	}
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
	RenderSettingsContentHeader();
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
bool RenderCollapsingHeader(
	const char* id,
	const char* labelIcon,
	const char* labelText,
	ImGuiTreeNodeFlags flags
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
#ifdef HOLLYCAST_TEST_HARNESS
	if (test_harness::ui::collectingInspection())
		test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", id ? id : labelText},
			{"semantic_label", labelText ? labelText : ""}, {"kind", "section"}, {"value", open}});
#endif
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

// Status strip at the top of the General page, mirroring the setup checklist.
static void RenderReadyToPlayBanner()
{
	static bool biosChecked = false;
	static bool biosFound = false;
	static std::vector<std::string> checkedContentPaths, checkedBiosPaths;
	if (!biosChecked || checkedContentPaths != config::ContentPath.get()
			|| checkedBiosPaths != config::BiosPath.get())
	{
		checkedContentPaths = config::ContentPath.get();
		checkedBiosPaths = config::BiosPath.get();
		biosFound = !hostfs::findFlash("dc_", "%bios.bin;%boot.bin").empty();
		biosChecked = true;
	}
	bool hasGamepad = false, hasKeyboard = false;
	for (int i = 0; i < GamepadDevice::GetGamepadCount(); ++i)
	{
		auto device = GamepadDevice::GetGamepad(i);
		if (!device) continue;
		if (dynamic_cast<KeyboardDevice*>(device.get())) hasKeyboard = true;
		else if (!dynamic_cast<Mouse*>(device.get()) && !device->is_virtual_gamepad()) hasGamepad = true;
	}
	const bool hasFolders = !config::ContentPath.get().empty();

	if (BeginSettingsCardSurface("ReadyToPlayBanner"))
	{
		if (largeFont != nullptr)
			ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(hasFolders ? T("Ready to play") : T("Set up your library"));
		if (largeFont != nullptr)
			ImGui::PopFont();

		// Divider between the headline and the status chips.
		ImGui::SameLine(0, SettingsLayoutScaled(22.0f));
		{
			const ImVec2 divPos = ImGui::GetCursorScreenPos();
			const float divHeight = ImGui::GetTextLineHeight();
			ImGui::Dummy(ImVec2(1.0f, divHeight));
			ImGui::GetWindowDrawList()->AddLine(
				ImVec2(divPos.x, divPos.y + SettingsLayoutScaled(2.0f)),
				ImVec2(divPos.x, divPos.y + divHeight - SettingsLayoutScaled(2.0f)),
				ImGui::GetColorU32(ImGuiCol_Border), 1.0f);
		}

		auto statusChip = [](bool ready, const char* label) {
			ImGui::SameLine(0, SettingsLayoutScaled(22.0f));
			if (settingsIconFont != nullptr)
			{
				ImGui::PushFont(settingsIconFont);
				ImGui::PushStyleColor(ImGuiCol_Text, ready ? SettingsSuccessColor() : SettingsWarningColor());
				ImGui::TextUnformatted(ready ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE);
				ImGui::PopStyleColor();
				ImGui::PopFont();
				ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
			}
			ImGui::TextUnformatted(label);
		};
		statusChip(hasFolders, T("Game folders"));
		statusChip(biosFound, biosFound ? T("Firmware") : T("Firmware: optional"));
		statusChip(hasGamepad, hasGamepad ? T("Controls")
			: (hasKeyboard ? T("Controls: review") : T("Controls: set up")));

		const char* reviewLabel = T("Review setup");
		ImFont* textFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
		ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : textFont;
		const float fontSize = ImGui::GetFontSize();
		const float buttonWidth = textFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, reviewLabel).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ARROW_RIGHT).x
			+ SettingsLayoutScaled(36.0f);
		const float buttonHeight = SettingsLayoutScaled(36.0f);
		ImGui::SameLine(RightColumnX(buttonWidth, 18.0f));
		const ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y - SettingsLayoutScaled(6.0f)));
		const ImVec4 accent = SettingsAccentColor();
		ImGui::PushStyleColor(ImGuiCol_Button, accent);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
			ImVec4(std::min(accent.x * 1.12f, 1.0f), std::min(accent.y * 1.12f, 1.0f), std::min(accent.z * 1.12f, 1.0f), 1.0f));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive,
			ImVec4(accent.x * 0.88f, accent.y * 0.88f, accent.z * 0.88f, 1.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.13f, 0.09f, 0.08f, 1.0f));
		const bool reviewPressed = SettingsLabelButton("##ReviewSetup", ICON_FA_ARROW_RIGHT,
			reviewLabel, ImVec2(buttonWidth, buttonHeight), true);
		ImGui::PopStyleColor(4);
		if (reviewPressed)
			gui_open_setup_checklist();
	}
	EndSettingsCardSurface();
	ImGui::Spacing();
}

// Shortcut card that jumps to the Game library page, where folders are managed.
static void RenderGameFoldersCard()
{
	if (BeginSettingsCardSurface("GameFoldersCard"))
	{
		const float headerX = ImGui::GetCursorPosX();
		float titleX = headerX;
		if (settingsIconFont != nullptr)
		{
			const float iconFontSize = SettingsPreviewFontSize(settingsIconFont) * 1.15f;
			const float iconWidth = settingsIconFont->CalcTextSizeA(iconFontSize, FLT_MAX, -1.0f, ICON_FA_FOLDER).x;
			ImGui::PushFont(settingsIconFont, iconFontSize);
			ImGui::TextUnformatted(ICON_FA_FOLDER);
			ImGui::PopFont();
			ImGui::SameLine(0, SettingsLayoutScaled(10.0f));
			titleX = headerX + iconWidth + SettingsLayoutScaled(10.0f);
		}
		if (largeFont != nullptr)
			ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(T("Game folders"));
		if (largeFont != nullptr)
			ImGui::PopFont();
		ImGui::SetCursorPosX(titleX);
		ImGui::TextDisabled("%s", T("Add and manage folders in Game library."));

		const char* manageLabel = T("Manage folders");
		ImFont* textFont = settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
		ImFont* iconFont = settingsIconFont != nullptr ? settingsIconFont : textFont;
		const float fontSize = ImGui::GetFontSize();
		const float buttonWidth = textFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, manageLabel).x
			+ iconFont->CalcTextSizeA(fontSize, FLT_MAX, -1.0f, ICON_FA_ARROW_RIGHT).x
			+ SettingsLayoutScaled(36.0f);
		ImGui::SameLine(RightColumnX(buttonWidth, 18.0f));
		const ImVec2 buttonPos = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(buttonPos.x, buttonPos.y - SettingsLayoutScaled(26.0f)));
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::PushStyleColor(ImGuiCol_Border, SettingsAccentColor());
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, SettingsLayoutScaled(8.0f));
		const bool managePressed = SettingsLabelButton("##ManageFolders", ICON_FA_ARROW_RIGHT,
			manageLabel, ImVec2(buttonWidth, SettingsLayoutScaled(36.0f)), true);
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(3);
		if (managePressed)
			openTab(SettingsTab::Library);
	}
	EndSettingsCardSurface();
	ImGui::Spacing();
}

// Render General tab with all settings from settings_general.cpp
static void renderSettingsContentTab(SettingsTab tab)
{
	using namespace SettingsUI;
	ScopedTwoLineRowStyle generalRowStyle(12.0f, true, 0.5f);
	if (tab == SettingsTab::General)
	{

	RenderReadyToPlayBanner();

	// ========================================
	// App Language Section
	// ========================================
	if (BeginSettingsCard("AppLanguageCard", ICON_FA_GLOBE, T("App language")))
	{
		{
			struct UILanguageOption
			{
				const char* label;
				const char* locale;
			};

			const UILanguageOption uiLanguageOptions[] = {
				{ T("System default"), "" },
				{ Tnop("English"), "en" },
				{ Tnop("Deutsch"), "de" },
				{ Tnop("Español"), "es" },
				{ Tnop("Français"), "fr" },
				{ Tnop("Magyar"), "hu" },
				{ Tnop("हिन्दी"), "hi" },
				{ Tnop("Italiano"), "it" },
				{ Tnop("日本語"), "ja" },
				{ Tnop("한국어"), "ko" },
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
			uiLanguageCfg.options.label = T("Interface language");
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
	}
	EndSettingsCard();

	// ========================================
	// Dreamcast Defaults Section
	// ========================================
	if (BeginSettingsCard("DreamcastDefaultsCard", ICON_FA_GAMEPAD, T("Dreamcast defaults"),
			T("Language, region and video settings for the emulated console.")))
	{

		// 0 -> JP, 1 -> EN, 2 -> DE, 3 -> FR, 4 -> SP, 5 -> IT, 6 -> default
		const char* languages[] = { T("Japanese"), T("English"), T("German"), T("French"), T("Spanish"), T("Italian"), T("Default") };
		SettingsUI::PopupConfig languageCfg {};
		languageCfg.type = SettingsUI::PopupType::Options;
		languageCfg.options.label = T("Game language");
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
				T("Preferred language for supported games."),
				languageCfg,
				T(
					"Language\n"
					"Sets the Dreamcast BIOS language that games read for menus and text.\n"
					"Most games will follow this setting; some may ignore it.\n\n"
					"Arcade platforms typically use automatic region/language behavior."
				)
			);

		const char* consoleRegion[] = { T("Japan"), T("USA"), T("Europe"), T("Default") };
		const char* arcadeRegion[] = { T("Japan"), T("USA"), i18n::translateCtx("Arcade region", "Export"), T("Korea") };
		const char* const* region = settings.platform.isArcade() ? arcadeRegion : consoleRegion;

		SettingsUI::PopupConfig regionCfg {};
		regionCfg.type = SettingsUI::PopupType::Options;
		regionCfg.options.label = T("Console region");
		regionCfg.options.icon = ICON_FA_EARTH_AMERICAS;
		regionCfg.options.popupID = Tnop("RegionPopup");
		regionCfg.options.options = region;
		regionCfg.options.optionCount = 4;
		regionCfg.options.currentValue = &config::Region.get();
		regionCfg.options.valueWidth = 220.0f;
			RenderGeneralPopupSettingRow(
				Tnop("RegionSetting"),
				T("Region reported by the Dreamcast BIOS."),
				regionCfg,
				T(
					"Region\n"
					"Sets the emulated BIOS region.\n"
					"This can affect game compatibility, default language choices, and region-locked behavior in some titles.\n\n"
					"If a game refuses to boot or behaves like the wrong region, try changing this."
				)
			);

		const char* broadcast[] = { Tnop("NTSC"), Tnop("PAL"), Tnop("PAL/M"), Tnop("PAL/N"), T("Default") };
		SettingsUI::PopupConfig broadcastCfg {};
		broadcastCfg.type = SettingsUI::PopupType::Options;
		broadcastCfg.options.label = T("TV standard");
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
				T("Broadcast format for non-VGA output."),
				broadcastCfg,
				T(
					"Broadcast\n"
					"Controls the TV broadcast standard used for non-VGA video modes.\n"
					"NTSC is typically 60 Hz; PAL is typically 50 Hz.\n\n"
					"If you see timing issues or a game expects a specific region video mode, try changing this."
				), false, true);

		const char* cable[] = { T("VGA"), T("RGB Component"), T("TV Composite") };
		const bool disabled = config::Cable.isReadOnly() || settings.platform.isArcade();

		SettingsUI::PopupConfig cableCfg {};
		cableCfg.type = SettingsUI::PopupType::Options;
		cableCfg.options.label = T("Video cable");
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
				T("Emulated connection used by the console."),
				cableCfg,
				T(
					"Cable\n"
					"Controls the emulated Dreamcast video cable type.\n"
					"Games can change behavior depending on whether they detect VGA vs. TV output.\n\n"
					"If a game has missing effects, wrong colors, or odd mode selection, this is worth trying."
				),
				disabled, true
			);
		RenderSettingsInfoBanner("DreamcastDefaultsNote",
			T("Game language is separate from the Hollycast interface language."));
	}
	EndSettingsCard();

	RenderGameFoldersCard();

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
						"If Hollycast cannot find a BIOS, double-check that your BIOS files are placed in the expected location and that their filenames match what Hollycast expects.\n"
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
				const std::string path = get_writable_config_path("");
				std::error_code pathError;
				const auto directory = path.empty()
						? std::filesystem::path{} : std::filesystem::absolute(path, pathError);
				if (pathError || directory.empty() || !std::filesystem::is_directory(directory, pathError))
				{
					WARN_LOG(COMMON, "Cannot reveal config folder '%s' (%s)", path.c_str(),
							pathError ? pathError.message().c_str() : "not a directory");
				}
				else
				{
					char command[] = "open";
					std::string operand = directory.string();
					char *argv[] = { command, operand.data(), nullptr };
					pid_t pid;
					const int spawnError = posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv, environ);
					if (spawnError != 0)
						WARN_LOG(COMMON, "Cannot launch Finder for config folder '%s' (error %d)", path.c_str(), spawnError);
					else
						std::thread([pid] {
							while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR) {}
						}).detach();
				}
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
			labels.push_back(strprintf(T("%s (Custom)"), file.substr(0, file.size() - 5).c_str()));
		int selection = config::UITheme.get();
		if (selection == CustomTheme::CustomThemeId) {
			auto selected = std::find(customFiles.begin(), customFiles.end(), config::UICustomTheme.get());
			if (selected != customFiles.end())
				selection = builtinCount + static_cast<int>(selected - customFiles.begin());
			else {
				selection = builtinCount + static_cast<int>(labels.size());
				labels.push_back(strprintf(T("%s (Missing)"), config::UICustomTheme.get().c_str()));
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
			// The UI palette/editor and library package palette are separate choices.
			// Selecting a UI theme clears a package so its palette cannot mask it.
			config::CustomTheme.get().clear();
			config::ThemeLayout.get().clear();
			SaveSettings();
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
		// Keep the popup ID stable while translating its title.
		const std::string importTitle = std::string(T("Import Theme JSON")) + "###ImportThemeJSON";
		if (ImGui::Button(T("Import Theme…")))
			ImGui::OpenPopup(importTitle.c_str());
		select_file_popup(importTitle.c_str(), [&](bool cancelled, const std::string& source) {
			if (!cancelled) {
				themeNotice.clear();
				std::string imported;
				if (CustomTheme::importFile(source, imported, themeError)) {
					customFiles = CustomTheme::list();
					themeNotice = strprintf(T("Imported %s"), imported.c_str());
				}
			}
			return true;
		}, true, "json");

		if (g_themeEditor.open) {
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			ImGui::SetNextWindowSize(ImVec2(std::max(1.0f, std::min(uiScaled(760.0f), display.x - uiScaled(24.0f))),
					std::max(1.0f, std::min(uiScaled(650.0f), display.y - uiScaled(24.0f)))));
		}
		const std::string editorTitle = std::string(g_themeEditor.editing ? T("Edit theme") : T("Create theme")) + "###ThemeEditorPopup";
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
#ifdef HOLLYCAST_TEST_HARNESS
				annotateTestText("theme.editor", "new_name", T("Theme name"));
#endif
			}
			ImGui::SetNextItemWidth(std::max(1.0f, ImGui::GetContentRegionAvail().x * 0.56f));
			const char* baseThemeLabel = T("Base theme");
			const bool baseThemeChanged = ImGui::Combo(baseThemeLabel, &baseIndex, baseNames.data(), static_cast<int>(baseNames.size()));
#ifdef HOLLYCAST_TEST_HARNESS
			if (test_harness::ui::collectingInspection())
			{
				test_harness::Json options = test_harness::Json::array();
				for (int i = 0; i < static_cast<int>(baseNames.size()); ++i)
					options.push_back({{"value", i}, {"label", baseNames[i]}, {"selected", baseIndex == i}});
				test_harness::Json metadata = {{"semantic_id", "theme.editor.base"}, {"semantic_label", baseThemeLabel},
					{"kind", "options"}, {"value", baseIndex}, {"options", std::move(options)}, {"requires_apply", true}};
				if (baseIndex >= 0 && baseIndex < static_cast<int>(baseNames.size()))
					metadata["value_label"] = baseNames[baseIndex];
				test_harness::ui::annotateItem(test_harness::ui::lastItemId(), metadata);
			}
#endif
			if (baseThemeChanged) {
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
						const auto editColor = [](const char* label, ImVec4& value) {
							const ImGuiStyle& style = ImGui::GetStyle();
							const float height = ImGui::GetFrameHeight();
							const float swatchWidth = height + style.ItemInnerSpacing.x;
							const float inputWidth = std::max(1.f, ImGui::CalcItemWidth() - swatchWidth);
							ImVec2 inputPos = ImGui::GetCursorScreenPos();
							if (style.ColorButtonPosition == ImGuiDir_Left)
								inputPos.x += swatchWidth;
							const ImRect inputs(inputPos, inputPos + ImVec2(inputWidth, height));
							return allowSettingsTouchDrag(ImGui::ColorEdit4(label, &value.x, ImGuiColorEditFlags_AlphaBar), &inputs);
						};
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
								const bool enabledChanged = ImGui::Checkbox(labels[i], &enabled);
#ifdef HOLLYCAST_TEST_HARNESS
								annotateTestToggle("theme.role", role, labels[i], enabled);
#endif
								if (enabledChanged) {
									if (enabled) {
										auto inherited = baseTheme.roles.find(role);
										g_themeEditor.draft.roles[role] = inherited != baseTheme.roles.end() ? inherited->second : suggestions[i];
									} else g_themeEditor.draft.roles.erase(role);
									g_themeEditor.previewing = true;
								}
								found = g_themeEditor.draft.roles.find(role);
								if (found != g_themeEditor.draft.roles.end()) {
									ImVec4 value = found->second;
									const bool colorChanged = editColor("##color", value);
#ifdef HOLLYCAST_TEST_HARNESS
									annotateTestColor("theme.role.color", role, labels[i], "##color", &value.x);
#endif
									if (colorChanged) {
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
#ifdef HOLLYCAST_TEST_HARNESS
							annotateTestText("theme.colors", "search", T("Search colors"));
#endif
							std::string query = g_themeEditor.colorSearch;
							std::transform(query.begin(), query.end(), query.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
							for (int i = 0; i < ImGuiCol_COUNT; ++i) {
								const char* name = ImGui::GetStyleColorName(i);
								std::string searchable = name;
								std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
								if (!query.empty() && searchable.find(query) == std::string::npos) continue;
								ImVec4 value = previewStyle.Colors[i];
								const bool colorChanged = editColor(name, value);
#ifdef HOLLYCAST_TEST_HARNESS
								annotateTestColor("theme.interface_color", name, name, name, &value.x);
#endif
								if (colorChanged) {
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
#ifdef HOLLYCAST_TEST_HARNESS
					const bool collectThemeMetadata = test_harness::ui::collectingInspection();
					const std::string widgetRoleId = collectThemeMetadata ? std::string(widget) + "." + role : std::string();
#endif
									const bool enabledChanged = ImGui::Checkbox(role, &enabled);
#ifdef HOLLYCAST_TEST_HARNESS
					if (collectThemeMetadata)
						annotateTestToggle("theme.widget_role", widgetRoleId.c_str(), role, enabled);
#endif
									if (enabledChanged) {
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
										const bool colorChanged = editColor("##color", value);
#ifdef HOLLYCAST_TEST_HARNESS
									if (collectThemeMetadata)
										annotateTestColor("theme.widget_role.color", widgetRoleId.c_str(), role, "##color", &value.x);
#endif
										if (colorChanged) { found->second = value; g_themeEditor.previewing = true; }
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
						const bool changed = allowSettingsTouchDrag(ImGui::SliderFloat(name, &value, 0.f, upper, "%.3f", ImGuiSliderFlags_AlwaysClamp));
#ifdef HOLLYCAST_TEST_HARNESS
						annotateTestFloat("theme.style", name, name, value, 0.f, upper);
#endif
						if (changed) {
							auto it = std::find_if(g_themeEditor.draft.scalars.begin(), g_themeEditor.draft.scalars.end(), [&](const auto& v) { return v.first == member; });
							if (it == g_themeEditor.draft.scalars.end()) g_themeEditor.draft.scalars.emplace_back(member, value); else it->second = value;
							g_themeEditor.previewing = true;
						}
					};
					auto vector = [&](const char* name, ImVec2 ImGuiStyle::*member, float max) {
						ImVec2 value = previewStyle.*member;
						const bool changed = allowSettingsTouchDrag(ImGui::DragFloat2(name, &value.x, 0.25f, 0.f, max, "%.3f", ImGuiSliderFlags_AlwaysClamp));
#ifdef HOLLYCAST_TEST_HARNESS
							annotateTestVector2("theme.style", name, name, name, value, 0.f, max);
#endif
						if (changed) {
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
							const bool changed = allowSettingsTouchDrag(ImGui::SliderFloat(item.first, &value, 0.2f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp));
#ifdef HOLLYCAST_TEST_HARNESS
							annotateTestFloat("theme.style", item.first, item.first, value, 0.2f, 1.f);
#endif
							if (changed) {
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
							const bool changed = allowSettingsTouchDrag(ImGui::SliderFloat(name, &shown, minimum, maximum, "%.3f", ImGuiSliderFlags_AlwaysClamp));
#ifdef HOLLYCAST_TEST_HARNESS
						annotateTestFloat("theme.layout", name, name, shown, minimum, maximum);
#endif
						if (changed) { value = shown; present = true; g_themeEditor.previewing = true; }
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
					auto assetInput = [&](const char* id, const char* label, std::string& value, bool font = false) {
						const bool changed = ImGui::InputText(label, &value);
						const bool editCommitted = font && ImGui::IsItemDeactivatedAfterEdit();
#ifdef HOLLYCAST_TEST_HARNESS
						annotateTestText("theme.asset", id, label);
#endif
						if (changed) { g_themeEditor.previewing = true; g_themeEditor.fontChanged |= font; }
						if (editCommitted) { g_themeEditor.previewing = true; g_themeEditor.fontReloadPending = true; }
					};
					assetInput("body_font", T("Body font path"), g_themeEditor.draft.assets.bodyFont, true);
					assetInput("bold_font", T("Bold font path"), g_themeEditor.draft.assets.boldFont, true);
					assetInput("background_image", T("Background image path"), g_themeEditor.draft.assets.background);
					std::string settingsIconPath;
					auto settingsIcon = g_themeEditor.draft.assets.icons.find("settings");
					if (settingsIcon != g_themeEditor.draft.assets.icons.end()) settingsIconPath = settingsIcon->second;
					const bool settingsIconChanged = ImGui::InputText(T("Settings icon path"), &settingsIconPath);
#ifdef HOLLYCAST_TEST_HARNESS
					annotateTestText("theme.asset", "settings_icon", T("Settings icon path"));
#endif
					if (settingsIconChanged) {
						if (settingsIconPath.empty()) g_themeEditor.draft.assets.icons.erase("settings");
						else g_themeEditor.draft.assets.icons["settings"] = settingsIconPath;
						g_themeEditor.previewing = true;
					}
					float opacity = g_themeEditor.draft.assets.backgroundOpacity;
					const char* opacityLabel = T("Background opacity");
						const bool opacityChanged = allowSettingsTouchDrag(ImGui::SliderFloat(opacityLabel, &opacity, 0.f, 1.f, "%.3f", ImGuiSliderFlags_AlwaysClamp));
#ifdef HOLLYCAST_TEST_HARNESS
					annotateTestFloat("theme.asset", "background_opacity", opacityLabel, opacity, 0.f, 1.f);
#endif
					if (opacityChanged) { g_themeEditor.draft.assets.backgroundOpacity = opacity; g_themeEditor.previewing = true; }
					const char* fits[] = {"cover", "contain", "stretch"};
					// Keep the JSON values independent of the translated option labels.
					const char* fitLabels[] = {i18n::translateCtx("Background fit", "Cover"),
						i18n::translateCtx("Background fit", "Contain"), i18n::translateCtx("Background fit", "Stretch")};
					int fit = g_themeEditor.draft.assets.backgroundFit == "contain" ? 1 : g_themeEditor.draft.assets.backgroundFit == "stretch" ? 2 : 0;
					const char* fitLabel = T("Background fit");
					const bool fitChanged = ImGui::Combo(fitLabel, &fit, fitLabels, IM_ARRAYSIZE(fitLabels));
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
					{
						test_harness::Json options = test_harness::Json::array();
						for (int i = 0; i < IM_ARRAYSIZE(fitLabels); ++i)
							options.push_back({{"value", fits[i]}, {"label", fitLabels[i]}, {"selected", fit == i}});
						test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "theme.asset.background_fit"},
							{"semantic_label", fitLabel}, {"kind", "options"}, {"value", fits[fit]},
							{"value_label", fitLabels[fit]}, {"options", std::move(options)}});
					}
#endif
					if (fitChanged) { g_themeEditor.draft.assets.backgroundFit = fits[fit]; g_themeEditor.previewing = true; }
					ImGui::InputText(T("Export folder"), &g_themeEditor.exportFolder);
#ifdef HOLLYCAST_TEST_HARNESS
					annotateTestText("theme.asset", "export_folder", T("Export folder"));
#endif
					const bool exportClicked = ImGui::Button(T("Export theme and assets"));
#ifdef HOLLYCAST_TEST_HARNESS
					if (test_harness::ui::collectingInspection())
						test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "theme.asset.export"},
							{"semantic_label", T("Export theme and assets")}, {"kind", "button"}});
#endif
					if (exportClicked) {
						const std::string exportName = themeFilenameWithExtension(g_themeEditor.editing ? g_themeEditor.filename : g_themeEditor.newFilename);
						if (CustomTheme::exportToDirectory(g_themeEditor.draft, exportName, g_themeEditor.exportFolder, g_themeEditor.error))
							g_themeEditor.error = strprintf(T("Exported %s and its referenced assets."), exportName.c_str());
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
						g_themeEditor.error = strprintf(T("Saved %s, but could not select it. %s. Close this editor, resolve the migration error, then select this theme from UI Theme."),
								filename.c_str(), migrationError.c_str());
					} else {
						config::CustomTheme.get().clear();
						config::ThemeLayout.get().clear();
						config::UITheme = CustomTheme::CustomThemeId;
						config::UICustomTheme = filename;
						SaveSettings();
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
	if (RenderCollapsingHeader("LibraryThemeSection", ICON_FA_SWATCHBOOK, T("Library Themes"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		// Installed library themes package a palette and a library layout. These controls affect
		// the library rather than the app-wide UI theme.
		std::vector<std::string> libraryThemeLabelStorage;
		std::vector<std::string> libraryThemeIds;
		libraryThemeLabelStorage.emplace_back(T("App UI Theme"));
		libraryThemeIds.emplace_back();
		for (const themes::CatalogEntry& entry : themes::catalog())
		{
			if (!entry.error.empty())
				continue;
			libraryThemeLabelStorage.push_back(entry.name.empty() ? entry.id : entry.name);
			libraryThemeIds.push_back(entry.id);
		}
		std::vector<const char*> libraryThemeLabels;
		for (const std::string& label : libraryThemeLabelStorage)
			libraryThemeLabels.push_back(label.c_str());

		int selectedLibraryThemeIndex = 0;
		if (!config::CustomTheme.get().empty())
		{
			auto selected = std::find(libraryThemeIds.begin(), libraryThemeIds.end(), config::CustomTheme.get());
			if (selected != libraryThemeIds.end())
				selectedLibraryThemeIndex = static_cast<int>(selected - libraryThemeIds.begin());
		}
		SettingsUI::PopupConfig libraryThemeCfg {};
		libraryThemeCfg.type = SettingsUI::PopupType::Options;
		libraryThemeCfg.options.label = T("Library Theme");
		libraryThemeCfg.options.icon = ICON_FA_SWATCHBOOK;
		libraryThemeCfg.options.popupID = Tnop("LibraryThemePopup");
		libraryThemeCfg.options.options = libraryThemeLabels.data();
		libraryThemeCfg.options.optionCount = static_cast<int>(libraryThemeLabels.size());
		libraryThemeCfg.options.currentValue = &selectedLibraryThemeIndex;
		libraryThemeCfg.options.valueWidth = 220.0f;
		libraryThemeCfg.options.onChange = [&](int index) {
			if (index < 0 || index >= static_cast<int>(libraryThemeIds.size()))
				return false;
			config::CustomTheme.get() = libraryThemeIds[index];
			config::ThemeLayout.get().clear();
			SaveSettings();
			g_applyThemeAfterSettings = true;
			return true;
		};
		RenderGeneralPopupSettingRow(
			Tnop("LibraryThemeSetting"),
			T("Choose a library theme package, or use the selected UI theme and default library layout."),
			libraryThemeCfg,
			T("Library Theme\nThemes are loaded from the library themes folder shown below."));
		gui_display_library_download_progress();
		ImGui::PushID("ThemeDownloadRow");
		const bool downloadThemes = BeginTwoLineSettingRow("##row",
			T("Download Themes\nBrowse theme previews and descriptions, then download and install one for your library."));
		const ImVec2 downloadThemesLine1Start = BeginTwoLineSettingRowContent();
		SettingIcon(ICON_FA_DOWNLOAD, SettingsLayoutSize(20.0f, 20.0f));
		ImGui::SameLine(0, SettingsLayoutScaled(8.0f));
		ImGui::PushFont(largeFont);
		ImGui::TextUnformatted(T("Download Themes"));
		ImGui::PopFont();
		ImGui::PushFont(settingsRightValueFont != nullptr ? settingsRightValueFont : largeFont);
		ImGui::SameLine(RightColumnX(ImGui::CalcTextSize(T("Download")).x));
		ImGui::SetCursorPosY(ImGui::GetCursorPosY()
				+ (TwoLineSettingContentHeight() - ImGui::GetTextLineHeight()) * 0.5f);
		ImGui::TextUnformatted(T("Download"));
		ImGui::PopFont();
		RenderTwoLineSettingDescription(downloadThemesLine1Start, T("Browse and install library themes from the online catalogue."));
		EndTwoLineSettingRow();
		ImGui::PopID();
		if (downloadThemes) themes::openBrowser();

		if (const themes::Theme* activeTheme = themes::current())
		{
			ImGui::Indent(SettingsLayoutScaled(28.0f));
			ImGui::TextWrapped(T("%s%s%s"), activeTheme->name.c_str(),
				activeTheme->author.empty() ? "" : " — ", activeTheme->author.c_str());
			if (!activeTheme->description.empty())
				ImGui::TextWrapped("%s", activeTheme->description.c_str());
			ImGui::Unindent(SettingsLayoutScaled(28.0f));

			if (!activeTheme->layouts.empty())
			{
				std::vector<std::string> layoutLabelStorage;
				std::vector<std::string> selectableLayoutIds;
				std::vector<const char*> layoutLabels;
				int selectedLayoutIndex = 0;
				for (size_t i = 0; i < activeTheme->layouts.size(); ++i)
				{
					const themes::Layout& layout = activeTheme->layouts[i];
					if (layout.screen != "library") continue;
					if (layout.id == config::ThemeLayout.get())
						selectedLayoutIndex = static_cast<int>(selectableLayoutIds.size());
					selectableLayoutIds.push_back(layout.id);
					layoutLabelStorage.push_back(layout.name.empty() ? layout.id : layout.name);
				}
				for (const std::string& label : layoutLabelStorage)
					layoutLabels.push_back(label.c_str());
				SettingsUI::PopupConfig layoutCfg {};
				layoutCfg.type = SettingsUI::PopupType::Options;
				layoutCfg.options.label = T("Library Layout");
				layoutCfg.options.icon = ICON_FA_TABLE_CELLS_LARGE;
				layoutCfg.options.popupID = Tnop("LibraryLayoutPopup");
				layoutCfg.options.options = layoutLabels.data();
				layoutCfg.options.optionCount = static_cast<int>(layoutLabels.size());
				layoutCfg.options.currentValue = &selectedLayoutIndex;
				layoutCfg.options.valueWidth = 220.0f;
				layoutCfg.options.onChange = [&](int index) {
					if (index >= 0 && index < static_cast<int>(selectableLayoutIds.size()))
					{
						config::ThemeLayout.get() = selectableLayoutIds[index];
						SaveSettings();
					}
					return true;
				};
				RenderGeneralPopupSettingRow(
					Tnop("LibraryLayoutSetting"),
					T("Choose the library layout supplied by this theme."),
					layoutCfg);
			}

			const char* iconMediaLabels[] = {
				T("Theme Default"), T("Default Artwork"), T("Mix Image"), T("Scraped Cover"),
				T("Case / 3D Box"), T("Screenshot"), T("Wheel / Marquee"), T("Physical Media"), T("Fan Art"), T("Title Screen"),
				T("Banner"), T("Back Cover"), T("Spine"), T("Box Texture")
			};
			const int iconMedia = config::ThemeIconMedia.get();
			const int manual = static_cast<int>(config::LibraryCoverMediaMode::Manual);
			int selectedIconMedia = iconMedia == manual ? 0 : iconMedia > manual ? iconMedia : iconMedia + 1;
			if (selectedIconMedia < 0 || selectedIconMedia >= IM_ARRAYSIZE(iconMediaLabels))
				selectedIconMedia = 0;
			SettingsUI::PopupConfig iconMediaCfg {};
			iconMediaCfg.type = SettingsUI::PopupType::Options;
			iconMediaCfg.options.label = T("Theme Icon Media");
			iconMediaCfg.options.icon = ICON_FA_IMAGE;
			iconMediaCfg.options.popupID = Tnop("ThemeIconMediaPopup");
			iconMediaCfg.options.options = iconMediaLabels;
			iconMediaCfg.options.optionCount = IM_ARRAYSIZE(iconMediaLabels);
			iconMediaCfg.options.currentValue = &selectedIconMedia;
			iconMediaCfg.options.valueWidth = 220.0f;
			iconMediaCfg.options.onChange = [](int index) {
				config::ThemeIconMedia = index > static_cast<int>(config::LibraryCoverMediaMode::Manual) ? index : index - 1;
				gui_refresh_custom_boxart(true);
				SaveSettings();
				return true;
			};
			RenderGeneralPopupSettingRow(Tnop("ThemeIconMediaSetting"),
				T("Choose the artwork used by theme grid icons and list thumbnails. Theme Default restores the layout's choice."), iconMediaCfg);
			SettingsUI::PopupConfig iconSizeCfg {};
			iconSizeCfg.type = SettingsUI::PopupType::Slider;
			iconSizeCfg.slider.label = T("Library Scale");
			iconSizeCfg.slider.icon = ICON_FA_IMAGE;
			iconSizeCfg.slider.popupID = "ThemeIconSizePopup";
			iconSizeCfg.slider.description = T("Resize library icons, shelves, covers and game information. The library toolbar has the same Icon Size control. General UI Scaling does not change library sizes.");
			iconSizeCfg.slider.currentValue = &config::LibraryIconScale.get();
			iconSizeCfg.slider.minValue = 50;
			iconSizeCfg.slider.maxValue = 200;
			iconSizeCfg.slider.defaultValue = 100;
			iconSizeCfg.slider.onApply = [] { config::LibraryIconScale.save(); };
			RenderGeneralPopupSettingRow("ThemeIconSizeSetting", T("Set the library scale, including cover art and game information."), iconSizeCfg);
		}

		if (const std::string& error = themes::error(); !error.empty())
			ImGui::TextWrapped(T("Library theme error: %s"), error.c_str());
		for (const themes::CatalogEntry& entry : themes::catalog())
		{
			if (!entry.error.empty())
				ImGui::TextWrapped(T("%s: %s"), entry.name.empty() ? entry.id.c_str() : entry.name.c_str(), entry.error.c_str());
		}

	}

	if (RenderCollapsingHeader("ArtworkScraperSection", ICON_FA_DOWNLOAD, T("Artwork Downloads"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		const char *providers[] = {T("TheGamesDB"), T("ScreenScraper"), T("LaunchBox")};
		SettingsUI::PopupConfig providerCfg {};
		providerCfg.type = SettingsUI::PopupType::Options;
		providerCfg.options.label = T("Artwork Provider");
		providerCfg.options.icon = ICON_FA_IMAGE;
		providerCfg.options.popupID = "ArtworkProviderPopup";
		providerCfg.options.options = providers;
		providerCfg.options.optionCount = IM_ARRAYSIZE(providers);
		providerCfg.options.currentValue = &config::ArtworkProvider.get();
		providerCfg.options.valueWidth = 220.0f;
		providerCfg.options.onChange = [](int provider) {
			if (provider != 1) screenscraper::disconnect();
			SaveSettings();
			return true;
		};
		RenderGeneralPopupSettingRow("ArtworkProviderSetting",
			T("Choose the preferred online artwork source. ScreenScraper finishes first, then LaunchBox and TheGamesDB fill missing requested media. Existing local media remains available."), providerCfg);
		if (config::ArtworkProvider.get() == 1)
		{
			ImGui::TextWrapped("%s", T("Basic views download covers, hover miximages, title logos and game information. Selecting a theme automatically downloads only its additional required media. Physical disc labels are read locally."));
			static char password[256] = {};
			const auto account = screenscraper::status();
			const bool configured = screenscraper::developerConfigured();
			ImGui::BeginDisabled(account.connecting);
			RenderGeneralTextInputSettingRow("ScreenScraperUsername", ICON_FA_USER, T("ScreenScraper Username"),
				T("Use your ScreenScraper screen name, not your email address, for your account's download and thread allowances."), [] {
					InputText("##SSUsername", &config::ScreenScraperUser.get(), ImGuiInputTextFlags_None);
				});
			RenderGeneralTextInputSettingRow("ScreenScraperPassword", ICON_FA_LOCK, T("Password"),
				T("Kept for this session; sign in again after restarting Hollycast."), [&] {
					InputText("##SSPassword", password, sizeof(password), ImGuiInputTextFlags_Password);
				});
			ImGui::BeginDisabled(!configured || config::ScreenScraperUser.get().empty() || !password[0]);
			if (ImGui::Button(T("Sign In / Refresh Account")))
			{
				screenscraper::connect(trim_ws(config::ScreenScraperUser.get()), password);
				std::fill(std::begin(password), std::end(password), '\0');
				SaveSettings();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(!configured);
			if (ImGui::Button(account.personalAccount ? T("Sign Out / Use Guest Access") : T("Reconnect Guest Access")))
			{
				screenscraper::connect("", "");
				std::fill(std::begin(password), std::end(password), '\0');
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			if (!configured)
				ImGui::TextWrapped("%s", T("ScreenScraper application access is not configured in this build. Use a provisioned Hollycast build to download media. Your media choices and existing local artwork remain available."));
			if (!account.message.empty())
				ImGui::TextWrapped("%s", account.message.c_str());
			if (account.connected)
			{
				ImGui::TextUnformatted(account.personalAccount ? T("Personal account") : T("Guest access; sign in for your account's allowances."));
				ImGui::Text(T("Account threads: %d | Active requests: %d"), account.maxThreads, account.activeRequests);
				gui_display_library_download_progress();
				ImGui::Text(T("Requests today: %llu / %llu | Unmatched: %llu / %llu"),
					static_cast<unsigned long long>(account.requestsToday), static_cast<unsigned long long>(account.requestsPerDay),
					static_cast<unsigned long long>(account.missesToday), static_cast<unsigned long long>(account.missesPerDay));
				ImGui::Text(T("Requests/minute: %llu | Download allowance: %llu KB/s (0 = unspecified)"),
					static_cast<unsigned long long>(account.requestsPerMinute), static_cast<unsigned long long>(account.downloadKBPerSecond));
			}
		}
		if (config::ArtworkProvider.get() == 0)
		{
			ImGui::TextWrapped("%s", T("TheGamesDB downloads original-resolution covers, screenshots, fanart, logos, banners, title screens and back covers. Disc scans, miximages, 3D boxes and spines are not supplied by this provider. Existing local images remain available."));
			gui_display_library_download_progress();
		}
		if (config::ArtworkProvider.get() == 2)
  {
   ImGui::TextWrapped("%s", T("LaunchBox supplies original covers, disc scans, 3D boxes, back covers, spines, screenshots, title screens, logos, banners and fanart. The first lookup downloads its public catalog. TheGamesDB fills unavailable supported types."));
   gui_display_library_download_progress();
  }
  const u32 supported = ArtworkAdapter::supportedMedia(config::ArtworkProvider.get());
		std::vector<const char*> mediaLabels;
		std::vector<int> mediaBits;
		const u32 selection = static_cast<u32>(config::ScreenScraperMedia.get()) & supported;
		unsigned selectedCount = 0;
		for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
		{
			if (!(supported & (1u << i))) continue;
			mediaLabels.push_back(T(artwork::mediaTypes[i].label));
			mediaBits.push_back(static_cast<int>(i));
			if (selection & (1u << i)) ++selectedCount;
		}
		const std::string selectionSummary = std::to_string(selectedCount) + " / "
			+ std::to_string(mediaLabels.size()) + " " + T("selected");
		SettingsUI::PopupConfig mediaCfg {};
		mediaCfg.type = SettingsUI::PopupType::Options;
		mediaCfg.options.label = T("Images to Download");
		mediaCfg.options.icon = ICON_FA_IMAGES;
		mediaCfg.options.popupID = "ArtworkMediaPopup";
		mediaCfg.options.description = T("Choose types for the manual Download Selected Media action. Automatic downloads follow the selected theme. Download All Media for All Themes prepares the installed themes. Existing images are kept.");
		mediaCfg.options.options = mediaLabels.data();
		mediaCfg.options.optionCount = static_cast<int>(mediaLabels.size());
		mediaCfg.options.currentValue = &config::ScreenScraperMedia.get();
		mediaCfg.options.multiSelect = true;
		mediaCfg.options.storageIndexMap = [mediaBits](int index) { return mediaBits[index]; };
		mediaCfg.options.selectionPresets = {
			{T("Covers + Banners"), 1 | (1 << 3)},
			{T("Select All"), static_cast<int>(supported)},
			{T("Clear Selection"), 0}
		};
		mediaCfg.options.valueWidth = 220.0f;
		mediaCfg.options.valueToString = [&selectionSummary](int) { return selectionSummary.c_str(); };
		mediaCfg.options.onChange = [](int) {
			SaveSettings();
			return true;
		};
		RenderGeneralPopupSettingRow("ArtworkMediaSetting",
			T("Choose image types for the manual Download Selected Media action."), mediaCfg);
		const bool canDownload = (config::ArtworkProvider.get() != 1 || screenscraper::status().connected) && config::FetchBoxart
			&& config::BoxartSourceMode.get() != static_cast<int>(BoxartSourceMode::PhysicalMediaOnly);
		ImGui::BeginDisabled(!canDownload);
		if (ImGui::Button(T("Download All Media for All Themes"), ImVec2(-FLT_MIN, 0)))
			gui_download_all_theme_media();
		ImGui::BeginDisabled(!(config::ScreenScraperMedia.get() & supported));
		if (ImGui::Button(T("Download Selected Media")))
			gui_download_library_media(static_cast<u32>(config::ScreenScraperMedia.get()) & supported);
		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button(T("Retry Unmatched / Unavailable Media")))
			gui_download_library_media(static_cast<u32>(config::ScreenScraperMedia.get()) & supported, true);
		ImGui::EndDisabled();
		if (!config::FetchBoxart)
			ImGui::TextWrapped("%s", T("Enable Fetch Box Art below to allow downloads."));
		if (config::BoxartSourceMode.get() == static_cast<int>(BoxartSourceMode::PhysicalMediaOnly))
			ImGui::TextWrapped("%s", T("Choose Original Box Art or Custom Boxart below to use downloaded media."));
		ImGui::TextWrapped("%s", T("Only unambiguous Dreamcast title matches are downloaded. Games without a match keep their existing artwork."));
	}

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
				T("Wheel / Marquee"),
				T("Physical Media"),
				T("Fan Art"),
				T("Title Screen"),
				T("Banner"),
				T("Back Cover"),
				T("Spine"),
				T("Box Texture"),
			};
			constexpr int manual = static_cast<int>(config::LibraryCoverMediaMode::Manual);
			int selectedCoverMedia = config::LibraryCoverMedia.get();
			if (selectedCoverMedia < 0 || selectedCoverMedia >= static_cast<int>(config::LibraryCoverMediaMode::Count))
				selectedCoverMedia = 0;
			SettingsUI::PopupConfig libraryCoverMediaCfg {};
			libraryCoverMediaCfg.type = SettingsUI::PopupType::Options;
			libraryCoverMediaCfg.options.label = T("Custom Boxart Source");
			libraryCoverMediaCfg.options.icon = ICON_FA_IMAGE;
			libraryCoverMediaCfg.options.popupID = Tnop("LibraryCoverMediaPopup");
			libraryCoverMediaCfg.options.options = libraryCoverMediaSources;
			libraryCoverMediaCfg.options.optionCount = IM_ARRAYSIZE(libraryCoverMediaSources);
			libraryCoverMediaCfg.options.currentValue = &selectedCoverMedia;
			libraryCoverMediaCfg.options.valueWidth = 240.0f;
			libraryCoverMediaCfg.options.storageIndexMap = [manual](int index) { return index >= manual ? index + 1 : index; };
			libraryCoverMediaCfg.options.valueToString = [&](int value) {
				return value == manual ? T("Manual") : libraryCoverMediaSources[value > manual ? value - 1 : value];
			};
			libraryCoverMediaCfg.options.onChange = [](int value) {
				config::LibraryCoverMedia = value;
				return true;
			};
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

		SettingsUI::PopupConfig hoverDelayCfg {};
		hoverDelayCfg.type = SettingsUI::PopupType::Slider;
		hoverDelayCfg.slider.label = T("Game Info Hover Delay");
		hoverDelayCfg.slider.icon = ICON_FA_CLOCK;
		hoverDelayCfg.slider.popupID = "LibraryGameInfoHoverDelayPopup";
		hoverDelayCfg.slider.description = T("Wait 1-10 seconds before showing game information when hovering over a game in Original or List views. The last position (11) turns hover panels off.");
		hoverDelayCfg.slider.currentValue = &config::LibraryGameInfoHoverDelay.get();
		hoverDelayCfg.slider.minValue = 1;
		hoverDelayCfg.slider.maxValue = 11;
		hoverDelayCfg.slider.defaultValue = 4;
		hoverDelayCfg.slider.format = "%d s";
		hoverDelayCfg.slider.valueWidth = 220.0f;
		hoverDelayCfg.slider.sliderWidth = 320.0f;
		hoverDelayCfg.slider.valueFormatter = [](int value) {
			return value >= 11 ? std::string(T("Off")) : std::to_string(value) + " s";
		};
		hoverDelayCfg.slider.onValueChange = [] { config::LibraryGameInfoHoverDelay.save(); };
		RenderGeneralPopupSettingRow(
			"LibraryGameInfoHoverDelaySetting",
			T("Set the game info hover delay, or turn hover panels off."),
			hoverDelayCfg);

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
			T("Automatically download missing covers for the basic library views."),
			static_cast<bool>(config::FetchBoxart),
			[](bool enabled) { config::FetchBoxart = enabled; },
			T(
				"Fetch Box Art\n"
				"Automatically downloads covers, hover miximages, title logos and game information from the selected provider.\n"
				"Selecting a library theme automatically downloads the additional media it requires.\n"
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
				"When enabled, the hidden Auto-Save slot loads automatically when a game starts, if a save is available."
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
			T("Ask to save before leaving a game when automatic saving is off."),
			static_cast<bool>(config::SaveProtection),
			[](bool enabled) { config::SaveProtection = enabled; },
			T(
				"Save Protection\n"
				"When enabled, Hollycast asks you to save before leaving a game if Auto-Save on Exit is off and save states "
				"are allowed."
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




void renderAdvancedTab()
{
	ScopedTwoLineRowStyle advancedRowStyle(12.0f, true, 0.5f);

	// Updates Section
	ImGui::Spacing();
	if (RenderCollapsingHeader("UpdatesSection", ICON_FA_DOWNLOAD, T("Updates"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"CheckForUpdatesOnStartup",
			ICON_FA_DOWNLOAD,
			T("Check for updates on startup"),
			T("Automatically check after the startup animation"),
			cfgLoadBool("updates", "startup", true),
			[](bool enabled) {
				cfgSaveBool("updates", "startup", enabled);
				SaveSettings();
			},
			T(
				"Check for updates on startup\n"
				"Checks Hollycast's selected update track after the startup intro.\n\n"
				"If an update is available, Hollycast shows its details. Download and installation start only after you confirm."
			));
	}

	if (RenderCollapsingHeader("RewindSection", ICON_FA_BACKWARD, T("Rewind"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		RenderGeneralToggleSettingRow(
			"RewindEnable",
			ICON_FA_BACKWARD,
			T("Enable Rewind"),
			T("Keep recent gameplay in memory. Hold your mapped Rewind button to rewind (Backspace on a new keyboard mapping; assign it under Controls if your saved mapping has none)."),
			static_cast<bool>(config::RewindEnable),
			[](bool enabled) { config::RewindEnable.set(enabled); },
			T("Rewind\n"
				"This setting takes effect the next time a game starts and can only be changed while no game is running.\n\n"
				"When available, Rewind uses the shared emulation/rendering thread and may reduce performance. It is unavailable for Hikaru, System SP, "
				"Hardcore mode, online or multiboard play, NetDIMM cartridges, arcade card readers or medal hoppers, "
				"host serial output, passthrough hardware, and phone sharing."),
			game_started);

		static constexpr int rewindIntervals[] = { 100, 250, 500, 1000 };
		const char* rewindIntervalLabels[] = { T("0.1 s"), T("0.25 s"), T("0.5 s"), T("1 s") };
		char customIntervalLabel[16];
		SettingsUI::PopupConfig rewindIntervalCfg {};
		rewindIntervalCfg.type = SettingsUI::PopupType::Options;
		rewindIntervalCfg.options.label = T("Snapshot Interval");
		rewindIntervalCfg.options.icon = ICON_FA_CLOCK;
		rewindIntervalCfg.options.popupID = Tnop("RewindIntervalPopup");
		rewindIntervalCfg.options.options = rewindIntervalLabels;
		rewindIntervalCfg.options.optionCount = IM_ARRAYSIZE(rewindIntervalLabels);
		rewindIntervalCfg.options.currentValue = &config::RewindInterval.get();
		rewindIntervalCfg.options.storageIndexMap = [](int index) {
			return index >= 0 && index < IM_ARRAYSIZE(rewindIntervals) ? rewindIntervals[index] : 250;
		};
		rewindIntervalCfg.options.valueToString = [&rewindIntervalLabels, &customIntervalLabel](int milliseconds) -> const char* {
			// Match the effective interval used by rewind for hand-edited configurations.
			milliseconds = std::clamp(milliseconds, rewindIntervals[0], rewindIntervals[IM_ARRAYSIZE(rewindIntervals) - 1]);
			for (int i = 0; i < IM_ARRAYSIZE(rewindIntervals); ++i)
				if (rewindIntervals[i] == milliseconds)
					return rewindIntervalLabels[i];
			std::snprintf(customIntervalLabel, sizeof(customIntervalLabel), "%d.%03d s",
				milliseconds / 1000, milliseconds % 1000);
			return customIntervalLabel;
		};
		rewindIntervalCfg.options.valueWidth = 170.0f;
		RenderGeneralPopupSettingRow(
			Tnop("RewindIntervalSetting"),
			T("Time between snapshots in guest time."),
			rewindIntervalCfg,
			T("Choose the guest-time interval between rewind snapshots. Changes clear recorded history, so this can only be changed while no game is running."),
			game_started);

		const char* rewindCompressionLabels[] = { T("Fast"), T("Balanced"), T("Maximum") };
		SettingsUI::PopupConfig rewindCompressionCfg {};
		rewindCompressionCfg.type = SettingsUI::PopupType::Options;
		rewindCompressionCfg.options.label = T("Compression");
		rewindCompressionCfg.options.icon = ICON_FA_MEMORY;
		rewindCompressionCfg.options.popupID = Tnop("RewindCompressionPopup");
		rewindCompressionCfg.options.options = rewindCompressionLabels;
		rewindCompressionCfg.options.optionCount = IM_ARRAYSIZE(rewindCompressionLabels);
		rewindCompressionCfg.options.currentValue = &config::RewindCompression.get();
		rewindCompressionCfg.options.valueWidth = 170.0f;
		RenderGeneralPopupSettingRow(
			Tnop("RewindCompressionSetting"),
			T("Trade capture speed for memory. Higher compression can reduce memory use for the target history window, but costs more CPU per snapshot."),
			rewindCompressionCfg,
			T("Fast favors smooth gameplay. Balanced and Maximum reduce memory use and can cost performance. Changes clear recorded history, so this can only be changed while no game is running."),
			game_started);

		SettingsUI::PopupConfig rewindDurationCfg {};
		rewindDurationCfg.type = SettingsUI::PopupType::Slider;
		rewindDurationCfg.slider.label = T("History Duration");
		rewindDurationCfg.slider.icon = ICON_FA_HOURGLASS_HALF;
		rewindDurationCfg.slider.popupID = Tnop("RewindDurationPopup");
		rewindDurationCfg.slider.description = T("Target length of recorded gameplay.");
		rewindDurationCfg.slider.currentValue = &config::RewindDuration.get();
		rewindDurationCfg.slider.minValue = 5;
		rewindDurationCfg.slider.maxValue = 120;
		rewindDurationCfg.slider.defaultValue = 30;
		rewindDurationCfg.slider.format = "%d s";
		rewindDurationCfg.slider.valueWidth = 170.0f;
		RenderGeneralPopupSettingRow(
			Tnop("RewindDurationSetting"),
			T("How much gameplay to keep. Memory limits and skipped captures can shorten the recorded history."),
			rewindDurationCfg, nullptr, game_started);
	}

	// CPU & Emulation Section
	if (RenderCollapsingHeader("CPU&EmulationSection", ICON_FA_MICROCHIP, T("CPU & Emulation"), ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (settings.platform.isHikaru()) {
			RenderGeneralInfoRow("CpuModeSetting", ICON_FA_MICROCHIP, T("CPU Mode"),
				T("Hikaru uses the interpreter; CPU mode applies to Dreamcast."), T("Interpreter"));
		}
#if FEAT_SHREC != DYNAREC_NONE
		// CPU Mode
		if (!settings.platform.isHikaru())
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

#endif
#if FEAT_SHREC == DYNAREC_NONE
		if (settings.platform.isHikaru())
#endif
		// SH4 Clock
		{
			const bool hikaru = settings.platform.isHikaru();
			static int sh4ClockTemp = config::Sh4Clock.get();
			if (hikaru) {
				sh4ClockTemp = std::clamp(config::Sh4Clock.get(), 100, 300);
				if (sh4ClockTemp != config::Sh4Clock.get())
					config::Sh4Clock.set(sh4ClockTemp);
			}

			SettingsUI::PopupSliderConfig sh4ClockCfg {};
			sh4ClockCfg.label = T("SH4 Clock");
			sh4ClockCfg.icon = ICON_FA_GAUGE_HIGH;
			sh4ClockCfg.popupID = Tnop("Sh4ClockPopup");
			sh4ClockCfg.description = hikaru ? T(
				"Adjust both Hikaru SH4 CPU clocks. Video and audio clocks stay unchanged.\n"
				"Higher settings require more CPU work and can change game timing.") : T(
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
				hikaru ? T("Adjust both Hikaru SH4 CPU clocks; audio and video clocks stay unchanged.")
				       : T("Over/Underclock the main SH4 CPU."),
				sh4ClockPopupCfg,
				hikaru ? T(
					"SH4 Clock\n"
					"Adjust both Hikaru SH4 CPU clocks. Video and audio clocks stay unchanged.\n\n"
					"The default is 200 MHz. Higher settings require more CPU work and can change game timing.") : T(
					"SH4 Clock\n"
					"Overclocks/underclocks the emulated Dreamcast CPU.\n\n"
					"Default is 200 MHz.\n"
					"Increasing the clock can help some CPU-limited games maintain full speed, but it can also introduce timing problems, bugs, or instability.\n"
					"Decreasing the clock can lower CPU requirements, but may reduce performance or cause glitches.\n\n"
					"If you change this, re-test gameplay with the FPS counter enabled and keep adjustments small."
				));
		}

#if FEAT_SHREC != DYNAREC_NONE
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

		const bool rewindForcingShared = game_started && rewind_state::requiresSharedThread()
			&& !config::ThreadedRendering;
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
				"On some drivers/devices it can introduce instability or odd timing issues.\n"
				"When Rewind is available and enabled, it uses the shared emulation/rendering thread, so multi-threading is disabled.\n\n"
				"If you see new crashes, visual glitches, or inconsistent frame pacing after enabling it, try turning it off and re-testing."
			),
			rewindForcingShared || gui_has_held_netplay_session());

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
			static_cast<bool>(config::GDB),
			[](bool enabled) { config::GDB.set(enabled); },
			T(
				"GDB Server\n"
				"Starts a GDB debug server for developers.\n\n"
				"By default, it accepts connections from this machine only.\n"
				"When enabled, Hollycast can pause startup and wait for a debugger to attach.\n"
				"Leave this disabled unless you are actively debugging."
			));

		// GDB Server options (shown when enabled)
		if (config::GDB.get())
		{
			ImGui::Indent();
			RenderGeneralToggleSettingRow(
				"GDBRemote",
				ICON_FA_LINK,
				T("Allow Remote Connections"),
				T("Allow GDB connections from other devices"),
				static_cast<bool>(config::GDBRemote),
				[](bool enabled) { config::GDBRemote.set(enabled); },
				T(
					"Allow Remote GDB Connections\n"
					"Allows GDB clients on other devices to connect over the network.\n\n"
					"The GDB server has no authentication. Enable this only on a trusted network.\n"
					"This setting takes effect the next time the GDB server starts."
				));

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
				const bool gdbPortChanged = ImGui::InputInt("##GDBPortInput", &gdbport);
#ifdef HOLLYCAST_TEST_HARNESS
				if (test_harness::ui::collectingInspection())
					test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", "settings.debug.gdb_port"},
						{"semantic_label", T("GDB Port")}, {"kind", "number"}, {"value", gdbport},
						{"read_only", false}});
#endif
				if (gdbPortChanged)
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
		RenderGeneralToggleSettingRow(
			"SplitTextureSheets",
			ICON_FA_IMAGE,
			T("Split Texture Sheets"),
			T("Extract and rebuild pieces of texture sheets."),
			static_cast<bool>(config::SplitTextureSheets),
			[](bool enabled) { config::SplitTextureSheets.set(enabled); },
			T("Enable for games that store several textures in one image.\n\n"
				"With Texture Dumping enabled, saves rectangular pieces and texture-atlas.json beside the original sheets.\n"
				"Copy the mapping, original sheets and edited pieces into the title's replacement folder.\n"
				"Every piece from a sheet must use the same scale, such as 2x or 4x.\n"
				"Sheets are rebuilt automatically when custom textures load. Uncertain regions remain in the original sheet."),
			game_started);
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








// Main entry point - renders the entire settings UI
void renderSettingsNew()
{
	ScrollSettingsFromController();
	if (!g_controllerPopupWasOpen && !IsAnySettingsPopupOpen()
			&& g_controllerSection != ControllerSection::None)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
		{
			ImGui::ClearActiveID();
			ImGui::NavMoveRequestCancel();
			if (g_tabSlideStart >= 0.0)
				SelectSettingsTab(g_tabSlideTarget);
			g_tabSlideStart = -1.0;
			g_focusSettingsContent = false;
			g_focusSettingsNavigation = g_controllerSection == ControllerSection::Content;
			g_focusSettingsBack = !g_focusSettingsNavigation;
		}
		// The navigation rail is for choosing a specific tab with A. Left and
		// right only change tabs after entering that tab's content pane.
		else if (!g_controllerEditing && g_controllerSection == ControllerSection::Content)
		{
			// The stick scrolls wide child lists while the D-pad still changes tabs.
			const ImGuiWindow* focused = GImGui->NavWindow;
			const bool stickScrollsHorizontally = focused != nullptr && focused->ScrollMax.x > 0.f && focused->ScrollbarX;
			const bool left = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)
					|| (!stickScrollsHorizontally && ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true));
			const bool right = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)
					|| (!stickScrollsHorizontally && ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true));
			if (left != right)
			{
				ImGui::NavMoveRequestCancel();
				if (g_tabSlideStart >= 0.0)
					SelectSettingsTab(g_tabSlideTarget);
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
			SelectSettingsTab(g_tabSlideTarget);
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

		const bool fromGame = game_started && !gui_has_held_netplay_session();
		const std::function<void()> exitSettings = [&]() {
			applyPendingSettings();

			if (fromGame)
				gui_setState(GuiState::Commands);
			else
				gui_setState(GuiState::Main);
		};

		const char* backLabel = fromGame ? T("Back to Game") : T("Back to library");

		// Split into left navigation rail and right content area
		renderNavigationRail(exitSettings, backLabel);
		ImGui::SameLine();
		renderContentArea();
		if (const auto chosen = themes::renderBrowser())
		{
			config::CustomTheme = *chosen;
			config::ThemeLayout = "";
			SaveSettings();
			g_applyThemeAfterSettings = true;
		}

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
