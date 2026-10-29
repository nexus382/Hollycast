// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_library.h"

#include "gui_util.h"
#include "gui_font.h"
#include "imgui_driver.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>

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

std::string artworkPath(const LibraryItem& item, const std::string& source, const std::string& fallback = "default")
{
	if (source.empty() && !item.image.empty())
		return item.image;
	const auto art = item.metadata.find("artwork." + (source.empty() ? "cover" : source));
	if (art != item.metadata.end() && !art->second.empty())
		return art->second;
	if (fallback == "none")
		return {};
	if (fallback != "default")
	{
		const auto alternate = item.metadata.find("artwork." + fallback);
		if (alternate != item.metadata.end() && !alternate->second.empty())
			return alternate->second;
	}
	return item.image;
}

std::string itemKey(const LibraryItem& item, size_t index)
{
	return item.id.empty() ? "##index-" + std::to_string(index) : item.id;
}

void drawFittedImage(ImDrawList* drawList, const ImRect& rect, const std::string& path,
		const std::string& fit = "contain", uint32_t tint = 0xffffffff, float rounding = 0)
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
	drawList->AddImageRounded(id, pos, ImVec2(pos.x + size.x, pos.y + size.y), uv0, uv1, color(tint), uiScaled(rounding));
	drawList->PopClipRect();
}

void drawSingleLine(ImDrawList* drawList, const ImRect& rect, const std::string& text, ImU32 textColor)
{
	if (rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
		return;
	drawList->PushClipRect(rect.Min, rect.Max, true);
	ImGui::PushStyleColor(ImGuiCol_Text, textColor);
	ImGui::RenderTextEllipsis(drawList, rect.Min, rect.Max, rect.Max.x, text.c_str(), text.c_str() + text.size(), nullptr);
	ImGui::PopStyleColor();
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
		const int start = drawList->VtxBuffer.Size;
		drawList->AddRectFilled(visible.Min, visible.Max,
				element.endColor ? IM_COL32_WHITE : color(element.color), uiScaled(element.rounding));
		if (element.endColor)
		{
			const ImVec4 from = ImGui::ColorConvertU32ToFloat4(color(element.color));
			const ImVec4 to = ImGui::ColorConvertU32ToFloat4(color(*element.endColor));
			for (int i = start; i < drawList->VtxBuffer.Size; ++i)
			{
				ImDrawVert& vertex = drawList->VtxBuffer[i];
				const float t = std::clamp(element.gradient == "horizontal"
						? (vertex.pos.x - rect.Min.x) / rect.GetWidth()
						: (vertex.pos.y - rect.Min.y) / rect.GetHeight(), 0.f, 1.f);
				// Interpolate opacity too, while preserving the rounded edge's antialias coverage.
				ImVec4 shade = ImLerp(from, to, t);
				shade.w *= ((vertex.col >> IM_COL32_A_SHIFT) & 0xff) / 255.f;
				vertex.col = ImGui::ColorConvertFloat4ToU32(shade);
			}
		}
	}
	else if (element.type == "image")
	{
		drawFittedImage(drawList, visible, element.resolvedAsset, element.fit, element.color, element.rounding);
	}
	else if (item && element.type == "artwork")
	{
		// A miximage already contains its own cover/logo/disc. Do not duplicate those
		// overlays when the hero falls back from a standalone screenshot to a miximage.
		if (!element.requiresArtwork.empty() && artworkPath(*item, element.requiresArtwork, "none").empty())
			return;
		drawFittedImage(drawList, visible, artworkPath(*item, element.source, element.fallback), element.fit, element.color, element.rounding);
	}
	else if (element.type == "text" || (item && element.type == "metadata"))
	{
		const std::string text = item ? elementText(element, *item) : element.type == "text" ? element.text : std::string();
		if (text.empty())
			return;
		const float fontSize = std::clamp(element.fontSize * canvas.size.y, 10.0f, 96.0f);
		ImGui::PushFont(element.bold ? boldFont : nullptr, fontSize);
		ImVec2 textPos = visible.Min;
		if (element.align != "left")
		{
			const float textWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX,
					element.wrap ? visible.GetWidth() : 0.f, text.c_str()).x;
			textPos.x += std::max(0.f, visible.GetWidth() - textWidth) * (element.align == "center" ? 0.5f : 1.f);
		}
		if (element.wrap)
		{
			// Clip at complete lines so a short description panel never shows half a glyph.
			const float bottom = visible.Min.y + std::floor(visible.GetHeight() / fontSize) * fontSize;
			drawList->PushClipRect(visible.Min, ImVec2(visible.Max.x, bottom), true);
			drawList->AddText(ImGui::GetFont(), fontSize, textPos, color(element.color), text.c_str(), nullptr, visible.GetWidth());
			drawList->PopClipRect();
		}
		else
			drawSingleLine(drawList, ImRect(textPos, visible.Max), text, color(element.color));
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
	const bool customSelector = layout.selectorColor.has_value() || layout.rounding > 0;
	const ImVec4 selector = layout.selectorColor
			? ImGui::ColorConvertU32ToFloat4(color(*layout.selectorColor)) : ImGui::GetStyleColorVec4(ImGuiCol_Header);
	if (customSelector)
	{
		ImVec4 fill = selector;
		if (layout.rounding > 0) fill.w = 0;
		ImGui::PushStyleColor(ImGuiCol_Header, fill);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, fill);
		fill.w *= 0.65f;
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, fill);
	}
	const bool pressed = ImGui::Selectable("##libraryItem", selected, ImGuiSelectableFlags_None, size);
	if (customSelector)
		ImGui::PopStyleColor(3);
	if (layout.rounding > 0 && (selected || ImGui::IsItemHovered()))
	{
		ImVec4 fill = selector;
		if (!selected && !pressed) fill.w *= 0.65f;
		ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
				ImGui::ColorConvertFloat4ToU32(fill), uiScaled(layout.rounding));
	}
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
// IsItemHovered follows active mouse/navigation highlighting; focus alone persists after mouse movement.
std::optional<std::string> iconArtworkSource(int mediaMode)
{
	static const std::array<const char*, 9> sources{
		"", "miximage", "cover", "case", "screenshot", "title", "physical", "fanart", "titlescreen"
	};
	if (mediaMode < 0 || mediaMode >= static_cast<int>(sources.size()))
		return std::nullopt;
	return sources[mediaMode];
}

int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem,
		const std::optional<std::string>& iconSource)
{
	const std::string& browserSource = iconSource ? *iconSource : layout.artworkSource;
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

	const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
	const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
	const Canvas canvas{canvasOrigin, canvasSize, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y)};
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(canvas.origin, canvas.max, color(layout.background));

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
			// ImGui snaps row positions to pixels; snap the height too to avoid overlapping hit boxes.
			const float rowHeight = std::ceil(layout.rowHeight > 0
					? std::max(ImGui::GetFontSize() + uiScaled(8.f), layout.rowHeight * canvas.size.y)
					: std::max(uiScaled(34.0f), ImGui::GetFontSize() + spacing));
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
					const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
					if (hovered || pressed)
					{
						selected = i;
						selectedId = key;
					}
					if (pressed)
						activated = i;
					const float padding = uiScaled(8.f);
					float textLeft = itemRect.Min.x + padding;
					if (layout.showArtwork || iconSource.has_value())
					{
						const float side = std::max(1.f, rowHeight - padding);
						const ImRect thumb(ImVec2(textLeft, itemRect.Min.y + padding * 0.5f),
								ImVec2(textLeft + side, itemRect.Max.y - padding * 0.5f));
						drawFittedImage(browserDrawList, thumb, artworkPath(item, browserSource), "contain", 0xffffffff, layout.rounding);
						textLeft += side + padding;
					}
					if (!item.title.empty())
					{
						const float controlsWidth = std::min(uiScaled(32.f), rowHeight - padding) + padding * 2.f;
						const ImRect textRect(ImVec2(textLeft, itemRect.Min.y + (rowHeight - ImGui::GetFontSize()) * 0.5f),
								ImVec2(itemRect.Max.x - controlsWidth, itemRect.Max.y));
						drawSingleLine(browserDrawList, textRect, item.title, browserTextColor(layout, selectedId == key || hovered));
					}
					if (layout.separatorColor)
						browserDrawList->AddLine(ImVec2(itemRect.Min.x + padding, itemRect.Max.y),
								ImVec2(itemRect.Max.x - padding, itemRect.Max.y), color(*layout.separatorColor));
					if (layout.selectorBorderColor && (selectedId == key || hovered))
						browserDrawList->AddRect(itemRect.Min, itemRect.Max, color(*layout.selectorBorderColor), uiScaled(layout.rounding));
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
						const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
						if (hovered || pressed)
						{
							selected = i;
							selectedId = key;
						}
						if (pressed)
							activated = i;

						const ImRect imageRect(itemRect.Min,
								ImVec2(itemRect.Max.x, itemRect.Min.y + cellHeight * (layout.showTitles ? 0.78f : 1.f)));
						const std::string image = artworkPath(item, browserSource);
						const uint32_t tint = 0xffffff00 | static_cast<uint32_t>(255.f * ((selectedId == key || hovered) ? 1.f : layout.inactiveOpacity));
						drawFittedImage(browserDrawList, imageRect, image, "contain", tint, layout.rounding);
						if (selectedId == key || hovered)
							browserDrawList->AddRect(itemRect.Min, itemRect.Max,
									layout.selectorBorderColor ? color(*layout.selectorBorderColor)
											: layout.selectorColor ? color(*layout.selectorColor) : ImGui::GetColorU32(ImGuiCol_NavHighlight),
									uiScaled(layout.rounding), 0, uiScaled(2.f));
						if (!layout.showTitles && image.empty())
							drawSingleLine(browserDrawList,
								ImRect(ImVec2(itemRect.Min.x + uiScaled(4.f), itemRect.Min.y + cellHeight * 0.5f),
										ImVec2(itemRect.Max.x - uiScaled(4.f), itemRect.Max.y)),
								item.title, browserTextColor(layout, selectedId == key || hovered));
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
	const LibraryItem* current = selected >= 0 ? &items[selected] : nullptr;
	// Resolve background media after hover/navigation so both layers show the same game.
	// The parent draw list is rendered beneath the browser child, regardless of submission order.
	drawElements(layout, canvas, themeDirectory, current, drawList, true);
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
