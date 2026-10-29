// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace themes
{
// Coordinates are fractions of the library content area; colors are RRGGBBAA.
struct Rect { float x = 0, y = 0, w = 1, h = 1; };
struct Element
{
	std::string type; // panel, image, artwork, text, metadata
	Rect rect;
	std::string source; // package-relative image path, or metadata field
	std::string text;
	uint32_t color = 0xffffffff;
	float fontSize = 0.035f; // fraction of library height
	int zIndex = 0;
	std::string resolvedAsset; // Validated at load time; never resolved during rendering.
	std::string fit = "contain";
	std::string align = "left";
	std::optional<uint32_t> endColor; // Panel gradient, including alpha.
	std::string gradient = "vertical";
	float rounding = 0; // Logical UI pixels, for panels and images.
	bool bold = false;
	bool wrap = true; // False renders one line with an ellipsis.
	std::string fallback = "default"; // Artwork source, default library art, or none.
	std::string requiresArtwork; // Hide an overlay when its companion source is missing.
};
struct Layout
{
	std::string id, name;
	std::string mode = "grid";
	Rect browser;
	int columns = 4;
	float itemAspect = 0.8f; // width / height, including label
	float spacing = 12; // logical UI pixels
	bool showTitles = true;
	uint32_t background = 0x151820ff;
	std::vector<Element> elements;
	float fontSize = 0; // Fraction of canvas height; zero inherits the application font.
	float rowHeight = 0; // Fraction of canvas height; zero uses automatic list sizing.
	std::optional<uint32_t> textColor, selectedTextColor, selectorColor;
	std::string artworkSource; // Empty inherits the library media preference.
	float rounding = 0;
	float inactiveOpacity = 1;
	std::optional<uint32_t> selectorBorderColor, separatorColor;
	bool showArtwork = false; // Optional square thumbnail in list rows.
	bool compactControls = false; // Show favourite buttons on hover/focus or favourites.
	bool showInfoTooltip = true;
};
struct Theme
{
	std::string id, name, author, description, directory;
	int baseTheme = 0;
	std::map<std::string, uint32_t> colors; // ImGui color names, without ImGuiCol_
	std::vector<Layout> layouts;
};
struct CatalogEntry { std::string id, name, error; };

// Throws std::runtime_error with an author-facing diagnostic on invalid input.
Theme parse(const std::string& json, const std::string& directory);
Theme load(const std::string& directory);
// Asset paths must stay inside the theme, including through symlinks.
std::string assetPath(const std::string& directory, const std::string& relative);

// UI-thread owned catalog. Invalid/unavailable selections return nullptr.
std::string directory();
const std::vector<CatalogEntry>& catalog();
void reload();
const Theme* current();
const Layout* currentLayout();
const std::string& error();
// Copy a validated folder into the user theme directory. Never overwrites.
std::string install(const std::string& sourceDirectory);
void applyPalette();
} // namespace themes
