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
	bool isGame = true; // BIOS/device entries are excluded from play-history statistics.
	double playTimeSeconds = 0;
	int64_t lastPlayed = 0; // Unix seconds; zero means no recorded launch.
};
bool hasPlayed(const LibraryItem& item);
// Numeric bindings use raw play history, never parse localized display strings.
std::optional<double> libraryStat(const std::string& source, const std::vector<LibraryItem>& items,
		int selected, int pageSize, int64_t now);
// Media IDs follow LibraryCoverMediaMode; manuals and invalid values use the theme default.
std::optional<std::string> iconArtworkSource(int mediaMode);
// A game's rail tags, in manifest order. A tag with no value for this game is left out.
std::vector<std::string> railBadges(const Rail& rail, const LibraryItem& item);
// A metadata element's text: its field values joined, or its fallback when none has a value.
std::string metadataText(const Element& element, const LibraryItem& item);
struct DiscVertex { float x, y, u, v; };
// A ring showing an image cut to a circle and turned by angle. Each of the segments + 1 spokes
// holds its outer vertex, then its inner one. The texture turns, so the outline stays round.
std::vector<DiscVertex> discRing(float centerX, float centerY, float radius, float hole, float angle, int segments);
// Draws within the current ImGui content area. Returns activated item index or -1.
// afterItem runs immediately after each item's widget for existing context menus.
// stateLayoutId preserves selection across responsive layouts.
// iconScale also scales the cover/metadata column, which scrolls when needed.
int renderLibrary(const Layout& layout, const std::string& themeDirectory,
		const std::vector<LibraryItem>& items, const std::function<void(size_t)>& afterItem,
		const std::optional<std::string>& iconSource = std::nullopt, float iconScale = 1.f,
		const std::string& stateLayoutId = {},
		const std::function<void(size_t)>& toggleFavourite = {},
		const std::function<void(const std::string&)>& playSound = {}); // Plays a validated package sound.
}
