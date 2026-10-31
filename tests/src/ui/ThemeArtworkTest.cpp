// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "ui/theme_artwork.h"
#include <algorithm>

TEST(ThemeArtworkTest, QuantizesWithoutMutatingSourceAndPreservesAspectAndTransparency)
{
	std::vector<uint8_t> source(8 * 4 * 4, 255);
	for (size_t i = 0; i < source.size(); i += 4) { source[i] = 0; source[i + 1] = 0; source[i + 2] = 0; }
	source[3] = 0;
	const auto original = source;
	themes::PixelArt style;
	style.resolution = 8;
	style.dark = 0x102030ff;
	style.light = 0xaabbccff;
	const auto image = themes::makePixelArtwork(source.data(), 8, 4, style);
	ASSERT_EQ(image.width, 8);
	ASSERT_EQ(image.height, 4);
	ASSERT_EQ(image.pixels.size(), source.size());
	EXPECT_EQ(image.pixels[0], 0x10);
	EXPECT_EQ(image.pixels[1], 0x20);
	EXPECT_EQ(image.pixels[2], 0x30);
	EXPECT_EQ(image.pixels[3], 0);
	EXPECT_EQ(image.pixels[7], 255);
	EXPECT_EQ(source, original);
	std::fill(source.begin(), source.end(), 255);
	const auto white = themes::makePixelArtwork(source.data(), 8, 4, style);
	EXPECT_EQ(white.pixels[0], 0xaa);
	EXPECT_EQ(white.pixels[1], 0xbb);
	EXPECT_EQ(white.pixels[2], 0xcc);
}

TEST(ThemeArtworkTest, DisabledOrInvalidEffectsNeverAllocateImages)
{
	const uint8_t pixel[] = {255, 255, 255, 255};
	themes::PixelArt style;
	EXPECT_TRUE(themes::makePixelArtwork(pixel, 1, 1, style).pixels.empty());
	style.resolution = 24;
	EXPECT_TRUE(themes::makePixelArtwork(nullptr, 1, 1, style).pixels.empty());
	EXPECT_TRUE(themes::makePixelArtwork(pixel, 0, 1, style).pixels.empty());
	EXPECT_TRUE(themes::makePixelArtwork(pixel, 4097, 1, style).pixels.empty());
	style.resolution = 129;
	EXPECT_TRUE(themes::makePixelArtwork(pixel, 1, 1, style).pixels.empty());
	style.resolution = 4;
	for (int levels : {1, 257})
	{
		style.levels = levels;
		EXPECT_TRUE(themes::makePixelArtwork(pixel, 1, 1, style).pixels.empty());
	}
}

TEST(ThemeArtworkTest, SmoothLcdPreservesCoverGradientsAndSourcePixels)
{
	std::vector<uint8_t> source(128 * 4, 255);
	for (int x = 0; x < 128; ++x)
		for (int channel = 0; channel < 3; ++channel) source[x * 4 + channel] = x * 2;
	const auto original = source;
	themes::PixelArt style;
	style.resolution = 128;
	style.levels = 32;
	style.dark = 0x26392dff;
	style.light = 0xa1b88fff;
	const auto image = themes::makePixelArtwork(source.data(), 128, 1, style);
	ASSERT_EQ(image.width, 128);
	ASSERT_EQ(image.height, 1);
	int distinct = 1;
	for (int x = 0; x < image.width; ++x)
	{
		const auto* pixel = image.pixels.data() + x * 4;
		EXPECT_GT(pixel[1], pixel[0]);
		EXPECT_GT(pixel[1], pixel[2]);
		EXPECT_EQ(pixel[3], 255);
		if (x > 0 && pixel[0] != pixel[-4]) ++distinct;
	}
	EXPECT_EQ(distinct, 32);
	EXPECT_EQ(source, original);
}

TEST(ThemeArtworkTest, SmoothLcdDownsamplingAveragesDetailWithoutTransparentColorBleed)
{
	std::vector<uint8_t> source(8 * 8 * 4, 255);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			for (int channel = 0; channel < 3; ++channel) source[(y * 8 + x) * 4 + channel] = (x + y) % 2 ? 254 : 0;
	themes::PixelArt style;
	style.resolution = 4;
	style.levels = 256;
	style.dark = 0x000000ff;
	style.light = 0xffffffff;
	const auto averaged = themes::makePixelArtwork(source.data(), 8, 8, style);
	ASSERT_EQ(averaged.pixels.size(), 4u * 4 * 4);
	for (size_t i = 0; i < averaged.pixels.size(); i += 4) EXPECT_EQ(averaged.pixels[i], 127);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			if ((x + y) % 2) source[(y * 8 + x) * 4 + 3] = 0;
	const auto transparent = themes::makePixelArtwork(source.data(), 8, 8, style);
	for (size_t i = 0; i < transparent.pixels.size(); i += 4)
	{
		EXPECT_EQ(transparent.pixels[i], 0);
		EXPECT_EQ(transparent.pixels[i + 3], 127);
	}
}

TEST(ThemeArtworkTest, CompactLabelsKeepWholeUtf8CharactersAndDoNotAlterTitles)
{
	EXPECT_EQ(themes::titleInitials("Crazy Taxi 2"), "CT2");
	EXPECT_EQ(themes::titleInitials("Phantasy Star Online"), "PSO");
	EXPECT_EQ(themes::titleInitials("  Space--Channel 5"), "SC5");
	EXPECT_EQ(themes::titleInitials(u8"ソニック Adventure 2"), u8"ソA2");
	EXPECT_TRUE(themes::titleInitials("").empty());
}
