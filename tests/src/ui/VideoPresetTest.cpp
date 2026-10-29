// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "imgui_internal.h"
#include "oslib/i18n.h"
#include "ui/settings_new.h"
#include "ui/video_preset.h"
#include <string>

class VideoPresetTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		config::Settings::instance().reset();
		previousContext = ImGui::GetCurrentContext();
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}

	void TearDown() override
	{
		ImGui::DestroyContext();
		ImGui::SetCurrentContext(previousContext);
		config::Settings::instance().reset();
	}

	std::string renderVideoTab()
	{
		ImGui::NewFrame();
		ImGui::SetNextWindowSize(ImVec2(800, 600));
		ImGui::Begin("Video preset test");
		// Exercise the real preset selector without opening unrelated settings.
		for (const char* section : {
			"###GraphicsAPISection", "###TransparentSortingSection",
			"###RenderingOptionsSection", "###AspectRatioSection",
			"###PerPixelSettingsSection", "###PerformanceSection",
			"###AdvancedSection", "###VideoRoutingSection" })
			ImGui::GetStateStorage()->SetInt(ImGui::GetID(section), 0);
		ImGui::LogToBuffer(0);
		SettingsNew::renderVideoTab();
		// Capture the displayed combo preview, not just the detector's result.
		const std::string text = ImGui::GetCurrentContext()->LogBuffer.c_str();
		ImGui::LogFinish();
		ImGui::End();
		ImGui::EndFrame();
		return text;
	}

	ImGuiContext* previousContext = nullptr;
};

TEST_F(VideoPresetTest, EnablingReconstructionUpdatesDisplayedPreset)
{
	using namespace SettingsNew;
	applyVideoPreset(*getPresetByLevel(VideoPresetLevel::Low));
	ASSERT_EQ(detectCurrentPreset(), VideoPresetLevel::Low);
	EXPECT_NE(renderVideoTab().find(std::string("{ ") + i18n::T("Low") + " }"), std::string::npos);

	config::SampleTextureFilter = 19; // Nearest-Neighbor reconstruction.
	ASSERT_EQ(detectCurrentPreset(), VideoPresetLevel::Custom);
	EXPECT_NE(renderVideoTab().find(std::string("{ ") + i18n::T("Custom") + " }"), std::string::npos);
	// The label must remain Custom on subsequent frames.
	EXPECT_NE(renderVideoTab().find(std::string("{ ") + i18n::T("Custom") + " }"), std::string::npos);
}

TEST_F(VideoPresetTest, ReapplyingLowDisablesReconstructionAndUpdatesDisplayedPreset)
{
	using namespace SettingsNew;
	applyVideoPreset(*getPresetByLevel(VideoPresetLevel::Low));
	config::SampleTextureFilter = 19;
	ASSERT_EQ(detectCurrentPreset(), VideoPresetLevel::Custom);
	EXPECT_NE(renderVideoTab().find(std::string("{ ") + i18n::T("Custom") + " }"), std::string::npos);

	applyVideoPreset(*getPresetByLevel(VideoPresetLevel::Low));
	EXPECT_EQ(config::SampleTextureFilter.get(), 0);
	EXPECT_EQ(detectCurrentPreset(), VideoPresetLevel::Low);
	EXPECT_NE(renderVideoTab().find(std::string("{ ") + i18n::T("Low") + " }"), std::string::npos);
}
