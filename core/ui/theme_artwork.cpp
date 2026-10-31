// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "theme_artwork.h"

#include <algorithm>
#include <cctype>

namespace themes
{
PixelArtwork makePixelArtwork(const uint8_t* rgba, int width, int height, const PixelArt& style)
{
	PixelArtwork result;
	if (!rgba || width <= 0 || height <= 0 || width > 4096 || height > 4096
			|| style.resolution < 4 || style.resolution > 128)
		return result;
	const int largest = std::max(width, height);
	result.width = std::max(1, width * style.resolution / largest);
	result.height = std::max(1, height * style.resolution / largest);
	result.pixels.resize(result.width * result.height * 4);
	for (int y = 0; y < result.height; ++y)
		for (int x = 0; x < result.width; ++x)
		{
			const int sx = std::min(width - 1, (2 * x + 1) * width / (2 * result.width));
			const int sy = std::min(height - 1, (2 * y + 1) * height / (2 * result.height));
			const uint8_t* source = rgba + (sy * width + sx) * 4;
			uint8_t* target = result.pixels.data() + (y * result.width + x) * 4;
			const int luminance = (77 * source[0] + 150 * source[1] + 29 * source[2]) / 256;
			const int level = std::min(3, (luminance + 42) / 85);
			for (int component = 0; component < 4; ++component)
			{
				const int shift = (3 - component) * 8;
				const int dark = (style.dark >> shift) & 255;
				const int light = (style.light >> shift) & 255;
				target[component] = (dark * (3 - level) + light * level) / 3;
			}
			target[3] = target[3] * source[3] / 255;
		}
	return result;
}

std::string titleInitials(const std::string& title)
{
	std::string result;
	bool start = true;
	for (size_t i = 0; i < title.size() && result.size() < 24;)
	{
		const unsigned char c = title[i];
		if (c < 128 && !std::isalnum(c))
		{
			start = true;
			++i;
			continue;
		}
		const size_t bytes = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
		if (start)
		{
			if (c < 128) result += static_cast<char>(std::toupper(c));
			else result.append(title, i, std::min(bytes, title.size() - i));
		}
		start = false;
		i += bytes;
	}
	return result;
}
}
