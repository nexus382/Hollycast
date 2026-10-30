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
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_map>

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
		return metadataText(element, item);
	return {};
}

// The library's overall state, for elements that respond to it.
struct LibraryState
{
	bool attracting = false;
	float launchElapsed = -1; // Seconds since the launch sequence began; negative when not launching.
	float launchProgress = 0; // 0 to 1 across the sequence.
	std::optional<ImRect> selectionRect;
	float selectionElapsed = 0;
	const std::vector<LibraryItem>* items = nullptr;
	int selected = -1, pageSize = 0;
};

// Spin and sweep share an image's rectangle as their clip region.
void drawMovingImage(const Element& element, const std::string& path, const Canvas& canvas, const ImRect& visible, ImDrawList* drawList,
		const LibraryState& state, uint32_t tint)
{
	ImguiFileTexture texture(path);
	const ImTextureID id = texture.getId();
	if (id == ImTextureID{})
		return;
	const double time = ImGui::GetTime();
	drawList->PushClipRect(visible.Min, visible.Max, true);
	if (element.tileSeconds > 0)
	{
		const float height = visible.GetWidth() / std::max(0.001f, imguiDriver->getAspectRatio(id));
		const float offset = static_cast<float>(std::fmod(time / element.tileSeconds, 1.0)) * height;
		for (float top = visible.Min.y + offset - height; top < visible.Max.y; top += height)
			drawList->AddImage(id, ImVec2(visible.Min.x, top), ImVec2(visible.Max.x, top + height), ImVec2(0, 0), ImVec2(1, 1), color(tint));
	}
	else if (element.spinSeconds > 0)
	{
		ImVec2 center(canvas.origin.x + element.spinCenterX * canvas.size.x, canvas.origin.y + element.spinCenterY * canvas.size.y);
		if (!element.anchor.empty() && state.selectionRect)
		{
			center.x = state.selectionRect->GetCenter().x;
			if (element.anchor == "selection") center.y = state.selectionRect->GetCenter().y;
		}
		const float radius = element.spinRadius * canvas.size.x;
		// Integrated rather than derived from the clock, so a change of pace does not jump the angle.
		static std::unordered_map<const Element*, double> turns;
		double speed = 1.0 / element.spinSeconds;
		if (state.launchElapsed >= 0 && element.spinLaunchSeconds > 0)
			speed += (1.0 / element.spinLaunchSeconds - speed) * state.launchProgress * state.launchProgress;
		double& turn = turns[&element];
		turn = std::fmod(turn + ImGui::GetIO().DeltaTime * speed, 1.0);
		const float angle = static_cast<float>(turn * 2.0 * IM_PI);
		if (element.spinHole >= 0)
		{
			constexpr int segments = 96;
			const std::vector<DiscVertex> ring = discRing(center.x, center.y, radius, element.spinHole, angle, segments);
			drawList->PushTexture(id);
			drawList->PrimReserve(segments * 6, static_cast<int>(ring.size()));
			const ImDrawIdx base = static_cast<ImDrawIdx>(drawList->_VtxCurrentIdx);
			for (const DiscVertex& vertex : ring)
				drawList->PrimWriteVtx(ImVec2(vertex.x, vertex.y), ImVec2(vertex.u, vertex.v), color(tint));
			for (int i = 0; i < segments; ++i)
			{
				const ImDrawIdx outer = static_cast<ImDrawIdx>(base + i * 2), inner = static_cast<ImDrawIdx>(outer + 1);
				const ImDrawIdx nextOuter = static_cast<ImDrawIdx>(outer + 2), nextInner = static_cast<ImDrawIdx>(outer + 3);
				drawList->PrimWriteIdx(outer);
				drawList->PrimWriteIdx(nextOuter);
				drawList->PrimWriteIdx(nextInner);
				drawList->PrimWriteIdx(outer);
				drawList->PrimWriteIdx(nextInner);
				drawList->PrimWriteIdx(inner);
			}
			drawList->PopTexture();
			// Textured triangles have hard edges: a soft line hides the steps around the rim.
			drawList->AddCircle(center, radius, IM_COL32(0, 0, 0, 70), segments, 1.5f);
			if (element.spinHole > 0)
				drawList->AddCircle(center, radius * element.spinHole, IM_COL32(0, 0, 0, 70), segments, 1.5f);
		}
		else
		{
			const float c = std::cos(angle) * radius, s = std::sin(angle) * radius;
			drawList->AddImageQuad(id, ImVec2(center.x - c + s, center.y - s - c), ImVec2(center.x + c + s, center.y + s - c),
					ImVec2(center.x + c - s, center.y + s + c), ImVec2(center.x - c - s, center.y - s + c),
					ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), color(tint));
		}
	}
	else
	{
		const float height = visible.GetWidth() / std::max(0.001f, imguiDriver->getAspectRatio(id));
		// A launch's sweep starts with the launch, entering from the top edge.
		const double clock = element.when == "launching" ? state.launchElapsed : time;
		const float phase = static_cast<float>(std::fmod(clock / element.sweepSeconds, 1.0));
		const float top = visible.Min.y - height + phase * (visible.GetHeight() + height);
		drawList->AddImage(id, ImVec2(visible.Min.x, top), ImVec2(visible.Max.x, top + height), ImVec2(0, 0), ImVec2(1, 1), color(tint));
	}
	drawList->PopClipRect();
}

void drawElement(const Element& element, const Canvas& canvas, const std::string& themeDirectory,
		const LibraryItem* item, ImDrawList* drawList, const std::string& previewSource, const LibraryState& state)
{
	if ((element.when == "attract" && !state.attracting) || (element.when == "interactive" && state.attracting)
			|| (element.when == "launching" && state.launchElapsed < 0)
			|| (element.when == "unplayed" && (!item || !item->isGame || hasPlayed(*item)))
			|| (element.when == "played" && (!item || !item->isGame || !hasPlayed(*item)))
			|| (element.onSelection > 0 && state.selectionElapsed >= element.onSelection))
		return;
	uint32_t tint = element.color;
	if (element.onSelection > 0)
	{
		const float fade = std::min(1.f, (1.f - state.selectionElapsed / element.onSelection) * 4.f);
		tint = (tint & 0xffffff00) | static_cast<uint32_t>((tint & 0xff) * fade);
	}
	if (element.launchFadeFrom >= 0)
	{
		const float fade = std::clamp((state.launchProgress - element.launchFadeFrom) / (1.f - element.launchFadeFrom), 0.f, 1.f);
		tint = (tint & 0xffffff00) | static_cast<uint32_t>((tint & 0xff) * fade);
	}
	ImRect rect = normalizedRect(canvas, element.rect);
	if (!element.anchor.empty())
	{
		if (!state.selectionRect) return;
		const ImVec2 center = state.selectionRect->GetCenter();
		const float width = rect.GetWidth(), height = rect.GetHeight();
		rect.Min.x = center.x - width * 0.5f;
		rect.Max.x = rect.Min.x + width;
		if (element.anchor == "selection")
		{
			rect.Min.y = center.y - height * 0.5f;
			rect.Max.y = rect.Min.y + height;
		}
		else if (element.anchor == "selectionTrail")
		{
			rect.Max.y = rect.Min.y;
			rect.Min.y = state.selectionRect->Max.y;
		}
	}
	const ImRect visible(ImMax(rect.Min, canvas.origin), ImMin(rect.Max, canvas.max));
	if (visible.GetWidth() <= 0 || visible.GetHeight() <= 0)
		return;
	if (element.type == "meter")
	{
		if (!state.items) return;
		const auto value = libraryStat(element.source, *state.items, state.selected, state.pageSize, std::time(nullptr));
		if (!value) return;
		const float fraction = std::clamp(static_cast<float>(*value * element.valueScale / element.maximum), 0.f, 1.f);
		drawList->AddRectFilled(visible.Min, ImVec2(visible.Min.x + visible.GetWidth() * fraction, visible.Max.y), color(tint), uiScaled(element.rounding));
	}
	else if (element.type == "panel")
	{
		const int start = drawList->VtxBuffer.Size;
		drawList->AddRectFilled(visible.Min, visible.Max,
				element.endColor ? IM_COL32_WHITE : color(tint), uiScaled(element.rounding));
		if (element.endColor)
		{
			const ImVec4 from = ImGui::ColorConvertU32ToFloat4(color(tint));
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
		if (!element.resolvedAsset.empty() && (element.spinSeconds > 0 || element.sweepSeconds > 0 || element.tileSeconds > 0))
			drawMovingImage(element, element.resolvedAsset, canvas, visible, drawList, state, tint);
		else
			drawFittedImage(drawList, visible, element.resolvedAsset, element.fit, tint, element.rounding);
	}
	else if (item && element.type == "artwork")
	{
		if (!element.requiresArtwork.empty() && artworkPath(*item, element.requiresArtwork, "none").empty())
			return;
		const std::string& source = element.source == "preview" ? previewSource : element.source;
		const std::string fallback = element.source == "preview" && source != "cover" ? "none" : element.fallback;
		std::string path = artworkPath(*item, source, fallback);
		if (path.empty()) path = element.resolvedFallbackAsset;
		if (element.spinSeconds > 0)
		{
			if (!path.empty())
				drawMovingImage(element, path, canvas, visible, drawList, state, tint);
		}
		else
			drawFittedImage(drawList, visible, path, element.fit, tint, element.rounding);
	}
	else if (element.type == "text" || element.type == "stat" || (item && element.type == "metadata"))
	{
		std::string text = item ? elementText(element, *item) : element.type == "text" ? element.text : std::string();
		if (element.type == "stat" && state.items)
		{
			const auto value = libraryStat(element.source, *state.items, state.selected, state.pageSize, std::time(nullptr));
			if (value)
			{
				std::ostringstream number;
				number << std::setfill('0') << std::setw(element.digits) << static_cast<uint64_t>(std::clamp(*value * element.valueScale, 0.0, 1e15));
				text = number.str();
			}
			else text = element.text;
		}
		if (text.empty())
			return;
		// Small label rectangles must still show a whole line on shorter windows.
		const float fontSize = std::min(visible.GetHeight(),
				std::clamp(element.fontSize * canvas.size.y * canvas.textScale, 1.f, 96.f));
		ImGui::PushFont(element.bold ? boldFont : nullptr, fontSize);
		ImVec2 textPos = visible.Min;
		float wrapWidth = visible.GetWidth();
		if (element.marqueeSeconds > 0)
		{
			// New text starts at the right edge, and any length travels at the same readable speed.
			struct Marquee { std::string text; double start = 0; };
			static std::unordered_map<const Element*, Marquee> marquees;
			Marquee& marquee = marquees[&element];
			if (marquee.text != text)
				marquee = {text, ImGui::GetTime()};
			const float textWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.f, text.c_str()).x;
			const double speed = visible.GetWidth() / element.marqueeSeconds;
			const double travelled = std::fmod((ImGui::GetTime() - marquee.start) * speed, visible.GetWidth() + textWidth);
			textPos.x = visible.Max.x - static_cast<float>(travelled);
			wrapWidth = 0;
		}
		else if (element.align != "left")
		{
			const float textWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX,
					element.wrap ? visible.GetWidth() : 0.f, text.c_str()).x;
			textPos.x += std::max(0.f, visible.GetWidth() - textWidth) * (element.align == "center" ? 0.5f : 1.f);
		}
		if (element.marqueeSeconds > 0 || element.slant > 0)
		{
			drawList->PushClipRect(visible.Min, visible.Max, true);
			const int firstVertex = drawList->VtxBuffer.Size;
			drawList->AddText(ImGui::GetFont(), fontSize, textPos, color(tint), text.c_str(), nullptr, wrapWidth);
			if (element.slant > 0)
				for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
					drawList->VtxBuffer[i].pos.x += (textPos.y + fontSize - drawList->VtxBuffer[i].pos.y) * element.slant;
			drawList->PopClipRect();
		}
		else if (element.wrap)
		{
			// Clip at complete lines so a short description panel never shows half a glyph.
			const float bottom = visible.Min.y + std::floor(visible.GetHeight() / fontSize) * fontSize;
			drawList->PushClipRect(visible.Min, ImVec2(visible.Max.x, bottom), true);
			drawList->AddText(ImGui::GetFont(), fontSize, textPos, color(tint), text.c_str(), nullptr, visible.GetWidth());
			drawList->PopClipRect();
		}
		else
			drawSingleLine(drawList, ImRect(textPos, visible.Max), text, color(tint));
		ImGui::PopFont();
	}
}

enum class ElementLayer { BehindBrowser, OverBrowser, OverEverything };

void drawElements(const Layout& layout, const Canvas& canvas, const Canvas& content, const std::string& themeDirectory,
		const LibraryItem* item, ImDrawList* drawList, ElementLayer layer, const Rect& details, bool detailsOnly,
		const std::string& previewSource, const LibraryState& state)
{
	std::vector<const Element*> sorted;
	sorted.reserve(layout.elements.size());
	for (const Element& element : layout.elements)
	{
		// A launch plays over the whole library, including its buttons, which draw in their own windows.
		const ElementLayer elementLayer = element.zIndex < 0 ? ElementLayer::BehindBrowser
				: element.when == "launching" ? ElementLayer::OverEverything : ElementLayer::OverBrowser;
		// A launch and bleeding elements span the library, so they can not scroll with the details column.
		const bool inDetails = elementLayer != ElementLayer::OverEverything && !element.bleed && element.anchor.empty()
				&& details.w > 0.f && element.rect.x >= details.x - 0.0001f
				&& element.rect.x + element.rect.w <= details.x + details.w + 0.0001f;
		if (elementLayer == layer && inDetails == detailsOnly)
			sorted.push_back(&element);
	}
	std::stable_sort(sorted.begin(), sorted.end(), [](const Element* a, const Element* b) {
		return a->zIndex < b->zIndex;
	});
	for (const Element* element : sorted)
		drawElement(*element, element->bleed ? content : canvas, themeDirectory, item, drawList, previewSource, state);
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
		{"playTime", "Play time"}, {"lastPlayed", "Last played"}, {"size", "File size"},
		{"system", "System"}
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
		const std::function<void(size_t)>& toggleFavourite, bool& focusFirst)
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
		const int pageSize = std::max(1, columns * layout.pagedGrid.rows);
		const bool disabled = count == 0 || missingArtwork
				|| (element.action == "previousPage" && selected < pageSize)
				|| (element.action == "nextPage" && selected / pageSize >= (static_cast<int>(count) - 1) / pageSize)
				|| (element.action == "favourite" && (!item || !item->canFavourite || !toggleFavourite));
		const uint32_t textColor = selectedButton && element.selectedColor ? *element.selectedColor : element.color;
		const auto buttonBackground = selectedButton && element.selectedBackground ? element.selectedBackground : element.background;
		const auto borderColor = selectedButton && element.selectedBorderColor ? element.selectedBorderColor : element.borderColor;
		const bool hasImage = !element.buttonImage.empty();
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color(textColor)));
		if (borderColor)
		{
			ImGui::PushStyleColor(ImGuiCol_Border, ImGui::ColorConvertU32ToFloat4(color(*borderColor)));
			ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, uiScaled(1.f));
		}
		if (hasImage)
		{
			// The artwork draws every state: a drawn hover fill would show past its shape.
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
		}
		else if (buttonBackground)
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
		if (focusFirst && !disabled)
		{
			ImGui::SetKeyboardFocusHere();
			ImGui::SetNavCursorVisible(true); // Show where the focus went: it arrived from a row with no cursor.
			focusFirst = false;
		}
		// Keep the widget ID stable when its visible label changes after a toggle.
		const bool pressed = ImGui::Button(((icon.empty() ? label : std::string()) + "###control").c_str(), rect.GetSize());
		if (!icon.empty()) drawButtonContents(rect, label, icon);
		ImGui::EndDisabled();
		const bool tooltipHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
		if (hasImage)
		{
			const bool lit = !disabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && GImGui->NavCursorVisible));
			const std::string& selectedImage = element.buttonSelectedImage.empty() ? element.buttonImage : element.buttonSelectedImage;
			const std::string& selectedLit = element.buttonSelectedHoverImage.empty() ? selectedImage : element.buttonSelectedHoverImage;
			const std::string& litImage = element.buttonHoverImage.empty() ? element.buttonImage : element.buttonHoverImage;
			const std::string& image = selectedButton ? (lit ? selectedLit : selectedImage) : (lit ? litImage : element.buttonImage);
			drawFittedImage(ImGui::GetWindowDrawList(), rect, image, "stretch", disabled ? 0xffffff66 : 0xffffffff);
		}
		if (fitIconLabel) ImGui::PopFont();
		ImGui::PopFont();
		ImGui::PopStyleColor((hasImage || buttonBackground ? 4 : 1) + (borderColor ? 1 : 0));
		ImGui::PopStyleVar((element.buttonRounding ? 2 : 1) + (borderColor ? 1 : 0));
		ImGui::EndChild();
		ImGui::PopStyleVar();
		// After all the button's styling, which would otherwise color and cramp the tooltip.
		if (tooltipHovered)
		{
			if (missingArtwork)
				ImGui::SetTooltip("%s", i18n::T("This artwork is not available for this game."));
			else if (element.action == "favourite" && item && item->canFavourite)
				ImGui::SetTooltip("%s", i18n::T(item->favourite ? "Remove from favourites" : "Add to favourites"));
		}
		if (!pressed || count == 0) continue;
		if (element.action == "activate") activated = selected;
		else if (element.action == "artwork") previewSource = element.source;
		else if (element.action == "favourite") toggleFavourite(static_cast<size_t>(selected));
		else if (element.action == "previousPage" || element.action == "nextPage")
		{
			selected = std::clamp(selected + (element.action == "previousPage" ? -pageSize : pageSize), 0, static_cast<int>(count) - 1);
			scrollSelection = true;
		}
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

ImU32 withAlpha(ImU32 value, float alpha)
{
	const unsigned int a = static_cast<unsigned int>(((value >> IM_COL32_A_SHIFT) & 0xff) * std::clamp(alpha, 0.f, 1.f));
	return (value & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

struct RailState
{
	float position = 0;
	// The last row clicked: the rail moves at once, so a second click can land on a different row.
	std::string clickedKey;
	double clickedAt = 0;
	ImVec2 clickedPos;
	float wheel = 0;
	float dragDistance = 0;
	bool dragging = false, dragged = false;
	bool rowFocused = false;
	bool moved = false; // The wheel or a drag moved the selection last frame.
	bool refocus = false; // Keyboard focus landed on another row; hand it to the selection.
	bool focusButtons = false; // Right was pressed on the rail: focus the layout's first button.
	std::string focusedKey;
};

// Rows move while the player browses, so they select when pressed, never on hover: a row
// sliding under a resting pointer must not change the selection. For the same reason Up and
// Down step along the rail directly. ImGui's geometric navigation scores moving rows against
// other controls, and can leave the rail with games still to reach.
void drawRail(const Layout& layout, const Canvas& canvas, const std::vector<LibraryItem>& items, ImDrawList* drawList,
		RailState& state, int& selected, std::string& selectedId, int& activated, bool selectionMoved,
		const std::function<void(size_t)>& afterItem)
{
	const Rail& rail = layout.rail;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const ImVec2 area = ImGui::GetContentRegionAvail();
	const float centerY = origin.y + area.y * rail.center;
	const float rowHeight = std::max(ImGui::GetFontSize() + uiScaled(8.f),
			layout.rowHeight > 0 ? layout.rowHeight * canvas.size.y * canvas.textScale : area.y / (rail.visible * 2 + 1));
	const float rowStep = rowHeight * 1.04f;
	const int count = static_cast<int>(items.size());

	const float blend = 1.f - std::exp(-ImGui::GetIO().DeltaTime * 14.f);
	state.position += (selected - state.position) * blend;
	state.position = std::clamp(state.position, selected - rail.visible - 1.f, selected + rail.visible + 1.f);

	std::vector<int> rows;
	for (int i = std::max(0, selected - rail.visible - 1); i <= std::min(count - 1, selected + rail.visible + 1); ++i)
		rows.push_back(i);
	// Nearer rows draw last, overlap farther ones, and receive input first.
	std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
		return std::fabs(a - state.position) > std::fabs(b - state.position);
	});

	ImGuiContext& g = *GImGui;
	// With nothing focused the keyboard has nowhere to go, and the first press would be lost.
	if (g.NavWindow == nullptr)
		ImGui::FocusWindow(ImGui::GetCurrentWindow());
	const bool locked = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0; // A launch is under way.
	if (locked && g.NavMoveSubmitted)
		ImGui::NavMoveRequestCancel();
	bool stepped = false;
	// Until the keyboard has been used, ImGui's focus sits unseen on some control. The player
	// is looking at the rail, so the arrows belong to it.
	if ((state.rowFocused || g.NavId == 0 || !g.NavCursorVisible) && g.NavMoveSubmitted && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)
			&& (g.NavMoveDir == ImGuiDir_Up || g.NavMoveDir == ImGuiDir_Down))
	{
		const int target = selected + (g.NavMoveDir == ImGuiDir_Down ? 1 : -1);
		// At either end the request carries on, so that the rail's neighbours stay reachable.
		if (target >= 0 && target < count)
		{
			ImGui::NavMoveRequestCancel();
			selected = target;
			selectedId = itemKey(items[target], static_cast<size_t>(target));
			stepped = true;
		}
	}
	// Buttons can sit far below the selected row, where Right would not find them by geometry.
	if (state.rowFocused && g.NavMoveSubmitted && g.NavMoveDir == ImGuiDir_Right
			&& std::any_of(layout.elements.begin(), layout.elements.end(), [](const Element& element) { return element.type == "button"; }))
	{
		ImGui::NavMoveRequestCancel();
		state.focusButtons = true;
	}
	// ImGui ignores confirm while its focus cursor is hidden, and stepping above leaves it hidden.
	if (!g.NavCursorVisible && !locked && selected >= 0 && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
		for (const ImGuiKey confirm : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Space, ImGuiKey_NavGamepadActivate})
			if (ImGui::IsKeyPressed(confirm, false))
				activated = selected;
	const bool moveFocus = stepped || state.refocus || ((selectionMoved || state.moved) && state.rowFocused);
	state.moved = false;
	state.refocus = false;
	bool rowFocused = false;
	const bool doubleClick = layout.activation == "doubleClick";
	ImGuiIO& io = ImGui::GetIO();
	// A double-click means the row that was clicked first, wherever the rail has since taken it.
	const ImVec2 sinceClick(io.MousePos.x - state.clickedPos.x, io.MousePos.y - state.clickedPos.y);
	const bool secondClick = doubleClick && !locked && !state.clickedKey.empty() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
			&& ImGui::GetTime() - state.clickedAt <= io.MouseDoubleClickTime
			&& ImLengthSqr(sinceClick) <= io.MouseDoubleClickMaxDist * io.MouseDoubleClickMaxDist;
	if (secondClick)
		for (int i = 0; i < count; ++i)
			if (itemKey(items[i], static_cast<size_t>(i)) == state.clickedKey)
				activated = i;
	const auto rememberClick = [&](const std::string& key) {
		state.clickedKey = key;
		state.clickedAt = ImGui::GetTime();
		state.clickedPos = io.MousePos;
	};
	for (const int i : rows)
	{
		const float distance = i - state.position;
		const float reach = std::fabs(distance);
		if (reach > rail.visible + 0.75f)
			continue;
		const LibraryItem& item = items[i];
		const std::string key = itemKey(item, static_cast<size_t>(i));
		const float scale = std::max(0.3f, 1.f - reach * rail.falloff);
		const float height = rowHeight * scale;
		const float y = centerY + distance * rowStep * (1.f - reach * rail.falloff * 0.5f);
		const float top = y - height * 0.5f, bottom = y + height * 0.5f;
		const float insetTop = rail.tilt * area.x * std::fabs(distance - 0.5f);
		const float insetBottom = rail.tilt * area.x * std::fabs(distance + 0.5f);
		const float slant = rail.skew * area.x;
		const ImVec2 points[4] = {
			ImVec2(origin.x + insetTop, top), ImVec2(origin.x + area.x - insetTop, top),
			ImVec2(origin.x + area.x - insetBottom - slant, bottom), ImVec2(origin.x + insetBottom, bottom)
		};
		// A row crossing the browser's edge fades away rather than being sliced by it.
		const float overflow = std::max(0.f, origin.y - top) + std::max(0.f, bottom - (origin.y + area.y));
		const float alpha = std::clamp(1.f - reach * rail.fade, 0.f, 1.f) * std::clamp(rail.visible + 0.75f - reach, 0.f, 1.f)
				* std::clamp(1.f - overflow / (height * 0.55f), 0.f, 1.f);

		ImguiID id(key);
		const float left = std::min(points[0].x, points[3].x);
		ImGui::SetCursorScreenPos(ImVec2(left, top));
		// Tabbing into the rail lands on the selection, not on whichever row comes first.
		ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, i != selected);
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
		// A focused row is the selected row, which already stands out; a square outline would not fit its shape.
		ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
		const ImGuiSelectableFlags flags = doubleClick ? ImGuiSelectableFlags_AllowDoubleClick : ImGuiSelectableFlags_None;
		const bool pressed = ImGui::Selectable("##libraryItem", selectedId == key, flags, ImVec2(std::max(1.f, points[1].x - left), height));
		ImGui::PopStyleColor(4);
		ImGui::PopItemFlag();
		// Rows submit far to near, so initial focus must be pointed at the selection.
		if (i == selected)
		{
			ImGui::SetItemDefaultFocus();
			if (moveFocus)
			{
				ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
				state.focusedKey = key;
				rowFocused = true;
			}
		}
		const bool hovered = ImGui::IsItemHovered();
		const bool focused = ImGui::IsItemFocused();
		if (focused)
		{
			rowFocused = true;
			if (state.focusedKey != key)
			{
				state.focusedKey = key;
				if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
				{
					// A click focuses its row on press, before Selectable reports it on release.
					if (!secondClick)
					{
						rememberClick(key);
						selected = i;
						selectedId = key;
					}
				}
				else if (i != selected)
					state.refocus = true;
			}
		}
		if (pressed && !state.dragged && !secondClick)
		{
			rememberClick(key);
			selected = i;
			selectedId = key;
			if (activatesItem(layout, pressed))
				activated = i;
		}
		const bool isSelected = selectedId == key;

		const ImU32 selector = layout.selectorColor ? color(*layout.selectorColor) : ImGui::GetColorU32(ImGuiCol_Header);
		const ImU32 row = rail.rowColor ? color(*rail.rowColor) : ImGui::GetColorU32(ImGuiCol_FrameBg);
		// Blend toward the selection color: row text must stay readable on the hovered fill.
		const ImU32 hoverRow = ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(row), ImGui::ColorConvertU32ToFloat4(selector), 0.35f));
		drawList->AddConvexPolyFilled(points, 4, withAlpha(isSelected ? selector : hovered ? hoverRow : row, alpha));

		const float fontSize = ImGui::GetFontSize() * scale;
		const float padding = height * 0.28f;
		const float textY = y - fontSize * 0.5f;
		const ImU32 text = withAlpha(browserTextColor(layout, isSelected), alpha);
		float textLeft = origin.x + std::max(insetTop, insetBottom) + padding;
		float textRight = origin.x + area.x - std::max(insetTop, insetBottom) - slant - padding * 0.5f;
		if (rail.numbers)
		{
			const std::string number = (i < 9 ? "0" : "") + std::to_string(i + 1);
			const float size = fontSize * 0.78f;
			drawList->AddText(ImGui::GetFont(), size, ImVec2(textLeft, y - size * 0.5f), withAlpha(text, 0.85f), number.c_str());
			textLeft += ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, "000").x + padding * 0.5f;
		}
		if (rail.favouriteMark && item.favourite)
		{
			const float radius = fontSize * 0.42f;
			const ImVec2 center(textLeft + radius, y);
			ImVec2 star[10];
			for (int point = 0; point < 10; ++point)
			{
				const float angle = -IM_PI / 2.f + point * IM_PI / 5.f;
				const float reachOut = point % 2 == 0 ? radius : radius * 0.42f;
				star[point] = ImVec2(center.x + std::cos(angle) * reachOut, center.y + std::sin(angle) * reachOut);
			}
			drawList->AddConcavePolyFilled(star, 10, withAlpha(isSelected ? text : selector, alpha));
			textLeft += radius * 2.f + padding * 0.5f;
		}
		const std::vector<std::string> badges = railBadges(rail, item);
		for (auto badge = badges.rbegin(); badge != badges.rend(); ++badge)
		{
			const float size = std::max(fontSize * 0.6f, 9.f);
			const ImVec2 badgeSize = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.f, badge->c_str());
			const float margin = size * 0.45f;
			// Metadata can be long; the game's title keeps most of the row.
			const float boxWidth = std::min(badgeSize.x + margin * 2.f, (textRight - textLeft) * 0.42f);
			const ImVec2 boxMax(textRight, y + badgeSize.y * 0.5f + margin * 0.6f);
			const ImVec2 boxMin(boxMax.x - boxWidth, y - badgeSize.y * 0.5f - margin * 0.6f);
			drawList->AddRect(boxMin, boxMax, text, 0.f, 0, 1.f);
			drawList->PushClipRect(ImVec2(boxMin.x + margin, boxMin.y), ImVec2(boxMax.x - margin, boxMax.y), true);
			drawList->AddText(ImGui::GetFont(), size, ImVec2(boxMin.x + margin, y - badgeSize.y * 0.5f), text, badge->c_str());
			drawList->PopClipRect();
			textRight = boxMin.x - padding * 0.5f;
		}
		if (!item.title.empty() && textRight > textLeft)
		{
			drawList->PushClipRect(ImVec2(textLeft, top), ImVec2(textRight, bottom), true);
			drawList->AddText(ImGui::GetFont(), fontSize, ImVec2(textLeft, textY), text, item.title.c_str());
			drawList->PopClipRect();
		}
		if (afterItem)
			afterItem(static_cast<size_t>(i));
	}
	state.rowFocused = rowFocused;
	if (!rowFocused)
		state.focusedKey.clear();
	if (secondClick) // The press focused whichever row was under the pointer.
		state.refocus = true;

	// Wheel and drag step through the games: the rail has no scrollbar to move.
	const bool hovering = !locked && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
	int steps = 0;
	if (hovering && io.MouseWheel != 0)
	{
		state.wheel -= io.MouseWheel;
		steps += static_cast<int>(state.wheel);
		state.wheel -= static_cast<int>(state.wheel);
	}
	if (hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		state.dragging = true;
		state.dragDistance = 0;
	}
	if (state.dragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left, uiScaled(6.f)))
	{
		state.dragged = true;
		state.dragDistance -= io.MouseDelta.y;
		steps += static_cast<int>(state.dragDistance / rowStep);
		state.dragDistance -= static_cast<int>(state.dragDistance / rowStep) * rowStep;
	}
	if (!io.MouseDown[ImGuiMouseButton_Left] && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		state.dragging = false;
		state.dragged = false;
	}
	if (steps != 0)
	{
		selected = std::clamp(selected + steps, 0, count - 1);
		selectedId = itemKey(items[selected], static_cast<size_t>(selected));
		state.moved = true;
	}
}

// A fresh press of anything, as opposed to a key or button still held from earlier.
bool playerPressedAnything()
{
	for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
		if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
			return true;
	return false;
}

int drawPagedGrid(const Layout& layout, const std::vector<LibraryItem>& items, const std::string& source,
		float iconScale, bool& focusedLastFrame, int& selected, std::string& selectedId, int& activated,
		const std::function<void(size_t)>& afterItem)
{
	const ImVec2 origin = ImGui::GetCursorScreenPos(), area = ImGui::GetContentRegionAvail();
	const float gap = std::min(uiScaled(layout.spacing) * iconScale, std::max(0.f, area.y) / (layout.pagedGrid.rows * 2.f));
	const int columns = std::clamp(static_cast<int>(std::round(layout.columns / iconScale)), 1,
			std::max(1, static_cast<int>((area.x + gap) / (uiScaled(layout.minItemWidth) * iconScale + gap))));
	const int pageSize = columns * layout.pagedGrid.rows;
	ImGuiContext& g = *GImGui;
	const bool locked = (g.CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
	bool moveFocus = false;
	if (!locked && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup) && !ImGui::GetIO().WantTextInput)
	{
		if ((focusedLastFrame || g.NavId == 0 || !g.NavCursorVisible) && g.NavMoveSubmitted && g.NavMoveDir != ImGuiDir_None)
		{
			const int step = g.NavMoveDir == ImGuiDir_Left ? -1 : g.NavMoveDir == ImGuiDir_Right ? 1 : g.NavMoveDir == ImGuiDir_Up ? -columns : columns;
			const int target = selected + step;
			if (target >= 0 && target < static_cast<int>(items.size()))
			{
				ImGui::NavMoveRequestCancel();
				selected = target;
				moveFocus = true;
			}
		}
		int pages = 0;
		if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0) pages = ImGui::GetIO().MouseWheel < 0 ? 1 : -1;
		if (focusedLastFrame)
		{
			if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) pages = 1;
			if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) pages = -1;
		}
		if (pages)
		{
			selected = std::clamp(selected + pages * pageSize, 0, static_cast<int>(items.size()) - 1);
			moveFocus = focusedLastFrame;
		}
		if (!g.NavCursorVisible)
			for (const ImGuiKey confirm : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_NavGamepadActivate})
				if (ImGui::IsKeyPressed(confirm, false)) activated = selected;
	}
	selectedId = itemKey(items[selected], static_cast<size_t>(selected));
	const int first = selected / pageSize * pageSize;
	const int last = std::min(first + pageSize, static_cast<int>(items.size()));
	const ImVec2 cell(std::max(1.f, (area.x - gap * (columns - 1)) / columns),
			std::max(1.f, (area.y - gap * (layout.pagedGrid.rows - 1)) / layout.pagedGrid.rows));
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	bool focused = false;
	for (int i = first; i < last; ++i)
	{
		const LibraryItem& item = items[i];
		const std::string key = itemKey(item, static_cast<size_t>(i));
		ImguiID id(key);
		const int slot = i - first;
		ImGui::SetCursorScreenPos(origin + ImVec2((slot % columns) * (cell.x + gap), (slot / columns) * (cell.y + gap)));
		ImGui::PushItemFlag(ImGuiItemFlags_NoTabStop, i != selected);
		const bool pressed = browserSelectable(layout, i == selected, cell);
		ImGui::PopItemFlag();
		if (i == selected)
		{
			ImGui::SetItemDefaultFocus();
			if (moveFocus) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
		}
		const bool itemFocused = ImGui::IsItemFocused();
		focused |= itemFocused;
		if (pressed || (!moveFocus && itemFocused && g.NavCursorVisible))
		{
			selected = i;
			selectedId = key;
		}
		if (activatesItem(layout, pressed)) activated = i;
		const ImRect rect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		const float titleHeight = layout.showTitles ? ImGui::GetFontSize() + uiScaled(4.f) : 0;
		const float padding = std::min(uiScaled(layout.pagedGrid.padding), std::min(cell.x, cell.y) * 0.2f);
		const float side = std::max(1.f, std::min(cell.x, cell.y - titleHeight));
		const ImVec2 artPos(rect.Min.x + (cell.x - side) * 0.5f, rect.Min.y);
		const ImRect artRect(artPos + ImVec2(padding, padding), artPos + ImVec2(side - padding, side - padding));
		std::string image = artworkPath(item, source);
		if (image.empty()) image = layout.placeholderAsset;
		drawFittedImage(drawList, artRect, image, "cover", 0xffffffff, layout.rounding);
		const std::string& frame = item.isGame && !hasPlayed(item) && !layout.pagedGrid.unplayedFrame.empty() ? layout.pagedGrid.unplayedFrame : layout.pagedGrid.frame;
		if (!frame.empty()) drawFittedImage(drawList, ImRect(artPos, artPos + ImVec2(side, side)), frame, "stretch");
		if (layout.showTitles)
			drawSingleLine(drawList, ImRect(ImVec2(rect.Min.x + padding, rect.Min.y + side), rect.Max), item.title, browserTextColor(layout, selected == i));
		if (layout.showFavouriteMarker && item.favourite)
			drawFavouriteMarker(layout, ImRect(artPos, artPos + ImVec2(side, side)), drawList);
		if (afterItem) afterItem(static_cast<size_t>(i));
	}
	focusedLastFrame = focused;
	return columns;
}

// Any player input keeps attract mode away, including an open menu or text field.
bool playerInputThisFrame()
{
	const ImGuiIO& io = ImGui::GetIO();
	if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
		return true;
	if (io.MouseWheel != 0 || io.MouseWheelH != 0 || ImGui::IsAnyMouseDown())
		return true;
	if (ImGui::IsMousePosValid() && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0))
		return true;
	for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
		if (ImGui::IsKeyDown(static_cast<ImGuiKey>(key)))
			return true;
	return false;
}
}

std::vector<DiscVertex> discRing(float centerX, float centerY, float radius, float hole, float angle, int segments)
{
	std::vector<DiscVertex> ring;
	ring.reserve((segments + 1) * 2);
	for (int i = 0; i <= segments; ++i)
	{
		const float spoke = 2.f * IM_PI * i / segments;
		const float x = std::cos(spoke), y = std::sin(spoke);
		// What shows at this spoke is the part of the image that has turned round to it.
		const float u = std::cos(spoke - angle), v = std::sin(spoke - angle);
		for (const float reach : {1.f, hole})
			ring.push_back({centerX + x * radius * reach, centerY + y * radius * reach, 0.5f + u * reach * 0.5f, 0.5f + v * reach * 0.5f});
	}
	return ring;
}

std::vector<std::string> railBadges(const Rail& rail, const LibraryItem& item)
{
	std::vector<std::string> result;
	for (const auto& fields : rail.badges)
		for (const std::string& field : fields)
		{
			const auto value = item.metadata.find(field);
			if (value != item.metadata.end() && !value->second.empty())
			{
				result.push_back(value->second);
				break;
			}
		}
	return result;
}

std::string metadataText(const Element& element, const LibraryItem& item)
{
	std::string joined;
	const std::vector<std::string> single{element.source};
	for (const std::string& field : element.fields.empty() ? single : element.fields)
	{
		const auto value = item.metadata.find(field);
		if (value == item.metadata.end() || value->second.empty())
			continue;
		if (!joined.empty())
			joined += element.separator;
		joined += value->second;
	}
	if (element.marqueeSeconds > 0) // A marquee is one line, whatever the source text does.
		std::replace_if(joined.begin(), joined.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
	return joined.empty() ? element.text : joined;
}

bool hasPlayed(const LibraryItem& item)
{
	return item.lastPlayed > 0 || item.playTimeSeconds > 0;
}

std::optional<double> libraryStat(const std::string& source, const std::vector<LibraryItem>& items,
		int selected, int pageSize, int64_t now)
{
	if (source == "position") return selected >= 0 ? selected + 1 : 0;
	if (source == "page") return selected < 0 ? 0 : pageSize > 0 ? selected / pageSize + 1 : 1;
	if (source == "pageCount") return items.empty() ? 0 : pageSize > 0 ? (items.size() + pageSize - 1) / pageSize : 1;
	if (source == "playHours" || source == "neglect")
	{
		if (selected < 0 || selected >= static_cast<int>(items.size()) || !items[selected].isGame) return std::nullopt;
		const LibraryItem& item = items[selected];
		if (source == "playHours") return std::max(0.0, item.playTimeSeconds) / 3600.0;
		if (!hasPlayed(item)) return 1.0;
		if (item.lastPlayed <= 0) return std::nullopt; // History can contain duration without a timestamp.
		const double days = std::max(0.0, static_cast<double>(now - item.lastPlayed) / 86400.0);
		return std::clamp(std::log2(days + 1.0) / 9.0, 0.05, 1.0);
	}
	double result = 0;
	for (const LibraryItem& item : items)
	{
		if (!item.isGame) continue;
		const double hours = std::max(0.0, item.playTimeSeconds) / 3600.0;
		if (source == "gameCount") result++;
		else if (source == "favouriteCount") result += item.favourite;
		else if (source == "unplayedCount") result += !hasPlayed(item);
		else if (source == "totalPlayHours") result += hours;
		else if (source == "maxPlayHours") result = std::max(result, hours);
	}
	if (source != "gameCount" && source != "favouriteCount" && source != "unplayedCount" && source != "totalPlayHours" && source != "maxPlayHours") return std::nullopt;
	return result;
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
		const std::string& stateLayoutId, const std::function<void(size_t)>& toggleFavourite,
		const std::function<void(const std::string&)>& playSound)
{
	iconScale = std::clamp(iconScale, 0.5f, 2.f);
	const std::string& browserSource = iconSource ? *iconSource : layout.artworkSource;
	static std::string selectedId;
	static std::string selectedLayoutId;
	static std::string previewSource;
	static bool scrollSelection = false;
	static RailState railState;
	static bool gridFocused = false;
	static std::string effectSelection;
	static double selectionTime = 0;
	static double lastInputTime = 0, lastAttractStep = 0;
	static std::string launchingId;
	static double launchStart = 0;
	static int launchLastFrame = 0;
	const std::string layoutKey = themeDirectory + '/' + (stateLayoutId.empty() ? layout.id : stateLayoutId);
	const bool layoutChanged = selectedLayoutId != layoutKey;
	if (layoutChanged)
	{
		selectedLayoutId = layoutKey;
		selectedId.clear();
		previewSource = layout.previewSource;
		scrollSelection = false;
		railState = {};
		gridFocused = false;
		effectSelection.clear();
		lastInputTime = ImGui::GetTime();
		launchingId.clear();
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
		railState.position = 0;
	}
	const std::string selectionBeforeBrowsing = selectedId;
	int launchingIndex = -1;
	// Leaving the library abandons a launch, so it can not fire when the player comes back.
	if (ImGui::GetFrameCount() > launchLastFrame + 1)
		launchingId.clear();
	if (!launchingId.empty())
	{
		for (int i = 0; i < static_cast<int>(items.size()); ++i)
			if (itemKey(items[i], static_cast<size_t>(i)) == launchingId)
				launchingIndex = i;
		if (launchingIndex < 0) // The game has left the list, for example through a rescan.
			launchingId.clear();
	}
	const bool launching = launchingIndex >= 0;
	LibraryState libraryState;
	if (launching)
	{
		libraryState.launchElapsed = static_cast<float>(ImGui::GetTime() - launchStart);
		libraryState.launchProgress = std::clamp(libraryState.launchElapsed / layout.launchSeconds, 0.f, 1.f);
	}
	bool selectionMoved = false;
	if (playerInputThisFrame())
		lastInputTime = ImGui::GetTime();
	const bool attracting = !launching && layout.attract.idleSeconds > 0 && ImGui::GetTime() - lastInputTime >= layout.attract.idleSeconds;
	libraryState.attracting = attracting;
	if (!attracting)
		lastAttractStep = ImGui::GetTime();
	else if (items.size() > 1 && ImGui::GetTime() - lastAttractStep >= layout.attract.intervalSeconds)
	{
		lastAttractStep = ImGui::GetTime();
		selected = (selected + 1) % static_cast<int>(items.size());
		selectedId = itemKey(items[selected], static_cast<size_t>(selected));
		scrollSelection = true;
		selectionMoved = true;
	}
	// Attract mode and a changed list have already moved the selection: only what follows is the player.
	const std::string selectedBeforeInput = selectedId;
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
	const Canvas content{contentOrigin, contentSize, ImVec2(contentOrigin.x + contentSize.x, contentOrigin.y + contentSize.y), iconScale};
	ImDrawList* drawList = ImGui::GetWindowDrawList();
	drawList->AddRectFilled(contentOrigin, contentOrigin + contentSize, color(layout.background));

	const ImRect browser = normalizedRect(canvas, layout.browser);
	const ImVec2 browserPos(ImMax(browser.Min.x, canvas.origin.x), ImMax(browser.Min.y, canvas.origin.y));
	const ImVec2 browserMax(ImMin(browser.Max.x, canvas.max.x), ImMin(browser.Max.y, canvas.max.y));
	const ImVec2 browserSize(std::max(0.0f, browserMax.x - browserPos.x), std::max(0.0f, browserMax.y - browserPos.y));
	ImGui::SetCursorScreenPos(browserPos);
	// Nothing else can be chosen once a game is on its way, but it should not look dimmed either.
	ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.f);
	ImGui::BeginDisabled(launching);
	const bool railMode = layout.mode == "rail";
	const bool paged = layout.pagedGrid.rows > 0;
	const bool childVisible = ImGui::BeginChild("##themeLibraryBrowser", browserSize, ImGuiChildFlags_NavFlattened,
			ImGuiWindowFlags_NoBackground | ((railMode || paged) ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None));
	if (layout.fontSize > 0)
		ImGui::PushFont(nullptr, std::clamp(layout.fontSize * canvas.size.y * iconScale, 10.f, 96.f));
	const ImVec2 previousItemSpacing = ImGui::GetStyle().ItemSpacing;
	// Selectable expands its hit area by half ItemSpacing. Narrow spines need exact
	// bounds; their gap is already supplied by the rack's explicit column spacing.
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(layout.mode == "rack" || paged ? 0.f : previousItemSpacing.x, 0.f));
	ImDrawList* browserDrawList = ImGui::GetWindowDrawList();
	int activated = -1;
	int browserColumns = 1;
	const auto observeItem = [&](size_t index) {
		if (static_cast<int>(index) == selected)
			libraryState.selectionRect = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
		if (afterItem) afterItem(index);
	};
	if (childVisible && !items.empty())
	{
		const float spacing = std::max(0.0f, uiScaled(layout.spacing) * iconScale);
		if (paged)
		{
			browserColumns = drawPagedGrid(layout, items, browserSource, iconScale, gridFocused, selected, selectedId, activated, observeItem);
			scrollSelection = false;
		}
		else if (railMode)
		{
			scrollSelection = false;
			drawRail(layout, canvas, items, browserDrawList, railState, selected, selectedId, activated, selectionMoved, observeItem);
		}
		else if (layout.mode == "list")
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
					observeItem(static_cast<size_t>(i));
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
						observeItem(static_cast<size_t>(i));
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
	if (effectSelection != selectedId || layoutChanged || ImGui::GetTime() < selectionTime)
	{
		effectSelection = selectedId;
		selectionTime = ImGui::GetTime();
	}
	libraryState.selectionElapsed = static_cast<float>(ImGui::GetTime() - selectionTime);
	libraryState.items = &items;
	libraryState.selected = selected;
	libraryState.pageSize = browserColumns * layout.pagedGrid.rows;
	// Theme details sit opposite the browser. Keep full-canvas backdrops fixed,
	// while the cover and metadata share a vertically scrollable scaled column.
	const bool detailsOnRight = layout.browser.x + layout.browser.w * 0.5f <= 0.5f;
	const float split = detailsOnRight ? layout.browser.x + layout.browser.w : layout.browser.x;
	const bool authoredDetails = std::any_of(layout.elements.begin(), layout.elements.end(), [](const Element& element) { return element.type == "details"; });
	const Rect details{detailsOnRight ? split : 0.f, 0.f, authoredDetails ? 0.f : detailsOnRight ? 1.f - split : split, 1.f};
	// Resolve background media after hover/navigation so both layers show the same game.
	// The parent draw list is rendered beneath the browser child, regardless of submission order.
	drawElements(layout, canvas, content, themeDirectory, current, drawList, ElementLayer::BehindBrowser, details, false, previewSource, libraryState);
	ImGui::SetCursorScreenPos(content.origin);
	ImGui::BeginChild("##themeLibraryOverlay", content.size, ImGuiChildFlags_None,
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus
					| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	drawElements(layout, canvas, content, themeDirectory, current, ImGui::GetWindowDrawList(), ElementLayer::OverBrowser, details, false, previewSource, libraryState);
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
			drawElements(layout, detailCanvas, detailCanvas, themeDirectory, current, ImGui::GetWindowDrawList(), ElementLayer::BehindBrowser, details, true, previewSource, libraryState);
			drawElements(layout, detailCanvas, detailCanvas, themeDirectory, current, ImGui::GetWindowDrawList(), ElementLayer::OverBrowser, details, true, previewSource, libraryState);
			ImGui::Dummy(ImVec2(width, size.y));
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
	}
	drawDetails(layout, canvas, current);
	drawButtons(layout, canvas, items, browserColumns, selected, previewSource, scrollSelection, activated, toggleFavourite, railState.focusButtons);
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	if (selected >= 0) selectedId = itemKey(items[selected], static_cast<size_t>(selected));
	if (playSound && !layout.moveSound.empty() && selectedId != selectedBeforeInput)
		playSound(layout.moveSound);
	if (launching)
	{
		ImGui::SetCursorScreenPos(content.origin);
		ImGui::BeginChild("##themeLibraryLaunch", content.size, ImGuiChildFlags_None,
				ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		drawElements(layout, canvas, content, themeDirectory, current, ImGui::GetWindowDrawList(), ElementLayer::OverEverything, details, false, previewSource, libraryState);
		ImGui::EndChild();
		launchLastFrame = ImGui::GetFrameCount();
		// Any press skips what remains: the sequence must never stand between the player and the game.
		if (libraryState.launchProgress >= 1.f || playerPressedAnything())
		{
			activated = launchingIndex;
			launchingId.clear();
		}
	}
	else if (activated >= 0 && layout.launchSeconds > 0)
	{
		launchingId = itemKey(items[activated], static_cast<size_t>(activated));
		launchStart = ImGui::GetTime();
		launchLastFrame = ImGui::GetFrameCount();
		selected = activated;
		selectedId = launchingId;
		activated = -1;
		if (playSound && !layout.launchSound.empty())
			playSound(layout.launchSound);
	}
	ImGui::SetCursorScreenPos(contentOrigin);
	ImGui::Dummy(contentSize);
	return activated;
}
} // namespace themes
