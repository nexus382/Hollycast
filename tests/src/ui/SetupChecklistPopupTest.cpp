// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/setup_checklist_popup.h"

class SetupChecklistPopupTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previous = ImGui::GetCurrentContext();
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
		ImGui::SetCurrentContext(previous);
	}
	ImGuiContext* previous = nullptr;
};

TEST_F(SetupChecklistPopupTest, ExistingDialogIsPreservedUntilItCloses)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	ImGui::NewFrame();
	ImGui::Begin("Library");
	ImGui::OpenPopup("Warning");
	EXPECT_FALSE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	EXPECT_TRUE(ImGui::IsPopupOpen("Warning"));
	EXPECT_FALSE(ImGui::IsPopupOpen("Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Warning"));
	ImGui::EndPopup();
	ImGui::End();
	ImGui::EndFrame();

	// The request survives into the next frame while the warning is open.
	ImGui::NewFrame();
	ImGui::Begin("Library");
	EXPECT_FALSE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Warning"));
	ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
	EXPECT_TRUE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	EXPECT_TRUE(ImGui::IsPopupOpen("Checklist"));
	ImGui::End();
	ImGui::EndFrame();
}

TEST_F(SetupChecklistPopupTest, ManualRequestWaitsForErrorDialog)
{
	SetupChecklistLifecycle lifecycle;
	ImGui::NewFrame();
	ImGui::Begin("Library");
	ImGui::OpenPopup("Error");
	lifecycle.requestManual();
	EXPECT_FALSE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Error"));
	ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
	EXPECT_TRUE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	EXPECT_TRUE(ImGui::IsPopupOpen("Checklist"));
	ImGui::End();
	ImGui::EndFrame();
}

TEST_F(SetupChecklistPopupTest, LaterWarningTemporarilyReplacesAndThenRestoresChecklist)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	ImGui::NewFrame();
	ImGui::Begin("Library");
	ASSERT_TRUE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Checklist"));
	ImGui::EndPopup();
	ImGui::End();
	ImGui::EndFrame();

	ImGui::NewFrame();
	ImGui::Begin("Library");
	ImGui::OpenPopup("Warning");
	EXPECT_FALSE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Warning"));
	ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
	EXPECT_TRUE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	EXPECT_TRUE(ImGui::IsPopupOpen("Checklist"));
	ImGui::End();
	ImGui::EndFrame();
}

TEST_F(SetupChecklistPopupTest, LaunchClosesAlreadyOpenAutomaticPopupOnReturn)
{
	SetupChecklistLifecycle lifecycle;
	lifecycle.libraryReady(true);
	ImGui::NewFrame();
	ImGui::Begin("Library");
	ASSERT_TRUE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Checklist"));
	ImGui::EndPopup();
	ImGui::End();
	ImGui::EndFrame();

	lifecycle.gameLaunchStarted(true, [](bool) {});
	ImGui::NewFrame();
	ImGui::Begin("Library");
	EXPECT_FALSE(openSetupChecklistIfUnblocked(lifecycle, "Checklist"));
	ASSERT_TRUE(ImGui::BeginPopupModal("Checklist"));
	ASSERT_TRUE(lifecycle.takeAutomaticCloseRequest());
	ImGui::CloseCurrentPopup();
	ImGui::EndPopup();
	EXPECT_FALSE(ImGui::IsPopupOpen("Checklist"));
	ImGui::End();
	ImGui::EndFrame();
}
