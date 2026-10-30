// Copyright 2026 The Hollycast Authors
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
}

TEST(ThemeArtworkTest, CompactLabelsKeepWholeUtf8CharactersAndDoNotAlterTitles)
{
	EXPECT_EQ(themes::titleInitials("Crazy Taxi 2"), "CT2");
	EXPECT_EQ(themes::titleInitials("Phantasy Star Online"), "PSO");
	EXPECT_EQ(themes::titleInitials("  Space--Channel 5"), "SC5");
	EXPECT_EQ(themes::titleInitials(u8"ソニック Adventure 2"), u8"ソA2");
	EXPECT_TRUE(themes::titleInitials("").empty());
}
