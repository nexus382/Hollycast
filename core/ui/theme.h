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
struct PixelArt
{
	int resolution = 0; // Zero preserves the original artwork and sampling.
	uint32_t dark = 0x263e32ff, light = 0x9eae91ff;
};
struct Element
{
	std::string type; // panel, image, artwork, text, metadata, details, button
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
	std::string action; // previous, next, up, down, activate, artwork, favourite (buttons only)
	std::optional<uint32_t> background; // Button fill; transparent permits asset-backed controls.
	std::optional<uint32_t> selectedColor, selectedBackground;
	std::optional<float> buttonRounding; // Fraction of the smaller button dimension, 0–0.5.
	std::string resolvedFallbackAsset; // Optional artwork image when library media is unavailable.
	std::vector<std::string> fields; // Ordered metadata bindings for a scrollable details element.
	int columns = 1; // Number of label/value pairs per details row.
	std::string selectedText; // Optional label for a selected artwork/favourite button.
	std::string icon, selectedIcon; // Optional vector button icons: star or star-filled.
	std::optional<uint32_t> borderColor, selectedBorderColor;
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
	// All presentation extensions are opt-in; defaults retain format-v1 behavior.
	float canvasAspect = 0;
	std::string toolbar = "standard";
	std::optional<uint32_t> toolbarSelectedTextColor;
	std::string toolbarLogoAsset; // Optional package image for compact-toolbar branding.
	float toolbarLogoHeight = 0; // Logical pixels; zero uses the toolbar control height.
	std::string activation = "singleClick";
	float minItemWidth = 96;
	std::string titleStyle = "full";
	std::string selectorStyle = "fill";
	bool showItemActions = true, hoverDetails = true;
	bool showFavouriteMarker = false; // Passive star; inherits text/pixel-art colors.
	PixelArt pixelArt;
	std::string placeholderAsset; // Validated package image for items with no artwork.
	std::string previewSource = "cover";
	std::string narrowLayout;
	float narrowAspect = 1.1f;
};
struct Theme
{
	std::string id, name, author, description, directory;
	int baseTheme = 0;
	std::map<std::string, uint32_t> colors; // ImGui color names, without ImGuiCol_
	std::vector<Layout> layouts;
	std::map<std::string, std::string> fonts; // Validated, resolved package font paths.
};
struct CatalogEntry { std::string id, name, error; };

// Throws std::runtime_error with an author-facing diagnostic on invalid input.
Theme parse(const std::string& json, const std::string& directory);
Theme load(const std::string& directory);
// Select an explicitly authored narrow-window fallback, without changing the saved layout.
const Layout& responsiveLayout(const Theme& theme, const Layout& layout, float aspect);
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
