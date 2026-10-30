// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/theme_library.h"
#include "ui/gui_util.h"
#include "imgui.h"
#include <stb_image_write.h>
#include "imgui_internal.h"

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
		canvasOrigin = ImGui::GetCursorScreenPos();
		canvasSize = ImGui::GetContentRegionAvail();
		bounds.resize(items.size());
		itemIds.assign(items.size(), 0);
		const int result = themes::renderLibrary(layout, {}, items, [&](size_t index) {
			const auto min = ImGui::GetItemRectMin();
			const auto max = ImGui::GetItemRectMax();
			bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
			const auto& browserVertices = ImGui::GetWindowDrawList()->VtxBuffer;
			browserMesh.assign(browserVertices.begin(), browserVertices.end());
			itemIds[index] = ImGui::GetItemID();
			const ImRect viewport = ImGui::GetCurrentWindow()->InnerRect;
			browserViewport = ImVec4(viewport.Min.x, viewport.Min.y, viewport.Max.x, viewport.Max.y);
		}, std::nullopt, iconScale, stateLayoutId, favouriteHandler);
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
	std::vector<ImDrawVert> backgroundMesh, browserMesh;
	float iconScale = 1.f;
	std::filesystem::path artworkRoot;
	bool useLibraryUi = false;
	float previousUiScale;
	int previousGeneralScale;
	ImGuiStyle baselineStyle;
	ImVec4 browserViewport;
	ImGuiWindow* detailsWindow = nullptr;
	std::vector<ImDrawVert> detailsMesh;
	ImVec2 canvasOrigin, canvasSize;
	std::string stateLayoutId;
	std::function<void(size_t)> favouriteHandler;
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

TEST_F(ThemeLibraryTest, OptInDoubleClickPreviewsOnSingleClickAndActivatesOnDoubleClick)
{
	layout.id = "double-click";
	layout.activation = "doubleClick";
	layout.columns = 2;
	renderFrame(ImVec2(-1, -1), false);
	const ImVec4 rect = bounds[1];
	const ImVec2 center((rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f);
	EXPECT_EQ(renderFrame(center, false), -1);
	EXPECT_EQ(renderFrame(center, true), -1);
	EXPECT_EQ(renderFrame(center, false), -1);
	EXPECT_EQ(renderFrame(center, true), 1);
	EXPECT_EQ(renderFrame(center, false), -1);
}

TEST_F(ThemeLibraryTest, AssetBackedNextAndPlayButtonsNavigateWithoutBlockingTheBrowser)
{
	layout.id = "button-controls";
	layout.activation = "doubleClick";
	layout.browser = {0, 0, 0.6f, 0.8f};
	themes::Element next, play;
	next.type = "button"; next.rect = {0.7f, 0.15f, 0.2f, 0.15f}; next.action = "next";
	next.background = 0; // Empty labels support controls painted into a package image.
	play = next; play.rect.y = 0.5f; play.action = "activate"; play.text = "Play";
	layout.elements = {next, play};
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	const ImVec2 nextCenter(canvasOrigin.x + canvasSize.x * 0.8f, canvasOrigin.y + canvasSize.y * 0.225f);
	EXPECT_EQ(renderFrame(nextCenter, false), -1);
	EXPECT_EQ(renderFrame(nextCenter, true), -1);
	EXPECT_EQ(renderFrame(nextCenter, false), -1);
	const ImVec2 playCenter(canvasOrigin.x + canvasSize.x * 0.8f, canvasOrigin.y + canvasSize.y * 0.575f);
	renderFrame(playCenter, false);
	EXPECT_EQ(renderFrame(playCenter, true), -1);
	EXPECT_EQ(renderFrame(playCenter, false), 1);
	// The overlapping decorative layer and separate action windows leave tiles clickable.
	const ImVec4 first = bounds[0];
	const ImVec2 tile((first.x + first.z) / 2, (first.y + first.w) / 2);
	renderFrame(tile, false);
	renderFrame(tile, true);
	EXPECT_EQ(renderFrame(tile, false), -1);
	// A row step larger than the library still wraps to a valid item.
	layout.columns = 3;
	layout.minItemWidth = 42;
	layout.elements[0].action = "up";
	renderFrame(nextCenter, false);
	renderFrame(nextCenter, true);
	renderFrame(nextCenter, false);
	renderFrame(playCenter, false);
	renderFrame(playCenter, true);
	EXPECT_EQ(renderFrame(playCenter, false), 1);
}

TEST_F(ThemeLibraryTest, VisibleFavouriteButtonTogglesTheSelectedGameAndKeepsItsWidgetId)
{
	layout.id = "favourite-controls";
	layout.activation = "doubleClick";
	layout.browser = {0, 0, 0.6f, 1};
	items[1].canFavourite = true;
	themes::Element favourite;
	favourite.type = "button"; favourite.action = "favourite";
	favourite.text = "Favourite"; favourite.selectedText = "Favourited";
	favourite.icon = "star"; favourite.selectedIcon = "star-filled";
	favourite.borderColor = 0x91a189ff; favourite.selectedBorderColor = 0x526b4eff;
	favourite.rect = {0.7f, 0.1f, 0.25f, 0.18f};
	layout.elements.push_back(favourite);
	int toggles = 0;
	favouriteHandler = [&](size_t index) { EXPECT_EQ(index, 1u); items[index].favourite = !items[index].favourite; ++toggles; };
	renderFrame(ImVec2(-1, -1), false);
	renderFrame(ImVec2(-1, -1), false);
	const auto second = bounds[1];
	const ImVec2 tile((second.x + second.z) / 2, (second.y + second.w) / 2);
	renderFrame(tile, false);
	renderFrame(tile, true);
	renderFrame(tile, false);
	const ImVec2 control(canvasOrigin.x + canvasSize.x * 0.825f, canvasOrigin.y + canvasSize.y * 0.19f);
	renderFrame(control, false);
	renderFrame(control, true);
	renderFrame(control, false);
	EXPECT_EQ(toggles, 1);
	EXPECT_TRUE(items[1].favourite);
	renderFrame(control, false);
	renderFrame(control, true);
	renderFrame(control, false);
	EXPECT_EQ(toggles, 2);
	EXPECT_FALSE(items[1].favourite);
	items[1].canFavourite = false;
	renderFrame(control, true);
	renderFrame(control, false);
	EXPECT_EQ(toggles, 2);
}

TEST_F(ThemeLibraryTest, ArtworkChoicesSwitchWhenAvailableAndDisableMissingMedia)
{
	layout.id = "artwork-controls";
	layout.mode = "list";
	layout.browser = {0, 0, 0.6f, 1};
	items[0].image = "cover.png";
	items[0].metadata["artwork.screenshot"] = "screenshot.png";
	themes::Element cover;
	cover.type = "button"; cover.action = "artwork"; cover.source = "cover";
	cover.text = "Cover"; cover.background = 0; cover.selectedBackground = 0xab1234ff;
	cover.rect = {0.7f, 0.1f, 0.25f, 0.15f};
	themes::Element screenshot = cover;
	screenshot.source = "screenshot"; screenshot.text = "Screenshot";
	screenshot.rect.y = 0.35f; screenshot.selectedBackground = 0x12ab34ff;
	layout.elements = {cover, screenshot};
	auto hasColor = [](ImU32 wanted) {
		for (auto* window : ImGui::GetCurrentContext()->Windows)
			for (const auto& vertex : window->DrawList->VtxBuffer)
				if (vertex.col == wanted) return true;
		return false;
	};
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	EXPECT_TRUE(hasColor(IM_COL32(0xab, 0x12, 0x34, 0xff)));
	const ImVec2 control(canvasOrigin.x + canvasSize.x * 0.825f, canvasOrigin.y + canvasSize.y * 0.425f);
	renderFrame(control, false);
	renderFrame(control, true);
	renderFrame(control, false);
	renderFrame(away, false);
	EXPECT_TRUE(hasColor(IM_COL32(0x12, 0xab, 0x34, 0xff)));
	items[0].metadata["artwork.screenshot"].clear();
	renderFrame(away, false);
	EXPECT_TRUE(hasColor(IM_COL32(0xab, 0x12, 0x34, 0xff)));
	renderFrame(control, false);
	renderFrame(control, true);
	renderFrame(control, false);
	renderFrame(away, false);
	EXPECT_TRUE(hasColor(IM_COL32(0xab, 0x12, 0x34, 0xff)));
	EXPECT_FALSE(hasColor(IM_COL32(0x12, 0xab, 0x34, 0xff)));
}

TEST_F(ThemeLibraryTest, ResponsiveFallbackPreservesTheSelectedGame)
{
	layout.id = "wide-state";
	stateLayoutId = layout.id;
	layout.columns = 2;
	layout.activation = "doubleClick";
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	const ImVec4 second = bounds[1];
	const ImVec2 tile((second.x + second.z) / 2, (second.y + second.w) / 2);
	renderFrame(tile, false);
	renderFrame(tile, true);
	renderFrame(tile, false);
	layout.id = "portrait-state";
	layout.canvasAspect = 0.55f;
	layout.browser = {0, 0, 1, 0.7f};
	themes::Element play;
	play.type = "button"; play.action = "activate"; play.text = "Play";
	play.rect = {0, 0.8f, 1, 0.15f};
	layout.elements.push_back(play);
	renderFrame(away, false);
	renderFrame(away, false);
	const ImVec2 playCenter(canvasOrigin.x + canvasSize.x * 0.5f, canvasOrigin.y + canvasSize.y * 0.875f);
	renderFrame(playCenter, false);
	renderFrame(playCenter, true);
	EXPECT_EQ(renderFrame(playCenter, false), 1);
}

TEST_F(ThemeLibraryTest, LongGameDetailsScrollWithoutBlockingTilesAndResetForTheNextGame)
{
	layout.id = "scrollable-details";
	layout.activation = "doubleClick";
	layout.browser = {0, 0, 0.45f, 1};
	themes::Element details;
	details.type = "details"; details.rect = {0.5f, 0, 0.5f, 0.5f};
	details.fields = {"developer", "publisher", "players", "description"}; details.columns = 2;
	layout.elements.push_back(details);
	items[0].metadata["developer"] = "Developer";
	for (int i = 0; i < 100; ++i) items[0].metadata["description"] += "A long game description. ";
	const ImVec2 away(-1, -1);
	renderFrame(away, false);
	renderFrame(away, false);
	ImGuiWindow* detailWindow = nullptr;
	for (auto* window : ImGui::GetCurrentContext()->Windows)
		if (std::string(window->Name).find("##details_") != std::string::npos) detailWindow = window;
	ASSERT_NE(detailWindow, nullptr);
	ASSERT_GT(detailWindow->ScrollMax.y, 0);
	const ImVec2 detailCenter(detailWindow->Pos.x + detailWindow->Size.x / 2, detailWindow->Pos.y + detailWindow->Size.y / 2);
	renderFrame(detailCenter, false);
	ImGui::GetIO().AddMouseWheelEvent(0, -3);
	renderFrame(detailCenter, false);
	renderFrame(detailCenter, false);
	EXPECT_GT(detailWindow->Scroll.y, 0);
	const auto second = bounds[1];
	const ImVec2 tile((second.x + second.z) / 2, (second.y + second.w) / 2);
	renderFrame(tile, false);
	renderFrame(tile, true);
	renderFrame(tile, false);
	renderFrame(away, false);
	EXPECT_FLOAT_EQ(detailWindow->Scroll.y, 0);
}

TEST_F(ThemeLibraryTest, SmallGridCellsAndAspectFitAreExplicitLayoutOptions)
{
	layout.id = "small-cells";
	layout.canvasAspect = 2;
	layout.browser = {0, 0, 0.5f, 1};
	layout.columns = 3;
	layout.minItemWidth = 42;
	items.push_back({"third", "Third", {}, {}});
	renderFrame(ImVec2(-1, -1), false);
	EXPECT_NEAR(bounds[0].y, bounds[2].y, 1.f);
	EXPECT_GT(bounds[2].x, bounds[0].x);
	EXPECT_GT(bounds[0].y, canvasOrigin.y);
	layout.minItemWidth = 96;
	renderFrame(ImVec2(-1, -1), false);
	EXPECT_GT(bounds[2].y, bounds[0].y);
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
			const auto& browserVertices = ImGui::GetWindowDrawList()->VtxBuffer;
			browserMesh.assign(browserVertices.begin(), browserVertices.end());
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

TEST_F(ThemeLibraryTest, PassiveFavouriteStarTracksStateWithoutAddingInteractiveControls)
{
 layout.id = "passive-favourites"; layout.browser = {0, 0, 1, 1}; layout.columns = 2;
 layout.showItemActions = false; layout.showFavouriteMarker = true;
 layout.pixelArt.resolution = 24; layout.pixelArt.dark = 0xe15a77ff;
 items[1].favourite = true;
 renderFrame(ImVec2(-1, -1), false); renderFrame(ImVec2(-1, -1), false);
 const auto ids = itemIds;
 auto markerVertices = [&] {
  return std::count_if(browserMesh.begin(), browserMesh.end(), [](const ImDrawVert& v) { return v.col == IM_COL32(0xe1, 0x5a, 0x77, 0xff); });
 };
 EXPECT_GT(markerVertices(), 0);
 items[1].favourite = false; renderFrame(ImVec2(-1, -1), false);
 EXPECT_EQ(markerVertices(), 0); EXPECT_EQ(ids, itemIds);
 items[1].favourite = true; layout.showFavouriteMarker = false; renderFrame(ImVec2(-1, -1), false);
 EXPECT_EQ(markerVertices(), 0); EXPECT_EQ(ids, itemIds);
}
