// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "theme.h"

namespace themes
{
struct PixelArtwork
{
	int width = 0, height = 0;
	std::vector<uint8_t> pixels;
};
// Tint bounded RGBA artwork between the LCD palette endpoints without modifying it.
// Four tones retain pixel sampling; higher counts use averaged pixels and linear sampling.
PixelArtwork makePixelArtwork(const uint8_t* rgba, int width, int height, const PixelArt& style);
std::string titleInitials(const std::string& title);
}
