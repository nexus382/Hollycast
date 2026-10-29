// Copyright 2026 The Hollycast Authors
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
};
// Draws within the current ImGui content area. Returns activated item index or -1.
// afterItem runs immediately after each item's widget for existing context menus.
int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem);
}
