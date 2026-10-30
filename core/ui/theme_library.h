// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "theme.h"
#include <functional>

namespace themes
{
struct LibraryItem
{
	std::string id, title, image;
	std::map<std::string, std::string> metadata;
	bool favourite = false, canFavourite = false;
};
// Media IDs follow LibraryCoverMediaMode; manuals and invalid values use the theme default.
std::optional<std::string> iconArtworkSource(int mediaMode);
// Draws within the current ImGui content area. Returns activated item index or -1.
// afterItem runs immediately after each item's widget for existing context menus.
// stateLayoutId preserves selection across responsive layouts.
// iconScale also scales the cover/metadata column, which scrolls when needed.
int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem,
		const std::optional<std::string>& iconSource = std::nullopt, float iconScale = 1.f,
		const std::string& stateLayoutId = {},
		const std::function<void(size_t)>& toggleFavourite = {});
}
