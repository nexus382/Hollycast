// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "theme.h"
#include "theme_font_validation.h"

#include "json.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace themes
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;

[[noreturn]] void invalid(const std::string& path, const std::string& reason)
{
	throw std::runtime_error(path + ": " + reason);
}

void objectFields(const Json& value, const std::string& path, std::initializer_list<std::string_view> allowed)
{
	if (!value.is_object())
		invalid(path, "must be an object");
	for (auto it = value.begin(); it != value.end(); ++it)
	{
		bool found = false;
		for (auto field : allowed)
			if (it.key() == field)
			{
				found = true;
				break;
			}
		if (!found)
			invalid(path + "." + it.key(), "unknown field");
	}
}

const Json& required(const Json& value, const char* key, const std::string& path)
{
	auto it = value.find(key);
	if (it == value.end())
		invalid(path + "." + key, "is required");
	return *it;
}

std::string stringValue(const Json& value, const std::string& path, bool allowEmpty = true)
{
	if (!value.is_string())
		invalid(path, "must be a string");
	std::string result = value.get<std::string>();
	if (!allowEmpty && result.empty())
		invalid(path, "must not be empty");
	return result;
}

bool boolValue(const Json& value, const std::string& path)
{
	if (!value.is_boolean())
		invalid(path, "must be a boolean");
	return value.get<bool>();
}

double numberValue(const Json& value, const std::string& path, double minimum, double maximum)
{
	if (!value.is_number())
		invalid(path, "must be a number");
	double n = value.get<double>();
	if (!std::isfinite(n) || n < minimum || n > maximum)
		invalid(path, "must be finite and between " + std::to_string(minimum) + " and " + std::to_string(maximum));
	return n;
}

int integerValue(const Json& value, const std::string& path, int minimum, int maximum)
{
	if (!value.is_number_integer())
		invalid(path, "must be an integer");
	if (value.is_number_unsigned())
	{
		const uint64_t n = value.get<uint64_t>();
		if (n > static_cast<uint64_t>(maximum) || (minimum > 0 && n < static_cast<uint64_t>(minimum)))
			invalid(path, "must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
		return static_cast<int>(n);
	}
	long long n = value.get<long long>();
	if (n < minimum || n > maximum)
		invalid(path, "must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
	return static_cast<int>(n);
}

bool validId(const std::string& id)
{
	if (id.empty() || id.size() > 64)
		return false;
	for (unsigned char c : id)
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
			return false;
	return true;
}

uint32_t colorValue(const Json& value, const std::string& path)
{
	std::string color = stringValue(value, path, false);
	if (color.size() != 6 && color.size() != 8)
		invalid(path, "must be a 6- or 8-digit hexadecimal color");
	uint32_t result = 0;
	for (char c : color)
	{
		unsigned int digit;
		if (c >= '0' && c <= '9') digit = c - '0';
		else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
		else invalid(path, "must contain only hexadecimal digits");
		result = (result << 4) | digit;
	}
	if (color.size() == 6)
		result = (result << 8) | 0xff;
	return result;
}

const std::set<std::string>& imguiColorNames()
{
	// Keep in sync with the public ImGuiCol_ enum; deliberately no ImGui runtime dependency.
	static const std::set<std::string> names = {
		"Text", "TextDisabled", "WindowBg", "ChildBg", "PopupBg", "Border", "BorderShadow",
		"FrameBg", "FrameBgHovered", "FrameBgActive", "TitleBg", "TitleBgActive", "TitleBgCollapsed",
		"MenuBarBg", "ScrollbarBg", "ScrollbarGrab", "ScrollbarGrabHovered", "ScrollbarGrabActive",
		"CheckMark", "SliderGrab", "SliderGrabActive", "Button", "ButtonHovered", "ButtonActive",
		"Header", "HeaderHovered", "HeaderActive", "Separator", "SeparatorHovered", "SeparatorActive",
		"ResizeGrip", "ResizeGripHovered", "ResizeGripActive", "InputTextCursor", "TabHovered", "Tab",
		"TabSelected", "TabSelectedOverline", "TabDimmed", "TabDimmedSelected", "TabDimmedSelectedOverline",
		"PlotLines", "PlotLinesHovered", "PlotHistogram", "PlotHistogramHovered", "TableHeaderBg",
		"TableBorderStrong", "TableBorderLight", "TableRowBg", "TableRowBgAlt", "TextLink", "TextSelectedBg",
		"TreeLines", "DragDropTarget", "DragDropTargetBg", "UnsavedMarker", "NavCursor", "NavWindowingHighlight",
		"NavWindowingDimBg", "ModalWindowDimBg"
	};
	return names;
}

Rect parseRect(const Json& value, const std::string& path, double minSize)
{
	if (!value.is_array() || value.size() != 4)
		invalid(path, "must be [x, y, w, h]");
	double x = numberValue(value[0], path + "[0]", 0, 1);
	double y = numberValue(value[1], path + "[1]", 0, 1);
	double w = numberValue(value[2], path + "[2]", minSize, 1);
	double h = numberValue(value[3], path + "[3]", minSize, 1);
	if (x + w > 1.0 || y + h > 1.0)
		invalid(path, "must lie wholly within normalized bounds [0, 1]");
	return {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
}

const std::set<std::string>& metadataFields()
{
	static const std::set<std::string> values = {"title", "description", "developer", "publisher", "genre", "players", "releaseDate", "productId", "region", "format", "playTime", "lastPlayed", "size"};
	return values;
}

const std::set<std::string>& artworkSources()
{
	static const std::set<std::string> values = {"", "cover", "miximage", "case", "screenshot", "fanart", "title", "physical", "titlescreen", "banner", "backcover", "spine"};
	return values;
}

bool pathWithin(const fs::path& root, const fs::path& path)
{
	auto r = root.begin();
	auto p = path.begin();
	for (; r != root.end(); ++r, ++p)
		if (p == path.end() || *r != *p)
			return false;
	return true;
}
} // namespace

std::string assetPath(const std::string& directory, const std::string& relative)
{
	if (relative.empty())
		invalid("asset", "path must not be empty");
	if (relative.find('\0') != std::string::npos || relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos)
		invalid("asset", "path must be a package-relative filesystem path");
	fs::path rel = fs::u8path(relative);
	if (rel.is_absolute() || rel.has_root_name() || rel.has_root_directory())
		invalid("asset", "absolute paths are not allowed");
	for (const auto& part : rel)
		if (part == "..")
			invalid("asset", "parent traversal is not allowed");
	std::error_code ec;
	fs::path root = fs::weakly_canonical(fs::absolute(fs::u8path(directory), ec), ec);
	if (ec)
		invalid("asset", "cannot resolve theme directory");
	fs::path candidate = fs::weakly_canonical(root / rel, ec);
	if (ec)
		invalid("asset", "cannot resolve path");
	if (!pathWithin(root, candidate))
		invalid("asset", "path resolves outside the theme directory");
	return candidate.u8string();
}

Theme parse(const std::string& json, const std::string& directory)
{
	Json root;
	try { root = Json::parse(json); }
	catch (const std::exception& e) { invalid("theme.json", std::string("invalid JSON: ") + e.what()); }
	objectFields(root, "theme", {"formatVersion", "id", "name", "author", "description", "baseTheme", "colors", "layouts", "fonts"});
	if (integerValue(required(root, "formatVersion", "theme"), "formatVersion", 1, 1) != 1)
		invalid("formatVersion", "unsupported format version");

	Theme theme;
	theme.directory = directory;
	theme.id = stringValue(required(root, "id", "theme"), "id", false);
	if (!validId(theme.id))
		invalid("id", "must contain 1 to 64 lowercase letters, digits, hyphens, or underscores");
	theme.name = stringValue(required(root, "name", "theme"), "name", false);
	if (auto it = root.find("author"); it != root.end()) theme.author = stringValue(*it, "author");
	if (auto it = root.find("description"); it != root.end()) theme.description = stringValue(*it, "description");
	if (auto it = root.find("baseTheme"); it != root.end()) theme.baseTheme = integerValue(*it, "baseTheme", 0, 5);
	if (auto it = root.find("colors"); it != root.end())
	{
		if (!it->is_object()) invalid("colors", "must be an object");
		for (auto color = it->begin(); color != it->end(); ++color)
		{
			if (!imguiColorNames().count(color.key())) invalid("colors." + color.key(), "unknown ImGui color name");
			theme.colors.emplace(color.key(), colorValue(color.value(), "colors." + color.key()));
		}
	}
	if (auto it = root.find("fonts"); it != root.end())
	{
		objectFields(*it, "fonts", {"body", "bold"});
		for (auto font = it->begin(); font != it->end(); ++font)
		{
			const std::string field = "fonts." + font.key();
			const std::string path = assetPath(directory, stringValue(font.value(), field, false));
			std::error_code ec;
			const auto size = fs::file_size(fs::u8path(path), ec);
			if (ec || size < 12 || size > 32 * 1024 * 1024)
				invalid(field, "must reference a package font no larger than 32 MiB");
			std::ifstream input(fs::u8path(path), std::ios::binary);
			std::vector<uint8_t> bytes(static_cast<size_t>(size));
			if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || !validThemeFontDirectory(bytes.data(), bytes.size()))
				invalid(field, "must reference a valid TrueType/OpenType font");
			theme.fonts.emplace(font.key(), path);
		}
	}

	const Json& layouts = required(root, "layouts", "theme");
	if (!layouts.is_array() || layouts.empty() || layouts.size() > 16)
		invalid("layouts", "must be an array containing 1 to 16 layouts");
	std::set<std::string> layoutIds;
	for (size_t i = 0; i < layouts.size(); ++i)
	{
		const std::string lp = "layouts[" + std::to_string(i) + "]";
		const Json& item = layouts[i];
		objectFields(item, lp, {"id", "name", "mode", "browser", "columns", "itemAspect", "spacing", "showTitles", "background", "elements", "fontSize", "rowHeight", "textColor", "selectedTextColor", "selectorColor", "artworkSource", "rounding", "inactiveOpacity", "selectorBorderColor", "separatorColor", "showArtwork", "compactControls", "showInfoTooltip", "canvasAspect", "toolbar", "toolbarSelectedTextColor", "toolbarLogo", "toolbarLogoHeight", "activation", "minItemWidth", "titleStyle", "selectorStyle", "showItemActions", "showFavouriteMarker", "hoverDetails", "pixelArt", "placeholder", "previewSource", "narrowLayout", "narrowAspect"});
		Layout layout;
		layout.id = stringValue(required(item, "id", lp), lp + ".id", false);
		if (!validId(layout.id)) invalid(lp + ".id", "must contain 1 to 64 lowercase letters, digits, hyphens, or underscores");
		if (!layoutIds.insert(layout.id).second) invalid(lp + ".id", "must be unique within the theme");
		layout.name = stringValue(required(item, "name", lp), lp + ".name", false);
		layout.mode = stringValue(required(item, "mode", lp), lp + ".mode", false);
		if (layout.mode != "grid" && layout.mode != "list" && layout.mode != "rack") invalid(lp + ".mode", "must be grid, list, or rack");
		layout.browser = parseRect(required(item, "browser", lp), lp + ".browser", 0.1);
		if (auto it = item.find("columns"); it != item.end()) layout.columns = integerValue(*it, lp + ".columns", 1, layout.mode == "rack" ? 64 : 12);
		if (auto it = item.find("itemAspect"); it != item.end()) layout.itemAspect = static_cast<float>(numberValue(*it, lp + ".itemAspect", layout.mode == "rack" ? 0.03 : 0.3, 3));
		if (auto it = item.find("spacing"); it != item.end()) layout.spacing = static_cast<float>(numberValue(*it, lp + ".spacing", 0, 48));
		if (auto it = item.find("showTitles"); it != item.end()) layout.showTitles = boolValue(*it, lp + ".showTitles");
		if (auto it = item.find("background"); it != item.end()) layout.background = colorValue(*it, lp + ".background");
		if (auto it = item.find("fontSize"); it != item.end()) layout.fontSize = static_cast<float>(numberValue(*it, lp + ".fontSize", 0.01, 0.08));
		if (auto it = item.find("rowHeight"); it != item.end()) layout.rowHeight = static_cast<float>(numberValue(*it, lp + ".rowHeight", 0.02, 0.2));
		if (auto it = item.find("textColor"); it != item.end()) layout.textColor = colorValue(*it, lp + ".textColor");
		if (auto it = item.find("selectedTextColor"); it != item.end()) layout.selectedTextColor = colorValue(*it, lp + ".selectedTextColor");
		if (auto it = item.find("selectorColor"); it != item.end()) layout.selectorColor = colorValue(*it, lp + ".selectorColor");
		if (auto it = item.find("artworkSource"); it != item.end())
		{
			layout.artworkSource = stringValue(*it, lp + ".artworkSource");
			if (!artworkSources().count(layout.artworkSource)) invalid(lp + ".artworkSource", "must name a supported artwork source");
		}
		if (auto it = item.find("rounding"); it != item.end()) layout.rounding = static_cast<float>(numberValue(*it, lp + ".rounding", 0, 48));
		if (auto it = item.find("inactiveOpacity"); it != item.end()) layout.inactiveOpacity = static_cast<float>(numberValue(*it, lp + ".inactiveOpacity", 0.1, 1));
		if (auto it = item.find("selectorBorderColor"); it != item.end()) layout.selectorBorderColor = colorValue(*it, lp + ".selectorBorderColor");
		if (auto it = item.find("separatorColor"); it != item.end()) layout.separatorColor = colorValue(*it, lp + ".separatorColor");
		if (auto it = item.find("showArtwork"); it != item.end()) layout.showArtwork = boolValue(*it, lp + ".showArtwork");
		if (auto it = item.find("compactControls"); it != item.end()) layout.compactControls = boolValue(*it, lp + ".compactControls");
		if (auto it = item.find("showInfoTooltip"); it != item.end()) layout.showInfoTooltip = boolValue(*it, lp + ".showInfoTooltip");
		if (auto it = item.find("toolbarSelectedTextColor"); it != item.end()) layout.toolbarSelectedTextColor = colorValue(*it, lp + ".toolbarSelectedTextColor");
		if (auto it = item.find("toolbarLogo"); it != item.end())
		{
			layout.toolbarLogoAsset = assetPath(directory, stringValue(*it, lp + ".toolbarLogo", false));
			std::error_code ec;
			if (!fs::is_regular_file(fs::u8path(layout.toolbarLogoAsset), ec) || ec)
				invalid(lp + ".toolbarLogo", "must reference an image inside the package");
		}
		if (auto it = item.find("toolbarLogoHeight"); it != item.end())
		{
			if (layout.toolbarLogoAsset.empty()) invalid(lp + ".toolbarLogoHeight", "requires toolbarLogo");
			layout.toolbarLogoHeight = static_cast<float>(numberValue(*it, lp + ".toolbarLogoHeight", 16, 96));
		}
		if (auto it = item.find("canvasAspect"); it != item.end()) layout.canvasAspect = static_cast<float>(numberValue(*it, lp + ".canvasAspect", 0.4, 3));
		if (auto it = item.find("minItemWidth"); it != item.end()) layout.minItemWidth = static_cast<float>(numberValue(*it, lp + ".minItemWidth", 32, 256));
		if (auto it = item.find("showItemActions"); it != item.end()) layout.showItemActions = boolValue(*it, lp + ".showItemActions");
		if (auto it = item.find("showFavouriteMarker"); it != item.end()) layout.showFavouriteMarker = boolValue(*it, lp + ".showFavouriteMarker");
		if (auto it = item.find("hoverDetails"); it != item.end()) layout.hoverDetails = boolValue(*it, lp + ".hoverDetails");
		if (auto it = item.find("placeholder"); it != item.end())
		{
			layout.placeholderAsset = assetPath(directory, stringValue(*it, lp + ".placeholder", false));
			std::error_code ec;
			if (!fs::is_regular_file(fs::u8path(layout.placeholderAsset), ec) || ec)
				invalid(lp + ".placeholder", "must reference an image inside the package");
		}
		if (auto it = item.find("toolbar"); it != item.end())
		{
			layout.toolbar = stringValue(*it, lp + ".toolbar");
			if (layout.toolbar != "standard" && layout.toolbar != "compact") invalid(lp + ".toolbar", "must be standard or compact");
		}
		if (auto it = item.find("activation"); it != item.end())
		{
			layout.activation = stringValue(*it, lp + ".activation");
			if (layout.activation != "singleClick" && layout.activation != "doubleClick") invalid(lp + ".activation", "must be singleClick or doubleClick");
		}
		if (auto it = item.find("titleStyle"); it != item.end())
		{
			layout.titleStyle = stringValue(*it, lp + ".titleStyle");
			if (layout.titleStyle != "full" && layout.titleStyle != "initials") invalid(lp + ".titleStyle", "must be full or initials");
		}
		if (auto it = item.find("selectorStyle"); it != item.end())
		{
			layout.selectorStyle = stringValue(*it, lp + ".selectorStyle");
			if (layout.selectorStyle != "fill" && layout.selectorStyle != "outline") invalid(lp + ".selectorStyle", "must be fill or outline");
		}
		if (auto it = item.find("previewSource"); it != item.end())
		{
			layout.previewSource = stringValue(*it, lp + ".previewSource");
			if (!artworkSources().count(layout.previewSource)) invalid(lp + ".previewSource", "must name an artwork source");
		}
		if (auto it = item.find("narrowLayout"); it != item.end()) layout.narrowLayout = stringValue(*it, lp + ".narrowLayout", false);
		if (auto it = item.find("narrowAspect"); it != item.end())
		{
			if (layout.narrowLayout.empty()) invalid(lp + ".narrowAspect", "requires narrowLayout");
			layout.narrowAspect = static_cast<float>(numberValue(*it, lp + ".narrowAspect", 0.4, 3));
		}
		if (auto it = item.find("pixelArt"); it != item.end())
		{
			const std::string pp = lp + ".pixelArt";
			objectFields(*it, pp, {"resolution", "dark", "light"});
			layout.pixelArt.resolution = integerValue(required(*it, "resolution", pp), pp + ".resolution", 4, 128);
			if (auto field = it->find("dark"); field != it->end()) layout.pixelArt.dark = colorValue(*field, pp + ".dark");
			if (auto field = it->find("light"); field != it->end()) layout.pixelArt.light = colorValue(*field, pp + ".light");
		}
		if (auto it = item.find("elements"); it != item.end())
		{
			if (!it->is_array() || it->size() > 64) invalid(lp + ".elements", "must be an array containing at most 64 elements");
			for (size_t j = 0; j < it->size(); ++j)
			{
				const std::string ep = lp + ".elements[" + std::to_string(j) + "]";
				const Json& source = (*it)[j];
				objectFields(source, ep, {"type", "rect", "source", "text", "color", "fontSize", "zIndex", "fit", "align", "endColor", "gradient", "rounding", "bold", "wrap", "fallback", "requiresArtwork", "selectedText", "icon", "selectedIcon", "borderColor", "selectedBorderColor", "action", "background", "selectedColor", "selectedBackground", "fields", "columns", "fallbackImage"});
				Element element;
				element.type = stringValue(required(source, "type", ep), ep + ".type", false);
				if (element.type != "panel" && element.type != "image" && element.type != "artwork" && element.type != "text" && element.type != "metadata" && element.type != "details" && element.type != "button") invalid(ep + ".type", "must be panel, image, artwork, text, metadata, details, or button");
				element.rect = parseRect(required(source, "rect", ep), ep + ".rect", element.type == "panel" ? 0.001 : 0.01);
				if (auto field = source.find("source"); field != source.end()) element.source = stringValue(*field, ep + ".source");
				if (auto field = source.find("text"); field != source.end()) element.text = stringValue(*field, ep + ".text");
				if (auto field = source.find("selectedText"); field != source.end())
				{
					if (element.type != "button") invalid(ep + ".selectedText", "is only valid on buttons");
					element.selectedText = stringValue(*field, ep + ".selectedText");
				}
				if (auto field = source.find("fallbackImage"); field != source.end())
				{
					if (element.type != "artwork") invalid(ep + ".fallbackImage", "is only valid on artwork elements");
					element.resolvedFallbackAsset = assetPath(directory, stringValue(*field, ep + ".fallbackImage", false));
					std::error_code ec;
					if (!fs::is_regular_file(fs::u8path(element.resolvedFallbackAsset), ec) || ec)
						invalid(ep + ".fallbackImage", "must reference an image inside the package");
				}
				if (auto field = source.find("color"); field != source.end()) element.color = colorValue(*field, ep + ".color");
				if (auto field = source.find("fontSize"); field != source.end()) element.fontSize = static_cast<float>(numberValue(*field, ep + ".fontSize", 0.01, 0.15));
				if (auto field = source.find("zIndex"); field != source.end()) element.zIndex = integerValue(*field, ep + ".zIndex", -100, 100);
				if (auto field = source.find("endColor"); field != source.end()) element.endColor = colorValue(*field, ep + ".endColor");
				if (element.type != "button")
					if (auto field = source.find("rounding"); field != source.end()) element.rounding = static_cast<float>(numberValue(*field, ep + ".rounding", 0, 48));
				if (auto field = source.find("bold"); field != source.end()) element.bold = boolValue(*field, ep + ".bold");
				if (auto field = source.find("wrap"); field != source.end()) element.wrap = boolValue(*field, ep + ".wrap");
				if (auto field = source.find("gradient"); field != source.end())
				{
					element.gradient = stringValue(*field, ep + ".gradient");
					if (element.gradient != "vertical" && element.gradient != "horizontal")
						invalid(ep + ".gradient", "must be vertical or horizontal");
				}
				if (auto field = source.find("fallback"); field != source.end())
				{
					element.fallback = stringValue(*field, ep + ".fallback", false);
					if (element.fallback != "default" && element.fallback != "none" && !artworkSources().count(element.fallback))
						invalid(ep + ".fallback", "must be default, none, or a supported artwork source");
				}
				if (auto field = source.find("requiresArtwork"); field != source.end())
				{
					element.requiresArtwork = stringValue(*field, ep + ".requiresArtwork");
					if (!artworkSources().count(element.requiresArtwork))
						invalid(ep + ".requiresArtwork", "must name a supported artwork source");
				}
				if (element.type == "details")
				{
					const Json& fields = required(source, "fields", ep);
					if (!fields.is_array() || fields.empty() || fields.size() > metadataFields().size())
						invalid(ep + ".fields", "must contain supported metadata fields");
					std::set<std::string> seen;
					for (const auto& value : fields)
					{
						const std::string field = stringValue(value, ep + ".fields", false);
						if (!metadataFields().count(field) || !seen.insert(field).second)
							invalid(ep + ".fields", "must contain unique supported metadata fields");
						element.fields.push_back(field);
					}
					if (auto field = source.find("columns"); field != source.end()) element.columns = integerValue(*field, ep + ".columns", 1, 3);
					if (element.zIndex < 0) invalid(ep + ".zIndex", "details must draw above the browser");
				}
				else if (source.count("fields") || source.count("columns")) invalid(ep, "fields and columns are only valid on details elements");
				if (auto field = source.find("action"); field != source.end()) element.action = stringValue(*field, ep + ".action", false);
				if (auto field = source.find("background"); field != source.end()) element.background = colorValue(*field, ep + ".background");
				if (auto field = source.find("selectedColor"); field != source.end()) element.selectedColor = colorValue(*field, ep + ".selectedColor");
				if (auto field = source.find("selectedBackground"); field != source.end()) element.selectedBackground = colorValue(*field, ep + ".selectedBackground");
				if (auto field = source.find("borderColor"); field != source.end()) element.borderColor = colorValue(*field, ep + ".borderColor");
				if (auto field = source.find("selectedBorderColor"); field != source.end()) element.selectedBorderColor = colorValue(*field, ep + ".selectedBorderColor");
				for (const auto& binding : {std::pair{"icon", &element.icon}, std::pair{"selectedIcon", &element.selectedIcon}})
					if (auto field = source.find(binding.first); field != source.end())
					{
						*binding.second = stringValue(*field, ep + "." + binding.first, false);
						if (*binding.second != "star" && *binding.second != "star-filled")
							invalid(ep + "." + binding.first, "must be star or star-filled");
					}
				if (element.type == "button")
					if (auto field = source.find("rounding"); field != source.end()) element.buttonRounding = static_cast<float>(numberValue(*field, ep + ".rounding", 0, 0.5));
				if (element.type != "button" && (!element.action.empty() || element.background || element.selectedColor || element.selectedBackground || element.buttonRounding || !element.icon.empty() || !element.selectedIcon.empty() || element.borderColor || element.selectedBorderColor))
					invalid(ep, "action and button appearance fields are only valid on buttons");
				if (auto field = source.find("fit"); field != source.end())
				{
					element.fit = stringValue(*field, ep + ".fit");
					if (element.fit != "contain" && element.fit != "cover" && element.fit != "stretch")
						invalid(ep + ".fit", "must be contain, cover, or stretch");
				}
				if (auto field = source.find("align"); field != source.end())
				{
					element.align = stringValue(*field, ep + ".align");
					if (element.align != "left" && element.align != "center" && element.align != "right")
						invalid(ep + ".align", "must be left, center, or right");
				}
				if (element.type == "image")
				{
					if (element.source.empty()) invalid(ep + ".source", "is required for image elements");
					std::string resolved = assetPath(directory, element.source);
					std::error_code ec;
					if (!fs::is_regular_file(fs::u8path(resolved), ec) || ec) invalid(ep + ".source", "must resolve to an existing regular file inside the theme package");
					element.resolvedAsset = std::move(resolved);
				}
				else if (element.type == "metadata")
				{
					if (!metadataFields().count(element.source)) invalid(ep + ".source", "must name a supported metadata field");
				}
				else if (element.type == "artwork" && element.source != "preview" && !artworkSources().count(element.source))
					invalid(ep + ".source", "must name a supported artwork source or preview");
				else if (element.type == "button")
				{
					if (element.action != "previous" && element.action != "next" && element.action != "up" && element.action != "down" && element.action != "activate" && element.action != "artwork" && element.action != "favourite")
						invalid(ep + ".action", "must be previous, next, up, down, activate, artwork, or favourite");
					if (element.zIndex < 0) invalid(ep + ".zIndex", "buttons must draw above the browser");
					if (element.action == "artwork" && (element.source.empty() || !artworkSources().count(element.source)))
						invalid(ep + ".source", "artwork buttons require a named artwork source");
					if (element.action != "artwork" && !element.source.empty())
						invalid(ep + ".source", "only artwork buttons accept a source");
				}
				layout.elements.push_back(std::move(element));
			}
		}
		theme.layouts.push_back(std::move(layout));
	}
	for (const auto& layout : theme.layouts)
		if (!layout.narrowLayout.empty())
		{
			if (layout.narrowLayout == layout.id || !layoutIds.count(layout.narrowLayout))
				invalid("layouts." + layout.id + ".narrowLayout", "must reference a different layout in this package");
		}
	return theme;
}

const Layout& responsiveLayout(const Theme& theme, const Layout& layout, float aspect)
{
	if (!layout.narrowLayout.empty() && aspect < layout.narrowAspect)
		for (const auto& candidate : theme.layouts)
			if (candidate.id == layout.narrowLayout)
				return candidate;
	return layout;
}

Theme load(const std::string& directory)
{
	const fs::path path = fs::u8path(assetPath(directory, "theme.json"));
	std::error_code ec;
	const auto size = fs::file_size(path, ec);
	if (ec) invalid("theme.json", "cannot read theme manifest");
	if (size > 256 * 1024) invalid("theme.json", "manifest exceeds 256 KiB");
	std::ifstream input(path, std::ios::binary);
	if (!input) invalid("theme.json", "cannot open theme manifest");
	std::string contents(static_cast<size_t>(size), '\0');
	if (size && !input.read(&contents[0], static_cast<std::streamsize>(size))) invalid("theme.json", "failed while reading theme manifest");
	return parse(contents, directory);
}
} // namespace themes
