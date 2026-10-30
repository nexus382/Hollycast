// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/gui_util.h"
#include "ui/gui_font.h"
#include "ui/boxart/screenscraper.h"
#include <array>

class SettingsPopupTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previousContext = ImGui::GetCurrentContext();
		previousFonts = {largeFont, settingsTitleFont, settingsValueFont, settingsRightValueFont, settingsIconFont};
		previousScale = settings.display.uiScale;
		settings.display.uiScale = 1.0f;
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(1280, 1000);
		io.DeltaTime = 1.0f / 60;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		ImFont *font = io.Fonts->AddFontDefault();
		largeFont = settingsTitleFont = settingsValueFont = settingsRightValueFont = settingsIconFont = font;
		unsigned char *pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		for (size_t i = 0; i < labels.size(); ++i)
			labels[i] = screenscraper::mediaTypes[i].label;
		cfg.type = SettingsUI::PopupType::Options;
		cfg.options.label = "Images to Download";
		cfg.options.icon = "I";
		cfg.options.popupID = "MediaChoices";
		cfg.options.description = "Choose the image types to download.";
		cfg.options.options = labels.data();
		cfg.options.optionCount = static_cast<int>(labels.size());
		cfg.options.currentValue = &value;
		cfg.options.multiSelect = true;
		cfg.options.valueToString = [](int) { return "Selected images"; };
		cfg.options.selectionPresets = {{"All", static_cast<int>(screenscraper::allMedia)}, {"None", 0}};
		cfg.options.onChange = [&](int) { ++changes; return true; };
	}
	void TearDown() override
	{
		ImGui::DestroyContext();
		ImGui::SetCurrentContext(previousContext);
		largeFont = previousFonts[0];
		settingsTitleFont = previousFonts[1];
		settingsValueFont = previousFonts[2];
		settingsRightValueFont = previousFonts[3];
		settingsIconFont = previousFonts[4];
		settings.display.uiScale = previousScale;
	}
	void frame(bool open = false)
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::Begin("Library Settings");
		if (open) ImGui::OpenPopup(cfg.options.popupID);
		SettingsUI::SettingPopup(cfg);
		ImGui::End();
		ImGui::Render();
	}
	ImGuiWindow *popup()
	{
		return GImGui->OpenPopupStack.empty() ? nullptr : GImGui->OpenPopupStack.back().Window;
	}
	ImGuiWindow *choices()
	{
		ImGuiWindow *owner = popup();
		if (!owner) return nullptr;
		for (ImGuiWindow *window : GImGui->Windows)
			if (window->ParentWindow == owner && window->ChildId == owner->GetID("##choices"))
				return window;
		return nullptr;
	}
	void toggle(int index)
	{
		ASSERT_NE(choices(), nullptr);
		// Activate the real checkbox, including its index scope, through ImGui navigation.
		const ImGuiID id = ImHashStr(labels[index], 0, choices()->GetID(index));
		ImGui::ActivateItemByID(id);
		frame();
	}
	void button(const char *label)
	{
		ASSERT_NE(popup(), nullptr);
		ImGui::ActivateItemByID(popup()->GetID(i18n::T(label)));
		frame();
	}
	ImGuiContext *previousContext = nullptr;
	std::array<ImFont*, 5> previousFonts{};
	float previousScale = 1;
	std::array<const char*, screenscraper::mediaTypes.size()> labels{};
	SettingsUI::PopupConfig cfg{};
	int value = 1;
	int changes = 0;
};

TEST_F(SettingsPopupTest, MultipleCheckboxesCommitTogetherOnlyOnApply)
{
	frame(true);
	frame();
	toggle(1);
	toggle(3);
	ASSERT_NE(popup(), nullptr);
	EXPECT_EQ(value, 1);
	EXPECT_EQ(changes, 0);
	button("Apply");
	EXPECT_EQ(value, 1 | (1 << 1) | (1 << 3));
	EXPECT_EQ(changes, 1);
	EXPECT_EQ(popup(), nullptr);
}

TEST_F(SettingsPopupTest, CancelDiscardsDraftAndReopeningUsesSavedSelection)
{
	frame(true);
	frame();
	toggle(2);
	button("Cancel");
	EXPECT_EQ(value, 1);
	EXPECT_EQ(changes, 0);
	frame(true);
	frame();
	button("Apply");
	EXPECT_EQ(value, 1);
}

TEST_F(SettingsPopupTest, PresetIncludesEveryImageAndBackCancelsIt)
{
	frame(true);
	frame();
	ASSERT_NE(popup(), nullptr);
	const int presetIndex = 0;
	ImGui::ActivateItemByID(ImHashStr("All", 0, popup()->GetID(presetIndex)));
	frame();
	EXPECT_EQ(value, 1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
	frame();
	EXPECT_EQ(popup(), nullptr);
	EXPECT_EQ(value, 1);
	EXPECT_EQ(changes, 0);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
	frame(true);
	frame();
	ImGui::ActivateItemByID(ImHashStr("All", 0, popup()->GetID(presetIndex)));
	frame();
	button("Apply");
	EXPECT_EQ(value, static_cast<int>(screenscraper::allMedia));
}

TEST_F(SettingsPopupTest, SmallWindowScrollsChoicesWithinBoundedDialog)
{
	ImGui::GetIO().DisplaySize = ImVec2(640, 480);
	frame(true);
	frame();
	ASSERT_NE(popup(), nullptr);
	ASSERT_NE(choices(), nullptr);
	EXPECT_LE(popup()->Size.x, 640 * 0.9f + 1);
	EXPECT_LE(popup()->Size.y, 480 * 0.85f + 1);
	EXPECT_GT(choices()->ScrollMax.y, 0);
	button("Cancel");
	EXPECT_EQ(popup(), nullptr);
}

TEST_F(SettingsPopupTest, SingleChoiceStillAppliesImmediately)
{
	cfg.options.multiSelect = false;
	frame(true);
	frame();
	ASSERT_NE(popup(), nullptr);
	ImGui::ActivateItemByID(popup()->GetID(labels[2]));
	frame();
	EXPECT_EQ(value, 2);
	EXPECT_EQ(changes, 1);
	EXPECT_EQ(popup(), nullptr);
}
