// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "setup_checklist_lifecycle.h"
#include "imgui.h"

// The checklist and existing dialogs share an ImGui popup stack. Do not open a
// competing popup until the current one has closed; retain the request meanwhile.
inline bool openSetupChecklistIfUnblocked(SetupChecklistLifecycle& lifecycle, const char* title)
{
	lifecycle.observePopup(ImGui::IsPopupOpen(title));
	const bool popupOpen = ImGui::IsPopupOpen(nullptr,
		ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
	if (!lifecycle.takeOpenRequest(popupOpen))
		return false;
	ImGui::OpenPopup(title);
	return true;
}
