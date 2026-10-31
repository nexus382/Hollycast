// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/theme_library.h"
#include "ui/gui_util.h"
#include "ui/theme.h"
#include "imgui.h"
#include <stb_image_write.h>
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <string>

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
		}, std::nullopt, iconScale, stateLayoutId, favouriteHandler, {}, pulledItemId);
		const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
		backgroundMesh.assign(vertices.begin(), vertices.end());
		detailsMesh.clear();
		overlayMesh.clear();
		detailsWindow = nullptr;
		const ImGuiID detailsId = ImGui::GetCurrentWindow()->GetID("##themeLibraryDetails");
		const ImGuiID overlayId = ImGui::GetCurrentWindow()->GetID("##themeLibraryOverlay");
		for (ImGuiWindow* child : ImGui::GetCurrentWindow()->DC.ChildWindows)
		{
			if (child->ChildId == overlayId)
				overlayMesh.assign(child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end());
			if (child->ChildId == detailsId)
			{
				detailsWindow = child;
				detailsMesh.assign(child->DrawList->VtxBuffer.begin(), child->DrawList->VtxBuffer.end());
				backgroundMesh.insert(backgroundMesh.end(), detailsMesh.begin(), detailsMesh.end());
			}
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
	std::vector<ImDrawVert> overlayMesh;
	float iconScale = 1.f;
	std::string pulledItemId;
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

class ThemeRailTest : public ThemeLibraryTest
{
protected:
	void SetUp() override
	{
		ThemeLibraryTest::SetUp();
		layout.mode = "rail";
		layout.browser = {0, 0, 1, 1};
		layout.rowHeight = 0.1f;
		layout.rail.visible = 2;
		items = {{"a", "A", {}, {}}, {"b", "B", {}, {}}, {"c", "C", {}, {}}, {"d", "D", {}, {}}};
	}
	void settle(ImVec2 mouse = ImVec2(-1, -1), int frames = 60)
	{
		for (int i = 0; i < frames; ++i)
			renderFrame(mouse, false);
	}
	// The selected game is the row the rail has centered.
	int centeredRow() const
	{
		const float center = canvasOrigin.y + canvasSize.y / 2;
		int best = -1;
		float nearest = 1e9f;
		for (size_t i = 0; i < bounds.size(); ++i)
		{
			const float distance = std::fabs((bounds[i].y + bounds[i].w) / 2 - center);
			if (bounds[i].w > bounds[i].y && distance < nearest)
			{
				nearest = distance;
				best = static_cast<int>(i);
			}
		}
		return best;
	}
	ImVec2 rowCenter(int index) const
	{
		return ImVec2((bounds[index].x + bounds[index].z) / 2, (bounds[index].y + bounds[index].w) / 2);
	}
	int overlayVertices() const
	{
		for (ImGuiWindow* window : GImGui->Windows)
			if (std::string(window->Name).find("themeLibraryOverlay") != std::string::npos)
				return window->DrawList->VtxBuffer.Size;
		return -1;
	}
};

TEST_F(ThemeRailTest, SelectedRowSitsAtTheConfiguredHeight)
{
	layout.id = "rail-center";
	layout.rail.center = 0.25f;
	settle();
	const float rowMiddle = (bounds[0].y + bounds[0].w) / 2;
	EXPECT_NEAR(rowMiddle, canvasOrigin.y + canvasSize.y * 0.25f, 2.f);
}

TEST_F(ThemeRailTest, CentersTheSelectionAndIgnoresHover)
{
	layout.id = "rail-hover";
	settle();
	ASSERT_EQ(centeredRow(), 0);
	const ImVec2 next = rowCenter(1);
	settle(next);
	EXPECT_EQ(centeredRow(), 0);
}

TEST_F(ThemeRailTest, PressSelectsAndDoubleClickModeDoesNotLaunch)
{
	layout.id = "rail-press";
	layout.activation = "doubleClick";
	settle();
	const ImVec2 next = rowCenter(1);
	EXPECT_EQ(renderFrame(next, false), -1);
	EXPECT_EQ(renderFrame(next, true), -1);
	EXPECT_EQ(renderFrame(next, false), -1);
	settle();
	EXPECT_EQ(centeredRow(), 1);
}

TEST_F(ThemeRailTest, SingleClickModeLaunchesThePressedRow)
{
	layout.id = "rail-launch";
	settle();
	const ImVec2 next = rowCenter(1);
	EXPECT_EQ(renderFrame(next, false), -1);
	EXPECT_EQ(renderFrame(next, true), -1);
	EXPECT_EQ(renderFrame(next, false), 1);
}

TEST_F(ThemeRailTest, WheelStepsAndStopsAtTheEnds)
{
	layout.id = "rail-wheel";
	settle();
	const ImVec2 inside(canvasOrigin.x + canvasSize.x / 2, canvasOrigin.y + canvasSize.y / 2);
	renderFrame(inside, false);
	ImGui::GetIO().AddMouseWheelEvent(0, -1);
	settle(inside);
	EXPECT_EQ(centeredRow(), 1);
	ImGui::GetIO().AddMouseWheelEvent(0, 10);
	settle(inside);
	EXPECT_EQ(centeredRow(), 0);
}

TEST_F(ThemeRailTest, DragStepsWithoutLaunching)
{
	layout.id = "rail-drag";
	settle();
	const ImVec2 start = rowCenter(0);
	EXPECT_EQ(renderFrame(start, false), -1);
	EXPECT_EQ(renderFrame(start, true), -1);
	EXPECT_EQ(renderFrame(ImVec2(start.x, start.y - 30), true), -1);
	EXPECT_EQ(renderFrame(ImVec2(start.x, start.y - 60), true), -1);
	EXPECT_EQ(renderFrame(ImVec2(start.x, start.y - 60), false), -1);
	settle();
	EXPECT_EQ(centeredRow(), 1);
}

TEST_F(ThemeRailTest, DoubleClickLaunchesTheRowThatWasClickedBeforeTheRailMoves)
{
	layout.id = "rail-double";
	layout.activation = "doubleClick";
	settle();
	const ImVec2 next = rowCenter(2);
	EXPECT_EQ(renderFrame(next, false), -1);
	EXPECT_EQ(renderFrame(next, true), -1);
	EXPECT_EQ(renderFrame(next, false), -1);
	EXPECT_EQ(renderFrame(next, false), -1);
	// ImGui reports a double-click on the second press.
	EXPECT_EQ(renderFrame(next, true), 2);
	renderFrame(next, false);
}

TEST_F(ThemeRailTest, TheRailStartsMovingAsSoonAsARowIsPressed)
{
	layout.id = "rail-prompt";
	layout.activation = "doubleClick";
	settle();
	const ImVec2 next = rowCenter(1);
	const float before = next.y;
	renderFrame(next, false);
	renderFrame(next, true);
	renderFrame(next, true);
	renderFrame(next, false);
	renderFrame(next, false);
	EXPECT_LT(rowCenter(1).y, before - 1); // Four frames on, well inside the double-click window.
}

TEST_F(ThemeRailTest, ASecondClickLaunchesTheFirstRowEvenWithAnotherRowNowUnderThePointer)
{
	layout.id = "rail-double-moved";
	layout.activation = "doubleClick";
	layout.moveSound = {};
	items = {{"a", "A", {}, {}}, {"b", "B", {}, {}}, {"c", "C", {}, {}}, {"d", "D", {}, {}},
			{"e", "E", {}, {}}, {"f", "F", {}, {}}, {"g", "G", {}, {}}};
	settle();
	const ImVec2 spot = rowCenter(2);
	renderFrame(spot, false);
	renderFrame(spot, true);
	renderFrame(spot, false);
	for (int i = 0; i < 12; ++i) // 0.2 seconds, within the double-click window: the rail has arrived.
		renderFrame(spot, false);
	EXPECT_LT(std::fabs(rowCenter(2).y - (canvasOrigin.y + canvasSize.y / 2)), std::fabs(spot.y - (canvasOrigin.y + canvasSize.y / 2)) / 2);
	EXPECT_EQ(renderFrame(spot, true), 2);
	EXPECT_EQ(renderFrame(spot, false), -1);
	settle();
	EXPECT_EQ(centeredRow(), 2); // The row that slid under the pointer was not selected either.
}

TEST_F(ThemeRailTest, ASlowSecondClickIsAnOrdinaryClickOnTheRowThere)
{
	layout.id = "rail-double-slow";
	layout.activation = "doubleClick";
	items = {{"a", "A", {}, {}}, {"b", "B", {}, {}}, {"c", "C", {}, {}}, {"d", "D", {}, {}},
			{"e", "E", {}, {}}, {"f", "F", {}, {}}, {"g", "G", {}, {}}};
	settle();
	const ImVec2 spot = rowCenter(2);
	renderFrame(spot, false);
	renderFrame(spot, true);
	renderFrame(spot, false);
	settle(spot, 40); // Two thirds of a second.
	EXPECT_EQ(renderFrame(spot, true), -1);
	EXPECT_EQ(renderFrame(spot, false), -1);
	settle();
	EXPECT_EQ(centeredRow(), 4);
}

TEST_F(ThemeRailTest, MoveCallbackFiresForThePlayerButNotAttractOrTheFirstFrame)
{
	layout.id = "rail-move";
	layout.moveSound = "move";
	layout.attract.idleSeconds = 1;
	layout.attract.intervalSeconds = 0.5f;
	int moves = 0;
	const auto render = [&](ImVec2 mouse, bool down) {
		auto& io = ImGui::GetIO();
		io.MousePos = mouse;
		io.MouseDown[0] = down;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(500, 400));
		ImGui::Begin("Theme Library");
		canvasOrigin = ImGui::GetCursorScreenPos();
		canvasSize = ImGui::GetContentRegionAvail();
		bounds.resize(items.size());
		const int result = themes::renderLibrary(layout, {}, items, [&](size_t index) {
			const auto min = ImGui::GetItemRectMin();
			const auto max = ImGui::GetItemRectMax();
			bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
		}, std::nullopt, 1.f, {}, {}, [&](const std::string&) { ++moves; });
		ImGui::End();
		ImGui::EndFrame();
		return result;
	};
	const ImVec2 away(10, 10);
	for (int i = 0; i < 30; ++i)
		render(away, false);
	EXPECT_EQ(moves, 0);
	const ImVec2 next = rowCenter(1);
	render(next, false);
	render(next, true);
	render(next, false);
	EXPECT_EQ(moves, 1);
	render(away, false);
	render(away, false);
	EXPECT_EQ(moves, 1); // The selection held still.
	for (int i = 0; i < 135; ++i) // Two steps: four would wrap back to this row.
		render(away, false);
	EXPECT_NE(centeredRow(), 1); // Attract mode moved on, silently.
	EXPECT_EQ(moves, 1);
}

class ThemeRailKeyboardTest : public ThemeRailTest
{
protected:
	void SetUp() override
	{
		ThemeRailTest::SetUp();
		auto& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.DisplaySize = ImVec2(1400, 800);
		const themes::Theme theme = themes::load(std::string(FLYCAST_TEST_FILES) + "/../../themes/demo-disc");
		layout = theme.layouts[0];
		// The real layout's controls compete for focus; its images need a graphics driver.
		std::vector<themes::Element> kept;
		for (auto element : layout.elements)
			if (element.type != "image" && element.type != "artwork")
			{
				element.buttonImage.clear();
				kept.push_back(element);
			}
		layout.elements = kept;
		layout.attract.idleSeconds = 0;
	}
	void games(int count)
	{
		items.clear();
		for (int i = 0; i < count; ++i)
			items.push_back({"game" + std::to_string(i), "game" + std::to_string(i), {}, {}});
	}
	void frame(ImVec2 mouse = ImVec2(-1, -1), bool down = false)
	{
		auto& io = ImGui::GetIO();
		io.MousePos = mouse;
		io.MouseDown[0] = down;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(1400, 800));
		ImGui::Begin("Theme Library", nullptr, ImGuiWindowFlags_NoDecoration);
		bounds.assign(items.size(), ImVec4(0, 0, 0, 0));
		const int result = themes::renderLibrary(layout, {}, items, [&](size_t index) {
			const auto min = ImGui::GetItemRectMin();
			const auto max = ImGui::GetItemRectMax();
			bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
			browserCenter = ImGui::GetCurrentWindow()->Pos.y + ImGui::GetCurrentWindow()->Size.y / 2;
		}, std::nullopt, 1.f, {}, [&](size_t index) {
			++favouriteHandlerCalls;
			favouriteTarget = static_cast<int>(index);
		});
		if (result >= 0)
			started.push_back(result);
		ImGui::End();
		ImGui::EndFrame();
	}
	std::vector<int> started;
	void press(ImGuiKey key, int held, int after)
	{
		auto& io = ImGui::GetIO();
		io.AddKeyEvent(key, true);
		for (int i = 0; i < 1 + held; ++i)
			frame();
		io.AddKeyEvent(key, false);
		for (int i = 0; i < after; ++i)
			frame();
	}
	int selectedRow() const
	{
		int best = -1;
		float nearest = 1e9f;
		for (size_t i = 0; i < bounds.size(); ++i)
			if (bounds[i].w > bounds[i].y)
			{
				const float distance = std::fabs((bounds[i].y + bounds[i].w) / 2 - browserCenter);
				if (distance < nearest)
				{
					nearest = distance;
					best = static_cast<int>(i);
				}
			}
		return best;
	}
	void settleRail()
	{
		for (int i = 0; i < 60; ++i)
			frame();
	}
	bool focusOnRow() const
	{
		return GImGui->NavWindow != nullptr && std::string(GImGui->NavWindow->Name).find("Browser") != std::string::npos;
	}
	float browserCenter = 0;
	int favouriteHandlerCalls = 0, favouriteTarget = -1;
};

TEST_F(ThemeRailKeyboardTest, DownReachesEveryGameAtAnySpeed)
{
	int run = 0;
	for (int count : {2, 3, 4, 5, 8, 16})
		for (int gap : {1, 2, 3, 4, 6, 10, 20, 40})
			for (int held : {0, 25})
				for (bool clickFirst : {false, true})
				{
					layout.id = "keyboard-" + std::to_string(run++);
					games(count);
					settleRail();
					// Holding Down in the last run may have left the rail.
					ImGui::FocusWindow(ImGui::FindWindowByName("Theme Library"));
					GImGui->NavId = 0;
					settleRail();
					if (clickFirst)
					{
						const ImVec2 row((bounds[0].x + bounds[0].z) / 2, (bounds[0].y + bounds[0].w) / 2);
						frame(row);
						frame(row, true);
						frame(row);
						frame();
						settleRail();
					}
					// Key repeat may step further than one per press, never fewer.
					for (int i = 0; i < count - 1; ++i)
						press(ImGuiKey_DownArrow, held, gap);
					settleRail();
					EXPECT_EQ(selectedRow(), count - 1) << "count " << count << " gap " << gap << " held " << held << " click " << clickFirst;
				}
}

TEST_F(ThemeRailKeyboardTest, TheFirstPressWorksWithNothingFocused)
{
	layout.id = "keyboard-first";
	games(4);
	ImGui::FocusWindow(nullptr);
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	EXPECT_EQ(selectedRow(), 1);
}

TEST_F(ThemeRailKeyboardTest, ArrowsDriveTheRailWhileFocusIsUnseenElsewhere)
{
	layout.id = "keyboard-unseen";
	games(4);
	settleRail();
	press(ImGuiKey_RightArrow, 0, 10); // Focus moves to a button.
	ASSERT_FALSE(focusOnRow());
	GImGui->NavCursorVisible = false; // As after using the mouse.
	press(ImGuiKey_DownArrow, 0, 40);
	EXPECT_EQ(selectedRow(), 1);
	press(ImGuiKey_DownArrow, 0, 40);
	EXPECT_EQ(selectedRow(), 2);
}

TEST_F(ThemeRailKeyboardTest, EachPressStepsExactlyOneGame)
{
	layout.id = "keyboard-steps";
	games(5);
	settleRail();
	for (int expected = 1; expected < 5; ++expected)
	{
		press(ImGuiKey_DownArrow, 0, 40);
		EXPECT_EQ(selectedRow(), expected);
	}
	for (int expected = 3; expected >= 0; --expected)
	{
		press(ImGuiKey_UpArrow, 0, 40);
		EXPECT_EQ(selectedRow(), expected);
	}
}

TEST_F(ThemeRailKeyboardTest, EndsLeaveTheRailAndComingBackKeepsTheSelection)
{
	layout.id = "keyboard-ends";
	games(4);
	settleRail();
	for (int i = 0; i < 3; ++i)
		press(ImGuiKey_DownArrow, 0, 40);
	ASSERT_EQ(selectedRow(), 3);
	EXPECT_TRUE(focusOnRow());
	press(ImGuiKey_DownArrow, 0, 40); // On past the last game, to the buttons below.
	EXPECT_FALSE(focusOnRow());
	EXPECT_EQ(selectedRow(), 3);
	press(ImGuiKey_UpArrow, 0, 40);
	EXPECT_TRUE(focusOnRow());
	EXPECT_EQ(selectedRow(), 3);
	press(ImGuiKey_UpArrow, 0, 40);
	EXPECT_EQ(selectedRow(), 2); // Stepping carries on from the selection.
}

TEST_F(ThemeRailKeyboardTest, RightReachesTheButtonsAndUpReturnsWithoutJumping)
{
	layout.id = "keyboard-return";
	games(8);
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	press(ImGuiKey_DownArrow, 0, 40);
	ASSERT_EQ(selectedRow(), 2);
	press(ImGuiKey_RightArrow, 0, 40);
	EXPECT_FALSE(focusOnRow());
	EXPECT_EQ(selectedRow(), 2);
	press(ImGuiKey_UpArrow, 0, 40); // Lands on whichever row is nearest.
	settleRail();
	EXPECT_TRUE(focusOnRow());
	EXPECT_EQ(selectedRow(), 2);
	press(ImGuiKey_DownArrow, 0, 40);
	EXPECT_EQ(selectedRow(), 3); // Stepping carries on from the selection.
}

TEST_F(ThemeLibraryTest, ButtonTooltipDoesNotInheritTheButtonsTextColor)
{
	layout.id = "tooltip-color";
	layout.browser = {0, 0, 1, 0.5f};
	themes::Element button;
	button.type = "button";
	button.action = "favourite";
	button.rect = {0, 0.6f, 0.5f, 0.2f};
	button.text = "Favourite";
	button.selectedColor = 0xff00ffff; // Magenta: no default style color matches it.
	button.background = 0x000000ff;
	layout.elements.push_back(button);
	items[0].favourite = true;
	items[0].canFavourite = true;
	favouriteHandler = [](size_t) {};
	renderFrame(ImVec2(-1, -1), false);
	renderFrame(ImVec2(-1, -1), false);
	const ImVec4 expected = ImGui::GetStyleColorVec4(ImGuiCol_Text);
	const ImVec2 over(canvasOrigin.x + canvasSize.x * 0.25f, canvasOrigin.y + canvasSize.y * 0.7f);
	const auto& g = *GImGui;
	for (int i = 0; i < 3; ++i)
		renderFrame(over, false);
	EXPECT_EQ(g.ColorStack.Size, 0); // Nothing the tooltip or later widgets could inherit.
	EXPECT_FLOAT_EQ(ImGui::GetStyleColorVec4(ImGuiCol_Text).x, expected.x);
	bool tooltip = false;
	for (ImGuiWindow* window : g.Windows)
		if (std::string(window->Name).find("##Tooltip") != std::string::npos && window->Active)
		{
			tooltip = true;
			ASSERT_GT(window->DrawList->CmdBuffer.Size, 0);
			for (const ImDrawVert& vertex : window->DrawList->VtxBuffer)
				EXPECT_NE(vertex.col, IM_COL32(255, 0, 255, 255)); // The button's text color stayed behind.
		}
	EXPECT_TRUE(tooltip);
}

TEST_F(ThemeRailKeyboardTest, EnterStartsTheSelectedGameAfterSteppingWithTheArrows)
{
	layout.id = "keyboard-enter";
	layout.launchSeconds = 0;
	games(8);
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	press(ImGuiKey_DownArrow, 0, 40);
	ASSERT_EQ(selectedRow(), 2);
	press(ImGuiKey_Enter, 0, 10);
	EXPECT_EQ(started, std::vector<int>{2});
}

TEST_F(ThemeRailKeyboardTest, EnterStartsTheSelectedGameBeforeAnyOtherInput)
{
	layout.id = "keyboard-enter-first";
	layout.launchSeconds = 0;
	games(8);
	settleRail();
	press(ImGuiKey_Enter, 0, 10);
	EXPECT_EQ(started, std::vector<int>{0});
}

TEST_F(ThemeRailKeyboardTest, EnterStartsTheGameOnceWhenTheCursorIsVisibleOnARow)
{
	layout.id = "keyboard-enter-visible";
	layout.launchSeconds = 0;
	games(8);
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	ASSERT_TRUE(focusOnRow());
	ImGui::SetNavCursorVisible(true); // ImGui now confirms the row by itself, and the rail must not as well.
	press(ImGuiKey_Enter, 0, 10);
	EXPECT_EQ(started, std::vector<int>{1});
}

TEST_F(ThemeRailKeyboardTest, EnterPlaysTheLaunchSequenceBeforeTheGameStarts)
{
	layout.id = "keyboard-enter-launch";
	layout.launchSeconds = 0.5f;
	games(8);
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	press(ImGuiKey_Enter, 0, 0);
	EXPECT_TRUE(started.empty());
	for (int i = 0; i < 40; ++i)
		frame();
	EXPECT_EQ(started, std::vector<int>{1});
}

TEST_F(ThemeRailKeyboardTest, FavouriteIsReachableMidRail)
{
	layout.id = "keyboard-favourite";
	games(8);
	for (auto& item : items)
		item.canFavourite = true;
	favouriteHandlerCalls = -1;
	settleRail();
	press(ImGuiKey_DownArrow, 0, 40);
	press(ImGuiKey_DownArrow, 0, 40);
	ASSERT_EQ(selectedRow(), 2);
	favouriteHandlerCalls = 0;
	press(ImGuiKey_RightArrow, 0, 40); // Play.
	press(ImGuiKey_RightArrow, 0, 40); // Favourite.
	press(ImGuiKey_Enter, 0, 10);
	EXPECT_EQ(favouriteHandlerCalls, 1);
	EXPECT_EQ(favouriteTarget, 2);
}

TEST(ThemeDiscRingTest, TheOutlineStaysRoundWhileTheImageTurns)
{
	constexpr int segments = 32;
	const auto still = themes::discRing(100, 200, 50, 0.2f, 0, segments);
	const auto turned = themes::discRing(100, 200, 50, 0.2f, IM_PI / 2, segments);
	ASSERT_EQ(still.size(), static_cast<size_t>((segments + 1) * 2));
	ASSERT_EQ(turned.size(), still.size());
	for (size_t i = 0; i < still.size(); ++i)
	{
		const float reach = i % 2 == 0 ? 50.f : 10.f;
		EXPECT_NEAR(std::hypot(still[i].x - 100, still[i].y - 200), reach, 0.01f);
		// Turning moves the picture, not the shape.
		EXPECT_FLOAT_EQ(turned[i].x, still[i].x);
		EXPECT_FLOAT_EQ(turned[i].y, still[i].y);
		// Corners of the image are cut away: nothing samples outside its inscribed circle.
		EXPECT_NEAR(std::hypot(still[i].u - 0.5f, still[i].v - 0.5f), reach / 100, 0.001f);
	}
	// The first spoke points right. A quarter turn clockwise brings the image's top round to it.
	EXPECT_NEAR(still[0].u, 1, 0.001f);
	EXPECT_NEAR(still[0].v, 0.5f, 0.001f);
	EXPECT_NEAR(turned[0].u, 0.5f, 0.001f);
	EXPECT_NEAR(turned[0].v, 0, 0.001f);
	// The ring closes on itself, leaving no gap at the seam.
	EXPECT_NEAR(still[segments * 2].x, still[0].x, 0.01f);
	EXPECT_NEAR(still[segments * 2].y, still[0].y, 0.01f);
}

TEST(ThemeRailBadgeTest, EachTagShowsTheFirstFieldWithAValue)
{
	themes::Rail rail;
	themes::LibraryItem item;
	EXPECT_TRUE(themes::railBadges(rail, item).empty());
	rail.badges = {{"system"}, {"genre", "region", "format"}};
	EXPECT_TRUE(themes::railBadges(rail, item).empty());
	item.metadata = {{"system", "Dreamcast"}, {"genre", ""}, {"region", "JP"}, {"format", "cdi"}};
	EXPECT_EQ(themes::railBadges(rail, item), (std::vector<std::string>{"Dreamcast", "JP"}));
	item.metadata["genre"] = "Fighting";
	EXPECT_EQ(themes::railBadges(rail, item), (std::vector<std::string>{"Dreamcast", "Fighting"}));
	item.metadata["system"] = "";
	EXPECT_EQ(themes::railBadges(rail, item), (std::vector<std::string>{"Fighting"}));
}

TEST(ThemeMetadataTextTest, JoinsFieldsThatHaveValuesAndFallsBackWhenNoneDo)
{
	themes::Element element;
	element.type = "metadata";
	element.fields = {"publisher", "genre", "region"};
	element.separator = " * ";
	element.text = "Nothing known";
	themes::LibraryItem item;
	EXPECT_EQ(themes::metadataText(element, item), "Nothing known");
	item.metadata = {{"publisher", "Sega"}, {"genre", ""}, {"region", "JP"}};
	EXPECT_EQ(themes::metadataText(element, item), "Sega * JP");
	element.fields.clear();
	element.source = "publisher";
	EXPECT_EQ(themes::metadataText(element, item), "Sega");
}

TEST(ThemeMetadataTextTest, MarqueeTextStaysOnOneLine)
{
	themes::Element element;
	element.type = "metadata";
	element.source = "description";
	themes::LibraryItem item;
	item.metadata = {{"description", "First line\nSecond line"}};
	EXPECT_EQ(themes::metadataText(element, item), "First line\nSecond line");
	element.marqueeSeconds = 10;
	EXPECT_EQ(themes::metadataText(element, item), "First line Second line");
}

TEST_F(ThemeRailTest, FavouriteMarkAddsToARowsDrawing)
{
	const auto rowVertices = [&](bool favourite, const char* id) {
		layout.id = id;
		layout.rail.favouriteMark = true;
		items[0].favourite = favourite;
		renderFrame(ImVec2(-1, -1), false);
		renderFrame(ImVec2(-1, -1), false);
		for (ImGuiWindow* window : GImGui->Windows)
			if (std::string(window->Name).find("themeLibraryBrowser") != std::string::npos)
				return window->DrawList->VtxBuffer.Size;
		return -1;
	};
	const int plain = rowVertices(false, "rail-star-off");
	ASSERT_GT(plain, 0);
	EXPECT_GT(rowVertices(true, "rail-star-on"), plain);
}

class ThemeLaunchTest : public ThemeRailTest
{
protected:
	void SetUp() override
	{
		ThemeRailTest::SetUp();
		layout.launchSeconds = 1;
		layout.launchSound = "launch";
		layout.moveSound = "move";
	}
	int frame(ImVec2 mouse = ImVec2(-1, -1), bool down = false)
	{
		auto& io = ImGui::GetIO();
		io.MousePos = mouse;
		io.MouseDown[0] = down;
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImVec2(500, 400));
		ImGui::Begin("Theme Library");
		canvasOrigin = ImGui::GetCursorScreenPos();
		canvasSize = ImGui::GetContentRegionAvail();
		bounds.resize(items.size());
		const int result = themes::renderLibrary(layout, {}, items, [&](size_t index) {
			const auto min = ImGui::GetItemRectMin();
			const auto max = ImGui::GetItemRectMax();
			bounds[index] = ImVec4(min.x, min.y, max.x, max.y);
		}, std::nullopt, 1.f, {}, {}, [&](const std::string& sound) { sounds.push_back(sound); });
		launchOverlayActive = false;
		for (ImGuiWindow* window : GImGui->Windows)
			if (std::string(window->Name).find("themeLibraryLaunch") != std::string::npos && window->Active)
				launchOverlayActive = true;
		ImGui::End();
		ImGui::EndFrame();
		return result;
	}
	// Clicks a row in single-click mode, returning what the release reported.
	int clickRow(int index)
	{
		for (int i = 0; i < 30; ++i)
			frame();
		const ImVec2 row = rowCenter(index);
		frame(row);
		frame(row, true);
		return frame(row);
	}
	std::vector<std::string> sounds;
	bool launchOverlayActive = false;
};

TEST_F(ThemeLaunchTest, HoldsTheGameBackForTheSequenceThenReleasesItOnce)
{
	layout.id = "launch-hold";
	EXPECT_EQ(clickRow(1), -1);
	EXPECT_EQ(std::count(sounds.begin(), sounds.end(), "launch"), 1);
	int released = -1, framesWaited = 0;
	for (; framesWaited < 120 && released < 0; ++framesWaited)
		released = frame();
	EXPECT_EQ(released, 1);
	EXPECT_NEAR(framesWaited, 60, 2); // One second at the harness's 60 frames per second.
	for (int i = 0; i < 90; ++i)
		EXPECT_EQ(frame(), -1); // Only once.
	EXPECT_EQ(std::count(sounds.begin(), sounds.end(), "launch"), 1);
}

TEST_F(ThemeLaunchTest, WithoutASequenceTheGameStartsAtOnce)
{
	layout.id = "launch-none";
	layout.launchSeconds = 0;
	EXPECT_EQ(clickRow(1), 1);
	EXPECT_EQ(std::count(sounds.begin(), sounds.end(), "launch"), 0);
}

TEST_F(ThemeLaunchTest, AnyPressSkipsToTheGameThatWasChosen)
{
	layout.id = "launch-skip-key";
	EXPECT_EQ(clickRow(1), -1);
	for (int i = 0; i < 5; ++i)
		EXPECT_EQ(frame(), -1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, true);
	EXPECT_EQ(frame(), 1);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Space, false);
	frame();
}

TEST_F(ThemeLaunchTest, ClickingAnotherRowSkipsButStillStartsTheChosenGame)
{
	layout.id = "launch-skip-click";
	EXPECT_EQ(clickRow(1), -1);
	const ImVec2 other = rowCenter(2);
	frame(other);
	int released = frame(other, true);
	if (released < 0)
		released = frame(other);
	EXPECT_EQ(released, 1);
}

TEST_F(ThemeLaunchTest, TheSelectionHoldsStillDuringTheSequence)
{
	layout.id = "launch-still";
	layout.attract.idleSeconds = 0.5f;
	layout.attract.intervalSeconds = 0.5f;
	EXPECT_EQ(clickRow(1), -1);
	sounds.clear();
	const ImVec2 inside(canvasOrigin.x + canvasSize.x / 2, canvasOrigin.y + canvasSize.y / 2);
	frame(inside);
	ImGui::GetIO().AddMouseWheelEvent(0, -1);
	frame(inside);
	for (int i = 0; i < 20; ++i)
		frame(inside);
	EXPECT_EQ(centeredRow(), 1);
	EXPECT_TRUE(sounds.empty());
}

TEST_F(ThemeLaunchTest, ChosenGameLeavingTheListCancelsTheSequence)
{
	layout.id = "launch-gone";
	EXPECT_EQ(clickRow(1), -1);
	items.erase(items.begin() + 1);
	for (int i = 0; i < 90; ++i)
		EXPECT_EQ(frame(), -1);
	EXPECT_FALSE(launchOverlayActive);
}

TEST_F(ThemeLaunchTest, LeavingTheLibraryAbandonsTheSequence)
{
	layout.id = "launch-left";
	EXPECT_EQ(clickRow(1), -1);
	for (int i = 0; i < 90; ++i) // Settings or another screen is showing instead.
	{
		ImGui::NewFrame();
		ImGui::EndFrame();
	}
	for (int i = 0; i < 90; ++i)
		EXPECT_EQ(frame(), -1);
	EXPECT_FALSE(launchOverlayActive);
}

TEST_F(ThemeLaunchTest, OverlayExistsOnlyDuringTheSequence)
{
	layout.id = "launch-overlay";
	frame();
	EXPECT_FALSE(launchOverlayActive);
	EXPECT_EQ(clickRow(1), -1);
	frame();
	EXPECT_TRUE(launchOverlayActive);
}

TEST_F(ThemeRailTest, MoveSoundIsOptional)
{
	layout.id = "rail-no-sound";
	settle();
	const ImVec2 next = rowCenter(1);
	renderFrame(next, false);
	renderFrame(next, true);
	EXPECT_EQ(renderFrame(next, false), 1);
}

TEST_F(ThemeRailTest, BadgesRenderAndDoNotDisturbTheRail)
{
	layout.id = "rail-badges";
	layout.rail.badges = {{"system"}, {"genre"}};
	items[1].metadata = {{"system", "Atomiswave"}, {"genre", "A very long genre description that would otherwise push the title off the row"}};
	settle(ImVec2(-1, -1), 2);
	EXPECT_EQ(centeredRow(), 0);
}

TEST_F(ThemeRailTest, AttractStepsWhenIdleAndStopsOnInput)
{
	layout.id = "rail-attract";
	layout.attract.idleSeconds = 1;
	layout.attract.intervalSeconds = 0.5f;
	settle(ImVec2(10, 10), 50);
	EXPECT_EQ(centeredRow(), 0);
	settle(ImVec2(10, 10), 60);
	const int moved = centeredRow();
	EXPECT_GE(moved, 1);
	// A moving pointer restarts the idle wait, and the selection holds still.
	for (int i = 0; i < 50; ++i)
		renderFrame(ImVec2(10.f + i, 10), false);
	EXPECT_EQ(centeredRow(), moved);
}

TEST_F(ThemeRailTest, AttractWrapsAroundTheLibrary)
{
	layout.id = "rail-wrap";
	layout.attract.idleSeconds = 1;
	layout.attract.intervalSeconds = 0.5f;
	items.resize(2);
	settle(ImVec2(10, 10), 60 + 30 * 2 + 25);
	EXPECT_EQ(centeredRow(), 0);
}

TEST_F(ThemeRailTest, ElementsAppearOnlyInTheirBrowsingState)
{
	layout.id = "rail-when";
	layout.attract.idleSeconds = 1;
	themes::Element panel;
	panel.type = "panel";
	panel.rect = {0, 0, 1, 1};
	panel.when = "attract";
	layout.elements.push_back(panel);
	renderFrame(ImVec2(10, 10), false);
	const int interactive = overlayVertices();
	ASSERT_GE(interactive, 0);
	settle(ImVec2(10, 10), 70);
	EXPECT_GT(overlayVertices(), interactive);
}

TEST_F(ThemeRailTest, BleedElementsReachPastAFixedAspectCanvas)
{
	layout.id = "rail-bleed";
	layout.canvasAspect = 1;
	themes::Element panel;
	panel.type = "panel";
	panel.rect = {0, 0, 1, 1};
	panel.color = 0xff0000ff;
	panel.bleed = true;
	layout.elements.push_back(panel);
	renderFrame(ImVec2(-1, -1), false);
	ImDrawList* drawList = nullptr;
	for (ImGuiWindow* window : GImGui->Windows)
		if (std::string(window->Name).find("themeLibraryOverlay") != std::string::npos)
			drawList = window->DrawList;
	ASSERT_NE(drawList, nullptr);
	ASSERT_GE(drawList->VtxBuffer.Size, 4);
	float width = 0;
	for (const ImDrawVert& vertex : drawList->VtxBuffer)
		width = std::max(width, vertex.pos.x - canvasOrigin.x);
	EXPECT_FLOAT_EQ(width, canvasSize.x);
	EXPECT_GT(canvasSize.x, canvasSize.y); // The panel covers more than the square canvas.
}

TEST_F(ThemeLibraryTest, MarqueeAndSlantedTextDrawWithoutWrapping)
{
	layout.id = "motion-text";
	themes::Element text;
	text.type = "text";
	text.rect = {0, 0, 1, 0.1f};
	text.text = "A long scrolling message";
	text.marqueeSeconds = 10;
	text.slant = 0.2f;
	layout.elements.push_back(text);
	renderFrame(ImVec2(-1, -1), false);
	renderFrame(ImVec2(-1, -1), false);
	EXPECT_NE(bounds[0].w, 0.f);
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

TEST_F(ThemeLibraryTest, RackPullOutKeepsTheSelectedGameAndBlocksExtraLaunches)
{
	layout.id = "pull-out-input";
	layout.mode = "rack";
	layout.columns = 3;
	layout.browser = {0, 0, 0.5f, 0.8f};
	layout.elements = {{"metadata", {0.55f, 0.55f, 0.4f, 0.2f}, "title", {}, 0xe15a77ff, 0.035f, -1}};
	items[0].metadata["title"] = "A";
	items[1].metadata["title"] = "BBBB";
	renderFrame(ImVec2(-1, -1), false);
	const ImVec4 other = bounds[1];
	const ImVec2 point((other.x + other.z) * 0.5f, (other.y + other.w) * 0.5f);
	pulledItemId = items[0].id;
	EXPECT_EQ(renderFrame(point, true), -1);
	EXPECT_EQ(renderFrame(point, false), -1);
	EXPECT_EQ(backgroundVertices, 4u);
}

TEST_F(ThemeLibraryTest, RackPullOutMovesFromTheSlotAndTurnsIntoAFrontCover)
{
	themes::RackCaseAnimation animation;
	animation.slot = ImVec4(40, 100, 56, 320);
	animation.viewport = ImVec4(0, 0, 500, 400);
	animation.title = "Game";
	auto extent = [&](float progress) {
		ImGui::NewFrame();
		themes::drawRackCasePullOut(animation, progress);
		const auto& vertices = ImGui::GetForegroundDrawList()->VtxBuffer;
		ImVec4 result(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
		for (const ImDrawVert& vertex : vertices)
		{
			result.x = std::min(result.x, vertex.pos.x);
			result.y = std::min(result.y, vertex.pos.y);
			result.z = std::max(result.z, vertex.pos.x);
			result.w = std::max(result.w, vertex.pos.y);
		}
		ImGui::EndFrame();
		return result;
	};
	const ImVec4 start = extent(0.f), end = extent(1.f);
	EXPECT_NEAR((start.x + start.z) * 0.5f, 48.f, 4.f);
	EXPECT_NEAR(start.w - start.y, 220.f, 6.f);
	EXPECT_GT(end.z - end.x, (start.z - start.x) * 5.f);
	EXPECT_NEAR((end.x + end.z) * 0.5f, 250.f, 20.f);
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
	EXPECT_EQ(themes::iconArtworkSource(13), "boxtexture");
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

TEST(ThemeStatsTest, CountsActualGamesAndUsesRawHistory)
{
	std::vector<themes::LibraryItem> games(4);
	games[0].isGame = false;
	games[0].favourite = true; games[0].playTimeSeconds = 360000;
	games[1].favourite = true; games[1].playTimeSeconds = 7200; games[1].lastPlayed = 1000;
	games[2].playTimeSeconds = 1800; // Duration without a recorded timestamp is still played.
	EXPECT_EQ(themes::libraryStat("gameCount", games, 1, 16, 2000), 3);
	EXPECT_EQ(themes::libraryStat("favouriteCount", games, 1, 16, 2000), 1);
	EXPECT_EQ(themes::libraryStat("unplayedCount", games, 1, 16, 2000), 1);
	EXPECT_EQ(themes::libraryStat("totalPlayHours", games, 1, 16, 2000), 2.5);
	EXPECT_EQ(themes::libraryStat("maxPlayHours", games, 1, 16, 2000), 2);
	EXPECT_EQ(themes::libraryStat("playHours", games, 2, 16, 2000), .5);
	EXPECT_FALSE(themes::libraryStat("playHours", games, 0, 16, 2000));
	EXPECT_FALSE(themes::libraryStat("neglect", games, 2, 16, 2000));
	EXPECT_FALSE(themes::libraryStat("unrecognized", games, 0, 16, 2000));
}

TEST(ThemeStatsTest, RecencyAndPageStatisticsHandleEmptyFutureAndOldHistory)
{
	std::vector<themes::LibraryItem> games(33);
	const int64_t now = 100000000;
	EXPECT_EQ(themes::libraryStat("neglect", games, 0, 16, now), 1);
	games[0].lastPlayed = now + 60;
	EXPECT_EQ(themes::libraryStat("neglect", games, 0, 16, now), .05);
	games[0].lastPlayed = now - 86400 * 800;
	EXPECT_EQ(themes::libraryStat("neglect", games, 0, 16, now), 1);
	EXPECT_EQ(themes::libraryStat("page", games, 32, 16, now), 3);
	EXPECT_EQ(themes::libraryStat("pageCount", games, 32, 16, now), 3);
	EXPECT_EQ(themes::libraryStat("position", games, 32, 16, now), 33);
	EXPECT_EQ(themes::libraryStat("pageCount", {}, -1, 16, now), 0);
	EXPECT_EQ(themes::libraryStat("page", {}, -1, 16, now), 0);
	EXPECT_FALSE(themes::libraryStat("neglect", {}, -1, 16, now));
}

TEST_F(ThemeLibraryTest, PagedGridSubmitsOneWaveAndPageButtonsReachThePartialLastWave)
{
	layout.id = "paged-grid-buttons";
	layout.browser = {0,0,.6f,1}; layout.columns = 2; layout.minItemWidth = 32;
	layout.pagedGrid.rows = 2; layout.activation = "doubleClick";
	items.clear();
	for (int i=0; i<9; ++i) items.push_back({std::to_string(i), "Game " + std::to_string(i), {}, {}});
	layout.elements = {{"button", {.65f,.3f,.3f,.15f}, {}, "Next"}};
	layout.elements[0].action = "nextPage";
	const ImVec2 away(-1,-1);
	renderFrame(away,false);
	EXPECT_NE(itemIds[0],0u); EXPECT_NE(itemIds[3],0u); EXPECT_EQ(itemIds[4],0u);
	const ImVec2 next(canvasOrigin.x+canvasSize.x*.8f,canvasOrigin.y+canvasSize.y*.375f);
	for (int page=0; page<2; ++page)
	{
		renderFrame(next,false); renderFrame(next,true); EXPECT_EQ(renderFrame(next,false),-1);
		renderFrame(away,false);
	}
	EXPECT_EQ(itemIds[0],0u); EXPECT_EQ(itemIds[7],0u); EXPECT_NE(itemIds[8],0u);
	layout.elements[0].action = "previousPage";
	renderFrame(next,false); renderFrame(next,true); renderFrame(next,false); renderFrame(away,false);
	EXPECT_NE(itemIds[4],0u); EXPECT_NE(itemIds[7],0u); EXPECT_EQ(itemIds[8],0u);
}

TEST_F(ThemeLibraryTest, PagedGridKeyboardMovesAcrossWaveBoundaryAndConfirmsActualGame)
{
	layout.id = "paged-grid-keys";
	layout.browser = {0,0,1,1}; layout.columns = 2; layout.pagedGrid.rows = 2;
	layout.activation = "doubleClick";
	items.clear();
	for (int i=0; i<8; ++i) items.push_back({std::to_string(i), "Game", {}, {}});
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	const ImVec2 away(-1,-1);
	renderFrame(away,false); renderFrame(away,false);
	for (int i=0; i<2; ++i)
	{
		ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow,true); renderFrame(away,false);
		ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow,false); renderFrame(away,false);
	}
	EXPECT_NE(itemIds[4],0u);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter,true);
	EXPECT_EQ(renderFrame(away,false),4);
}

TEST_F(ThemeLibraryTest, SelectionAnchorsFollowTheChosenCellAndTimedUnplayedBannerExpires)
{
	layout.id = "selection-effect-geometry";
	layout.browser = {0,0,1,.7f}; layout.columns = 2; layout.pagedGrid.rows = 1;
	layout.activation = "doubleClick"; items[0].playTimeSeconds = 1;
	themes::Element follower{"panel", {.4f,.8f,.03f,.05f}, {}, {}, 0x07ea92ff};
	follower.anchor = "selectionX";
	themes::Element banner{"panel", {.1f,.5f,.8f,.1f}, {}, {}, 0xea0792ff};
	banner.when = "unplayed"; banner.onSelection = .25f;
	layout.elements = {follower,banner};
	const auto colored = [&](ImU32 col) {
		std::vector<ImVec2> points;
		for (const auto& vertex: overlayMesh) if (vertex.col == col) points.push_back(vertex.pos);
		return points;
	};
	const ImU32 green=IM_COL32(7,234,146,255), pink=IM_COL32(234,7,146,255);
	renderFrame(ImVec2(-1,-1),false);
	const auto first = colored(green);
	ASSERT_FALSE(first.empty()); EXPECT_TRUE(colored(pink).empty());
	const ImVec4 box = bounds[1]; const ImVec2 second((box.x+box.z)/2,(box.y+box.w)/2);
	renderFrame(second,false); renderFrame(second,true); renderFrame(second,false);
	const auto moved = colored(green);
	ASSERT_FALSE(moved.empty()); EXPECT_GT(moved.front().x,first.front().x);
	EXPECT_FALSE(colored(pink).empty());
	ImGui::GetIO().DeltaTime = .4f; renderFrame(ImVec2(-1,-1),false);
	EXPECT_TRUE(colored(pink).empty()); EXPECT_FALSE(colored(green).empty());
	items[1].isGame = false;
	layout.id = "selection-device-not-a-boss";
	renderFrame(second,true); renderFrame(second,false);
	EXPECT_TRUE(colored(pink).empty());
}

TEST_F(ThemeLibraryTest, PagedGridFitsItsRowsAtLargeIconScaleAndTabCanLeaveIt)
{
	layout.id = "paged-grid-large-scale"; layout.browser={0,0,.6f,1};
	layout.columns=2; layout.minItemWidth=32; layout.pagedGrid.rows=4;
	layout.spacing=48; iconScale=2;
	items.resize(4);
	for (int i=0;i<4;++i) items[i].id=std::to_string(i);
	layout.elements={{"button",{.65f,.3f,.3f,.15f},{},"Next"}};
	layout.elements[0].action="next";
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	const ImVec2 away(-1,-1);
	renderFrame(away,false); renderFrame(away,false);
	for (size_t i=0;i<items.size();++i)
	{
		ASSERT_NE(itemIds[i],0u);
		EXPECT_GE(bounds[i].y,browserViewport.y-1);
		EXPECT_LE(bounds[i].w,browserViewport.w+1);
	}
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab,true); renderFrame(away,false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab,false); renderFrame(away,false);
	const ImGuiID before=GImGui->NavId;
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab,true); renderFrame(away,false);
	ImGui::GetIO().AddKeyEvent(ImGuiKey_Tab,false); renderFrame(away,false);
	EXPECT_NE(GImGui->NavId,before);
}
