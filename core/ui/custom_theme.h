// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "imgui.h"
#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace CustomTheme {

// Parse completely before modifying a style, so a malformed file cannot leave
// the interface with a partially applied palette.
struct Theme {
	struct Assets {
		std::string directory;
		std::string bodyFont;
		std::string boldFont;
		std::string background;
		std::map<std::string, std::string> icons;
		float backgroundOpacity = 0.22f;
		std::string backgroundFit = "cover";
	};
	struct PaletteLayer {
		bool hasAccent = false;
		ImVec4 accent;
		std::vector<std::pair<int, ImVec4>> colors;
		std::map<std::string, ImVec4> roles;
		std::map<std::string, std::map<std::string, ImVec4>> widgetRoles;
	};
	struct Layout {
		float density = 1.0f;
		float settingsSidebarWidth = 0.0f;
		float libraryCoverSpacing = -1.0f;
		float artworkCornerRadius = -1.0f;
		bool hasDensity = false;
		bool hasSettingsSidebarWidth = false;
		bool hasLibraryCoverSpacing = false;
		bool hasArtworkCornerRadius = false;
	};
	int base = 0;
	std::vector<std::pair<int, ImVec4>> colors;
	std::vector<std::pair<float ImGuiStyle::*, float>> scalars;
	std::vector<std::pair<ImVec2 ImGuiStyle::*, ImVec2>> vectors;
	std::map<std::string, ImVec4> roles;
	std::map<std::string, std::map<std::string, ImVec4>> widgetRoles;
	std::vector<PaletteLayer> paletteLayers;
	Assets assets;
	Layout layout;
	// Unknown top-level extension fields are retained for forward-compatible edits.
	std::string preservedFieldsJson;
};

struct Builtin {
	int id;
	const char* name;
	const char* filename;
	const char* resource;
};

constexpr int CustomThemeId = 6;
const std::array<Builtin, CustomThemeId>& builtins();
std::string builtinDirectory();
bool seedBuiltins(const std::string& path);
bool loadBundled(int id, Theme& theme, std::string& error);
// Migrates a legacy editable built-in overlay into a new custom theme without
// changing or removing the legacy file. Returns true only when migration succeeds.
bool migrateLegacyBuiltin(const std::string& path, int id, std::string& filename, std::string& error,
		bool recordCompletion = true);
bool markLegacyBuiltinMigrationComplete(const std::string& path, int id,
		const std::string& filename, std::string& error);
bool readPendingBuiltinMigration(const std::string& path, int& id,
		std::string& filename, std::string& error);
bool writePendingBuiltinMigration(const std::string& path, int id,
		const std::string& filename, std::string& error);
bool clearPendingBuiltinMigration(const std::string& path, std::string& error);
// Compatibility entry point; built-in presets always load from bundled resources.
bool loadBuiltinFromDirectory(const std::string& path, int id, Theme& theme, std::string& error);
void apply(const Theme& theme, ImGuiStyle& style);
// Style-only variant for editor previews; it never mutates active semantic roles.
void applyStyle(const Theme& theme, ImGuiStyle& style);
// Call once before layering the builtin base and selected theme overrides.
void resetRoles();
ImVec4 roleColor(const std::string& role, const ImVec4& fallback,
		const std::string& widget = std::string());
void setActiveAssets(const Theme::Assets& assets);
std::string assetPath(const std::string& name);
std::vector<std::string> assetPaths();
std::string fontPath(bool bold);
float backgroundOpacity();
std::string backgroundFit();
void setActiveLayout(const Theme::Layout& layout);
const Theme::Layout& activeLayout();

bool parse(const std::string& contents, Theme& theme, std::string& error);
// Serialize every supported theme field. Editing a loaded Theme and serializing
// it keeps all untouched colors, styles, semantic roles, layout and asset refs.
std::string serialize(const Theme& theme, std::string& error);
bool load(const std::string& filename, Theme& theme, std::string& error);
bool loadFromDirectory(const std::string& path, const std::string& filename, Theme& theme, std::string& error);
// Store a custom theme and its referenced assets. Saving refuses to replace an
// existing file unless replaceExisting is explicitly true.
bool save(const std::string& filename, const Theme& theme, bool replaceExisting, std::string& error);
// Import a theme JSON as a uniquely named custom theme. Referenced files are
// copied under assets/<theme-name>/ and rewritten to portable relative paths.
bool importFile(const std::string& source, std::string& importedFilename, std::string& error);
// Directory-targeted form used by portable import flows and tests.
bool importFromFile(const std::string& source, const std::string& destination,
		std::string& importedFilename, std::string& error);
// Export JSON and all referenced assets into a destination directory.
bool exportToDirectory(const Theme& theme, const std::string& filename,
		const std::string& destination, std::string& error);
std::vector<std::string> list();
std::vector<std::string> list(const std::string& path);
std::string directory();

}
