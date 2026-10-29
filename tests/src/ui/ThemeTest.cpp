// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gtest/gtest.h"
#include "ui/theme.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
namespace fs = std::filesystem;

class TempDirectory
{
public:
	TempDirectory()
	{
		static std::atomic<unsigned long long> sequence{0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::temp_directory_path() /
			("hollycast-theme-test-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
		fs::create_directories(path);
	}
	~TempDirectory()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	fs::path path;
};

const std::string kManifest = R"json({
  "formatVersion": 1,
  "id": "test-theme",
  "name": "Test Theme",
  "author": "Hollycast Tests",
  "description": "A parser fixture",
  "baseTheme": 2,
  "colors": {"Text": "112233", "WindowBg": "01020380"},
  "layouts": [{
    "id": "main-grid",
    "name": "Main Grid",
    "mode": "grid",
    "browser": [0.1, 0.2, 0.8, 0.7],
    "columns": 5,
    "itemAspect": 0.75,
    "spacing": 8,
    "showTitles": false,
    "background": "AABBCC",
    "elements": [
      {"type": "panel", "rect": [0, 0, 1, 1], "color": "102030"},
      {"type": "image", "rect": [0.1, 0.1, 0.2, 0.2], "source": "art/overlay.png"},
      {"type": "artwork", "rect": [0.2, 0.2, 0.3, 0.4], "source": "cover"},
      {"type": "text", "rect": [0.1, 0.1, 0.4, 0.1], "text": "Hello", "fontSize": 0.05, "zIndex": 4},
      {"type": "metadata", "rect": [0.1, 0.3, 0.4, 0.1], "source": "developer"}
    ]
  }, {
    "id": "compact-list", "name": "Compact List", "mode": "list", "browser": [0, 0, 1, 1]
  }]
})json";

std::string replaceOnce(std::string text, const std::string& from, const std::string& to)
{
	const auto pos = text.find(from);
	EXPECT_NE(pos, std::string::npos) << "Fixture token missing: " << from;
	if (pos != std::string::npos)
		text.replace(pos, from.size(), to);
	return text;
}

themes::Theme parseFixture(const std::string& json = kManifest, const fs::path& directory = fs::temp_directory_path())
{
	return themes::parse(json, directory.u8string());
}

void expectRejected(const std::string& json, const std::string& diagnostic)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	try
	{
		parseFixture(json, package.path);
		FAIL() << "Expected invalid theme to be rejected";
	}
	catch (const std::runtime_error& error)
	{
		if (!diagnostic.empty())
			EXPECT_NE(std::string(error.what()).find(diagnostic), std::string::npos) << error.what();
	}
}
} // namespace

TEST(ThemeParserTest, ParsesLayoutsElementsAndPalette)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";

	const themes::Theme theme = parseFixture(kManifest, package.path);
	ASSERT_EQ(theme.id, "test-theme");
	EXPECT_EQ(theme.name, "Test Theme");
	EXPECT_EQ(theme.author, "Hollycast Tests");
	EXPECT_EQ(theme.description, "A parser fixture");
	EXPECT_EQ(theme.baseTheme, 2);
	EXPECT_EQ(theme.colors.at("Text"), 0x112233ff);
	EXPECT_EQ(theme.colors.at("WindowBg"), 0x01020380);
	ASSERT_EQ(theme.layouts.size(), 2u);
	const themes::Layout& grid = theme.layouts[0];
	EXPECT_EQ(grid.mode, "grid");
	EXPECT_FLOAT_EQ(grid.browser.x, 0.1f);
	EXPECT_EQ(grid.columns, 5);
	EXPECT_FLOAT_EQ(grid.itemAspect, 0.75f);
	EXPECT_FLOAT_EQ(grid.spacing, 8.0f);
	EXPECT_FALSE(grid.showTitles);
	EXPECT_EQ(grid.background, 0xaabbccff);
	ASSERT_EQ(grid.elements.size(), 5u);
	EXPECT_EQ(grid.elements[0].type, "panel");
	EXPECT_EQ(grid.elements[1].source, "art/overlay.png");
	EXPECT_EQ(grid.elements[2].source, "cover");
	EXPECT_EQ(grid.elements[3].text, "Hello");
	EXPECT_EQ(grid.elements[3].zIndex, 4);
	EXPECT_EQ(grid.elements[4].source, "developer");
	EXPECT_EQ(theme.layouts[1].mode, "list");
}

TEST(ThemeParserTest, AcceptsMiximageArtworkBinding)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	const auto theme = parseFixture(replaceOnce(kManifest, "\"source\": \"cover\"", "\"source\": \"miximage\""), package.path);
	EXPECT_EQ(theme.layouts[0].elements[2].source, "miximage");
}

TEST(ThemeParserTest, RejectsUnsupportedVersionUnknownFieldsAndInvalidNumbers)
{
	expectRejected(replaceOnce(kManifest, "\"formatVersion\": 1", "\"formatVersion\": 2"), "must be between 1 and 1");
	expectRejected(replaceOnce(kManifest, "\"name\": \"Test Theme\"", "\"unexpected\": true, \"name\": \"Test Theme\""), "unknown field");
	expectRejected(replaceOnce(kManifest, "\"columns\": 5", "\"columns\": 5.0"), "must be an integer");
	expectRejected(replaceOnce(kManifest, "\"columns\": 5", "\"columns\": 13"), "must be between 1 and 12");
	expectRejected(replaceOnce(kManifest, "[0.1, 0.2, 0.8, 0.7]", "[0.1, 0.2, 0.8, 0.9]"), "normalized bounds");
	expectRejected(replaceOnce(kManifest, "\"showTitles\": false", "\"showTitles\": \"false\""), "must be a boolean");
	expectRejected(replaceOnce(kManifest, "\"zIndex\": 4", "\"zIndex\": 18446744073709551615"), "");
	expectRejected(replaceOnce(kManifest, "\"Text\": \"112233\"", "\"NotAnImGuiColor\": \"112233\""), "unknown ImGui color name");
	expectRejected(replaceOnce(kManifest, "\"Text\": \"112233\"", "\"Text\": \"12xz33\""), "hexadecimal digits");
}

TEST(ThemeParserTest, AcceptsRectanglesEndingAtNormalizedBoundary)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	const auto theme = parseFixture(replaceOnce(kManifest, "[0.1, 0.2, 0.8, 0.7]", "[0.2, 0.2, 0.8, 0.8]"), package.path);
	ASSERT_FALSE(theme.layouts.empty());
	EXPECT_FLOAT_EQ(theme.layouts.front().browser.x + theme.layouts.front().browser.w, 1.0f);
	EXPECT_FLOAT_EQ(theme.layouts.front().browser.y + theme.layouts.front().browser.h, 1.0f);
}

TEST(ThemeParserTest, ParsesPerLayoutFontRowAndSelectorColorsAndElementPresentation)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	std::string json = kManifest;
	json = replaceOnce(json, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"fontSize\": 0.034, \"rowHeight\": 0.058, \"textColor\": \"abcdef\", \"selectedTextColor\": \"01020380\", \"selectorColor\": \"334455\",");
	json = replaceOnce(json, "\"source\": \"art/overlay.png\"", "\"source\": \"art/overlay.png\", \"fit\": \"cover\", \"align\": \"right\", \"color\": \"abcdef80\"");
	const themes::Theme theme = parseFixture(json, package.path);
	const auto& layout = theme.layouts.front();
	EXPECT_FLOAT_EQ(layout.fontSize, 0.034f);
	EXPECT_FLOAT_EQ(layout.rowHeight, 0.058f);
	ASSERT_TRUE(layout.textColor.has_value());
	EXPECT_EQ(*layout.textColor, 0xabcdefff);
	ASSERT_TRUE(layout.selectedTextColor.has_value());
	EXPECT_EQ(*layout.selectedTextColor, 0x01020380);
	ASSERT_TRUE(layout.selectorColor.has_value());
	EXPECT_EQ(*layout.selectorColor, 0x334455ff);
	EXPECT_EQ(layout.elements[1].fit, "cover");
	EXPECT_EQ(layout.elements[1].align, "right");
	EXPECT_EQ(layout.elements[1].color, 0xabcdef80);
	EXPECT_EQ(fs::weakly_canonical(layout.elements[1].resolvedAsset),
			fs::weakly_canonical(package.path / "art" / "overlay.png"));
}

TEST(ThemeParserTest, RejectsInvalidLayoutTypographyColorsAndElementPresentation)
{
	expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"fontSize\": 0.009,"), "fontSize");
	expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"fontSize\": 0.081,"), "fontSize");
	expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"rowHeight\": 0.019,"), "rowHeight");
	expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"rowHeight\": 0.201,"), "rowHeight");
	expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\", \"selectorColor\": \"not-hex\","), "hexadecimal");
	expectRejected(replaceOnce(kManifest, "\"source\": \"art/overlay.png\"", "\"source\": \"art/overlay.png\", \"fit\": \"tile\""), "must be contain, cover, or stretch");
	expectRejected(replaceOnce(kManifest, "\"source\": \"art/overlay.png\"", "\"source\": \"art/overlay.png\", \"align\": \"justify\""), "must be left, center, or right");
}

TEST(ThemeParserTest, ParsesMediaPresentationAndPreservesLegacyDefaults)
{
	const auto theme = themes::parse(R"({"formatVersion":1,"id":"preview","name":"Preview *","layouts":[
		{"id":"grid","name":"Grid","mode":"grid","browser":[0,0,1,1],"artworkSource":"cover",
		"rounding":12,"inactiveOpacity":0.65,"selectorBorderColor":"abcdef","separatorColor":"12345680",
		"showArtwork":true,"compactControls":true,"showInfoTooltip":false,
		"elements":[{"type":"panel","rect":[0,0,1,1],"color":"10203000","endColor":"102030ff","gradient":"horizontal","rounding":14},
		{"type":"artwork","source":"physical","fallback":"none","requiresArtwork":"screenshot","rect":[0,0,0.2,0.2]},
		{"type":"metadata","source":"title","rect":[0,0,1,0.2],"bold":true,"wrap":false}]},
		{"id":"old","name":"Old","mode":"list","browser":[0,0,1,1],"elements":[{"type":"artwork","rect":[0,0,1,1]}]}]})", "");
	const auto& layout = theme.layouts[0];
	EXPECT_EQ(layout.artworkSource, "cover");
	EXPECT_FLOAT_EQ(layout.rounding, 12.f);
	EXPECT_FLOAT_EQ(layout.inactiveOpacity, 0.65f);
	EXPECT_EQ(layout.selectorBorderColor, 0xabcdefff);
	EXPECT_EQ(layout.separatorColor, 0x12345680);
	EXPECT_TRUE(layout.showArtwork);
	EXPECT_TRUE(layout.compactControls);
	EXPECT_FALSE(layout.showInfoTooltip);
	EXPECT_EQ(layout.elements[0].endColor, 0x102030ff);
	EXPECT_EQ(layout.elements[0].gradient, "horizontal");
	EXPECT_FLOAT_EQ(layout.elements[0].rounding, 14.f);
	EXPECT_EQ(layout.elements[1].fallback, "none");
	EXPECT_EQ(layout.elements[1].requiresArtwork, "screenshot");
	EXPECT_TRUE(layout.elements[2].bold);
	EXPECT_FALSE(layout.elements[2].wrap);
	const auto& old = theme.layouts[1];
	EXPECT_TRUE(old.artworkSource.empty());
	EXPECT_FLOAT_EQ(old.rounding, 0.f);
	EXPECT_FLOAT_EQ(old.inactiveOpacity, 1.f);
	EXPECT_FALSE(old.showArtwork);
	EXPECT_FALSE(old.compactControls);
	EXPECT_TRUE(old.showInfoTooltip);
	EXPECT_EQ(old.elements[0].fallback, "default");
}

TEST(ThemeParserTest, RejectsInvalidMediaPresentation)
{
	for (const std::string& field : {"\"rounding\":-1", "\"rounding\":49", "\"inactiveOpacity\":0", "\"inactiveOpacity\":1.1",
			"\"artworkSource\":\"../outside.png\"", "\"showArtwork\":1", "\"compactControls\":\"yes\"", "\"showInfoTooltip\":1"})
		expectRejected(replaceOnce(kManifest, "\"mode\": \"grid\",", "\"mode\": \"grid\"," + field + ","), "");
	for (const std::string& field : {"\"gradient\":\"diagonal\"", "\"rounding\":49", "\"endColor\":\"red\"",
			"\"bold\":1", "\"wrap\":\"no\"", "\"fallback\":\"../secret.png\"", "\"fallback\":\"\"", "\"requiresArtwork\":\"../secret.png\""})
		expectRejected(replaceOnce(kManifest, "\"type\": \"panel\",", "\"type\": \"panel\"," + field + ","), "");
}

TEST(ThemeParserTest, LoadsAllBundledPackagesIncludingSixStarredPreviews)
{
	const fs::path collection = fs::path(FLYCAST_TEST_FILES).parent_path().parent_path() / "themes";
	int previews = 0;
	for (const auto& entry : fs::directory_iterator(collection))
	{
		if (!entry.is_directory() || !fs::is_regular_file(entry.path() / "theme.json"))
			continue;
		SCOPED_TRACE(entry.path().u8string());
		const auto theme = themes::load(entry.path().u8string());
		EXPECT_EQ(theme.id, entry.path().filename().u8string());
		if (theme.id.find("preview-") == 0)
		{
			++previews;
			ASSERT_FALSE(theme.name.empty());
			EXPECT_EQ(theme.name.back(), '*');
			ASSERT_EQ(theme.layouts.size(), 1u);
			EXPECT_FALSE(theme.layouts[0].showInfoTooltip);
		}
	}
	EXPECT_EQ(previews, 6);
}

TEST(ThemeParserTest, RejectsDuplicateLayoutIdsAndUnsupportedElementsOrBindings)
{
	expectRejected(replaceOnce(kManifest, "\"compact-list\"", "\"main-grid\""), "must be unique within the theme");
	expectRejected(replaceOnce(kManifest, "\"type\": \"panel\"", "\"type\": \"video\""), "must be panel, image, artwork, text, or metadata");
	expectRejected(replaceOnce(kManifest, "\"source\": \"developer\"", "\"source\": \"command\""), "supported metadata field");
	expectRejected(replaceOnce(kManifest, "\"source\": \"cover\"", "\"source\": \"../../outside.png\""), "supported artwork source");
	expectRejected(replaceOnce(kManifest, "\"source\": \"art/overlay.png\"", "\"source\": \"\""), "is required for image elements");
}

TEST(ThemeAssetPathTest, RejectsTraversalAbsoluteAndUriPaths)
{
	TempDirectory package;
	for (const std::string path : {"../outside.png", "/tmp/outside.png", "file:///tmp/outside.png", "C:/outside.png", "art\\\\outside.png"})
		EXPECT_THROW(themes::assetPath(package.path.u8string(), path), std::runtime_error) << path;
}

TEST(ThemeAssetPathTest, RejectsSymlinksThatEscapePackage)
{
	TempDirectory package;
	TempDirectory outside;
	std::ofstream(outside.path / "secret.png", std::ios::binary) << "fixture";
	std::error_code ec;
	fs::create_symlink(outside.path / "secret.png", package.path / "escape.png", ec);
	if (ec)
		GTEST_SKIP() << "Symlinks are unavailable in this test environment: " << ec.message();
	EXPECT_THROW(themes::assetPath(package.path.u8string(), "escape.png"), std::runtime_error);
}

TEST(ThemeParserTest, LoadRejectsOversizedAndMalformedManifests)
{
	TempDirectory package;
	{
		std::ofstream manifest(package.path / "theme.json", std::ios::binary);
		manifest << std::string(256 * 1024 + 1, ' ');
	}
	try
	{
		themes::load(package.path.u8string());
		FAIL() << "Expected oversized manifest to be rejected";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("exceeds 256 KiB"), std::string::npos);
	}
	std::ofstream(package.path / "theme.json", std::ios::binary | std::ios::trunc) << "{not-json";
	try
	{
		themes::load(package.path.u8string());
		FAIL() << "Expected malformed JSON to be rejected";
	}
	catch (const std::runtime_error& error)
	{
		EXPECT_NE(std::string(error.what()).find("invalid JSON"), std::string::npos);
	}
}

TEST(ThemeParserTest, LoadRejectsManifestSymlinkOutsideThemeDirectory)
{
	TempDirectory package;
	TempDirectory outside;
	std::ofstream(outside.path / "theme.json", std::ios::binary) << kManifest;
	std::error_code ec;
	fs::create_symlink(outside.path / "theme.json", package.path / "theme.json", ec);
	if (ec)
		GTEST_SKIP() << "Symlinks are unavailable in this test environment: " << ec.message();
	EXPECT_THROW(themes::load(package.path.u8string()), std::runtime_error);
}
