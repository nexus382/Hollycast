// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "theme.h"
#include "boxart/artwork_media.h"
#include "theme_font_validation.h"
#include "audio/ui_sound.h"

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
	static const std::set<std::string> values = {"title", "description", "developer", "publisher", "genre", "players", "releaseDate", "productId", "region", "format", "playTime", "lastPlayed", "size", "system"};
	return values;
}

const std::set<std::string>& artworkSources()
{
	static const std::set<std::string> values = [] {
		std::set<std::string> names{"", "disc"};
		for (const auto& media : artwork::mediaTypes) names.insert(media.name);
		return names;
	}();
	return values;
}

const std::set<std::string>& statSources()
{
	static const std::set<std::string> values = {"gameCount", "favouriteCount", "unplayedCount", "totalPlayHours", "maxPlayHours", "playHours", "neglect", "page", "pageCount", "position"};
	return values;
}

std::vector<std::string> metadataFieldList(const Json& value, const std::string& path)
{
	if (!value.is_array() || value.empty() || value.size() > metadataFields().size())
		invalid(path, "must contain supported metadata fields");
	std::vector<std::string> result;
	std::set<std::string> seen;
	for (const auto& entry : value)
	{
		const std::string field = stringValue(entry, path, false);
		if (!metadataFields().count(field) || !seen.insert(field).second)
			invalid(path, "must contain unique supported metadata fields");
		result.push_back(field);
	}
	return result;
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

static std::string soundPath(const std::string& directory, const Json& value, const std::string& path);

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

static std::string soundPath(const std::string& directory, const Json& value, const std::string& path)
{
	const std::string resolved = assetPath(directory, stringValue(value, path, false));
	std::error_code ec;
	const auto size = fs::file_size(fs::u8path(resolved), ec);
	if (ec || size > 1024 * 1024)
		invalid(path, "must reference a package sound no larger than 1 MiB");
	std::ifstream input(fs::u8path(resolved), std::ios::binary);
	std::vector<u8> bytes(static_cast<size_t>(size));
	std::vector<s16> decoded;
	if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()) || !decodeUiSound(bytes.data(), bytes.size(), decoded))
		invalid(path, "must be a 44.1 kHz 16-bit PCM mono or stereo WAV file");
	return resolved;
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
		objectFields(item, lp, {"id", "name", "mode", "browser", "columns", "itemAspect", "spacing", "showTitles", "background", "elements", "fontSize", "rowHeight", "textColor", "selectedTextColor", "selectorColor", "artworkSource", "rounding", "inactiveOpacity", "selectorBorderColor", "separatorColor", "showArtwork", "compactControls", "showInfoTooltip", "canvasAspect", "toolbar", "toolbarSelectedTextColor", "toolbarLogo", "toolbarLogoHeight", "activation", "minItemWidth", "titleStyle", "selectorStyle", "showItemActions", "showFavouriteMarker", "hoverDetails", "pixelArt", "placeholder", "previewSource", "narrowLayout", "narrowAspect", "rail", "attract", "sounds", "launch", "pagedGrid", "requiredMedia", "list"});
		Layout layout;
		layout.id = stringValue(required(item, "id", lp), lp + ".id", false);
		if (!validId(layout.id)) invalid(lp + ".id", "must contain 1 to 64 lowercase letters, digits, hyphens, or underscores");
		if (!layoutIds.insert(layout.id).second) invalid(lp + ".id", "must be unique within the theme");
		layout.name = stringValue(required(item, "name", lp), lp + ".name", false);
		layout.mode = stringValue(required(item, "mode", lp), lp + ".mode", false);
		if (layout.mode != "grid" && layout.mode != "list" && layout.mode != "rack" && layout.mode != "rail" && layout.mode != "orbit") invalid(lp + ".mode", "must be grid, list, rack, rail, or orbit");
		layout.browser = parseRect(required(item, "browser", lp), lp + ".browser", 0.1);
		if (auto it = item.find("list"); it != item.end())
		{
			const std::string path = lp + ".list";
			if (layout.mode != "list") invalid(path, "requires list mode");
			objectFields(*it, path, {"systemBadge", "favouriteBadge", "favouriteColor", "selectOnHover"});
			if (auto field = it->find("systemBadge"); field != it->end()) layout.list.systemBadge = boolValue(*field, path + ".systemBadge");
			if (auto field = it->find("favouriteBadge"); field != it->end()) layout.list.favouriteBadge = boolValue(*field, path + ".favouriteBadge");
			if (auto field = it->find("favouriteColor"); field != it->end()) layout.list.favouriteColor = colorValue(*field, path + ".favouriteColor");
			if (auto field = it->find("selectOnHover"); field != it->end()) layout.list.selectOnHover = boolValue(*field, path + ".selectOnHover");
		}
		if (auto it = item.find("requiredMedia"); it != item.end())
		{
			if (!it->is_array() || it->size() > artworkSources().size()) invalid(lp + ".requiredMedia", "must be an array of media source names");
			std::set<std::string> seen;
			for (const auto& entry : *it)
			{
				const std::string name = stringValue(entry, lp + ".requiredMedia", false);
				if (!artworkSources().count(name) || !seen.insert(name).second) invalid(lp + ".requiredMedia", "must contain unique supported media sources");
				layout.requiredMedia.push_back(name);
			}
		}
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
			objectFields(*it, pp, {"resolution", "dark", "light", "levels"});
			layout.pixelArt.resolution = integerValue(required(*it, "resolution", pp), pp + ".resolution", 4, 128);
			if (auto field = it->find("levels"); field != it->end()) layout.pixelArt.levels = integerValue(*field, pp + ".levels", 2, 256);
			if (auto field = it->find("dark"); field != it->end()) layout.pixelArt.dark = colorValue(*field, pp + ".dark");
			if (auto field = it->find("light"); field != it->end()) layout.pixelArt.light = colorValue(*field, pp + ".light");
		}
		if (auto it = item.find("rail"); it != item.end())
		{
			const std::string rp = lp + ".rail";
			if (layout.mode != "rail") invalid(rp, "requires rail mode");
			objectFields(*it, rp, {"visible", "center", "tilt", "skew", "falloff", "fade", "rowColor", "numbers", "favouriteMark", "badges"});
			if (auto field = it->find("visible"); field != it->end()) layout.rail.visible = integerValue(*field, rp + ".visible", 1, 8);
			if (auto field = it->find("center"); field != it->end()) layout.rail.center = static_cast<float>(numberValue(*field, rp + ".center", 0.1, 0.9));
			if (auto field = it->find("tilt"); field != it->end()) layout.rail.tilt = static_cast<float>(numberValue(*field, rp + ".tilt", 0, 0.1));
			if (auto field = it->find("skew"); field != it->end()) layout.rail.skew = static_cast<float>(numberValue(*field, rp + ".skew", 0, 0.1));
			if (auto field = it->find("falloff"); field != it->end()) layout.rail.falloff = static_cast<float>(numberValue(*field, rp + ".falloff", 0, 0.1));
			if (auto field = it->find("fade"); field != it->end()) layout.rail.fade = static_cast<float>(numberValue(*field, rp + ".fade", 0, 0.25));
			if (auto field = it->find("rowColor"); field != it->end()) layout.rail.rowColor = colorValue(*field, rp + ".rowColor");
			if (auto field = it->find("numbers"); field != it->end()) layout.rail.numbers = boolValue(*field, rp + ".numbers");
			if (auto field = it->find("favouriteMark"); field != it->end()) layout.rail.favouriteMark = boolValue(*field, rp + ".favouriteMark");
			if (auto badges = it->find("badges"); badges != it->end())
			{
				if (!badges->is_array() || badges->empty() || badges->size() > 4)
					invalid(rp + ".badges", "must contain 1 to 4 lists of metadata fields");
				for (const auto& badge : *badges)
					layout.rail.badges.push_back(metadataFieldList(badge, rp + ".badges"));
			}
		}
		if (auto it = item.find("pagedGrid"); it != item.end())
		{
			const std::string gp = lp + ".pagedGrid";
			if (layout.mode != "grid") invalid(gp, "requires grid mode");
			objectFields(*it, gp, {"rows", "padding", "frame", "unplayedFrame"});
			layout.pagedGrid.rows = integerValue(required(*it, "rows", gp), gp + ".rows", 1, 12);
			if (auto field = it->find("padding"); field != it->end()) layout.pagedGrid.padding = static_cast<float>(numberValue(*field, gp + ".padding", 0, 24));
			for (auto binding : {std::pair{"frame", &layout.pagedGrid.frame}, std::pair{"unplayedFrame", &layout.pagedGrid.unplayedFrame}})
				if (auto field = it->find(binding.first); field != it->end())
				{
					*binding.second = assetPath(directory, stringValue(*field, gp + "." + binding.first, false));
					std::error_code ec;
					if (!fs::is_regular_file(fs::u8path(*binding.second), ec) || ec) invalid(gp + "." + binding.first, "must reference an image inside the package");
				}
		}
		if (auto it = item.find("sounds"); it != item.end())
		{
			const std::string sp = lp + ".sounds";
			objectFields(*it, sp, {"move", "launch"});
			if (auto field = it->find("move"); field != it->end()) layout.moveSound = soundPath(directory, *field, sp + ".move");
			if (auto field = it->find("launch"); field != it->end()) layout.launchSound = soundPath(directory, *field, sp + ".launch");
		}
		if (auto it = item.find("launch"); it != item.end())
		{
			const std::string ap = lp + ".launch";
			objectFields(*it, ap, {"seconds"});
			layout.launchSeconds = static_cast<float>(numberValue(required(*it, "seconds", ap), ap + ".seconds", 0.1, 5));
		}
		if (auto it = item.find("attract"); it != item.end())
		{
			const std::string ap = lp + ".attract";
			objectFields(*it, ap, {"idleSeconds", "intervalSeconds"});
			layout.attract.idleSeconds = static_cast<float>(numberValue(required(*it, "idleSeconds", ap), ap + ".idleSeconds", 1, 3600));
			if (auto field = it->find("intervalSeconds"); field != it->end()) layout.attract.intervalSeconds = static_cast<float>(numberValue(*field, ap + ".intervalSeconds", 0.5, 600));
		}
		if (auto it = item.find("elements"); it != item.end())
		{
			if (!it->is_array() || it->size() > 64) invalid(lp + ".elements", "must be an array containing at most 64 elements");
			for (size_t j = 0; j < it->size(); ++j)
			{
				const std::string ep = lp + ".elements[" + std::to_string(j) + "]";
				const Json& source = (*it)[j];
				objectFields(source, ep, {"type", "rect", "source", "text", "color", "fontSize", "zIndex", "fit", "align", "endColor", "gradient", "rounding", "bold", "wrap", "fallback", "requiresArtwork", "selectedText", "icon", "selectedIcon", "borderColor", "selectedBorderColor", "action", "background", "selectedColor", "selectedBackground", "fields", "columns", "autoFit", "fallbackImage", "separator", "when", "launchFadeFrom", "bleed", "image", "hoverImage", "selectedImage", "selectedHoverImage", "slant", "marqueeSeconds", "sweepSeconds", "spin", "anchor", "onSelection", "tileSeconds", "maximum", "valueScale", "digits"});
				Element element;
				element.type = stringValue(required(source, "type", ep), ep + ".type", false);
				if (element.type != "panel" && element.type != "image" && element.type != "artwork" && element.type != "text" && element.type != "metadata" && element.type != "details" && element.type != "button" && element.type != "stat" && element.type != "meter") invalid(ep + ".type", "must be panel, image, artwork, text, metadata, details, button, stat, or meter");
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
				if (auto field = source.find("when"); field != source.end())
				{
					element.when = stringValue(*field, ep + ".when");
					if (element.when != "attract" && element.when != "interactive" && element.when != "launching" && element.when != "unplayed" && element.when != "played") invalid(ep + ".when", "must be attract, interactive, launching, unplayed, or played");
					if (element.type == "button" || element.type == "details") invalid(ep + ".when", "is not valid on interactive elements");
				}
				if (auto field = source.find("bleed"); field != source.end())
				{
					if (element.type == "button" || element.type == "details") invalid(ep + ".bleed", "is not valid on interactive elements");
					element.bleed = boolValue(*field, ep + ".bleed");
				}
				if (auto field = source.find("launchFadeFrom"); field != source.end())
				{
					if (element.when != "launching") invalid(ep + ".launchFadeFrom", "requires when to be launching");
					element.launchFadeFrom = static_cast<float>(numberValue(*field, ep + ".launchFadeFrom", 0, 0.99));
				}
				const bool textElement = element.type == "text" || element.type == "metadata" || element.type == "stat";
				if (auto field = source.find("anchor"); field != source.end())
				{
					if (element.type == "button" || element.type == "details") invalid(ep + ".anchor", "is not valid on interactive elements");
					element.anchor = stringValue(*field, ep + ".anchor", false);
					if (element.anchor != "selection" && element.anchor != "selectionX" && element.anchor != "selectionTrail") invalid(ep + ".anchor", "must be selection, selectionX, or selectionTrail");
					if (element.bleed) invalid(ep + ".anchor", "cannot be combined with bleed");
				}
				if (auto field = source.find("onSelection"); field != source.end())
				{
					if (element.type == "button" || element.type == "details") invalid(ep + ".onSelection", "is not valid on interactive elements");
					element.onSelection = static_cast<float>(numberValue(*field, ep + ".onSelection", 0.05, 5));
				}
				if (auto field = source.find("tileSeconds"); field != source.end())
				{
					if (element.type != "image" || source.count("spin") || source.count("sweepSeconds")) invalid(ep + ".tileSeconds", "requires an image without spin or sweepSeconds");
					element.tileSeconds = static_cast<float>(numberValue(*field, ep + ".tileSeconds", 0.5, 600));
				}
				if (element.type == "stat" || element.type == "meter")
				{
					if (!statSources().count(element.source)) invalid(ep + ".source", "must name a supported numeric statistic");
					if (auto field = source.find("maximum"); field != source.end()) element.maximum = static_cast<float>(numberValue(*field, ep + ".maximum", 0.001, 1000000000));
					if (auto field = source.find("valueScale"); field != source.end()) element.valueScale = static_cast<float>(numberValue(*field, ep + ".valueScale", 0.001, 1000000));
					if (auto field = source.find("digits"); field != source.end()) element.digits = integerValue(*field, ep + ".digits", 0, 12);
				}
				else if (source.count("maximum") || source.count("valueScale") || source.count("digits")) invalid(ep, "numeric appearance fields require a stat or meter");
				if (auto field = source.find("slant"); field != source.end())
				{
					if (!textElement) invalid(ep + ".slant", "is only valid on text and metadata elements");
					element.slant = static_cast<float>(numberValue(*field, ep + ".slant", 0, 0.5));
				}
				if (auto field = source.find("marqueeSeconds"); field != source.end())
				{
					if (!textElement) invalid(ep + ".marqueeSeconds", "is only valid on text and metadata elements");
					element.marqueeSeconds = static_cast<float>(numberValue(*field, ep + ".marqueeSeconds", 0.5, 600));
				}
				if (auto field = source.find("sweepSeconds"); field != source.end())
				{
					if (element.type != "image") invalid(ep + ".sweepSeconds", "is only valid on image elements");
					element.sweepSeconds = static_cast<float>(numberValue(*field, ep + ".sweepSeconds", 0.5, 600));
				}
				if (auto spin = source.find("spin"); spin != source.end())
				{
					const std::string sp = ep + ".spin";
					if (element.type != "image" && element.type != "artwork") invalid(sp, "is only valid on image and artwork elements");
					if (element.sweepSeconds > 0) invalid(sp, "cannot be combined with sweepSeconds");
					objectFields(*spin, sp, {"seconds", "launchSeconds", "center", "radius", "hole"});
					if (auto field = spin->find("hole"); field != spin->end()) element.spinHole = static_cast<float>(numberValue(*field, sp + ".hole", 0, 0.9));
					if (auto field = spin->find("launchSeconds"); field != spin->end()) element.spinLaunchSeconds = static_cast<float>(numberValue(*field, sp + ".launchSeconds", 0.02, 600));
					element.spinSeconds = static_cast<float>(numberValue(required(*spin, "seconds", sp), sp + ".seconds", 0.5, 600));
					const Json& center = required(*spin, "center", sp);
					if (!center.is_array() || center.size() != 2) invalid(sp + ".center", "must be [x, y]");
					element.spinCenterX = static_cast<float>(numberValue(center[0], sp + ".center[0]", -1, 2));
					element.spinCenterY = static_cast<float>(numberValue(center[1], sp + ".center[1]", -1, 2));
					element.spinRadius = static_cast<float>(numberValue(required(*spin, "radius", sp), sp + ".radius", 0.01, 2));
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
					element.fields = metadataFieldList(required(source, "fields", ep), ep + ".fields");
					if (auto field = source.find("columns"); field != source.end()) element.columns = integerValue(*field, ep + ".columns", 1, 3);
					if (auto field = source.find("autoFit"); field != source.end()) element.autoFit = boolValue(*field, ep + ".autoFit");
					if (element.zIndex < 0) invalid(ep + ".zIndex", "details must draw above the browser");
				}
				else if (source.count("columns")) invalid(ep + ".columns", "is only valid on details elements");
				else if (auto field = source.find("fields"); field != source.end())
				{
					if (element.type != "metadata") invalid(ep + ".fields", "is only valid on details and metadata elements");
					if (source.count("source")) invalid(ep + ".fields", "cannot be combined with source");
					element.fields = metadataFieldList(*field, ep + ".fields");
				}
				if (auto field = source.find("separator"); field != source.end())
				{
					if (element.type != "metadata" || element.fields.empty()) invalid(ep + ".separator", "requires a metadata element with fields");
					element.separator = stringValue(*field, ep + ".separator");
				}
				if (element.type != "details" && source.count("autoFit")) invalid(ep + ".autoFit", "is only valid on details elements");
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
				const std::pair<const char*, std::string Element::*> buttonImages[] = {
					{"image", &Element::buttonImage}, {"hoverImage", &Element::buttonHoverImage},
					{"selectedImage", &Element::buttonSelectedImage}, {"selectedHoverImage", &Element::buttonSelectedHoverImage}};
				for (const auto& [name, member] : buttonImages)
					if (auto field = source.find(name); field != source.end())
					{
						if (element.type != "button") invalid(ep + "." + name, "is only valid on buttons");
						std::string resolved = assetPath(directory, stringValue(*field, ep + "." + name, false));
						std::error_code ec;
						if (!fs::is_regular_file(fs::u8path(resolved), ec) || ec) invalid(ep + "." + name, "must reference an image inside the package");
						element.*member = std::move(resolved);
					}
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
					if (element.fields.empty() && !metadataFields().count(element.source)) invalid(ep + ".source", "must name a supported metadata field");
				}
				else if (element.type == "artwork" && element.source != "preview" && !artworkSources().count(element.source))
					invalid(ep + ".source", "must name a supported artwork source or preview");
				else if (element.type == "button")
				{
					if (element.action != "previous" && element.action != "next" && element.action != "up" && element.action != "down" && element.action != "activate" && element.action != "artwork" && element.action != "favourite" && element.action != "previousPage" && element.action != "nextPage")
						invalid(ep + ".action", "must be previous, next, up, down, activate, artwork, favourite, previousPage, or nextPage");
					if (element.zIndex < 0) invalid(ep + ".zIndex", "buttons must draw above the browser");
					if ((element.action == "previousPage" || element.action == "nextPage") && layout.pagedGrid.rows == 0)
						invalid(ep + ".action", "page buttons require pagedGrid");
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
