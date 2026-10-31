// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "gui_util.h"
#include <algorithm>

namespace exit_prompt
{
enum class Choice { None, Save, Discard, Cancel };

inline float buttonWidth(const char* save, const char* discard, const char* cancel)
{
	return std::max({ImGui::CalcTextSize(save).x, ImGui::CalcTextSize(discard).x,
			ImGui::CalcTextSize(cancel).x}) + uiScaled(44.f);
}

inline void sizeNextWindow(const char* message, const char* save, const char* discard, const char* cancel)
{
	const float padding = ImGui::GetStyle().WindowPadding.x * 2;
	const float preferred = std::max(ImGui::CalcTextSize(message).x,
			buttonWidth(save, discard, cancel) * 3 + uiScaled(40.f)) + padding;
	const float available = std::max(1.f, ImGui::GetIO().DisplaySize.x - uiScaled(24.f));
	ImGui::SetNextWindowSize(ImVec2(std::min(preferred, available), 0), ImGuiCond_Always);
}

inline Choice drawChoices(const char* message, const char* save, const char* discard, const char* cancel)
{
	ImGui::TextWrapped("%s", message);
	ImGui::NewLine();
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
	const float available = ImGui::GetContentRegionAvail().x;
	const float preferred = buttonWidth(save, discard, cancel);
	const bool stacked = preferred * 3 + ImGui::GetStyle().ItemSpacing.x * 2 > available + 1.f;
	const ImVec2 size(stacked ? available : preferred, 0);
	Choice choice = Choice::None;
	if (ImGui::Button(save, size)) choice = Choice::Save;
	if (!stacked) ImGui::SameLine();
	if (ImGui::Button(discard, size)) choice = Choice::Discard;
	if (!stacked) ImGui::SameLine();
	if (ImGui::Button(cancel, size)) choice = Choice::Cancel;
	ImGui::PopStyleVar(2);
	return choice;
}
}
