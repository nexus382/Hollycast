// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/theme_library.h"
#include "ui/gui_util.h"
#include "imgui.h"
#include <stb_image_write.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <filesystem>

class ThemeLibraryTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		previousUiScale = settings.display.uiScale;
		previousGeneralScale = config::UIScaling.get();
		previous = ImGui::GetCurrentContext();
		ImGui::CreateContext();
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.DisplaySize = ImVec2(800, 600);
		io.DeltaTime = 1.0f / 60.0f;
		unsigned char* pixels;
		int width, height;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		baselineStyle = ImGui::GetStyle();
		LibraryUiScope::setBaseStyle(baselineStyle);
	}
	void TearDown() override
	{
		if (!artworkRoot.empty())
		{
			std::error_code ec;
			std::filesystem::remove_all(artworkRoot, ec);
		}
		ImGui::DestroyContext();
		ImGui::SetCurrentContext(previous);
		settings.display.uiScale = previousUiScale;
		config::UIScaling = previousGeneralScale;
	}
	int renderFrame(ImVec2 mouse, bool down)
	{
		auto& io = ImGui::GetIO();
		io.MousePos = mouse;
		io.MouseDown[0] = down;
		ImGui::NewFrame();
		std::optional<LibraryUiScope> libraryUi;
		if (useLibraryUi)
			libraryUi.emplace(1.f);
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
			const ImRect viewport = ImGui::GetCurrentWindow()->InnerRect;
			browserViewport = ImVec4(viewport.Min.x, viewport.Min.y, viewport.Max.x, viewport.Max.y);
		}, std::nullopt, iconScale);
		const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
		backgroundMesh.assign(vertices.begin(), vertices.end());
		detailsMesh.clear();
		detailsWindow = nullptr;
		const ImGuiID detailsId = ImGui::GetCurrentWindow()->GetID("##themeLibraryDetails");
		for (ImGuiWindow* child : ImGui::GetCurrentWindow()->DC.ChildWindows)
			if (child->ChildId == detailsId)
			{
				detailsWindow = child;
				detailsMesh.assign(child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end());
				backgroundMesh.insert(backgroundMesh.end(), detailsMesh.begin(), detailsMesh.end());
			}
		backgroundVertices = std::count_if(backgroundMesh.begin(), backgroundMesh.end(), [](const ImDrawVert& vertex) {
			return vertex.col == IM_COL32(0xe1, 0x5a, 0x77, 0xff);
		});
		ImGui::End();
		libraryUi.reset();
		ImGui::EndFrame();
		return result;
	}
	ImGuiContext* previous = nullptr;
	themes::Layout layout;
	std::vector<themes::LibraryItem> items{{"first", "First", {}, {}}, {"second", "Second", {}, {}}};
	std::vector<ImVec4> bounds;
	std::vector<ImGuiID> itemIds;
	size_t backgroundVertices = 0;
	std::vector<ImDrawVert> backgroundMesh;
	float iconScale = 1.f;
	std::filesystem::path artworkRoot;
	bool useLibraryUi = false;
	float previousUiScale;
	int previousGeneralScale;
	ImGuiStyle baselineStyle;
	ImVec4 browserViewport;
	ImGuiWindow* detailsWindow = nullptr;
	std::vector<ImDrawVert> detailsMesh;
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

TEST_F(ThemeLibraryTest, LibraryIconSizeResizesEveryThemeBrowser)
{
	for (const char *mode : {"grid", "list", "rack"})
	{
		layout.id = std::string("scale-") + mode;
		layout.mode = mode;
		layout.columns = 4;
		layout.rowHeight = 0.1f;
		iconScale = 1.f;
		renderFrame(ImVec2(-1, -1), false);
		const float initialWidth = bounds[0].z - bounds[0].x;
		const float initialHeight = bounds[0].w - bounds[0].y;
		iconScale = 2.f;
		renderFrame(ImVec2(-1, -1), false);
		if (layout.mode == "list") EXPECT_GT(bounds[0].w - bounds[0].y, initialHeight);
		else EXPECT_GT(bounds[0].z - bounds[0].x, initialWidth);
	}
}

TEST_F(ThemeLibraryTest, GeneralUiScaleDoesNotChangeLibraryMetricsOrLeakStyle)
{
	useLibraryUi = true;
	layout.mode = "rack";
	layout.browser = {0.025f, 0.1f, 0.55f, 0.88f};
	layout.elements = {{"metadata", {0.62f, 0.5f, 0.35f, 0.2f}, "title", {}, 0xe15a77ff, 0.04f, 0}};
	items[0].metadata["title"] = "Game information";
	std::vector<ImVec4> expectedBounds;
	std::vector<ImDrawVert> expectedDetails;
	for (int generalScale : {50, 100, 200})
	{
		SCOPED_TRACE(generalScale);
		config::UIScaling = generalScale;
		settings.display.uiScale = generalScale / 100.f;
		ImGui::GetStyle() = baselineStyle;
		ImGui::GetStyle().ScaleAllSizes(settings.display.uiScale);
		ImGui::GetStyle().FontSizeBase = 17.f * settings.display.uiScale;
		const ImGuiStyle outsideStyle = ImGui::GetStyle();
		renderFrame(ImVec2(-1, -1), false);
		renderFrame(ImVec2(-1, -1), false);
		if (expectedBounds.empty())
		{
			expectedBounds = bounds;
			expectedDetails = detailsMesh;
			ASSERT_FALSE(expectedDetails.empty());
		}
		else
		{
			for (size_t i = 0; i < bounds.size(); ++i)
			{
				EXPECT_FLOAT_EQ(bounds[i].x, expectedBounds[i].x);
				EXPECT_FLOAT_EQ(bounds[i].y, expectedBounds[i].y);
				EXPECT_FLOAT_EQ(bounds[i].z, expectedBounds[i].z);
				EXPECT_FLOAT_EQ(bounds[i].w, expectedBounds[i].w);
			}
			ASSERT_EQ(detailsMesh.size(), expectedDetails.size());
			for (size_t i = 0; i < detailsMesh.size(); ++i)
			{
				EXPECT_FLOAT_EQ(detailsMesh[i].pos.x, expectedDetails[i].pos.x);
				EXPECT_FLOAT_EQ(detailsMesh[i].pos.y, expectedDetails[i].pos.y);
			}
		}
		EXPECT_FLOAT_EQ(settings.display.uiScale, generalScale / 100.f);
		EXPECT_EQ(config::UIScaling.get(), generalScale);
		EXPECT_FLOAT_EQ(ImGui::GetStyle().WindowPadding.x, outsideStyle.WindowPadding.x);
		EXPECT_FLOAT_EQ(ImGui::GetStyle().FramePadding.y, outsideStyle.FramePadding.y);
		EXPECT_FLOAT_EQ(ImGui::GetStyle().FontSizeBase, outsideStyle.FontSizeBase);
	}
}

TEST_F(ThemeLibraryTest, LibraryScaleResizesCoverAndInfoInEitherColumnAndKeepsThemScrollable)
{
	useLibraryUi = true;
	layout.mode = "list";
	for (bool onRight : {false, true})
	{
		SCOPED_TRACE(onRight);
		layout.id = onRight ? "details-right" : "details-left";
		layout.browser = {onRight ? 0.02f : 0.6f, 0.02f, onRight ? 0.55f : 0.38f, 0.95f};
		const float x = onRight ? 0.62f : 0.02f;
		layout.elements = {
			{"panel", {x, 0.02f, 0.35f, 0.3f}, {}, {}, 0xe15a77ff, 0, 0},
			{"metadata", {x, 0.36f, 0.35f, 0.3f}, "title", {}, 0x18bc6fff, 0.04f, 0}
		};
		items[0].metadata["title"] = "A";
		float coverHeight = 0, textHeight = 0;
		for (float scale : {1.f, 2.f})
		{
			iconScale = scale;
			renderFrame(ImVec2(-1, -1), false);
			renderFrame(ImVec2(-1, -1), false);
			ASSERT_NE(detailsWindow, nullptr);
			auto heightOf = [&](ImU32 color) {
				float top = FLT_MAX, bottom = -FLT_MAX;
				for (const ImDrawVert& vertex : detailsMesh)
					if (vertex.col == color)
					{
						top = std::min(top, vertex.pos.y);
						bottom = std::max(bottom, vertex.pos.y);
					}
				return bottom - top;
			};
			const float cover = heightOf(IM_COL32(0xe1, 0x5a, 0x77, 0xff));
			const float text = heightOf(IM_COL32(0x18, 0xbc, 0x6f, 0xff));
			ASSERT_GT(cover, 0.f);
			ASSERT_GT(text, 0.f);
			if (scale == 1.f)
			{
				coverHeight = cover;
				textHeight = text;
			}
			else
			{
				EXPECT_NEAR(cover, coverHeight * 2.f, 1.f);
				EXPECT_GT(text, textHeight * 1.7f);
				EXPECT_GT(detailsWindow->ScrollMax.y, 0.f);
			}
		}
		const ImVec2 hover = detailsWindow->InnerRect.GetCenter();
		renderFrame(hover, false);
		ImGui::GetIO().AddMouseWheelEvent(0, -5);
		renderFrame(hover, false);
		renderFrame(hover, false);
		EXPECT_GT(detailsWindow->Scroll.y, 0.f);
	}
}

TEST_F(ThemeLibraryTest, RackCasesFitTheViewportAtEveryLibraryScale)
{
	useLibraryUi = true;
	layout.mode = "rack";
	layout.browser = {0.025f, 0.12f, 0.55f, 0.855f};
	layout.columns = 64;
	layout.itemAspect = 0.045f;
	for (float scale : {0.5f, 1.f, 2.f})
	{
		iconScale = scale;
		renderFrame(ImVec2(-1, -1), false);
		EXPECT_GE(bounds[0].y, browserViewport.y);
		EXPECT_LE(bounds[0].w, browserViewport.w);
		EXPECT_GT(browserViewport.w - bounds[0].w, 20.f);
	}
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
TEST_F(ThemeLibraryTest, BackgroundFollowsMouseHoverInTheSameFrameForEveryBrowser)
{
	items[0].title = "A";
	items[1].title = "BBBB";
	for (auto& item : items)
		item.metadata["title"] = item.title;
	for (const char* mode : {"list", "grid", "rack"})
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

TEST_F(ThemeLibraryTest, RoundedGradientPreservesOpacityInBothDirections)
{
	items.clear();
	themes::Element panel;
	panel.type = "panel";
	panel.rect = {0.1f, 0.1f, 0.8f, 0.8f};
	panel.color = 0xe15a7700;
	panel.endColor = 0xe15a77ff;
	panel.rounding = 12.f;
	panel.zIndex = -1;
	for (const char* direction : {"vertical", "horizontal"})
	{
		SCOPED_TRACE(direction);
		panel.gradient = direction;
		layout.elements = {panel};
		renderFrame(ImVec2(-1, -1), false);
		float transparentEdge = FLT_MAX;
		float opaqueEdge = -FLT_MAX;
		for (const auto& vertex : backgroundMesh)
		{
			if ((vertex.col & ~IM_COL32_A_MASK) != (IM_COL32(0xe1, 0x5a, 0x77, 0) & ~IM_COL32_A_MASK))
				continue;
			const auto alpha = (vertex.col >> IM_COL32_A_SHIFT) & 0xff;
			const float position = panel.gradient == "horizontal" ? vertex.pos.x : vertex.pos.y;
			if (alpha == 0) transparentEdge = std::min(transparentEdge, position);
			if (alpha == 255) opaqueEdge = std::max(opaqueEdge, position);
		}
		EXPECT_LT(transparentEdge, opaqueEdge);
	}
}

TEST_F(ThemeLibraryTest, RoundedThumbnailListRetainsNavigationAndItemBounds)
{
	layout.id = "rounded-thumbnail-list";
	layout.mode = "list";
	layout.showArtwork = true;
	layout.rounding = 8;
	layout.selectorColor = 0x234567aa;
	layout.selectorBorderColor = 0xabcdefee;
	layout.separatorColor = 0x12345677;
	layout.rowHeight = 0.08f;
	items[1].title = "An extremely long title that must not cover the neighbouring row or its controls";
	renderFrame(ImVec2(-1, -1), false);
	const ImVec4 rect = bounds[1];
	EXPECT_LE(bounds[0].w, rect.y);
	const ImVec2 center((rect.x + rect.z) / 2, (rect.y + rect.w) / 2);
	EXPECT_EQ(renderFrame(center, true), -1);
	EXPECT_EQ(renderFrame(center, false), 1);
}

TEST(ThemeLibraryMediaTest, IconMediaSupportsEveryImageCategoryAndRestoresThemeDefault)
{
	const std::array<const char*, 9> sources{
		"", "miximage", "cover", "case", "screenshot", "title", "physical", "fanart", "titlescreen"
	};
	for (size_t i = 0; i < sources.size(); ++i)
	{
		const auto source = themes::iconArtworkSource(static_cast<int>(i));
		ASSERT_TRUE(source.has_value());
		EXPECT_EQ(*source, sources[i]);
	}
	for (int invalid : {-2, -1, 9, 100})
		EXPECT_FALSE(themes::iconArtworkSource(invalid).has_value());
	EXPECT_EQ(themes::iconArtworkSource(10), "banner");
	EXPECT_EQ(themes::iconArtworkSource(11), "backcover");
	EXPECT_EQ(themes::iconArtworkSource(12), "spine");
}

TEST_F(ThemeLibraryTest, RackNavigatesAcrossSpinesAndDownShelvesWithGamepad)
{
	layout.id = "rack-navigation";
	layout.mode = "rack";
	layout.columns = 3;
	layout.itemAspect = 0.13f;
	layout.artworkSource = "spine";
	layout.showTitles = false;
	layout.spacing = 3;
	for (int i = 2; i < 90; ++i)
		items.push_back({std::to_string(i), "A long game title on a missing spine " + std::to_string(i), {}, {}});
	auto& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	EXPECT_GT(bounds[0].w - bounds[0].y, bounds[0].z - bounds[0].x);
	EXPECT_LT(bounds[0].z, bounds[1].x);
	for (ImGuiKey key : {ImGuiKey_Tab, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadDpadDown})
	{
		io.AddKeyEvent(key, true);
		renderFrame(away, false);
		io.AddKeyEvent(key, false);
		renderFrame(away, false);
	}
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, true);
	EXPECT_EQ(renderFrame(away, false), 4);
}

TEST_F(ThemeLibraryTest, RegionalSpinesReserveFullHeightProportionalSlotsAndWrap)
{
	artworkRoot = std::filesystem::temp_directory_path() / ("hollycast-spines-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(artworkRoot);
	std::array<std::string, 3> paths;
	const std::array<int, 3> widths{39, 87, 680}, heights{680, 680, 87};
	for (size_t i = 0; i < paths.size(); ++i)
	{
		paths[i] = (artworkRoot / (std::to_string(i) + ".png")).string();
		const std::vector<unsigned char> pixels(widths[i] * heights[i] * 3, 255);
		ASSERT_NE(stbi_write_png(paths[i].c_str(), widths[i], heights[i], 3, pixels.data(), widths[i] * 3), 0);
	}
	layout.id = "regional-spines";
	layout.mode = "rack";
	layout.columns = 64;
	layout.itemAspect = 0.045f;
	layout.browser = {0, 0, 1, 1};
	layout.artworkSource = "spine";
	layout.showTitles = false;
	layout.spacing = 3;
	items.assign(30, {"pal", "PAL", {}, {{"artwork.spine", paths[1]}}});
	for (size_t i = 0; i < items.size(); ++i)
		items[i].id = std::to_string(i);
	for (size_t i = 0; i < paths.size(); ++i)
		items[i].metadata["artwork.spine"] = paths[i];
	renderFrame(ImVec2(-1, -1), false);
	for (size_t i = 0; i < paths.size(); ++i)
	{
		SCOPED_TRACE(i);
		EXPECT_NEAR(bounds[i].w - bounds[i].y, bounds[0].w - bounds[0].y, 0.01f);
		// Exclude the one-pixel jewel-case edge on either side of the scan.
		EXPECT_NEAR((bounds[i].z - bounds[i].x - 2.f) / (bounds[i].w - bounds[i].y),
				(i == 0 ? 39.f : 87.f) / 680.f, 0.001f);
	}
	EXPECT_GT(bounds[1].z - bounds[1].x - 2.f, (bounds[0].z - bounds[0].x - 2.f) * 2.f);
	EXPECT_NEAR(bounds[1].z - bounds[1].x, bounds[2].z - bounds[2].x, 0.01f);
	size_t nextShelf = 1;
	while (nextShelf < bounds.size() && bounds[nextShelf].y == bounds[0].y)
	{
		EXPECT_LT(bounds[nextShelf - 1].z, bounds[nextShelf].x);
		++nextShelf;
	}
	ASSERT_LT(nextShelf, bounds.size());
	ASSERT_GT(bounds[nextShelf].y, bounds[0].w);
	const ImVec4 target = bounds[nextShelf];
	const ImVec2 point((target.x + target.z) * 0.5f, target.y + 8.f);
	renderFrame(point, false);
	EXPECT_EQ(renderFrame(point, true), -1);
	EXPECT_EQ(renderFrame(point, false), static_cast<int>(nextShelf));
}
