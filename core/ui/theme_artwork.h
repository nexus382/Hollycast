// Copyright 2026 The Hollycast Authors
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
// Quantize a bounded RGBA image into a small, four-tone palette texture.
// The caller uploads with nearest-neighbor sampling; source pixels are never modified.
PixelArtwork makePixelArtwork(const uint8_t* rgba, int width, int height, const PixelArt& style);
std::string titleInitials(const std::string& title);
}
