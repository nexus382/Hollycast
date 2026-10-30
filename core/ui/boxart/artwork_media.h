// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cfg/option.h"
#include <array>

namespace artwork {
struct MediaType
{
	const char *name;
	const char *label;
	config::LibraryCoverMediaMode mode;
};
// Persisted selection bits are shared by providers. Append new categories.
inline constexpr std::array<MediaType, 11> mediaTypes{{
	{"cover", "Cover", config::LibraryCoverMediaMode::Cover},
	{"miximage", "Mix Image", config::LibraryCoverMediaMode::MixImage},
	{"title", "Wheel / Marquee", config::LibraryCoverMediaMode::Title},
	{"banner", "Banner", config::LibraryCoverMediaMode::Banner},
	{"physical", "Physical Media", config::LibraryCoverMediaMode::Physical},
	{"fanart", "Fan Art", config::LibraryCoverMediaMode::FanArt},
	{"screenshot", "Screenshot", config::LibraryCoverMediaMode::Screenshot},
	{"titlescreen", "Title Screen", config::LibraryCoverMediaMode::TitleScreen},
	{"case", "Case / 3D Box", config::LibraryCoverMediaMode::Case},
	{"backcover", "Back Cover", config::LibraryCoverMediaMode::BackCover},
	{"spine", "Spine", config::LibraryCoverMediaMode::Spine},
}};
inline constexpr u32 allMedia = (1u << mediaTypes.size()) - 1;
inline constexpr u32 coverMedia = 1u;
}
