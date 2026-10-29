// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "theme.h"

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
	static const std::set<std::string> values = {"", "cover", "miximage", "screenshot", "fanart", "title", "physical"};
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
	objectFields(root, "theme", {"formatVersion", "id", "name", "author", "description", "baseTheme", "colors", "layouts"});
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

	const Json& layouts = required(root, "layouts", "theme");
	if (!layouts.is_array() || layouts.empty() || layouts.size() > 16)
		invalid("layouts", "must be an array containing 1 to 16 layouts");
	std::set<std::string> layoutIds;
	for (size_t i = 0; i < layouts.size(); ++i)
	{
		const std::string lp = "layouts[" + std::to_string(i) + "]";
		const Json& item = layouts[i];
		objectFields(item, lp, {"id", "name", "mode", "browser", "columns", "itemAspect", "spacing", "showTitles", "background", "elements", "fontSize", "rowHeight", "textColor", "selectedTextColor", "selectorColor"});
		Layout layout;
		layout.id = stringValue(required(item, "id", lp), lp + ".id", false);
		if (!validId(layout.id)) invalid(lp + ".id", "must contain 1 to 64 lowercase letters, digits, hyphens, or underscores");
		if (!layoutIds.insert(layout.id).second) invalid(lp + ".id", "must be unique within the theme");
		layout.name = stringValue(required(item, "name", lp), lp + ".name", false);
		layout.mode = stringValue(required(item, "mode", lp), lp + ".mode", false);
		if (layout.mode != "grid" && layout.mode != "list") invalid(lp + ".mode", "must be grid or list");
		layout.browser = parseRect(required(item, "browser", lp), lp + ".browser", 0.1);
		if (auto it = item.find("columns"); it != item.end()) layout.columns = integerValue(*it, lp + ".columns", 1, 12);
		if (auto it = item.find("itemAspect"); it != item.end()) layout.itemAspect = static_cast<float>(numberValue(*it, lp + ".itemAspect", 0.3, 3));
		if (auto it = item.find("spacing"); it != item.end()) layout.spacing = static_cast<float>(numberValue(*it, lp + ".spacing", 0, 48));
		if (auto it = item.find("showTitles"); it != item.end()) layout.showTitles = boolValue(*it, lp + ".showTitles");
		if (auto it = item.find("background"); it != item.end()) layout.background = colorValue(*it, lp + ".background");
		if (auto it = item.find("fontSize"); it != item.end()) layout.fontSize = static_cast<float>(numberValue(*it, lp + ".fontSize", 0.01, 0.08));
		if (auto it = item.find("rowHeight"); it != item.end()) layout.rowHeight = static_cast<float>(numberValue(*it, lp + ".rowHeight", 0.02, 0.2));
		if (auto it = item.find("textColor"); it != item.end()) layout.textColor = colorValue(*it, lp + ".textColor");
		if (auto it = item.find("selectedTextColor"); it != item.end()) layout.selectedTextColor = colorValue(*it, lp + ".selectedTextColor");
		if (auto it = item.find("selectorColor"); it != item.end()) layout.selectorColor = colorValue(*it, lp + ".selectorColor");
		if (auto it = item.find("elements"); it != item.end())
		{
			if (!it->is_array() || it->size() > 64) invalid(lp + ".elements", "must be an array containing at most 64 elements");
			for (size_t j = 0; j < it->size(); ++j)
			{
				const std::string ep = lp + ".elements[" + std::to_string(j) + "]";
				const Json& source = (*it)[j];
				objectFields(source, ep, {"type", "rect", "source", "text", "color", "fontSize", "zIndex", "fit", "align"});
				Element element;
				element.type = stringValue(required(source, "type", ep), ep + ".type", false);
				if (element.type != "panel" && element.type != "image" && element.type != "artwork" && element.type != "text" && element.type != "metadata") invalid(ep + ".type", "must be panel, image, artwork, text, or metadata");
				element.rect = parseRect(required(source, "rect", ep), ep + ".rect", element.type == "panel" ? 0.001 : 0.01);
				if (auto field = source.find("source"); field != source.end()) element.source = stringValue(*field, ep + ".source");
				if (auto field = source.find("text"); field != source.end()) element.text = stringValue(*field, ep + ".text");
				if (auto field = source.find("color"); field != source.end()) element.color = colorValue(*field, ep + ".color");
				if (auto field = source.find("fontSize"); field != source.end()) element.fontSize = static_cast<float>(numberValue(*field, ep + ".fontSize", 0.01, 0.15));
				if (auto field = source.find("zIndex"); field != source.end()) element.zIndex = integerValue(*field, ep + ".zIndex", -100, 100);
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
				else if (element.type == "artwork" && !artworkSources().count(element.source))
					invalid(ep + ".source", "must be empty, cover, miximage, screenshot, fanart, title, or physical");
				layout.elements.push_back(std::move(element));
			}
		}
		theme.layouts.push_back(std::move(layout));
	}
	return theme;
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
