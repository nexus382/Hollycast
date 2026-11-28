// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "gui_util.h"
#include "imgui.h"

#include <cstdint>
#include <atomic>
#include <array>
#include <string>

#ifdef HOLLYCAST_TEST_HARNESS
#include "test_harness_ui.h"
#endif

extern ImFont* largeFont;

namespace SettingsNew {

#ifdef HOLLYCAST_TEST_HARNESS
void annotateTestToggle(const char* prefix, const char* id, const char* label, bool value);
void annotateTestFloat(const char* prefix, const char* id, const char* label,
		float value, float minimum, float maximum);
void annotateTestInteger(const char* prefix, const char* id, const char* label,
		int value, int minimum, int maximum);
void annotateTestVector2(const char* prefix, const char* id, const char* label,
		const char* widgetId, const ImVec2& value, float minimum, float maximum);
void annotateTestColor(const char* prefix, const char* id, const char* label,
		const char* widgetId, const float* value);
void annotateTestText(const char* prefix, const char* id, const char* label);
#endif

extern std::atomic<bool> g_textureFilterLiveTune;
extern float g_twoLineRowExtraHeightPx;
extern bool g_twoLineRowSeparatorsEnabled;
extern float g_twoLineRowSeparatorAlpha;
inline constexpr std::array<int, 19> sampleTextureFilterValues { 0, 19, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 16, 17, 18 };
inline constexpr std::array<int, 8> maxFilteredTextureSizeValues { 8, 16, 32, 64, 128, 256, 512, 1024 };

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

float SettingsLayoutScaled(float px);
ImVec2 SettingsLayoutSize(float width, float height);
float SettingsPreviewFontSize(ImFont* font);
float RightColumnX(float itemWidth, float rightPadding = 28.0f);
float TwoLineSettingContentHeight();
void RenderToggleSwitchVisual(bool value);
ImVec2 BeginTwoLineSettingRowContent();
bool BeginTwoLineSettingRow(const char* rowId, const char* tooltip = nullptr, bool disabled = false);
void EndTwoLineSettingRow();
void RenderTwoLineSettingDescription(const ImVec2& line1Start, const char* description);
void SetSettingsFooterText(const char* text);
bool RenderGeneralPopupSettingRow(const char* id, const char* description,
		SettingsUI::PopupConfig& popupCfg, const char* tooltip = nullptr, bool disabled = false,
		bool infoMarker = false);
void applyPendingSettings();
std::string cfgLoadStr(const std::string& section, const std::string& key, const std::string& def);
void cfgSaveStr(const std::string& section, const std::string& key, const std::string& value);
bool cfgLoadBool(const std::string& section, const std::string& key, bool def);
void cfgSaveBool(const std::string& section, const std::string& key, bool value);
void cfgSaveInt(const std::string& section, const std::string& key, int value);
int cfgLoadInt(const std::string& section, const std::string& key, int def);
bool SettingsIconButton(const char* id, const char* icon, const ImVec2& size);
ImVec4 SettingsErrorColor();
bool ControllerPopupCancelPressed();
void ShowFooterHelpMarker(const char* desc);
extern bool g_mapleDevicesChangedInSettings;
extern int g_controllerPopupDepth;
std::array<const char*, 19> getSampleTextureFilterLabels();
int getSampleTextureFilterOptionIndex(int value);
std::array<const char*, 8> getMaxFilteredTextureSizeLabels();
bool RenderCollapsingHeader(const char* id, const char* labelIcon, const char* labelText,
		ImGuiTreeNodeFlags flags = 0);

template <typename OnToggle>
bool RenderGeneralToggleSettingRow(const char* id, const char* icon, const char* label,
		const char* description, bool value, OnToggle&& onToggle, const char* tooltip = nullptr,
		bool disabled = false)
{
	ImGui::PushID(id);

	const char* rowTooltip = tooltip != nullptr ? tooltip : description;
	const bool rowActivated = BeginTwoLineSettingRow("##row", rowTooltip, disabled);
#ifdef HOLLYCAST_TEST_HARNESS
	const std::uint32_t rowItemId = test_harness::ui::collectingInspection() ? test_harness::ui::lastItemId() : 0;
#endif
	if (rowActivated)
	{
		value = !value;
		onToggle(value);
	}
#ifdef HOLLYCAST_TEST_HARNESS
	if (rowItemId != 0)
		test_harness::ui::annotateItem(rowItemId, {{"semantic_id", id ? id : ""}, {"semantic_label", label ? label : ""},
			{"kind", "toggle"}, {"value", value}});
#endif

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
void RenderGeneralTextInputSettingRow(const char* id, const char* icon, const char* label,
		const char* description, DrawInput&& drawInput, const char* tooltip = nullptr, bool disabled = false,
		float inputWidthPx = 220.0f, const char* controlKind = "text")
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
#ifdef HOLLYCAST_TEST_HARNESS
		if (test_harness::ui::collectingInspection())
			test_harness::ui::annotateItem(test_harness::ui::lastItemId(), {{"semantic_id", id ? id : ""},
				{"semantic_label", label ? label : ""}, {"kind", controlKind ? controlKind : "text"}});
#endif
	}

	RenderTwoLineSettingDescription(line1Start, description);
	EndTwoLineSettingRow();
	ImGui::PopID();
}

} // namespace SettingsNew
