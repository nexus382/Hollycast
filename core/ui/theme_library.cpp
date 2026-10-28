// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_library.h"

#include "gui_util.h"
#include "imgui_driver.h"
#include "imgui.h"

#include <algorithm>

namespace themes
{
namespace
{
ImU32 color(uint32_t rgba)
{
	return IM_COL32((rgba >> 24) & 0xff, (rgba >> 16) & 0xff,
			(rgba >> 8) & 0xff, rgba & 0xff);
}

struct Canvas
{
	ImVec2 origin;
	ImVec2 size;
	ImVec2 max;
};

ImRect normalizedRect(const Canvas& canvas, const Rect& rect)
{
	return ImRect(ImVec2(canvas.origin.x + rect.x * canvas.size.x, canvas.origin.y + rect.y * canvas.size.y),
			ImVec2(canvas.origin.x + (rect.x + rect.w) * canvas.size.x,
					canvas.origin.y + (rect.y + rect.h) * canvas.size.y));
}

std::string artworkPath(const LibraryItem& item, const std::string& source)
{
	if (source.empty())
	{
		if (!item.image.empty())
			return item.image;
		const auto cover = item.metadata.find("artwork.cover");
		return cover == item.metadata.end() ? std::string() : cover->second;
	}
	if (source == "artwork.cover" || source == "cover")
		return item.metadata.count("artwork.cover") ? item.metadata.at("artwork.cover") : item.image;
	if (source == "artwork.screenshot" || source == "screenshot")
		return item.metadata.count("artwork.screenshot") ? item.metadata.at("artwork.screenshot") : std::string();
	if (source == "artwork.fanart" || source == "fanart")
		return item.metadata.count("artwork.fanart") ? item.metadata.at("artwork.fanart") : std::string();
	if (source == "artwork.title" || source == "title")
		return item.metadata.count("artwork.title") ? item.metadata.at("artwork.title") : std::string();
	if (source == "artwork.physical" || source == "physical")
		return item.metadata.count("artwork.physical") ? item.metadata.at("artwork.physical") : std::string();
	return item.metadata.count(source) ? item.metadata.at(source) : std::string();
}

std::string itemKey(const LibraryItem& item, size_t index)
{
	return item.id.empty() ? "##index-" + std::to_string(index) : item.id;
}

void drawFittedImage(ImDrawList* drawList, const ImRect& rect, const std::string& path,
		const std::string& fit = "contain", uint32_t tint = 0xffffffff)
{
	if (path.empty() || rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
		return;
	ImguiFileTexture texture(path);
	const ImTextureID id = texture.getId();
	if (id == ImTextureID{})
		return;
	const float aspect = std::max(0.001f, imguiDriver->getAspectRatio(id));
	float width = rect.GetWidth();
	float height = rect.GetHeight();
	ImVec2 uv0(0, 0), uv1(1, 1);
	if (fit == "cover")
	{
		const float targetAspect = width / height;
		if (aspect > targetAspect)
		{
			const float visibleWidth = targetAspect / aspect;
			uv0.x = (1.f - visibleWidth) / 2.f;
			uv1.x = 1.f - uv0.x;
		}
		else
		{
			const float visibleHeight = aspect / targetAspect;
			uv0.y = (1.f - visibleHeight) / 2.f;
			uv1.y = 1.f - uv0.y;
		}
	}
	else if (fit == "contain")
	{
		height = width / aspect;
		if (height > rect.GetHeight())
		{
			height = rect.GetHeight();
			width = height * aspect;
		}
	}
	const ImVec2 size(width, height);
	const ImVec2 pos(rect.Min.x + (rect.GetWidth() - width) * 0.5f,
			rect.Min.y + (rect.GetHeight() - height) * 0.5f);
	drawList->PushClipRect(rect.Min, rect.Max, true);
	drawList->AddImage(id, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1, color(tint));
	drawList->PopClipRect();
}

std::string elementText(const Element& element, const LibraryItem& item)
{
	if (element.type == "text")
		return element.text;
	if (element.type == "metadata")
	{
		const auto it = item.metadata.find(element.source);
		return it == item.metadata.end() ? std::string() : it->second;
	}
	return {};
}

void drawElement(const Element& element, const Canvas& canvas, const std::string& themeDirectory,
		const LibraryItem* item, ImDrawList* drawList)
{
	const ImRect rect = normalizedRect(canvas, element.rect);
	const ImRect visible(ImMax(rect.Min, canvas.origin), ImMin(rect.Max, canvas.max));
	if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0)
		return;
	if (element.type == "panel")
	{
		drawList->AddRectFilled(visible.Min, visible.Max, color(element.color));
	}
	else if (element.type == "image")
	{
		drawFittedImage(drawList, visible, element.resolvedAsset, element.fit, element.color);
	}
	else if (item && element.type == "artwork")
	{
		drawFittedImage(drawList, visible, artworkPath(*item, element.source), element.fit, element.color);
	}
	else if (element.type == "text" || (item && element.type == "metadata"))
	{
		const std::string text = item ? elementText(element, *item) : element.type == "text" ? element.text : std::string();
		if (text.empty())
			return;
		const float fontSize = std::clamp(element.fontSize * canvas.size.y, 10.0f, 96.0f);
		ImGui::PushFont(nullptr, fontSize);
		ImVec2 textPos = visible.Min;
		if (element.align != "left")
		{
			const float textWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, visible.GetWidth(), text.c_str()).x;
			textPos.x += std::max(0.f, visible.GetWidth() - textWidth) * (element.align == "center" ? 0.5f : 1.f);
		}
		drawList->PushClipRect(visible.Min, visible.Max, true);
		drawList->AddText(ImGui::GetFont(), fontSize, textPos, color(element.color), text.c_str(), nullptr, visible.GetWidth());
		drawList->PopClipRect();
		ImGui::PopFont();
	}
}

void drawElements(const Layout& layout, const Canvas& canvas, const std::string& themeDirectory,
		const LibraryItem* item, ImDrawList* drawList, bool beforeBrowser)
{
	std::vector<const Element*> sorted;
	sorted.reserve(layout.elements.size());
	for (const Element& element : layout.elements)
		if ((element.zIndex < 0) == beforeBrowser)
			sorted.push_back(&element);
	std::stable_sort(sorted.begin(), sorted.end(), [](const Element* a, const Element* b) {
		return a->zIndex < b->zIndex;
	});
	for (const Element* element : sorted)
		drawElement(*element, canvas, themeDirectory, item, drawList);
}

bool browserSelectable(const Layout& layout, bool selected, const ImVec2& size)
{
	if (layout.selectorColor)
	{
		ImVec4 selector = ImGui::ColorConvertU32ToFloat4(color(*layout.selectorColor));
		ImGui::PushStyleColor(ImGuiCol_Header, selector);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, selector);
		selector.w *= 0.65f;
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selector);
	}
	const bool pressed = ImGui::Selectable("##libraryItem", selected, ImGuiSelectableFlags_None, size);
	if (layout.selectorColor)
		ImGui::PopStyleColor(3);
	return pressed;
}

ImU32 browserTextColor(const Layout& layout, bool selected)
{
	if (selected && layout.selectedTextColor)
		return color(*layout.selectedTextColor);
	return layout.textColor ? color(*layout.textColor) : ImGui::GetColorU32(ImGuiCol_Text);
}
}

// Negative zIndex elements are drawn behind the browser; zero and positive
// elements are drawn over it. All element geometry uses the full content canvas.
int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem)
{
	static std::string selectedId;
	static std::string selectedLayoutId;
	const std::string layoutKey = themeDirectory + '/' + layout.id;
	if (selectedLayoutId != layoutKey)
	{
		selectedLayoutId = layoutKey;
		selectedId.clear();
	}
	int selected = -1;
	for (int i = 0; i < static_cast<int>(items.size()); ++i)
		if (itemKey(items[i], static_cast<size_t>(i)) == selectedId)
		{
			selected = i;
			break;
		}
	if (selected < 0 && !items.empty())
	{
		selected = 0;
		selectedId = itemKey(items.front(), 0);
	}
	const LibraryItem* current = selected >= 0 ? &items[selected] : nullptr;

	const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	const Canvas canvas{canvasOrigin, canvasSize, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y)};
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(canvas.origin, canvas.max, color(layout.background));
	drawElements(layout, canvas, themeDirectory, current, drawList, true);

	const ImRect browser = normalizedRect(canvas, layout.browser);
	const ImVec2 browserPos(ImMax(browser.Min.x, canvas.origin.x), ImMax(browser.Min.y, canvas.origin.y));
	const ImVec2 browserMax(ImMin(browser.Max.x, canvas.max.x), ImMin(browser.Max.y, canvas.max.y));
	const ImVec2 browserSize(std::max(0.0f, browserMax.x - browserPos.x), std::max(0.0f, browserMax.y - browserPos.y));
	ImGui::SetCursorScreenPos(browserPos);
	const bool childVisible = ImGui::BeginChild("##themeLibraryBrowser", browserSize, ImGuiChildFlags_NavFlattened,
			ImGuiWindowFlags_NoBackground);
	if (layout.fontSize > 0)
		ImGui::PushFont(nullptr, std::clamp(layout.fontSize * canvas.size.y, 10.f, 96.f));
	const ImVec2 previousItemSpacing = ImGui::GetStyle().ItemSpacing;
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(previousItemSpacing.x, 0.0f));
	ImDrawList* browserDrawList = ImGui::GetWindowDrawList();
	int activated = -1;
	if (childVisible && !items.empty())
	{
		const float spacing = std::max(0.0f, uiScaled(layout.spacing));
		if (layout.mode == "list")
		{
			const float rowHeight = layout.rowHeight > 0
					? std::max(ImGui::GetFontSize() + uiScaled(8.f), layout.rowHeight * canvas.size.y)
					: std::max(uiScaled(34.0f), ImGui::GetFontSize() + spacing);
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(items.size()), rowHeight);
			if (selected >= 0)
				clipper.IncludeItemsByIndex(selected, selected + 1);
			while (clipper.Step())
				for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
				{
					const LibraryItem& item = items[i];
					const std::string key = itemKey(item, static_cast<size_t>(i));
					ImguiID id(key);
					const bool isSelected = selectedId == key;
					const bool pressed = browserSelectable(layout, isSelected, ImVec2(0.0f, rowHeight));
					const bool hovered = ImGui::IsItemHovered();
					const bool focused = ImGui::IsItemFocused();
					const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
					if (hovered || focused || pressed)
					{
						selected = i;
						selectedId = key;
					}
					if (pressed)
						activated = i;
					if (!item.title.empty())
					{
						browserDrawList->PushClipRect(itemRect.Min, itemRect.Max, true);
						browserDrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
								ImVec2(itemRect.Min.x + uiScaled(5.0f), itemRect.Min.y + uiScaled(4.0f)),
								browserTextColor(layout, selectedId == key || hovered), item.title.c_str(), nullptr,
								std::max(0.0f, itemRect.GetWidth() - uiScaled(10.0f)));
						browserDrawList->PopClipRect();
					}
					// Draw per-game controls after the artwork/title, retaining the selectable's item data.
					if (afterItem)
						afterItem(static_cast<size_t>(i));
				}
		}
		else
		{
			const float availWidth = ImGui::GetContentRegionAvail().x;
			const int columns = std::clamp(layout.columns, 1, std::max(1, static_cast<int>((availWidth + spacing) / uiScaled(96.0f))));
			const float cellWidth = std::max(1.0f, (availWidth - spacing * (columns - 1)) / columns);
			const float aspect = std::clamp(layout.itemAspect, 0.3f, 3.0f);
			const float cellHeight = std::max(uiScaled(70.0f), cellWidth / aspect);
			const int rows = (static_cast<int>(items.size()) + columns - 1) / columns;
			ImGuiListClipper clipper;
			clipper.Begin(rows, cellHeight + spacing);
			if (selected >= 0)
				clipper.IncludeItemsByIndex(selected / columns, selected / columns + 1);
			while (clipper.Step())
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				{
					const float rowY = ImGui::GetCursorPosY();
					const float rowX = ImGui::GetCursorPosX();
					for (int col = 0; col < columns; ++col)
					{
						const int i = row * columns + col;
						if (i >= static_cast<int>(items.size()))
							break;
						const LibraryItem& item = items[i];
						const float x = rowX + col * (cellWidth + spacing);
						ImGui::SetCursorPos(ImVec2(x, rowY));
						const std::string key = itemKey(item, static_cast<size_t>(i));
						ImguiID id(key);
						const bool isSelected = selectedId == key;
						const bool pressed = browserSelectable(layout, isSelected, ImVec2(cellWidth, cellHeight));
						const bool hovered = ImGui::IsItemHovered();
						const bool focused = ImGui::IsItemFocused();
						const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
						if (hovered || focused || pressed)
						{
							selected = i;
							selectedId = key;
						}
						if (pressed)
							activated = i;

						const ImRect imageRect(itemRect.Min, ImVec2(itemRect.Max.x, itemRect.Min.y + cellHeight * 0.78f));
						const std::string image = item.image.empty()
								? artworkPath(item, "") : item.image;
						drawFittedImage(browserDrawList, imageRect, image);
						if (selectedId == key || hovered)
							browserDrawList->AddRect(itemRect.Min, itemRect.Max,
									layout.selectorColor ? color(*layout.selectorColor) : ImGui::GetColorU32(ImGuiCol_NavHighlight), 0.0f, 0, 2.0f);
						if (layout.showTitles && !item.title.empty())
						{
							const ImRect titleRect(ImVec2(itemRect.Min.x, imageRect.Max.y), itemRect.Max);
							browserDrawList->PushClipRect(titleRect.Min, titleRect.Max, true);
							browserDrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
									ImVec2(titleRect.Min.x + uiScaled(4.0f), titleRect.Min.y + uiScaled(3.0f)),
									browserTextColor(layout, selectedId == key || hovered), item.title.c_str(), nullptr,
									std::max(0.0f, titleRect.GetWidth() - uiScaled(8.0f)));
							browserDrawList->PopClipRect();
						}
						if (afterItem)
							afterItem(static_cast<size_t>(i));
					}
					ImGui::SetCursorPos(ImVec2(rowX, rowY + cellHeight + spacing));
				}
		}
	}
	else if (childVisible)
	{
		ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(browserTextColor(layout, false)), "%s", i18n::T("No games found"));
	}
	ImGui::PopStyleVar();
	if (layout.fontSize > 0)
		ImGui::PopFont();
	ImGui::EndChild();
	current = selected >= 0 ? &items[selected] : nullptr;
	ImGui::SetCursorScreenPos(canvas.origin);
	ImGui::BeginChild("##themeLibraryOverlay", canvas.size, ImGuiChildFlags_None,
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus
					| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawElements(layout, canvas, themeDirectory, current, ImGui::GetWindowDrawList(), false);
	ImGui::EndChild();
	ImGui::SetCursorScreenPos(canvas.max);
	return activated;
}
} // namespace themes
