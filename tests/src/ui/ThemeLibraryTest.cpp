// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/theme_library.h"
#include "imgui.h"

#include <algorithm>

class ThemeLibraryTest : public ::testing::Test
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
	int renderFrame(ImVec2 mouse, bool down)
	{
		auto& io = ImGui::GetIO();
		io.MousePos = mouse;
		io.MouseDown[0] = down;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(500, 400));
		ImGui::Begin("Theme Library");
		bounds.resize(items.size());
		itemIds.assign(items.size(), 0);
		const int result = themes::renderLibrary(layout, {}, items, [&](size_t index) {
			const auto min = ImGui::GetItemRectMin();
			const auto max = ImGui::GetItemRectMax();
			bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
			itemIds[index] = ImGui::GetItemID();
		});
		const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
		backgroundVertices = std::count_if(vertices.begin(), vertices.end(), [](const ImDrawVert& vertex) {
			return vertex.col == IM_COL32(0xe1, 0x5a, 0x77, 0xff);
		});
		ImGui::End();
		ImGui::EndFrame();
		return result;
	}
	ImGuiContext* previous = nullptr;
	themes::Layout layout;
	std::vector<themes::LibraryItem> items{{"first", "First", {}, {}}, {"second", "Second", {}, {}}};
	std::vector<ImVec4> bounds;
	std::vector<ImGuiID> itemIds;
	size_t backgroundVertices = 0;
};

TEST_F(ThemeLibraryTest, GridWidgetActivatesTheClickedStableItem)
{
	layout.columns = 2;
	layout.itemAspect = 0.8f;
	layout.browser = {0, 0, 1, 1};
	EXPECT_EQ(renderFrame(ImVec2(100, 100), false), -1);
	ASSERT_NE(itemIds[0], 0u);
	ASSERT_NE(itemIds[1], 0u);
	EXPECT_NE(itemIds[0], itemIds[1]);
	const ImGuiID firstItemId = itemIds[0];
	EXPECT_EQ(renderFrame(ImVec2(100, 100), false), -1);
	EXPECT_EQ(itemIds[0], firstItemId);
	EXPECT_EQ(renderFrame(ImVec2(100, 100), true), -1);
	EXPECT_EQ(renderFrame(ImVec2(100, 100), false), 0);
}

TEST_F(ThemeLibraryTest, ListActivatesSecondItemAndPreservesItsFullTitle)
{
	layout.mode = "list";
	items[1].title = "Second ## edition";
	renderFrame(ImVec2(-1, -1), false);
	const ImVec4 rect = bounds[1];
	ASSERT_GT(rect.w - rect.y, 0.f);
	const ImVec2 center((rect.x + rect.z) / 2, (rect.y + rect.w) / 2);
	EXPECT_EQ(renderFrame(center, false), -1);
	EXPECT_EQ(renderFrame(center, true), -1);
	EXPECT_EQ(renderFrame(center, false), 1);
}

TEST_F(ThemeLibraryTest, EmptyLibraryStillRendersWithoutActivating)
{
	items.clear();
	layout.elements.push_back({"text", {0.1f, 0.1f, 0.8f, 0.2f}, {}, "Library is empty", 0xffffffff, 0.04f, 0});
	EXPECT_EQ(renderFrame(ImVec2(-1, -1), false), -1);
}

TEST_F(ThemeLibraryTest, KeyboardCanNavigateAndActivateAListItem)
{
	layout.mode = "list";
	layout.fontSize = 0.034f;
	layout.rowHeight = 0.058f;
	layout.textColor = 0xff00ffff;
	layout.selectedTextColor = 0x00ff00ff;
	layout.selectorColor = 0x0000ffff;
	auto& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_Tab, true);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_Tab, false);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_DownArrow, true);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_DownArrow, false);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_Enter, true);
	EXPECT_EQ(renderFrame(away, false), 1);
}

TEST_F(ThemeLibraryTest, CompactListUsesConfiguredRowHeightAndRestoresFontAndStyleAroundCallbacks)
{
	layout.mode = "list";
	layout.browser = {0, 0, 1, 1};
	layout.fontSize = 0.034f;
	layout.rowHeight = 0.058f;
	layout.textColor = 0xddddddff;
	layout.selectedTextColor = 0xffffffff;
	layout.selectorColor = 0x445566ff;
	ImGuiStyle& style = ImGui::GetStyle();
	ImVec2 spacing;
	ImVec4 header, active, hovered;
	ImFont* font = nullptr;
	float fontSize = 0;
	float callbackFontSize = 0;
	int callbacks = 0;
	ImGui::NewFrame();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(500, 400));
	ImGui::Begin("Compact Theme Library");
	const float canvasHeight = ImGui::GetContentRegionAvail().y;
	spacing = style.ItemSpacing;
	header = style.Colors[ImGuiCol_Header];
	active = style.Colors[ImGuiCol_HeaderActive];
	hovered = style.Colors[ImGuiCol_HeaderHovered];
	font = ImGui::GetFont();
	fontSize = ImGui::GetFontSize();
	bounds.resize(items.size());
	themes::renderLibrary(layout, "compact-layout", items, [&](size_t index) {
		++callbacks;
		const ImVec4 callbackHeader = ImGui::GetStyle().Colors[ImGuiCol_Header];
		EXPECT_FLOAT_EQ(callbackHeader.x, header.x);
		EXPECT_FLOAT_EQ(callbackHeader.y, header.y);
		EXPECT_FLOAT_EQ(ImGui::GetStyle().Colors[ImGuiCol_HeaderActive].x, active.x);
		EXPECT_FLOAT_EQ(ImGui::GetStyle().Colors[ImGuiCol_HeaderHovered].x, hovered.x);
		callbackFontSize = ImGui::GetFontSize();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();
		bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
		// Context-menu work can temporarily customize its text without leaking it.
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 1, 1));
		ImGui::PushFont(nullptr, ImGui::GetFontSize() + 1.f);
		ImGui::PopFont();
		ImGui::PopStyleColor();
	});
	EXPECT_EQ(ImGui::GetFont(), font);
	EXPECT_FLOAT_EQ(ImGui::GetFontSize(), fontSize);
	EXPECT_FLOAT_EQ(style.ItemSpacing.x, spacing.x);
	EXPECT_FLOAT_EQ(style.ItemSpacing.y, spacing.y);
	EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_Header].x, header.x);
	EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_HeaderActive].x, active.x);
	EXPECT_FLOAT_EQ(style.Colors[ImGuiCol_HeaderHovered].x, hovered.x);
	ImGui::End();
	ImGui::EndFrame();
	ASSERT_EQ(callbacks, 2);
	EXPECT_NEAR(callbackFontSize, layout.fontSize * canvasHeight, 1.0f);
	ASSERT_GT(bounds[0].w - bounds[0].y, 0.f);
	EXPECT_NEAR(bounds[0].w - bounds[0].y,
			std::max(callbackFontSize + 8.f, layout.rowHeight * canvasHeight), 1.0f);
}

// A uniquely colored title lets us check the selected background item without
// a GPU/texture driver. One visible glyph emits four vertices.
TEST_F(ThemeLibraryTest, BackgroundFollowsMouseHoverInTheSameFrameForListAndGrid)
{
	items[0].title = "A";
	items[1].title = "BBBB";
	for (auto& item : items)
		item.metadata["title"] = item.title;
	for (const char* mode : {"list", "grid"})
	{
		SCOPED_TRACE(mode);
		layout.id = std::string("hover-background-") + mode;
		layout.mode = mode;
		layout.columns = 2;
		layout.browser = {0, 0, 0.5f, 0.5f};
		layout.elements = {{"metadata", {0.55f, 0.55f, 0.4f, 0.2f}, "title", {}, 0xe15a77ff, 0.035f, -1}};
		renderFrame(ImVec2(-1, -1), false);
		for (size_t index : {1u, 0u})
		{
			const ImVec4 rect = bounds[index];
			const ImVec2 center((rect.x + rect.z) / 2, (rect.y + rect.w) / 2);
			EXPECT_EQ(renderFrame(center, false), -1);
			EXPECT_EQ(backgroundVertices, index == 0 ? 4u : 16u);
		}
	}
}

TEST_F(ThemeLibraryTest, BackgroundFollowsGamepadNavigationAndThenMouseHover)
{
	layout.id = "gamepad-background";
	layout.mode = "list";
	layout.browser = {0, 0, 0.5f, 0.5f};
	layout.elements = {{"metadata", {0.55f, 0.55f, 0.4f, 0.2f}, "title", {}, 0xe15a77ff, 0.035f, -1}};
	items[0].title = "A";
	items[1].title = "BBBB";
	for (auto& item : items)
		item.metadata["title"] = item.title;
	auto& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_Tab, true);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_Tab, false);
	renderFrame(away, false);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
	EXPECT_EQ(renderFrame(away, false), -1);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
	EXPECT_EQ(renderFrame(away, false), -1);
	EXPECT_EQ(backgroundVertices, 16u);

	// Mouse movement must take precedence over the previous navigation focus.
	const ImVec4 rect = bounds[0];
	const ImVec2 center((rect.x + rect.z) / 2, (rect.y + rect.w) / 2);
	EXPECT_EQ(renderFrame(center, false), -1);
	EXPECT_EQ(backgroundVertices, 4u);
}
