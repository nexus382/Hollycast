// Portions Copyright 2026 The Hollycast Authors
#pragma once
enum class GuiState
{
	Closed,
	Commands,
	Main,
	Onboarding,
	Loading,
	NetworkStart
};
extern GuiState gui_state;
inline bool gui_is_open()
{
	return gui_state != GuiState::Closed;
}
