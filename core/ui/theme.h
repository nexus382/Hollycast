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
	std::string type; // panel, image, artwork, text, metadata, details, button, stat, meter
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
	std::vector<std::string> fields; // Ordered metadata bindings for a details or metadata element.
	std::string separator = "  /  "; // Joins the values of a multi-field metadata element.
	int columns = 1; // Number of label/value pairs per details row.
	std::string selectedText; // Optional label for a selected artwork/favourite button.
	std::string icon, selectedIcon; // Optional vector button icons: star or star-filled.
	std::optional<uint32_t> borderColor, selectedBorderColor;
	// Validated package artwork that replaces a button's drawn background, by state.
	std::string buttonImage, buttonHoverImage, buttonSelectedImage, buttonSelectedHoverImage;
	std::string when; // Empty, attract, interactive, launching, played, or unplayed.
	float launchFadeFrom = -1; // While launching, fades in from this fraction of the launch; negative disables.
	bool bleed = false; // Rect and spin use the whole library area, past a fixed-aspect canvas.
	float slant = 0; // Text shear, as horizontal shift per unit of height.
	float marqueeSeconds = 0; // Seconds for text to travel one rectangle width, right to left.
	float sweepSeconds = 0; // Image travels down its rectangle in this period.
	float spinSeconds = 0; // Image rotates around spinCenter once in this period, clipped to rect.
	float spinLaunchSeconds = 0; // Period the spin accelerates to during a launch; zero keeps its pace.
	float spinCenterX = 0.5f, spinCenterY = 0.5f; // Canvas fractions; may lie outside the canvas.
	float spinRadius = 0; // Fraction of canvas width.
	float spinHole = -1; // Cuts the image to a round disc with this fraction of its radius open; negative keeps it square.
	std::string anchor; // selection, selectionX, or selectionTrail; empty uses the authored rectangle.
	float onSelection = 0; // Show briefly after selection changes; zero is persistent.
	float tileSeconds = 0; // Seamlessly tile an image downward; zero leaves it still.
	float maximum = 1; // Meter's full-scale value.
	float valueScale = 1; // Multiplier for numeric stat text.
	int digits = 0; // Minimum zero-padded digits for stat text.
};
// Optional fixed rows per grid page. Zero keeps the ordinary scrolling grid.
struct PagedGrid
{
	int rows = 0;
	float padding = 4;
	std::string frame, unplayedFrame; // Validated package cell overlays.
};
// A perspective list that keeps the selected game at its center.
struct Rail
{
	int visible = 4; // Rows drawn on each side of the selection.
	float center = 0.5f; // Where the selected row sits, as a fraction of browser height.
	float tilt = 0.03f; // Row inset per row of distance, as a fraction of browser width.
	float skew = 0.03f; // Right-edge slant of each row, as a fraction of browser width.
	float falloff = 0.05f; // Scale lost per row of distance.
	float fade = 0.14f; // Opacity lost per row of distance.
	std::optional<uint32_t> rowColor;
	bool numbers = true;
	bool favouriteMark = false; // Draw a star on rows of favourite games.
	std::vector<std::vector<std::string>> badges; // One tag per entry: the first of its fields with a value.
};
// Steps through the library after a period without player input.
struct Attract
{
	float idleSeconds = 0; // Zero disables attract mode.
	float intervalSeconds = 3.5f;
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
	Rail rail; // Used by rail mode only.
	std::string moveSound; // Validated package sound for moving to another game.
	std::string launchSound; // Validated package sound that opens a launch sequence.
	float launchSeconds = 0; // Delay between choosing a game and starting it; zero starts at once.
	Attract attract;
	PagedGrid pagedGrid;
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
