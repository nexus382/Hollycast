// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_library.h"
#include "theme_artwork.h"

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
	float textScale = 1.f;
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
		const std::string& fit = "contain", uint32_t tint = 0xffffffff, float rounding = 0, const PixelArt& pixelArt = {})
{
	if (!imguiDriver || path.empty() || rect.GetWidth() <= 0 || rect.GetHeight() <= 0)
		return;
	ImguiFileTexture texture(path, pixelArt.resolution > 0);
	texture.setPixelArt(pixelArt);
	const ImTextureID id = texture.getId();
	if (id == ImTextureID{})
		return;
	float aspect = std::max(0.001f, imguiDriver->getAspectRatio(id));
	// Spine scans may be stored horizontally; rotate them instead of stretching a cover.
	const bool rotateSpine = fit == "spine" && aspect > 1.f;
	if (rotateSpine) aspect = 1.f / aspect;
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
	else if (fit == "spine")
	{
		// Shelf slots reserve the proportional width, so PAL scans can reach full
		// case height without being squeezed into the narrower NTSC case width.
		width = height * aspect;
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
	if (rotateSpine)
		drawList->AddImageQuad(id, pos, ImVec2(pos.x + size.x, pos.y), ImVec2(pos.x + size.x, pos.y + size.y),
				ImVec2(pos.x, pos.y + size.y), ImVec2(0, 1), ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), color(tint));
	else
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

void drawSpine(ImDrawList* drawList, const ImRect& slot, const LibraryItem& item,
		const std::string& source, bool selected, uint32_t tint)
{
	const ImRect rect(ImVec2(slot.Min.x + 1.f, slot.Min.y + (selected ? 0.f : uiScaled(6.f))),
			ImVec2(slot.Max.x - 1.f, slot.Max.y));
	// Missing scans get a deliberately typeset spine, never a narrow crop of the front cover.
	const std::string path = artworkPath(item, source, "none");
	const ImU32 base = IM_COL32(35, 44, 55, 255);
	drawList->AddRectFilledMultiColor(rect.Min, rect.Max, IM_COL32(72, 84, 98, 255), base, base, IM_COL32(55, 65, 78, 255));
	if (!path.empty())
		drawFittedImage(drawList, rect, path, source == "spine" ? "spine" : "contain", tint);
	else if (rect.GetWidth() > 4.f)
	{
		const float padding = uiScaled(8.f);
		const float fontSize = std::max(9.f, std::min(rect.GetWidth() - uiScaled(7.f), uiScaled(17.f)));
		const float textLength = std::max(1.f, rect.GetHeight() - padding * 2.f);
		ImGui::PushFont(nullptr, fontSize);
		const ImVec2 clipMin = ImMax(drawList->GetClipRectMin(), rect.Min);
		const ImVec2 clipMax = ImMin(drawList->GetClipRectMax(), rect.Max);
		// Lay out horizontal glyphs with ellipsis, then rotate their geometry. Replace
		// their temporary clip rectangles so partially visible shelves still clip correctly.
		drawList->PushClipRect(ImVec2(0, 0), ImVec2(textLength, fontSize), false);
		const int firstCommand = drawList->CmdBuffer.Size - 1;
		const int firstVertex = drawList->VtxBuffer.Size;
		drawSingleLine(drawList, ImRect(0, 0, textLength, fontSize), item.title, IM_COL32(240, 235, 221, 255));
		for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
		{
			ImVec2& pos = drawList->VtxBuffer[i].pos;
			pos = ImVec2(rect.GetCenter().x + fontSize * 0.5f - pos.y, rect.Min.y + padding + pos.x);
		}
		for (int i = firstCommand; i < drawList->CmdBuffer.Size; ++i)
			drawList->CmdBuffer[i].ClipRect = ImVec4(clipMin.x, clipMin.y, clipMax.x, clipMax.y);
		drawList->PopClipRect();
		ImGui::PopFont();
	}
	// The thin highlight is the jewel case edge, not additional artwork.
	drawList->AddLine(rect.Min, ImVec2(rect.Min.x, rect.Max.y), IM_COL32(255, 255, 255, 70));
	drawList->AddLine(ImVec2(rect.Max.x, rect.Min.y), rect.Max, IM_COL32(0, 0, 0, 180));
}

std::string elementText(const Element& element, const LibraryItem& item)
{
	if (element.type == "text")
		return element.text;
	if (element.type == "metadata")
	{
		const auto it = item.metadata.find(element.source);
		return it == item.metadata.end() || it->second.empty() ? element.text : it->second;
	}
	return {};
}

void drawElement(const Element& element, const Canvas& canvas, const std::string& themeDirectory,
		const LibraryItem* item, ImDrawList* drawList, const std::string& previewSource)
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
		if (!element.requiresArtwork.empty() && artworkPath(*item, element.requiresArtwork, "none").empty())
			return;
		const std::string& source = element.source == "preview" ? previewSource : element.source;
		const std::string fallback = element.source == "preview" && source != "cover" ? "none" : element.fallback;
		std::string path = artworkPath(*item, source, fallback);
		if (path.empty()) path = element.resolvedFallbackAsset;
		drawFittedImage(drawList, visible, path, element.fit, element.color, element.rounding);
	}
	else if (element.type == "text" || (item && element.type == "metadata"))
	{
		const std::string text = item ? elementText(element, *item) : element.type == "text" ? element.text : std::string();
		if (text.empty())
			return;
		// Small label rectangles must still show a whole line on shorter windows.
		const float fontSize = std::min(visible.GetHeight(),
				std::clamp(element.fontSize * canvas.size.y * canvas.textScale, 1.f, 96.f));
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
		const LibraryItem* item, ImDrawList* drawList, bool beforeBrowser, const Rect& details, bool detailsOnly, const std::string& previewSource)
{
	std::vector<const Element*> sorted;
	sorted.reserve(layout.elements.size());
	for (const Element& element : layout.elements)
	{
		const bool inDetails = details.w > 0.f && element.rect.x >= details.x - 0.0001f
				&& element.rect.x + element.rect.w <= details.x + details.w + 0.0001f;
		if ((element.zIndex < 0) == beforeBrowser && inDetails == detailsOnly)
			sorted.push_back(&element);
	}
	std::stable_sort(sorted.begin(), sorted.end(), [](const Element* a, const Element* b) {
		return a->zIndex < b->zIndex;
	});
	for (const Element* element : sorted)
		drawElement(*element, canvas, themeDirectory, item, drawList, previewSource);
}

bool browserSelectable(const Layout& layout, bool selected, const ImVec2& size)
{
	const bool customSelector = layout.selectorColor.has_value() || layout.rounding > 0 || layout.selectorStyle == "outline";
	const ImVec4 selector = layout.selectorColor
			? ImGui::ColorConvertU32ToFloat4(color(*layout.selectorColor)) : ImGui::GetStyleColorVec4(ImGuiCol_Header);
	if (customSelector)
	{
		ImVec4 fill = selector;
		if (layout.rounding > 0 || layout.selectorStyle == "outline") fill.w = 0;
		ImGui::PushStyleColor(ImGuiCol_Header, fill);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, fill);
		fill.w *= 0.65f;
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, fill);
	}
	const ImGuiSelectableFlags flags = layout.activation == "doubleClick" ? ImGuiSelectableFlags_AllowDoubleClick : ImGuiSelectableFlags_None;
	const bool pressed = ImGui::Selectable("##libraryItem", selected, flags, size);
	if (customSelector)
		ImGui::PopStyleColor(3);
	if (layout.rounding > 0 && layout.selectorStyle != "outline" && (selected || ImGui::IsItemHovered()))
	{
		ImVec4 fill = selector;
		if (!selected && !pressed) fill.w *= 0.65f;
		ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
				ImGui::ColorConvertFloat4ToU32(fill), uiScaled(layout.rounding));
	}
	return pressed;
}

bool activatesItem(const Layout& layout, bool pressed)
{
	return pressed && (layout.activation == "singleClick" || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
			|| GImGui->NavActivateId == ImGui::GetItemID());
}

const char* metadataLabel(const std::string& field)
{
	static const std::map<std::string, const char*> labels = {
		{"title", "Title"}, {"developer", "Developer"}, {"publisher", "Publisher"},
		{"genre", "Genre"}, {"players", "Players"}, {"releaseDate", "Released"},
		{"productId", "Product ID"}, {"region", "Region"}, {"format", "Format"},
		{"playTime", "Play time"}, {"lastPlayed", "Last played"}, {"size", "File size"}
	};
	const auto label = labels.find(field);
	return label == labels.end() ? field.c_str() : i18n::T(label->second);
}

void drawDetails(const Layout& layout, const Canvas& canvas, const LibraryItem* item)
{
	for (size_t i = 0; i < layout.elements.size(); ++i)
	{
		const Element& element = layout.elements[i];
		if (element.type != "details") continue;
		const ImRect rect = normalizedRect(canvas, element.rect);
		ImGui::SetCursorScreenPos(rect.Min);
		ImguiID id("theme-details-" + std::to_string(i));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool visible = ImGui::BeginChild("##details", rect.GetSize(), ImGuiChildFlags_None,
				ImGuiWindowFlags_NoBackground);
		if (visible && item)
		{
			ImGui::PushFont(nullptr, std::clamp(element.fontSize * canvas.size.y, 10.f, 96.f));
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color(element.color)));
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(uiScaled(5), uiScaled(6)));
			const ImGuiID itemId = ImGui::GetID(item->id.c_str());
			ImGuiStorage* state = ImGui::GetStateStorage();
			if (state->GetInt(ImGui::GetID("selectedItem")) != static_cast<int>(itemId))
			{
				ImGui::SetScrollY(0);
				state->SetInt(ImGui::GetID("selectedItem"), static_cast<int>(itemId));
			}
			bool hasData = false;
			std::string description;
			if (ImGui::BeginTable("##metadata", element.columns * 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				for (int col = 0; col < element.columns; ++col)
				{
					ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.36f);
					ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.64f);
				}
				for (const std::string& field : element.fields)
				{
					const auto value = item->metadata.find(field);
					if (value == item->metadata.end() || value->second.empty()) continue;
					if (field == "description") { description = value->second; continue; }
					ImGui::TableNextColumn();
					ImGui::TextDisabled("%s", metadataLabel(field));
					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", value->second.c_str());
					hasData = true;
				}
				ImGui::EndTable();
			}
			if (!description.empty())
			{
				if (hasData) ImGui::Separator();
				ImGui::TextWrapped("%s", description.c_str());
			}
			else if (!hasData)
				ImGui::TextDisabled("%s", element.text.empty() ? i18n::T("No game details available.") : element.text.c_str());
			ImGui::PopStyleVar();
			ImGui::PopStyleColor();
			ImGui::PopFont();
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
	}
}

void drawFavouriteMarker(const Layout& layout, const ImRect& rect, ImDrawList* drawList)
{
 const float padding = uiScaled(4.f);
 const float side = std::min({ImGui::GetFontSize() * 1.2f, rect.GetWidth() - padding * 2, rect.GetHeight() - padding * 2});
 if (side < 7.f) return;
 const ImU32 ink = color(layout.pixelArt.resolution > 0 ? layout.pixelArt.dark : layout.textColor.value_or(0xffffffff));
 drawList->PushClipRect(rect.Min, rect.Max, true);
 if (layout.pixelArt.resolution > 0)
 {
  // A seven-dot LCD star stays sharp at small VMU sizes.
  static constexpr const char* rows[] = {"0001000", "0011100", "1111111", "0111110", "0011100", "0110110", "1100011"};
  const float dot = std::max(1.f, std::floor(side / 7.f));
  const ImVec2 origin(std::floor(rect.Max.x - padding - dot * 7), std::floor(rect.Max.y - padding - dot * 7));
  drawList->AddRectFilled(ImVec2(origin.x - dot, origin.y - dot), ImVec2(origin.x + dot * 8, origin.y + dot * 8), color(layout.pixelArt.light));
  for (int y = 0; y < 7; ++y)
   for (int x = 0; x < 7; ++x)
    if (rows[y][x] == '1')
     drawList->AddRectFilled(ImVec2(origin.x + x * dot, origin.y + y * dot), ImVec2(origin.x + (x + 1) * dot, origin.y + (y + 1) * dot), ink);
 }
 else
 {
  const ImVec2 center(rect.Max.x - padding - side * 0.5f, rect.Max.y - padding - side * 0.5f);
  ImVec2 points[10];
  for (int i = 0; i < 10; ++i) {
   const float angle = -IM_PI * 0.5f + i * IM_PI * 0.2f;
   const float radius = side * (i % 2 ? 0.23f : 0.5f);
   points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
  }
  drawList->AddConcavePolyFilled(points, 10, ink);
 }
 drawList->PopClipRect();
}

void drawButtonContents(const ImRect& rect, const std::string& label, const std::string& icon)
{
	const float fontSize = ImGui::GetFontSize();
	const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
	const float iconSize = fontSize;
	const float gap = label.empty() ? 0.f : fontSize * 0.45f;
	const float width = iconSize + gap + textSize.x;
	const ImVec2 center(rect.Min.x + (rect.GetWidth() - width + iconSize) * 0.5f,
			rect.Min.y + rect.GetHeight() * 0.5f);
	ImVec2 points[10];
	for (int i = 0; i < 10; ++i)
	{
		const float angle = -IM_PI * 0.5f + i * IM_PI * 0.2f;
		const float radius = iconSize * (i % 2 == 0 ? 0.5f : 0.23f);
		points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
	}
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	const ImU32 foreground = ImGui::GetColorU32(ImGuiCol_Text);
	drawList->PushClipRect(rect.Min, rect.Max, true);
	if (icon == "star-filled")
		drawList->AddConcavePolyFilled(points, 10, foreground);
	drawList->AddPolyline(points, 10, foreground, ImDrawFlags_Closed, std::max(1.f, fontSize * 0.075f));
	drawList->AddText(ImVec2(center.x + iconSize * 0.5f + gap,
			rect.Min.y + (rect.GetHeight() - textSize.y) * 0.5f), foreground, label.c_str());
	drawList->PopClipRect();
}

void drawButtons(const Layout& layout, const Canvas& canvas, const std::vector<LibraryItem>& items,
		int columns, int& selected, std::string& previewSource, bool& scrollSelection, int& activated,
		const std::function<void(size_t)>& toggleFavourite)
{
	const size_t count = items.size();
	for (size_t i = 0; i < layout.elements.size(); ++i)
	{
		const Element& element = layout.elements[i];
		if (element.type != "button") continue;
		const ImRect rect = normalizedRect(canvas, element.rect);
		ImGui::SetCursorScreenPos(rect.Min);
		ImguiID id("theme-action-" + std::to_string(i));
		// A small input window gives a button its own hit region without a full-canvas
		// overlay stealing mouse input from the game browser underneath.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::BeginChild("##action", rect.GetSize(), ImGuiChildFlags_NavFlattened,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
		if (element.buttonRounding) ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, *element.buttonRounding * std::min(rect.GetWidth(), rect.GetHeight()));
		const LibraryItem* item = selected >= 0 ? &items[selected] : nullptr;
		const bool selectedButton = (element.action == "artwork" && element.source == previewSource)
				|| (element.action == "favourite" && item && item->favourite);
		const bool missingArtwork = element.action == "artwork" && item && artworkPath(*item, element.source, element.source == "cover" ? "default" : "none").empty();
		const bool disabled = count == 0 || missingArtwork
				|| (element.action == "favourite" && (!item || !item->canFavourite || !toggleFavourite));
		const uint32_t textColor = selectedButton && element.selectedColor ? *element.selectedColor : element.color;
		const auto buttonBackground = selectedButton && element.selectedBackground ? element.selectedBackground : element.background;
		const auto borderColor = selectedButton && element.selectedBorderColor ? element.selectedBorderColor : element.borderColor;
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color(textColor)));
		if (borderColor)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(color(*borderColor)));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, uiScaled(1.f));
		}
		if (buttonBackground)
		{
			const ImVec4 background = ImGui::ColorConvertU32ToFloat4(color(*buttonBackground));
			ImVec4 hovered = background;
			hovered.w = std::max(hovered.w, 0.15f);
			// Opt-in outlined controls get visible hover feedback even with opaque fills.
			if (borderColor)
			{
				const ImVec4 foreground = ImGui::ColorConvertU32ToFloat4(color(textColor));
				hovered.x += (foreground.x - hovered.x) * 0.1f;
				hovered.y += (foreground.y - hovered.y) * 0.1f;
				hovered.z += (foreground.z - hovered.z) * 0.1f;
			}
			ImGui::PushStyleColor(ImGuiCol_Button, background);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, hovered);
		}
		ImGui::PushFont(nullptr, std::clamp(element.fontSize * canvas.size.y, 10.f, 96.f));
		const std::string& label = selectedButton && !element.selectedText.empty() ? element.selectedText : element.text;
		const std::string& icon = selectedButton && !element.selectedIcon.empty() ? element.selectedIcon : element.icon;
		const float contentWidth = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetFontSize() * (label.empty() ? 1.f : 1.45f);
		const bool fitIconLabel = !icon.empty() && contentWidth > rect.GetWidth() - uiScaled(16.f);
		if (fitIconLabel) ImGui::PushFont(nullptr, ImGui::GetFontSize() * std::max(0.1f, rect.GetWidth() - uiScaled(16.f)) / contentWidth);
		ImGui::BeginDisabled(disabled);
		// Keep the widget ID stable when its visible label changes after a toggle.
		const bool pressed = ImGui::Button(((icon.empty() ? label : std::string()) + "###control").c_str(), rect.GetSize());
		if (!icon.empty()) drawButtonContents(rect, label, icon);
		ImGui::EndDisabled();
		if (fitIconLabel) ImGui::PopFont();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		{
			if (missingArtwork)
				ImGui::SetTooltip("%s", i18n::T("This artwork is not available for this game."));
			else if (element.action == "favourite" && item && item->canFavourite)
				ImGui::SetTooltip("%s", i18n::T(item->favourite ? "Remove from favourites" : "Add to favourites"));
		}
		ImGui::PopFont();
		ImGui::PopStyleColor((buttonBackground ? 4 : 1) + (borderColor ? 1 : 0));
		ImGui::PopStyleVar((element.buttonRounding ? 2 : 1) + (borderColor ? 1 : 0));
		ImGui::EndChild();
		ImGui::PopStyleVar();
		if (!pressed || count == 0) continue;
		if (element.action == "activate") activated = selected;
		else if (element.action == "artwork") previewSource = element.source;
		else if (element.action == "favourite") toggleFavourite(static_cast<size_t>(selected));
		else
		{
			const int step = element.action == "previous" ? -1 : element.action == "up" ? -columns : element.action == "down" ? columns : 1;
			const int length = static_cast<int>(count);
			selected = ((selected + step) % length + length) % length;
			scrollSelection = true;
		}
	}
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
	static const std::array<const char*, 13> sources{
		"", "miximage", "cover", "case", "screenshot", "title", "physical", "fanart", "titlescreen", "", "banner", "backcover", "spine"
	};
	if (mediaMode < 0 || mediaMode >= static_cast<int>(sources.size()) || mediaMode == 9)
		return std::nullopt;
	return sources[mediaMode];
}

int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem,
		const std::optional<std::string>& iconSource, float iconScale,
		const std::string& stateLayoutId, const std::function<void(size_t)>& toggleFavourite)
{
	iconScale = std::clamp(iconScale, 0.5f, 2.f);
	const std::string& browserSource = iconSource ? *iconSource : layout.artworkSource;
	static std::string selectedId;
	static std::string selectedLayoutId;
	static std::string previewSource;
	static bool scrollSelection = false;
	const std::string layoutKey = themeDirectory + '/' + (stateLayoutId.empty() ? layout.id : stateLayoutId);
	const bool layoutChanged = selectedLayoutId != layoutKey;
	if (layoutChanged)
	{
		selectedLayoutId = layoutKey;
		selectedId.clear();
		previewSource = layout.previewSource;
		scrollSelection = false;
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
	const std::string selectionBeforeBrowsing = selectedId;
	const LibraryItem* initial = selected >= 0 ? &items[selected] : nullptr;
	if (initial && artworkPath(*initial, previewSource, previewSource == "cover" ? "default" : "none").empty() && !artworkPath(*initial, "cover").empty())
		previewSource = "cover";

	const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
	const ImVec2 contentSize = ImGui::GetContentRegionAvail();
	ImVec2 canvasOrigin = contentOrigin, canvasSize = contentSize;
	if (layout.canvasAspect > 0 && canvasSize.x > 0 && canvasSize.y > 0)
	{
		canvasSize.x = std::min(canvasSize.x, canvasSize.y * layout.canvasAspect);
		canvasSize.y = canvasSize.x / layout.canvasAspect;
		canvasOrigin = canvasOrigin + (contentSize - canvasSize) * 0.5f;
	}
	const Canvas canvas{canvasOrigin, canvasSize, ImVec2(canvasOrigin.x + canvasSize.x, canvasOrigin.y + canvasSize.y), iconScale};
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(contentOrigin, contentOrigin + contentSize, color(layout.background));

	const ImRect browser = normalizedRect(canvas, layout.browser);
	const ImVec2 browserPos(ImMax(browser.Min.x, canvas.origin.x), ImMax(browser.Min.y, canvas.origin.y));
	const ImVec2 browserMax(ImMin(browser.Max.x, canvas.max.x), ImMin(browser.Max.y, canvas.max.y));
	const ImVec2 browserSize(std::max(0.0f, browserMax.x - browserPos.x), std::max(0.0f, browserMax.y - browserPos.y));
	ImGui::SetCursorScreenPos(browserPos);
	const bool childVisible = ImGui::BeginChild("##themeLibraryBrowser", browserSize, ImGuiChildFlags_NavFlattened,
			ImGuiWindowFlags_NoBackground);
	if (layout.fontSize > 0)
		ImGui::PushFont(nullptr, std::clamp(layout.fontSize * canvas.size.y * iconScale, 10.f, 96.f));
	const ImVec2 previousItemSpacing = ImGui::GetStyle().ItemSpacing;
	// Selectable expands its hit area by half ItemSpacing. Narrow spines need exact
	// bounds; their gap is already supplied by the rack's explicit column spacing.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(layout.mode == "rack" ? 0.f : previousItemSpacing.x, 0.f));
	ImDrawList* browserDrawList = ImGui::GetWindowDrawList();
	int activated = -1;
	int browserColumns = 1;
	if (childVisible && !items.empty())
	{
		const float spacing = std::max(0.0f, uiScaled(layout.spacing) * iconScale);
		if (layout.mode == "list")
		{
			// ImGui snaps row positions to pixels; snap the height too to avoid overlapping hit boxes.
			const float rowHeight = std::ceil(layout.rowHeight > 0
					? std::max(ImGui::GetFontSize() + uiScaled(8.f), layout.rowHeight * canvas.size.y * iconScale)
					: std::max(uiScaled(34.0f) * iconScale, ImGui::GetFontSize() + spacing));
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
					if (activatesItem(layout, pressed))
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
					if (layout.showFavouriteMarker && item.favourite) drawFavouriteMarker(layout, itemRect, browserDrawList);
					// Draw per-game controls after the artwork/title, retaining the selectable's item data.
					if (afterItem)
						afterItem(static_cast<size_t>(i));
					if (scrollSelection && i == selected) { ImGui::SetScrollHereY(); scrollSelection = false; }
				}
		}
		else
		{
			const bool rack = layout.mode == "rack";
			const float availWidth = ImGui::GetContentRegionAvail().x;
			const float availHeight = ImGui::GetContentRegionAvail().y;
			const int desiredColumns = std::max(1, static_cast<int>(std::round(layout.columns / iconScale)));
			const int columns = std::clamp(desiredColumns, 1, std::max(1, static_cast<int>((availWidth + spacing)
					/ (uiScaled(rack ? 12.f : layout.minItemWidth) * iconScale + (rack ? spacing : 0.f)))));
			browserColumns = columns;
			const float cellWidth = std::max(1.0f, (availWidth - spacing * (columns - 1)) / columns);
			const float aspect = std::clamp(layout.itemAspect, rack ? 0.03f : 0.3f, 3.0f);
			const float rowSpacing = rack ? std::max(spacing, uiScaled(26.f)) : spacing;
			// Always fit a complete jewel case and shelf in the browser viewport.
			const float cellHeight = rack ? std::min(std::max(1.f, availHeight - rowSpacing),
					std::clamp(cellWidth / aspect, uiScaled(140.f) * iconScale, uiScaled(280.f) * iconScale))
					: std::max(uiScaled(std::min(70.f, layout.minItemWidth)), cellWidth / aspect);
			std::vector<float> spineWidths;
			std::vector<int> shelfStarts;
			if (rack && browserSource == "spine")
			{
				spineWidths.reserve(items.size());
				shelfStarts.push_back(0);
				float shelfWidth = 0.f;
				for (size_t i = 0; i < items.size(); ++i)
				{
					const float imageAspect = ImguiFileTexture(artworkPath(items[i], browserSource, "none")).getAspectRatio();
					const float spineAspect = imageAspect > 1.f ? 1.f / imageAspect : imageAspect;
					// Read headers only, keeping off-screen scans out of the GPU cache.
					// Wider regional cases occupy more shelf space, with a shared height.
					const float width = spineAspect > 0.f
							? std::min(availWidth, std::max(3.f, cellHeight * spineAspect + 2.f)) : cellWidth;
					if (i > static_cast<size_t>(shelfStarts.back()) && shelfWidth + width > availWidth + 0.5f)
					{
						shelfStarts.push_back(static_cast<int>(i));
						shelfWidth = 0.f;
					}
					spineWidths.push_back(width);
					shelfWidth += width + spacing;
				}
			}
			const int rows = shelfStarts.empty() ? (static_cast<int>(items.size()) + columns - 1) / columns
					: static_cast<int>(shelfStarts.size());
			ImGuiListClipper clipper;
			clipper.Begin(rows, cellHeight + rowSpacing);
			if (selected >= 0)
			{
				const int selectedRow = shelfStarts.empty() ? selected / columns
						: static_cast<int>(std::upper_bound(shelfStarts.begin(), shelfStarts.end(), selected) - shelfStarts.begin()) - 1;
				clipper.IncludeItemsByIndex(selectedRow, selectedRow + 1);
			}
			while (clipper.Step())
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
				{
					const float rowY = ImGui::GetCursorPosY();
					const float rowX = ImGui::GetCursorPosX();
					if (rack)
					{
						const ImVec2 start = ImGui::GetCursorScreenPos();
						const float shelfY = start.y + cellHeight + uiScaled(2.f);
						const ImU32 edge = layout.separatorColor ? color(*layout.separatorColor) : IM_COL32(136, 110, 79, 255);
						browserDrawList->AddRectFilledMultiColor(ImVec2(start.x, shelfY),
								ImVec2(start.x + availWidth, shelfY + uiScaled(14.f)), edge, edge,
								IM_COL32(25, 22, 20, 255), IM_COL32(25, 22, 20, 255));
						browserDrawList->AddRectFilled(ImVec2(start.x, shelfY + uiScaled(14.f)),
								ImVec2(start.x + availWidth, shelfY + uiScaled(20.f)), IM_COL32(0, 0, 0, 100));
					}
					const int first = shelfStarts.empty() ? row * columns : shelfStarts[row];
					const int last = shelfStarts.empty() ? std::min(first + columns, static_cast<int>(items.size()))
							: (row + 1 < rows ? shelfStarts[row + 1] : static_cast<int>(items.size()));
					float x = rowX;
					for (int i = first; i < last; ++i)
					{
						const LibraryItem& item = items[i];
						const float itemWidth = spineWidths.empty() ? cellWidth : spineWidths[i];
						ImGui::SetCursorPos(ImVec2(x, rowY));
						const std::string key = itemKey(item, static_cast<size_t>(i));
						ImguiID id(key);
						const bool isSelected = selectedId == key;
						const bool pressed = browserSelectable(layout, isSelected, ImVec2(itemWidth, cellHeight));
						const bool hovered = ImGui::IsItemHovered();
						const ImRect itemRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
						if (hovered || pressed)
						{
							selected = i;
							selectedId = key;
						}
						if (activatesItem(layout, pressed))
							activated = i;

						const float titleHeight = layout.showTitles
								? std::max(cellHeight * 0.22f, ImGui::GetFontSize() + uiScaled(6.f)) : 0.f;
						const ImRect imageRect(itemRect.Min, ImVec2(itemRect.Max.x,
								itemRect.Min.y + std::max(1.f, cellHeight - titleHeight)));
						std::string image = artworkPath(item, browserSource, rack ? "none" : "default");
						if (image.empty() && !rack) image = layout.placeholderAsset;
						const uint32_t tint = 0xffffff00 | static_cast<uint32_t>(255.f * ((selectedId == key || hovered) ? 1.f : layout.inactiveOpacity));
						if (rack)
							drawSpine(browserDrawList, itemRect, item, browserSource, selectedId == key || hovered, tint);
						else
							drawFittedImage(browserDrawList, imageRect, image, "contain", tint, layout.rounding, layout.pixelArt);
						if (selectedId == key || hovered)
							browserDrawList->AddRect(itemRect.Min, itemRect.Max,
									layout.selectorBorderColor ? color(*layout.selectorBorderColor)
											: layout.selectorColor ? color(*layout.selectorColor) : ImGui::GetColorU32(ImGuiCol_NavHighlight),
									uiScaled(layout.rounding), 0, uiScaled(2.f));
						if (!rack && !layout.showTitles && image.empty())
							drawSingleLine(browserDrawList,
								ImRect(ImVec2(itemRect.Min.x + uiScaled(4.f), itemRect.Min.y + cellHeight * 0.5f),
										ImVec2(itemRect.Max.x - uiScaled(4.f), itemRect.Max.y)),
								item.title, browserTextColor(layout, selectedId == key || hovered));
						if (!rack && layout.showTitles && !item.title.empty())
						{
							const ImRect titleRect(ImVec2(itemRect.Min.x, imageRect.Max.y), itemRect.Max);
							browserDrawList->PushClipRect(titleRect.Min, titleRect.Max, true);
							const std::string label = layout.titleStyle == "initials" ? titleInitials(item.title) : item.title;
							browserDrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
									ImVec2(titleRect.Min.x + uiScaled(4.0f), titleRect.Min.y + uiScaled(3.0f)),
									browserTextColor(layout, selectedId == key || hovered), label.c_str(), nullptr,
									std::max(0.0f, titleRect.GetWidth() - uiScaled(8.0f)));
							browserDrawList->PopClipRect();
						}
						if (layout.showFavouriteMarker && item.favourite) drawFavouriteMarker(layout, itemRect, browserDrawList);
						if (afterItem)
							afterItem(static_cast<size_t>(i));
						x += itemWidth + spacing;
						if (scrollSelection && i == selected) { ImGui::SetScrollHereY(); scrollSelection = false; }
					}
					ImGui::SetCursorPos(ImVec2(rowX, rowY + cellHeight + rowSpacing));
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
	// Theme details sit opposite the browser. Keep full-canvas backdrops fixed,
	// while the cover and metadata share a vertically scrollable scaled column.
	const bool detailsOnRight = layout.browser.x + layout.browser.w * 0.5f <= 0.5f;
	const float split = detailsOnRight ? layout.browser.x + layout.browser.w : layout.browser.x;
	const bool authoredDetails = std::any_of(layout.elements.begin(), layout.elements.end(), [](const Element& element) { return element.type == "details"; });
	const Rect details{detailsOnRight ? split : 0.f, 0.f, authoredDetails ? 0.f : detailsOnRight ? 1.f - split : split, 1.f};
	// Resolve background media after hover/navigation so both layers show the same game.
	// The parent draw list is rendered beneath the browser child, regardless of submission order.
	drawElements(layout, canvas, themeDirectory, current, drawList, true, details, false, previewSource);
	ImGui::SetCursorScreenPos(canvas.origin);
	ImGui::BeginChild("##themeLibraryOverlay", canvas.size, ImGuiChildFlags_None,
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus
					| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawElements(layout, canvas, themeDirectory, current, ImGui::GetWindowDrawList(), false, details, false, previewSource);
	ImGui::EndChild();
	if (details.w > 0.f && canvasSize.y > 0.f)
	{
		const ImRect pane = normalizedRect(canvas, details);
		ImGui::SetCursorScreenPos(pane.Min);
		if (layoutChanged || selectedId != selectionBeforeBrowsing)
			ImGui::SetNextWindowScroll(ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		const bool visible = ImGui::BeginChild("##themeLibraryDetails", pane.GetSize(), ImGuiChildFlags_None,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_DragScrolling);
		if (visible)
		{
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
			const ImVec2 size(width / details.w, canvasSize.y * iconScale);
			const ImVec2 offset(origin.x - details.x * size.x, origin.y);
			const Canvas detailCanvas{offset, size, offset + size};
			drawElements(layout, detailCanvas, themeDirectory, current, ImGui::GetWindowDrawList(), true, details, true, previewSource);
			drawElements(layout, detailCanvas, themeDirectory, current, ImGui::GetWindowDrawList(), false, details, true, previewSource);
			ImGui::Dummy(ImVec2(width, size.y));
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
	}
	drawDetails(layout, canvas, current);
	drawButtons(layout, canvas, items, browserColumns, selected, previewSource, scrollSelection, activated, toggleFavourite);
	if (selected >= 0) selectedId = itemKey(items[selected], static_cast<size_t>(selected));
	ImGui::SetCursorScreenPos(contentOrigin);
	ImGui::Dummy(contentSize);
	return activated;
}
} // namespace themes
