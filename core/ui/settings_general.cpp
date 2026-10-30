/*
	Copyright 2025 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "settings.h"
#include "gui.h"
#include "settings_new.h"
#include "custom_theme.h"
#include "gui_menu.h"
#include "oslib/storage.h"
#include "imgui_driver.h"
#include "theme.h"
#include "gui_util.h"
#include <map>

namespace {

CustomTheme::Theme previewTheme;
bool hasPreviewTheme = false;
bool initialThemeMigrationAttempted = false;

void addContentPathCallback(const std::string& path)
{
	auto& contentPath = config::ContentPath.get();
	if (std::count(contentPath.begin(), contentPath.end(), path) == 0)
	{
		scanner.stop();
		contentPath.push_back(path);
		if (gui_state == GuiState::Main)
			SaveSettings();
		gui_refresh_files();
	}
}

} // namespace

void addContentPath(bool start)
{
	const char* title = "Select a Content Folder";
	select_file_popup(title, [](bool cancelled, std::string selection) {
		if (!cancelled)
			addContentPathCallback(selection);
		return true;
	});
#ifdef __ANDROID__
	if (start)
	{
		bool supported = hostfs::addStorage(true, false, title, [](bool cancelled, std::string selection) {
			if (!cancelled)
				addContentPathCallback(selection);
		});
		if (!supported)
			ImGui::OpenPopup(title);
	}
#else
	if (start)
		ImGui::OpenPopup(title);
#endif
}

// Legacy compatibility entry point. The active General settings tab is implemented
// in settings_new.cpp; this symbol is preserved for compatibility and merge stability.
void gui_settings_general()
{
	SettingsNew::renderGeneralTab();
}

void applyCurrentTheme()
{
	if (imguiDriver)
		for (const std::string& path : CustomTheme::assetPaths())
			imguiDriver->deleteTexture(path);
	CustomTheme::resetRoles();
	ImGuiStyle& style = ImGui::GetStyle();
	style = ImGuiStyle{};
#if defined(__ANDROID__) || defined(TARGET_IPHONE) || defined(__SWITCH__)
	style.TouchExtraPadding = ImVec2(1, 1);
#endif
	if (!hasPreviewTheme && !initialThemeMigrationAttempted) {
		initialThemeMigrationAttempted = true;
		const std::string themeDirectory = CustomTheme::directory();
		const int initiallySelectedTheme = config::UITheme.get();
		std::string migrationError;
		int pendingId = -1;
		std::string pendingFilename;
		if (CustomTheme::readPendingBuiltinMigration(themeDirectory, pendingId, pendingFilename, migrationError)) {
			if (config::UITheme.get() == pendingId) {
				// The previous process may have exited before ordinary settings persistence.
				// Reapply the custom appearance in memory and keep the journal for another launch.
				config::UICustomTheme = pendingFilename;
				config::UITheme = CustomTheme::CustomThemeId;
			} else if (config::UITheme.get() == CustomTheme::CustomThemeId
					&& config::UICustomTheme.get() == pendingFilename) {
				std::string completionError;
				if (CustomTheme::markLegacyBuiltinMigrationComplete(themeDirectory, pendingId, pendingFilename, completionError)) {
					if (!CustomTheme::clearPendingBuiltinMigration(themeDirectory, completionError))
						WARN_LOG(COMMON, "Cannot clear pending UI theme migration: %s", completionError.c_str());
				} else {
					WARN_LOG(COMMON, "Cannot finish UI theme migration: %s", completionError.c_str());
				}
			} else {
				std::string completionError;
				if (CustomTheme::markLegacyBuiltinMigrationComplete(themeDirectory, pendingId, pendingFilename, completionError)) {
					if (!CustomTheme::clearPendingBuiltinMigration(themeDirectory, completionError))
						WARN_LOG(COMMON, "Cannot clear abandoned UI theme migration: %s", completionError.c_str());
				} else {
					WARN_LOG(COMMON, "Cannot finish abandoned UI theme migration: %s", completionError.c_str());
				}
			}
		} else if (!migrationError.empty()) {
			WARN_LOG(COMMON, "Cannot read pending UI theme migration: %s", migrationError.c_str());
		}

		std::map<int, std::string> migratedThemes;
		for (const auto& entry : CustomTheme::builtins()) {
			std::string filename, error;
			if (CustomTheme::migrateLegacyBuiltin(themeDirectory, entry.id, filename, error, false))
				migratedThemes.emplace(entry.id, std::move(filename));
			else if (!error.empty())
				WARN_LOG(COMMON, "Cannot migrate legacy UI theme '%s': %s", entry.name, error.c_str());
		}

		auto selectedMigration = migratedThemes.find(initiallySelectedTheme);
		if (selectedMigration != migratedThemes.end()) {
			if (CustomTheme::writePendingBuiltinMigration(themeDirectory, initiallySelectedTheme,
					selectedMigration->second, migrationError)) {
				config::UICustomTheme = selectedMigration->second;
				config::UITheme = CustomTheme::CustomThemeId;
			} else {
				WARN_LOG(COMMON, "Cannot protect migrated UI theme selection: %s", migrationError.c_str());
			}
		}
		for (const auto& migration : migratedThemes) {
			// Keep the active preset retryable until a later startup observes the
			// saved custom selection or the UI explicitly finalizes its journal.
			if (migration.first == initiallySelectedTheme) continue;
			std::string error;
			if (!CustomTheme::markLegacyBuiltinMigrationComplete(themeDirectory, migration.first, migration.second, error))
				WARN_LOG(COMMON, "Cannot finish UI theme migration '%s': %s", migration.second.c_str(), error.c_str());
		}
	}
	CustomTheme::Theme theme;
	std::string error;
	if (hasPreviewTheme)
	{
		theme = previewTheme;
		CustomTheme::Theme base;
		if (CustomTheme::loadBundled(theme.base, base, error))
			CustomTheme::apply(base, style);
		CustomTheme::apply(theme, style);
		CustomTheme::setActiveAssets(theme.assets);
		if (!theme.layout.hasDensity) theme.layout.density = base.layout.density;
		if (!theme.layout.hasSettingsSidebarWidth) theme.layout.settingsSidebarWidth = base.layout.settingsSidebarWidth;
		if (!theme.layout.hasLibraryCoverSpacing) theme.layout.libraryCoverSpacing = base.layout.libraryCoverSpacing;
		if (!theme.layout.hasArtworkCornerRadius) theme.layout.artworkCornerRadius = base.layout.artworkCornerRadius;
		CustomTheme::setActiveLayout(theme.layout);
	}
	else if (config::UITheme == CustomTheme::CustomThemeId && CustomTheme::load(config::UICustomTheme.get(), theme, error))
	{
		CustomTheme::Theme base;
		if (CustomTheme::loadBundled(theme.base, base, error))
			CustomTheme::apply(base, style);
		else
			WARN_LOG(COMMON, "Cannot load bundled UI theme: %s", error.c_str());
		CustomTheme::apply(theme, style);
		CustomTheme::setActiveAssets(theme.assets);
	if (!theme.layout.hasDensity) theme.layout.density = base.layout.density;
	if (!theme.layout.hasSettingsSidebarWidth) theme.layout.settingsSidebarWidth = base.layout.settingsSidebarWidth;
	if (!theme.layout.hasLibraryCoverSpacing) theme.layout.libraryCoverSpacing = base.layout.libraryCoverSpacing;
	if (!theme.layout.hasArtworkCornerRadius) theme.layout.artworkCornerRadius = base.layout.artworkCornerRadius;
		CustomTheme::setActiveLayout(theme.layout);
	}
	else
	{
		const bool custom = config::UITheme == CustomTheme::CustomThemeId;
		if (custom)
			WARN_LOG(COMMON, "Cannot load custom UI theme '%s': %s", config::UICustomTheme.get().c_str(), error.c_str());
		const bool loaded = custom ? CustomTheme::loadBundled(0, theme, error)
				: CustomTheme::loadBuiltinFromDirectory(CustomTheme::directory(), config::UITheme.get(), theme, error);
		if (loaded)
			CustomTheme::apply(theme, style);
		CustomTheme::setActiveAssets(loaded ? theme.assets : CustomTheme::Theme::Assets{});
		CustomTheme::setActiveLayout(loaded ? theme.layout : CustomTheme::Theme::Layout{});
		if (!error.empty())
			WARN_LOG(COMMON, "UI theme fallback: %s", error.c_str());
	}
	// An installed library package supplies the baseline palette for its own colors.
	// A live UI-theme editor preview takes precedence until its popup is closed.
	if (!hasPreviewTheme)
	{
		if (const themes::Theme* libraryTheme = themes::current())
		{
			CustomTheme::Theme base;
			std::string baseError;
			if (CustomTheme::loadBundled(libraryTheme->baseTheme, base, baseError))
				CustomTheme::applyStyle(base, style);
			else
				WARN_LOG(COMMON, "Cannot load library theme base palette: %s", baseError.c_str());
			themes::applyPalette();
		}
	}
	// Preserve the original metrics; undoing a scaled style loses pixel precision.
	LibraryUiScope::setBaseStyle(style);
#if defined(__ANDROID__)
	GuiMenu::setAndroidMenuStyle(ImGui::GetStyle(), settings.display.uiScale);
#endif
	if (settings.display.uiScale != 1.f)
		ImGui::GetStyle().ScaleAllSizes(settings.display.uiScale);
}

void setThemePreview(const CustomTheme::Theme* theme)
{
	hasPreviewTheme = false;
	if (theme == nullptr) return;
	std::string error;
	const std::string encoded = CustomTheme::serialize(*theme, error);
	if (error.empty() && CustomTheme::parse(encoded, previewTheme, error)) {
		previewTheme.assets.directory = theme->assets.directory;
		hasPreviewTheme = true;
	}
}
