// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
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
			|| style.resolution < 4 || style.resolution > 128 || style.levels < 2 || style.levels > 256)
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
			int luminance = (77 * source[0] + 150 * source[1] + 29 * source[2]) / 256;
			int alpha = source[3];
			if (style.levels > 4)
			{
				// Smooth LCD covers average the source footprint instead of picking
				// one pixel. Alpha weighting prevents transparent borders bleeding in.
				// Work is bounded by the decoded image size and cached after upload.
				const int left = x * width / result.width, top = y * height / result.height;
				const int right = std::max(left + 1, (x + 1) * width / result.width);
				const int bottom = std::max(top + 1, (y + 1) * height / result.height);
				uint64_t totalLuminance = 0, totalAlpha = 0;
				for (int sampleY = top; sampleY < bottom; ++sampleY)
					for (int sampleX = left; sampleX < right; ++sampleX)
					{
						const uint8_t* sample = rgba + (sampleY * width + sampleX) * 4;
						totalLuminance += static_cast<uint64_t>(77 * sample[0] + 150 * sample[1] + 29 * sample[2]) * sample[3];
						totalAlpha += sample[3];
					}
				luminance = totalAlpha == 0 ? 0 : static_cast<int>(totalLuminance / (totalAlpha * 256));
				alpha = static_cast<int>(totalAlpha / ((right - left) * (bottom - top)));
			}
			const int maximum = style.levels - 1;
			const int level = (luminance * maximum + 127) / 255;
			for (int component = 0; component < 4; ++component)
			{
				const int shift = (3 - component) * 8;
				const int dark = (style.dark >> shift) & 255;
				const int light = (style.light >> shift) & 255;
				target[component] = (dark * (maximum - level) + light * level) / maximum;
			}
			target[3] = target[3] * alpha / 255;
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
