// Portions Copyright 2026 The Hollycast Authors
#include "rend/custom_texture/TextureAtlas.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>

namespace
{
using namespace texture_atlas;

Image solid(uint32_t width, uint32_t height, uint8_t value)
{
	return { width, height, std::vector<uint8_t>(size_t(width) * height * 4, value) };
}

const Sheet TwoPieces { 0x123abc, 8, 4, { { 0, 0, 4, 4 }, { 4, 0, 4, 4 } } };

std::optional<Region> quadRegion(const std::array<std::array<float, 2>, 4>& uv, uint32_t width, uint32_t height)
{
	RegionDetector detector(width, height);
	detector.submitTriangle({uv[0], uv[1], uv[2]});
	return detector.submitTriangle({uv[1], uv[2], uv[3]});
}
}

TEST(TextureAtlas, MatchesComplementaryStandaloneTrianglesAcrossSubmissions)
{
	RegionDetector detector(256, 256);
	const std::array<std::array<float, 2>, 3> first {{{0.501953125f, 0.251953125f},
			{0.744140625f, 0.251953125f}, {0.744140625f, 0.494140625f}}};
	const std::array<std::array<float, 2>, 3> second {{{0.501953125f, 0.251953125f},
			{0.744140625f, 0.494140625f}, {0.501953125f, 0.494140625f}}};
	EXPECT_FALSE(detector.submitTriangle(first));
	EXPECT_FALSE(detector.submitTriangle(first));
	const auto region = detector.submitTriangle(second);
	ASSERT_TRUE(region);
	EXPECT_EQ(*region, (Region {128, 129, 63, 63}));
	EXPECT_FALSE(detector.submitTriangle(second));
}

TEST(TextureAtlas, RejectsClippedRectanglesAndInsetWholeImages)
{
	// This clipped corner comes directly from the Chronicles capture; it is not an authored edge.
	EXPECT_FALSE(quadRegion({{{0.501953125f, 0.251953125f}, {0.595392227f, 0.251953125f},
			{0.501953125f, 0.494140625f}, {0.595392227f, 0.494140625f}}}, 256, 256));
	EXPECT_FALSE(quadRegion({{{0.5f/256, 0.5f/256}, {254.5f/256, 0.5f/256},
			{0.5f/256, 254.5f/256}, {254.5f/256, 254.5f/256}}}, 256, 256));
}

TEST(TextureAtlas, CapturedChroniclesStripsRecoverGameplayPieces)
{
	std::ifstream input(FLYCAST_TEST_FILES "/texture-atlas/chronicles-uv.csv");
	ASSERT_TRUE(input.is_open());
	std::map<uint32_t, RegionDetector> detectors;
	std::map<uint32_t, Sheet> sheets;
	std::string line;
	while (std::getline(input, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::replace(line.begin(), line.end(), ',', ' ');
		std::istringstream row(line);
		row.imbue(std::locale::classic());
		uint32_t hash, width, height, count;
		int matrix, secondTexture;
		ASSERT_TRUE(bool(row >> std::hex >> hash >> std::dec >> width >> height >> count >> matrix >> secondTexture));
		ASSERT_EQ(count, 3u);
		std::array<std::array<float, 2>, 3> uv;
		for (auto& point : uv)
			ASSERT_TRUE(bool(row >> point[0] >> point[1]));
		auto& detector = detectors.try_emplace(hash, width, height).first->second;
		if (const auto region = detector.submitTriangle(uv))
		{
			auto& sheet = sheets.try_emplace(hash, Sheet {hash, width, height, {}}).first->second;
			addRegion(sheet, *region);
		}
	}
	ASSERT_EQ(sheets.size(), 2u);
	for (const auto& [hash, sheet] : sheets)
	{
		ASSERT_EQ(sheet.regions.size(), 3u) << std::hex << hash;
		for (const auto& region : sheet.regions)
		{
			EXPECT_EQ(region.width, 63u);
			EXPECT_EQ(region.height, 63u);
		}
	}
}

TEST(TextureAtlas, ExtractsRectangleWithImageCoordinates)
{
	const auto region = quadRegion({{{0, 0}, {0.25f, 0}, {0, 0.5f}, {0.25f, 0.5f}}}, 256, 128);
	ASSERT_TRUE(region);
	EXPECT_EQ(*region, (Region { 0, 64, 64, 64 }));
}

TEST(TextureAtlas, HalfTexelInsetsKeepCompleteSourcePixels)
{
	const auto region = quadRegion({{{0.5f/256, 0.5f/128}, {63.5f/256, 0.5f/128},
			{0.5f/256, 63.5f/128}, {63.5f/256, 63.5f/128}}}, 256, 128);
	ASSERT_TRUE(region);
	EXPECT_EQ(*region, (Region { 0, 64, 64, 64 }));
}

TEST(TextureAtlas, RejectsWholeSheetsWrappedAndInvalidCoordinates)
{
	EXPECT_FALSE(quadRegion({{{0, 0}, {1, 0}, {0, 1}, {1, 1}}}, 256, 256));
	EXPECT_FALSE(quadRegion({{{-0.1f, 0}, {0.5f, 0}, {-0.1f, 0.5f}, {0.5f, 0.5f}}}, 256, 256));
	EXPECT_FALSE(quadRegion({{{0, 0}, {1.1f, 0}, {0, 0.5f}, {1.1f, 0.5f}}}, 256, 256));
	EXPECT_FALSE(quadRegion({{{0, 0}, {0.5f, 0}, {0, 0.5f},
			{std::numeric_limits<float>::quiet_NaN(), 0.5f}}}, 256, 256));
}

TEST(TextureAtlas, RejectsIrregularAndDegenerateStripPairs)
{
	EXPECT_FALSE(quadRegion({{{0, 0}, {0.5f, 0}, {0.25f, 0.25f}, {0.5f, 0.5f}}}, 256, 256));
	EXPECT_FALSE(quadRegion({{{0, 0}, {0.5f, 0}, {0, 0}, {0.5f, 0.5f}}}, 256, 256));
	// Four corners alone are insufficient: these triangles share a side, not the diagonal.
	EXPECT_FALSE(quadRegion({{{0, 0}, {0.5f, 0}, {0.5f, 0.5f}, {0, 0.5f}}}, 256, 256));
}

TEST(TextureAtlas, RejectsOverlapsDuplicatesAndOutsideRegions)
{
	Sheet sheet { 1, 8, 8, {} };
	EXPECT_TRUE(addRegion(sheet, {0, 0, 4, 4}));
	EXPECT_FALSE(addRegion(sheet, {0, 0, 4, 4}));
	EXPECT_FALSE(addRegion(sheet, {2, 2, 4, 4}));
	EXPECT_FALSE(addRegion(sheet, {7, 7, 2, 2}));
	EXPECT_FALSE(addRegion(sheet, {UINT32_MAX, 0, 2, 2}));
	EXPECT_TRUE(addRegion(sheet, {4, 0, 4, 4}));
}

TEST(TextureAtlas, CompleteFacesReplaceContainedCutsWithoutMergingDistinctPieces)
{
	Sheet sheet {1, 256, 256, {{0, 209, 63, 47}}};
	EXPECT_TRUE(addRegion(sheet, {0, 193, 63, 63}));
	ASSERT_EQ(sheet.regions.size(), 1u);
	EXPECT_EQ(sheet.regions[0], (Region {0, 193, 63, 63}));
	EXPECT_FALSE(addRegion(sheet, {0, 209, 63, 47}));
	EXPECT_TRUE(addRegion(sheet, {64, 193, 63, 63}));
	EXPECT_FALSE(addRegion(sheet, {0, 192, 128, 64}));
	EXPECT_EQ(sheet.regions.size(), 2u);
}

TEST(TextureAtlas, MappingRoundTripPreservesDimensionsAndFilenames)
{
	const auto decoded = parseMapping(writeMapping({TwoPieces}));
	ASSERT_EQ(decoded.size(), 1u);
	EXPECT_EQ(decoded[0].hash, TwoPieces.hash);
	EXPECT_EQ(decoded[0].width, 8u);
	EXPECT_EQ(decoded[0].height, 4u);
	EXPECT_EQ(decoded[0].regions, TwoPieces.regions);
	EXPECT_EQ(imageFilename(TwoPieces.hash), "123abc.png");
	EXPECT_EQ(regionFilename(TwoPieces.hash, TwoPieces.regions[1]), "atlas_123abc_4_0_4_4.png");
}

TEST(TextureAtlas, FilenamesAreIndependentOfTheUsersLocale)
{
	struct GroupedNumbers : std::numpunct<char>
	{
		std::string do_grouping() const override { return "\3"; }
		char do_thousands_sep() const override { return ','; }
	};
	const auto originalLocale = std::locale::global(std::locale(std::locale::classic(), new GroupedNumbers));
	EXPECT_EQ(imageFilename(0x123abc), "123abc.png");
	EXPECT_EQ(regionFilename(0x123abc, {1024, 2048, 32, 64}), "atlas_123abc_1024_2048_32_64.png");
	std::locale::global(originalLocale);
}

TEST(TextureAtlas, RejectsMalformedUnsupportedAndOverlappingMappings)
{
	EXPECT_THROW(parseMapping("{"), std::exception);
	EXPECT_THROW(parseMapping(R"({"version":2,"sheets":[]})"), std::exception);
	EXPECT_THROW(parseMapping(R"({"version":1,"sheets":[{"hash":1,"width":8,"height":8,"regions":[[-1,0,4,4]]}]})"), std::exception);
	EXPECT_THROW(parseMapping(R"({"version":1,"sheets":[{"hash":1,"width":8,"height":8,"regions":[[0,0,4,4],[2,2,4,4]]}]})"), std::exception);
	EXPECT_THROW(parseMapping(R"({"version":1,"sheets":[{"hash":1,"width":8,"height":8,"regions":[[0,0,4,4],[0,0,8,8]]}]})"), std::exception);
	EXPECT_THROW(parseMapping(writeMapping({TwoPieces, TwoPieces})), std::exception);
}

TEST(TextureAtlas, ReassemblesPiecesAtTheirScaledPositions)
{
	const auto output = rebuild(TwoPieces, solid(8, 4, 1), {solid(16, 16, 2), solid(16, 16, 3)}, 1024, 1024);
	ASSERT_EQ(output.width, 32u);
	ASSERT_EQ(output.height, 16u);
	for (uint32_t y = 0; y < output.height; y++)
		for (uint32_t x = 0; x < output.width; x++)
			EXPECT_EQ(output.pixels[(size_t(y) * output.width + x) * 4], x < 16 ? 2 : 3);
}

TEST(TextureAtlas, KeepsMissingAndUnknownRegionsFromTheOriginal)
{
	const auto output = rebuild(TwoPieces, solid(8, 4, 7), {std::nullopt, solid(8, 8, 9)}, 1024, 1024);
	EXPECT_EQ(output.width, 16u);
	EXPECT_EQ(output.pixels[0], 7);
	EXPECT_EQ(output.pixels[8 * 4], 9);
	const auto original = rebuild(TwoPieces, solid(8, 4, 5), {std::nullopt, std::nullopt}, 1024, 1024);
	EXPECT_EQ(original.pixels, solid(8, 4, 5).pixels);
}

TEST(TextureAtlas, PreservesAsymmetricImageOrientation)
{
	const Sheet sheet { 3, 4, 4, { {0, 0, 2, 2}, {2, 2, 2, 2} } };
	const auto output = rebuild(sheet, solid(4, 4, 1), {solid(4, 4, 2), solid(4, 4, 3)}, 1024, 1024);
	EXPECT_EQ(output.pixels[0], 2);
	EXPECT_EQ(output.pixels[7 * 4], 1);
	EXPECT_EQ(output.pixels[(7 * 8) * 4], 1);
	EXPECT_EQ(output.pixels[(7 * 8 + 7) * 4], 3);
}

TEST(TextureAtlas, RejectsDifferentScalesAndChangedAspectRatios)
{
	EXPECT_THROW(rebuild(TwoPieces, solid(8, 4, 1), {solid(8, 8, 2), solid(16, 16, 3)}, 1024, 1024), std::exception);
	EXPECT_THROW(rebuild(TwoPieces, solid(8, 4, 1), {solid(8, 12, 2), std::nullopt}, 1024, 1024), std::exception);
}

TEST(TextureAtlas, AcceptsExactFractionalScaleWhenBoundariesAlign)
{
	const auto output = rebuild(TwoPieces, solid(8, 4, 1), {solid(6, 6, 2), solid(6, 6, 3)}, 1024, 1024);
	EXPECT_EQ(output.width, 12u);
	EXPECT_EQ(output.height, 6u);
	const Sheet misaligned { 1, 8, 4, {{1, 0, 4, 4}} };
	EXPECT_THROW(rebuild(misaligned, solid(8, 4, 1), {solid(6, 6, 2)}, 1024, 1024), std::exception);
}

TEST(TextureAtlas, RejectsWrongOriginalAndRendererOverflow)
{
	EXPECT_THROW(rebuild(TwoPieces, solid(16, 8, 1), {std::nullopt, std::nullopt}, 1024, 1024), std::exception);
	EXPECT_THROW(rebuild(TwoPieces, solid(8, 4, 1), {solid(16, 16, 2), std::nullopt}, 16, 16), std::exception);
	EXPECT_THROW(rebuild(TwoPieces, solid(8, 4, 1), {}, 1024, 1024), std::exception);
}
