// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "cfg/option.h"
#include <array>
#include <string_view>

namespace artwork {
struct MediaType
{
	const char *name;
	const char *label;
	config::LibraryCoverMediaMode mode;
};
// Persisted selection bits are shared by providers. Append new categories.
inline constexpr std::array<MediaType, 12> mediaTypes{{
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
	{"boxtexture", "Box Texture", config::LibraryCoverMediaMode::BoxTexture},
}};
inline constexpr u32 allMedia = (1u << mediaTypes.size()) - 1;
inline constexpr u32 coverMedia = 1u;
// A job flag, not an image category: metadata lookups must not force a cover download.
inline constexpr u32 metadataMedia = 1u << 30;
// Basic icons and hover details consume only covers, miximages, title logos and
// metadata. Optional media browsers must not expand automatic download requirements.
inline constexpr u32 basicMedia = coverMedia | (1u << 1) | (1u << 2) | metadataMedia;
// The single name-to-bit mapping for requirements, provider queues and future media types.
inline u32 mediaBit(std::string_view source)
{
	if (source.empty() || source == "default") return coverMedia;
	if (source == "disc") source = "physical";
	for (size_t i = 0; i < mediaTypes.size(); ++i)
		if (source == mediaTypes[i].name) return 1u << i;
	return 0;
}
}
