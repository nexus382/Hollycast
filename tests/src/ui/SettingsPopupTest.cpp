// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/gui_util.h"
#include "ui/gui_font.h"
#include "ui/boxart/screenscraper.h"
#include "ui/settings_new.h"
#include "ui/theme.h"
#include "ui/exit_save_prompt.h"
#include <array>
#include <chrono>
#include <filesystem>

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

TEST_F(SettingsPopupTest, ExitSavePromptKeepsCancelVisibleAndReachableInNarrowWindows)
{
	const char* message = "Do you want to save state before returning to the library?";
	const char* save = "Save & Exit";
	const char* discard = "Exit Without Saving";
	const char* cancel = "Cancel";
	for (float width : {320.f, 600.f, 1400.f})
	{
		SCOPED_TRACE(width);
		ImGui::GetIO().DisplaySize = ImVec2(width, 900);
		exit_prompt::Choice choice = exit_prompt::Choice::None;
		ImGuiWindow* dialog = nullptr;
		ImRect cancelRect;
		auto draw = [&](bool open) {
			ImGui::NewFrame();
			ImGui::Begin("Host");
			if (open) ImGui::OpenPopup("Exit test");
			ImGui::PushFont(nullptr, 24.f);
			exit_prompt::sizeNextWindow(message, save, discard, cancel);
			ImGui::SetNextWindowPos(ImGui::GetIO().DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal("Exit test", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			{
				dialog = ImGui::GetCurrentWindow();
				choice = exit_prompt::drawChoices(message, save, discard, cancel);
				cancelRect = GImGui->LastItemData.Rect;
				if (choice != exit_prompt::Choice::None) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			ImGui::PopFont();
			ImGui::End();
			ImGui::Render();
		};
		draw(true);
		draw(false);
		ASSERT_NE(dialog, nullptr);
		EXPECT_GE(dialog->Pos.x, 0.f);
		EXPECT_LE(dialog->Pos.x + dialog->Size.x, width);
		EXPECT_TRUE(dialog->InnerRect.Contains(cancelRect));
		EXPECT_FLOAT_EQ(dialog->ScrollMax.x, 0.f);
		ImGui::ActivateItemByID(dialog->GetID(cancel));
		draw(false);
		EXPECT_EQ(choice, exit_prompt::Choice::Cancel);
	}
}

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

TEST_F(SettingsPopupTest, FilteredProviderChoicesRetainTheirSharedStorageBits)
{
	cfg.options.optionCount = 2;
	labels[0] = "Cover"; labels[1] = "Screenshot";
	cfg.options.storageIndexMap = [](int index) { return index == 0 ? 0 : 6; };
	frame(true); frame();
	toggle(1); button("Apply");
	EXPECT_EQ(value, 1 | (1 << 6));
	EXPECT_EQ(changes, 1);
}

class SettingsPageControllerTest : public SettingsPopupTest
{
protected:
	void SetUp() override
	{
		SettingsPopupTest::SetUp();
		oldData = get_writable_data_path("");
		oldConfig = get_writable_config_path("");
		oldTheme = config::CustomTheme.get();
		oldLayout = config::ThemeLayout.get();
		oldDisplayStyle = config::LibraryDisplayStyle.get();
		oldTab = SettingsNew::g_state.currentTab;
		root = std::filesystem::temp_directory_path() / ("hollycast-settings-controller-"
				+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		set_user_data_dir(root.string());
		set_user_config_dir(root.string());
		config::CustomTheme = "pocket-dimension";
		config::ThemeLayout = "";
		themes::reload();
		SettingsNew::resetState();
		SettingsNew::openTab(SettingsNew::SettingsTab::Library);
	}
	void TearDown() override
	{
		config::CustomTheme = oldTheme;
		config::ThemeLayout = oldLayout;
		config::LibraryDisplayStyle = oldDisplayStyle;
		set_user_data_dir(oldData);
		set_user_config_dir(oldConfig.empty() ? "." : oldConfig);
		themes::reload();
		SettingsNew::resetState();
		SettingsNew::openTab(oldTab);
		std::error_code ec;
		std::filesystem::remove_all(root, ec);
		SettingsPopupTest::TearDown();
	}
	void pageFrame()
	{
		SettingsNew::prepareControllerNavigation();
		ImGui::NewFrame();
		SettingsNew::renderSettingsNew();
		ImGui::Render();
	}
	ImGuiWindow* content()
	{
		for (ImGuiWindow* window : GImGui->Windows)
			if (window->Active && std::string(window->Name).find("/ContentArea_") != std::string::npos)
				return window;
		return nullptr;
	}
	void focusRow(const char* setting)
	{
		ASSERT_NE(content(), nullptr);
		const ImGuiID scope = content()->GetID(setting);
		ImGui::SetFocusID(ImHashStr("##row", 0, scope), content());
		ImGui::SetNavCursorVisible(true);
		pageFrame();
	}
	std::filesystem::path root;
	std::string oldData, oldConfig, oldTheme, oldLayout;
	SettingsNew::SettingsTab oldTab;
	int oldDisplayStyle = 0;
};

TEST_F(SettingsPageControllerTest, EveryNewLibraryRowOpensAndKeepsItsDialogWithCrossHeld)
{
	pageFrame(); pageFrame();
	for (const char* setting : {"LibraryThemeSetting", "LibraryLayoutSetting", "ThemeIconMediaSetting", "ThemeIconSizeSetting"})
	{
		SCOPED_TRACE(setting);
		focusRow(setting);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
		pageFrame();
		ASSERT_NE(popup(), nullptr);
		const ImGuiID dialog = popup()->ID;
		for (int i = 0; i < 5; ++i)
		{
			pageFrame();
			ASSERT_NE(popup(), nullptr);
			EXPECT_EQ(popup()->ID, dialog);
		}
		ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
		pageFrame();
		ASSERT_NE(popup(), nullptr);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
		pageFrame();
		EXPECT_EQ(popup(), nullptr);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
		pageFrame();
		EXPECT_EQ(SettingsNew::g_state.currentTab, SettingsNew::SettingsTab::Library);
	}
}

TEST_F(SettingsPageControllerTest, FirstRunOffersDemoDiscOriginalViewsAndEveryIncludedTheme)
{
	config::CustomTheme.reset();
	EXPECT_EQ(config::CustomTheme.get(), "demo-disc");
	ASSERT_NE(themes::current(), nullptr);
	EXPECT_EQ(themes::current()->id, "demo-disc");
	ImGuiID combo = 0;
	const auto frameChoice = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(620, 680));
		ImGui::Begin("First-run theme choice");
		combo = ImGui::GetID("##setup-library-theme");
		const bool changed = drawSetupThemePicker();
		ImGui::End();
		ImGui::Render();
		return changed;
	};
	frameChoice(); frameChoice();
	for (const char* original : {"Original list view", "Original icon view"})
	{
		SCOPED_TRACE(original);
		ImGui::ActivateItemByID(combo);
		frameChoice(); frameChoice();
		ASSERT_NE(popup(), nullptr);
		ImGui::ActivateItemByID(popup()->GetID(i18n::T(original)));
		EXPECT_TRUE(frameChoice());
		EXPECT_TRUE(config::CustomTheme.get().empty());
		EXPECT_EQ(config::LibraryDisplayStyle.get(), std::string(original) == "Original list view" ? 1 : 0);
	}
	for (const auto& entry : themes::catalog())
	{
		if (!entry.error.empty()) continue;
		SCOPED_TRACE(entry.id);
		ImGui::ActivateItemByID(combo);
		frameChoice(); frameChoice();
		ASSERT_NE(popup(), nullptr);
		const ImGuiID scope = popup()->GetID(entry.id.c_str());
		ImGui::ActivateItemByID(ImHashStr(entry.name.c_str(), 0, scope));
		EXPECT_TRUE(frameChoice());
		EXPECT_EQ(config::CustomTheme.get(), entry.id);
		EXPECT_TRUE(config::ThemeLayout.get().empty());
	}
}

TEST_F(SettingsPopupTest, SharedEmptyLibraryFolderButtonAcceptsControllerConfirm)
{
	ImGuiID button = 0;
	const auto frameEmpty = [&] {
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(640, 400));
		ImGui::Begin("Empty library");
		const bool clicked = drawEmptyLibraryPrompt();
		button = ImHashStr(i18n::T("Add Game Folder"), 0, ImGui::GetID("empty"));
		ImGui::End();
		ImGui::Render();
		return clicked;
	};
	EXPECT_FALSE(frameEmpty());
	EXPECT_FALSE(frameEmpty());
	ASSERT_NE(button, 0u);
	ImGui::ActivateItemByID(button);
	EXPECT_TRUE(frameEmpty());
}
