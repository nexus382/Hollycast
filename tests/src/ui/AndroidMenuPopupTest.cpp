// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/gui_util.h"
#include "ui/settings.h"
#include "log/LogManager.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif

// Compile the real Android menu on the host so CI exercises its fullscreen,
// touch and focus behavior with real ImGui frames. Load dependencies first to
// retain the host ABI, and isolate the menu's symbols from the normal build.
#pragma push_macro("__ANDROID__")
#pragma push_macro("TARGET_MAC")
#ifndef __ANDROID__
#define __ANDROID__
#endif
#undef TARGET_MAC
#define GuiMenu AndroidGuiMenu
#include "../../../core/ui/gui_menu.cpp"
#undef GuiMenu
#pragma pop_macro("TARGET_MAC")
#pragma pop_macro("__ANDROID__")

class AndroidMenuPopupTest : public ::testing::TestWithParam<const char*>
{
protected:
	void SetUp() override
	{
		previousContext = ImGui::GetCurrentContext();
		previousState = gui_state;
		previousFont = settingsTitleFont;
		previousVisibility = config::MenuBarVisibility;
		gui_state = GuiState::Closed; // Live Tune runs while the game is unpaused.
		settingsTitleFont = nullptr;
		config::MenuBarVisibility = static_cast<int>(config::MenuBarMode::Auto);
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
		AndroidGuiMenu::initialize();
	}

	void TearDown() override
	{
		AndroidGuiMenu::shutdown();
		ImGui::DestroyContext();
		ImGui::SetCurrentContext(previousContext);
		gui_state = previousState;
		settingsTitleFont = previousFont;
		config::MenuBarVisibility = previousVisibility;
	}

	void frame(const char* label = "Sample Texture Filter")
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 350));
		ImGui::SetNextWindowSize(ImVec2(800, 250));
		ImGui::Begin("Texture Filtering Live Tune");
		const bool open = ImGui::BeginCombo(label, "Current");
		if (!open)
			comboCenter = ImGui::GetItemRectMin() + ImVec2(20, 10);
		if (open)
		{
			if (ImGui::Selectable("New value"))
				selected = true;
			optionCenter = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
			ImGui::EndCombo();
		}
		ImGui::End();
		// Match gui_draw_osd(): the live panel is submitted before the top bar.
		AndroidGuiMenu::renderMainMenuBar();
		ImGui::Render();
		popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
	}

	void touch(const ImVec2& pos, bool down, const char* label = "Sample Texture Filter")
	{
		ImGui::GetIO().AddMousePosEvent(pos.x, pos.y);
		ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, down);
		// ImGui deliberately processes touchscreen motion and button changes on
		// separate frames, since a touch has no preceding hover event.
		frame(label);
		frame(label);
	}

	ImGuiContext* previousContext = nullptr;
	GuiState previousState = GuiState::Closed;
	ImFont* previousFont = nullptr;
	int previousVisibility = 0;
	ImVec2 comboCenter;
	ImVec2 optionCenter;
	bool popupOpen = false;
	bool selected = false;
};

TEST_P(AndroidMenuPopupTest, HiddenBarDoesNotCloseLiveTuneDropdown)
{
	const char* label = GetParam();
	frame(label);
	frame(label);
	ASSERT_EQ(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	touch(comboCenter, true, label);
	touch(comboCenter, false, label);
	ASSERT_TRUE(popupOpen);
	EXPECT_EQ(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	for (int i = 0; i < 3; ++i)
		frame(label);
	ASSERT_TRUE(popupOpen);
	EXPECT_EQ(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	touch(optionCenter, true, label);
	touch(optionCenter, false, label);
	EXPECT_TRUE(selected);
	EXPECT_FALSE(popupOpen);
}

INSTANTIATE_TEST_SUITE_P(LiveTune, AndroidMenuPopupTest,
	::testing::Values("Sample Texture Filter", "Filter Resolution Limit"));

TEST_F(AndroidMenuPopupTest, VisibleBarStaysPutWhileLiveTuneDropdownIsOpen)
{
	config::MenuBarVisibility = static_cast<int>(config::MenuBarMode::On);
	frame();
	frame();
	touch(comboCenter, true);
	touch(comboCenter, false);
	ASSERT_TRUE(popupOpen);
	config::MenuBarVisibility = static_cast<int>(config::MenuBarMode::Auto);
	ImGui::GetIO().DeltaTime = 6.0f;
	frame();
	EXPECT_TRUE(popupOpen);
	EXPECT_GT(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	// Explicit visibility locks still take precedence over popup ownership.
	config::MenuBarVisibility = static_cast<int>(config::MenuBarMode::Off);
	frame();
	EXPECT_EQ(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	EXPECT_TRUE(popupOpen);
}

TEST_F(AndroidMenuPopupTest, TopMenuStaysOpenAfterRevealTimerExpires)
{
	frame();
	frame();
	touch(ImVec2(15, 8), true);
	touch(ImVec2(15, 8), false);
	ASSERT_GT(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	// The first tap reveals the bar; the next opens its File menu.
	touch(ImVec2(15, 8), true);
	touch(ImVec2(15, 8), false);
	ASSERT_TRUE(popupOpen);
	ImGui::GetIO().DeltaTime = 6.0f;
	frame();
	EXPECT_TRUE(popupOpen);
	EXPECT_GT(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
	// An outside tap still dismisses the popup and lets the expired bar hide.
	touch(ImVec2(700, 300), true);
	touch(ImVec2(700, 300), false);
	frame();
	EXPECT_FALSE(popupOpen);
	EXPECT_EQ(AndroidGuiMenu::mainMenuBarHeight(), 0.f);
}
