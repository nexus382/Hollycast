// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gtest/gtest.h"
#include "audio/ui_sound.h"
#include "ui/theme.h"
#include "ui/boxart/artwork_media.h"

#include <algorithm>
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

TEST(ThemeParserTest, ListPresentationIsOptionalAndValidatesItsFields)
{
	const std::string plain = R"({"formatVersion":1,"id":"plain","name":"Plain","layouts":[{"id":"list","name":"List","mode":"list","browser":[0,0,1,1]}]})";
	const auto defaults = parseFixture(plain).layouts.front().list;
	EXPECT_FALSE(defaults.systemBadge);
	EXPECT_FALSE(defaults.favouriteBadge);
	EXPECT_FALSE(defaults.favouriteColor.has_value());
	EXPECT_TRUE(defaults.selectOnHover);
	const auto configured = replaceOnce(plain, "\"mode\":\"list\"",
		R"("mode":"list","list":{"systemBadge":true,"favouriteBadge":true,"favouriteColor":"EAD171","selectOnHover":false})");
	const auto list = parseFixture(configured).layouts.front().list;
	EXPECT_TRUE(list.systemBadge);
	EXPECT_TRUE(list.favouriteBadge);
	ASSERT_TRUE(list.favouriteColor.has_value());
	EXPECT_EQ(*list.favouriteColor, 0xead171ff);
	EXPECT_FALSE(list.selectOnHover);
	expectRejected(replaceOnce(configured, "\"mode\":\"list\"", "\"mode\":\"grid\""), "requires list mode");
	expectRejected(replaceOnce(configured, "\"selectOnHover\":false", "\"selectOnHover\":1"), "must be a boolean");
	expectRejected(replaceOnce(configured, "\"systemBadge\":true", "\"unknown\":true"), "unknown field");
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

TEST(ThemeParserTest, PresentationExtensionsAreOptionalAndKeepLegacyDefaults)
{
	const auto theme = parseFixture(R"({"formatVersion":1,"id":"plain","name":"Plain","layouts":[{"id":"grid","name":"Grid","mode":"grid","browser":[0,0,1,1]}]})");
	ASSERT_EQ(theme.layouts.size(), 1u);
	const auto& layout = theme.layouts.front();
	EXPECT_EQ(layout.toolbar, "standard");
	EXPECT_EQ(layout.activation, "singleClick");
	EXPECT_EQ(layout.titleStyle, "full");
	EXPECT_EQ(layout.selectorStyle, "fill");
	EXPECT_EQ(layout.pixelArt.resolution, 0);
	EXPECT_FLOAT_EQ(layout.canvasAspect, 0);
	EXPECT_FLOAT_EQ(layout.minItemWidth, 96);
	EXPECT_TRUE(layout.showItemActions);
	EXPECT_FALSE(layout.showFavouriteMarker);
	EXPECT_TRUE(layout.hoverDetails);
	EXPECT_TRUE(layout.narrowLayout.empty());
	EXPECT_FALSE(layout.toolbarSelectedTextColor.has_value());
	EXPECT_TRUE(layout.toolbarLogoAsset.empty());
	EXPECT_FLOAT_EQ(layout.toolbarLogoHeight, 0);
	EXPECT_TRUE(theme.fonts.empty());
}

TEST(ThemeParserTest, BrandingAndArtworkFallbackStayInsideThePackage)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	const std::string branded = replaceOnce(replaceOnce(kManifest,
			"\"columns\": 5", "\"toolbarLogo\":\"art/overlay.png\",\"toolbarLogoHeight\":50,\"columns\":5"),
			"\"source\": \"cover\"", "\"source\":\"cover\",\"fallbackImage\":\"art/overlay.png\"");
	const auto theme = parseFixture(branded, package.path);
	EXPECT_EQ(theme.layouts[0].toolbarLogoAsset, fs::canonical(package.path / "art" / "overlay.png").u8string());
	EXPECT_FLOAT_EQ(theme.layouts[0].toolbarLogoHeight, 50);
	EXPECT_EQ(theme.layouts[0].elements[2].resolvedFallbackAsset, theme.layouts[0].toolbarLogoAsset);
	expectRejected(replaceOnce(branded, "\"toolbarLogo\":\"art/overlay.png\"", "\"toolbarLogo\":\"../outside.png\""), "parent traversal");
	expectRejected(replaceOnce(branded, "\"toolbarLogo\":\"art/overlay.png\"", "\"toolbarLogo\":\"missing.png\""), "image inside");
	expectRejected(replaceOnce(branded, "\"fallbackImage\":\"art/overlay.png\"", "\"fallbackImage\":\"../outside.png\""), "parent traversal");
	expectRejected(replaceOnce(branded, "\"fallbackImage\":\"art/overlay.png\"", "\"fallbackImage\":\"missing.png\""), "image inside");
	expectRejected(replaceOnce(branded, "\"toolbarLogoHeight\":50", "\"toolbarLogoHeight\":0"), "between");
	expectRejected(replaceOnce(kManifest, "\"text\": \"Hello\"", "\"text\":\"Hello\",\"fallbackImage\":\"art/overlay.png\""), "only valid on artwork");
}

TEST(ThemeParserTest, DetailsPanelsDeclareTheirMetadataFieldsAndColumnCount)
{
	const std::string minimal = R"({"formatVersion":1,"id":"details-demo","name":"Details","layouts":[{"id":"main","name":"Main","mode":"grid","browser":[0,0,0.4,1],"elements":[{"type":"details","rect":[0.5,0,0.5,1],"fields":["developer","publisher","players","description"],"columns":2}]}]})";
	const auto theme = parseFixture(minimal);
	EXPECT_EQ(theme.layouts[0].elements[0].fields.size(), 4u);
	EXPECT_EQ(theme.layouts[0].elements[0].columns, 2);
	EXPECT_FALSE(theme.layouts[0].elements[0].autoFit);
	EXPECT_TRUE(parseFixture(replaceOnce(minimal, "\"columns\":2", "\"columns\":2,\"autoFit\":true")).layouts[0].elements[0].autoFit);
	expectRejected(replaceOnce(minimal, "\"columns\":2", "\"columns\":2,\"autoFit\":\"yes\""), "boolean");
	expectRejected(replaceOnce(replaceOnce(minimal, ",\"columns\":2", ""),
			"\"type\":\"details\"", "\"autoFit\":true,\"type\":\"metadata\""), ".autoFit");
	expectRejected(replaceOnce(minimal, "\"developer\"", "\"remote-data\""), "supported metadata");
	expectRejected(replaceOnce(minimal, "\"publisher\"", "\"developer\""), "unique");
	expectRejected(replaceOnce(minimal, "\"columns\":2", "\"columns\":4"), "between");
	expectRejected(replaceOnce(minimal, "\"type\":\"details\"", "\"type\":\"text\""), "only valid on details");
	expectRejected(replaceOnce(minimal, "\"columns\":2", "\"columns\":2,\"zIndex\":-1"), "above the browser");
}

TEST(ThemeParserTest, FavouriteActionsAndSelectedLabelsAreReusableButtonOptions)
{
	const std::string manifest = R"({"formatVersion":1,"id":"actions-demo","name":"Actions","layouts":[{"id":"main","name":"Main","mode":"grid","browser":[0,0,0.6,1],"elements":[{"type":"button","rect":[0.7,0.1,0.2,0.1],"action":"favourite","text":"Favourite","selectedText":"Favourited"}]}]})";
	const auto theme = parseFixture(manifest);
	EXPECT_EQ(theme.layouts[0].elements[0].action, "favourite");
	EXPECT_EQ(theme.layouts[0].elements[0].selectedText, "Favourited");
	expectRejected(replaceOnce(manifest, "\"type\":\"button\"", "\"type\":\"text\""), "only valid on buttons");
}

TEST(ThemeParserTest, ButtonIconsAndBordersAreOptInAndValidated)
{
	const std::string manifest = R"({"formatVersion":1,"id":"icon-demo","name":"Icons","layouts":[{"id":"main","name":"Main","mode":"list","browser":[0,0,0.6,1],"elements":[{"type":"button","rect":[0.7,0.1,0.2,0.1],"action":"favourite","text":"Favourite","icon":"star","selectedIcon":"star-filled","borderColor":"91a189","selectedBorderColor":"526b4e"}]}]})";
	const auto theme = parseFixture(manifest);
	const auto& button = theme.layouts[0].elements[0];
	EXPECT_EQ(button.icon, "star");
	EXPECT_EQ(button.selectedIcon, "star-filled");
	EXPECT_EQ(button.borderColor, 0x91a189ff);
	EXPECT_EQ(button.selectedBorderColor, 0x526b4eff);
	expectRejected(replaceOnce(manifest, "\"icon\":\"star\"", "\"icon\":\"unknown\""), "star or star-filled");
	expectRejected(replaceOnce(manifest, "\"selectedIcon\":\"star-filled\"", "\"selectedIcon\":\"unknown\""), "star or star-filled");
	expectRejected(replaceOnce(manifest, "\"type\":\"button\"", "\"type\":\"text\""), "only valid on buttons");
	const auto plain = parseFixture(replaceOnce(manifest,
			",\"icon\":\"star\",\"selectedIcon\":\"star-filled\",\"borderColor\":\"91a189\",\"selectedBorderColor\":\"526b4e\"", ""));
	EXPECT_TRUE(plain.layouts[0].elements[0].icon.empty());
	EXPECT_FALSE(plain.layouts[0].elements[0].borderColor);
}

TEST(ThemeParserTest, ParsesReusablePresentationControlsAndResponsiveFallback)
{
	const auto theme = parseFixture(R"({"formatVersion":1,"id":"lcd-demo","name":"LCD demo","layouts":[
	 {"id":"wide","name":"Wide","mode":"grid","browser":[0.1,0.1,0.4,0.4],"canvasAspect":1.6,"toolbar":"compact","toolbarSelectedTextColor":"ffffff","activation":"doubleClick","minItemWidth":42,"titleStyle":"initials","selectorStyle":"outline","showItemActions":false,"showFavouriteMarker":true,"hoverDetails":false,"pixelArt":{"resolution":24,"dark":"102030","light":"aabbcc"},"previewSource":"screenshot","narrowLayout":"tall","narrowAspect":1.15,"elements":[{"type":"button","rect":[0.6,0.1,0.1,0.1],"action":"artwork","source":"cover","background":"00000000","rounding":0.5,"selectedColor":"ffffff","selectedBackground":"60745a"},{"type":"artwork","rect":[0.6,0.3,0.3,0.3],"source":"preview"}]},
	 {"id":"tall","name":"Tall","mode":"list","browser":[0,0,1,1]}]})");
	const auto& layout = theme.layouts[0];
	EXPECT_EQ(layout.pixelArt.resolution, 24);
	EXPECT_EQ(layout.pixelArt.levels, 4);
	EXPECT_EQ(layout.pixelArt.dark, 0x102030ff);
	EXPECT_TRUE(layout.showFavouriteMarker);
	EXPECT_EQ(layout.titleStyle, "initials");
	EXPECT_EQ(layout.elements[0].action, "artwork");
	EXPECT_EQ(*layout.elements[0].background, 0u);
	EXPECT_FLOAT_EQ(*layout.elements[0].buttonRounding, 0.5f);
	EXPECT_EQ(*layout.elements[0].selectedColor, 0xffffffff);
	EXPECT_EQ(*layout.elements[0].selectedBackground, 0x60745aff);
	EXPECT_EQ(*layout.toolbarSelectedTextColor, 0xffffffff);
	EXPECT_EQ(&themes::responsiveLayout(theme, layout, 1.6f), &theme.layouts[0]);
	EXPECT_EQ(&themes::responsiveLayout(theme, layout, 0.6f), &theme.layouts[1]);
	EXPECT_EQ(&themes::responsiveLayout(theme, theme.layouts[1], 0.6f), &theme.layouts[1]);
}

TEST(ThemeParserTest, RejectsMalformedPresentationExtensionsInsteadOfSilentlyIgnoringThem)
{
	const std::string minimal = R"({"formatVersion":1,"id":"test","name":"Test","layouts":[{"id":"grid","name":"Grid","mode":"grid","browser":[0,0,1,1], EXTRA}]})";
	for (const std::string& value : {
			R"("canvasAspect":0)", R"("toolbar":"hidden")", R"("activation":"never")",
			R"("minItemWidth":1)", R"("titleStyle":"guess")", R"("selectorStyle":"invalid")",
			R"("pixelArt":{"resolution":1024})", R"("pixelArt":{"resolution":24,"typo":true})",
			R"("pixelArt":{"resolution":128,"levels":1})", R"("pixelArt":{"resolution":128,"levels":257})",
			R"("pixelArt":{"resolution":128,"levels":2.5})",
			R"("narrowLayout":"missing")", R"("narrowLayout":"grid")", R"("narrowAspect":1.2)",
			R"("elements":[{"type":"button","rect":[0,0,0.2,0.2],"action":"script"}])",
			R"("elements":[{"type":"button","rect":[0,0,0.2,0.2],"action":"artwork","source":"remote"}])",
			R"("elements":[{"type":"button","rect":[0,0,0.2,0.2],"action":"next","zIndex":-1}])",
			R"("elements":[{"type":"button","rect":[0,0,0.2,0.2],"action":"next","rounding":0.6}])",
			R"("elements":[{"type":"text","rect":[0,0,0.2,0.2],"action":"next"}])"})
		expectRejected(replaceOnce(minimal, "EXTRA", value), "");
	expectRejected(replaceOnce(kManifest, "\"baseTheme\": 2", "\"fonts\":{\"body\":\"../outside.ttf\"},\"baseTheme\":2"), "parent traversal");
	expectRejected(replaceOnce(kManifest, "\"baseTheme\": 2", "\"fonts\":{\"body\":\"missing.ttf\"},\"baseTheme\":2"), "package font");
}

TEST(ThemeParserTest, RailAttractAndElementMotionFieldsParse)
{
	const auto theme = themes::parse(R"({
		"formatVersion": 1, "id": "motion", "name": "Motion",
		"layouts": [{
			"id": "rail", "name": "Rail", "mode": "rail", "browser": [0, 0, 1, 1],
			"attract": {"idleSeconds": 12, "intervalSeconds": 2},
			"rail": {"visible": 3, "center": 0.3, "tilt": 0.02, "skew": 0.01, "falloff": 0.04, "fade": 0.1, "rowColor": "010203", "numbers": false,
				"badges": [["system"], ["genre", "region"]]},
			"elements": [
				{"type": "text", "rect": [0, 0, 1, 0.1], "text": "News", "marqueeSeconds": 20, "slant": 0.2, "when": "attract"},
				{"type": "panel", "rect": [0, 0, 1, 0.1], "when": "interactive", "bleed": true},
				{"type": "metadata", "rect": [0, 0, 1, 0.1], "fields": ["description", "genre"], "separator": " * "}
			]
		}]
	})", ".");
	const auto& layout = theme.layouts[0];
	EXPECT_EQ(layout.mode, "rail");
	EXPECT_FLOAT_EQ(layout.attract.idleSeconds, 12);
	EXPECT_FLOAT_EQ(layout.attract.intervalSeconds, 2);
	EXPECT_EQ(layout.rail.visible, 3);
	EXPECT_FLOAT_EQ(layout.rail.center, 0.3f);
	EXPECT_FALSE(layout.rail.numbers);
	ASSERT_TRUE(layout.rail.rowColor.has_value());
	EXPECT_EQ(*layout.rail.rowColor, 0x010203ffu);
	ASSERT_EQ(layout.rail.badges.size(), 2u);
	EXPECT_EQ(layout.rail.badges[0], (std::vector<std::string>{"system"}));
	EXPECT_EQ(layout.rail.badges[1], (std::vector<std::string>{"genre", "region"}));
	EXPECT_EQ(layout.elements[0].when, "attract");
	EXPECT_FLOAT_EQ(layout.elements[0].marqueeSeconds, 20);
	EXPECT_FLOAT_EQ(layout.elements[0].slant, 0.2f);
	EXPECT_EQ(layout.elements[1].when, "interactive");
	EXPECT_TRUE(layout.elements[1].bleed);
	EXPECT_FALSE(layout.elements[0].bleed);
	EXPECT_EQ(layout.elements[2].fields, (std::vector<std::string>{"description", "genre"}));
	EXPECT_EQ(layout.elements[2].separator, " * ");
}

TEST(ThemeParserTest, MotionFieldsRejectInvalidCombinations)
{
	const auto layoutWith = [](const std::string& fields) {
		return R"({"formatVersion": 1, "id": "motion", "name": "Motion", "layouts": [{"id": "main", "name": "Main", "browser": [0, 0, 1, 1], )" + fields + "}]}";
	};
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "rail": {"visible": 3})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"visible": 9})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"center": 0.95})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"badges": [["mood"]]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"badges": []})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"badges": [[]]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "rail", "rail": {"badges": ["genre"]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "attract": {"intervalSeconds": 2})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "sounds": {"move": "missing.wav"}})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "sounds": {"jump": "missing.wav"}})"), "."), std::runtime_error);
	const auto withElement = [&](const std::string& element) {
		return layoutWith(R"("mode": "grid", "elements": [)" + element + "]");
	};
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "when": "sometimes"})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "button", "rect": [0, 0, 1, 1], "action": "next", "bleed": true})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "metadata", "rect": [0, 0, 1, 1], "fields": ["mood"]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "metadata", "rect": [0, 0, 1, 1], "source": "genre", "fields": ["genre"]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "metadata", "rect": [0, 0, 1, 1], "source": "genre", "separator": "-"})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "text", "rect": [0, 0, 1, 1], "fields": ["genre"]})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "button", "rect": [0, 0, 1, 1], "action": "next", "when": "attract"})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "slant": 0.1})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "launchFadeFrom": 0.5})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "when": "launching", "launchFadeFrom": 1.5})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "launch": {})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "launch": {"seconds": 9})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(layoutWith(R"("mode": "grid", "sounds": {"launch": "missing.wav"}})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "hoverImage": "a.png"})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "button", "rect": [0, 0, 1, 1], "action": "next", "image": "missing.png"})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "marqueeSeconds": 10})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "sweepSeconds": 10})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "panel", "rect": [0, 0, 1, 1], "spin": {"seconds": 10, "center": [0, 0], "radius": 1}})"), "."), std::runtime_error);
	EXPECT_THROW(themes::parse(withElement(R"({"type": "artwork", "rect": [0, 0, 1, 1], "source": "disc", "spin": {"seconds": 10, "center": [0, 0], "radius": 1, "hole": 2}})"), "."), std::runtime_error);
	const auto round = themes::parse(withElement(R"({"type": "artwork", "rect": [0, 0, 1, 1], "source": "disc", "spin": {"seconds": 10, "center": [0, 0], "radius": 1, "hole": 0.13}})"), ".");
	EXPECT_FLOAT_EQ(round.layouts[0].elements[0].spinHole, 0.13f);
	const auto square = themes::parse(withElement(R"({"type": "artwork", "rect": [0, 0, 1, 1], "source": "disc", "spin": {"seconds": 10, "center": [0, 0], "radius": 1}})"), ".");
	EXPECT_LT(square.layouts[0].elements[0].spinHole, 0);
}

namespace
{
std::vector<u8> wavBytes(u32 rate, u16 channels, u16 bits, u16 format, const std::vector<s16>& samples)
{
	const auto put = [](std::vector<u8>& out, u32 value, int bytes) {
		for (int i = 0; i < bytes; ++i)
			out.push_back(static_cast<u8>(value >> (i * 8)));
	};
	const auto tag = [](std::vector<u8>& out, const char* text) { out.insert(out.end(), text, text + 4); };
	std::vector<u8> out;
	tag(out, "RIFF");
	put(out, static_cast<u32>(36 + samples.size() * 2), 4);
	tag(out, "WAVE");
	tag(out, "fmt ");
	put(out, 16, 4);
	put(out, format, 2);
	put(out, channels, 2);
	put(out, rate, 4);
	put(out, rate * channels * bits / 8, 4);
	put(out, channels * bits / 8, 2);
	put(out, bits, 2);
	tag(out, "data");
	put(out, static_cast<u32>(samples.size() * 2), 4);
	for (s16 sample : samples)
		put(out, static_cast<u16>(sample), 2);
	return out;
}
}

TEST(UiSoundTest, DecodesMonoAndStereoToStereo)
{
	std::vector<s16> stereo;
	auto bytes = wavBytes(44100, 1, 16, 1, {100, -200});
	ASSERT_TRUE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	EXPECT_EQ(stereo, (std::vector<s16>{100, 100, -200, -200}));
	bytes = wavBytes(44100, 2, 16, 1, {1, 2, 3, 4});
	ASSERT_TRUE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	EXPECT_EQ(stereo, (std::vector<s16>{1, 2, 3, 4}));
}

TEST(UiSoundQueueTest, ReleasesAWholeSoundAtThePaceItPlays)
{
	UiSoundQueue queue;
	const std::vector<s16> sound(44100 * 2, 100); // One second of stereo frames.
	queue.add(sound);
	s16 chunk[512 * 2];
	u64 taken = 0;
	const auto takeDue = [&](double elapsed) {
		u64 frames = 0;
		while (const u32 count = queue.take(elapsed, chunk, 512))
			frames += count;
		taken += frames;
		return frames;
	};
	const u64 early = takeDue(0);
	EXPECT_GE(early, 2048u);
	EXPECT_LT(early, 44100u); // A one-second sound is not sent all at once.
	EXPECT_EQ(takeDue(0), 0u);
	EXPECT_GT(takeDue(0.5), 0u);
	EXPECT_LT(taken, 44100u);
	takeDue(5);
	EXPECT_TRUE(queue.empty());
	EXPECT_GE(taken, 44100u); // Every frame arrives, followed by trailing silence.
}

TEST(UiSoundQueueTest, MixesOverlappingSoundsWithoutWrapping)
{
	UiSoundQueue queue;
	queue.add(std::vector<s16>(4, 30000));
	queue.add(std::vector<s16>(2, 30000));
	s16 chunk[2048 * 2];
	const u32 frames = queue.take(1, chunk, 2048);
	ASSERT_GE(frames, 2u);
	EXPECT_EQ(chunk[0], 32767);
	EXPECT_EQ(chunk[1], 32767);
	EXPECT_EQ(chunk[2], 30000);
	EXPECT_EQ(chunk[3], 30000);
}

TEST(UiSoundQueueTest, EmptyQueueStartsAgainCleanly)
{
	UiSoundQueue queue;
	s16 chunk[2];
	EXPECT_TRUE(queue.empty());
	EXPECT_EQ(queue.take(10, chunk, 1), 0u);
	queue.add(std::vector<s16>(2, 5));
	EXPECT_FALSE(queue.empty());
	while (queue.take(10, chunk, 1) != 0) {}
	EXPECT_TRUE(queue.empty());
	queue.add(std::vector<s16>(2, 7));
	ASSERT_EQ(queue.take(0, chunk, 1), 1u); // The pace restarts with the new sound.
	EXPECT_EQ(chunk[0], 7);
}

TEST(UiSoundTest, RejectsFormatsItCannotPlayAndDamagedFiles)
{
	std::vector<s16> stereo;
	auto bytes = wavBytes(22050, 1, 16, 1, {1});
	EXPECT_FALSE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	bytes = wavBytes(44100, 1, 8, 1, {1});
	EXPECT_FALSE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	bytes = wavBytes(44100, 3, 16, 1, {1, 2, 3});
	EXPECT_FALSE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	bytes = wavBytes(44100, 1, 16, 3, {1});
	EXPECT_FALSE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	bytes = wavBytes(44100, 1, 16, 1, {1, 2});
	bytes.resize(bytes.size() - 2); // The data chunk now claims more than the file holds.
	EXPECT_FALSE(decodeUiSound(bytes.data(), bytes.size(), stereo));
	const u8 notWav[16] = {'n', 'o', 't'};
	EXPECT_FALSE(decodeUiSound(notWav, sizeof(notWav), stereo));
	EXPECT_FALSE(decodeUiSound(nullptr, 0, stereo));
}

TEST(ThemeParserTest, BundledDemoDiscUsesGenericExtensionsAndLocalAssets)
{
	const auto root = fs::path(FLYCAST_TEST_FILES).parent_path().parent_path();
	const auto theme = themes::load((root / "themes" / "demo-disc").u8string());
	EXPECT_EQ(theme.id, "demo-disc");
	ASSERT_EQ(theme.layouts.size(), 2u);
	EXPECT_EQ(theme.layouts[0].mode, "rail");
	EXPECT_EQ(theme.layouts[0].toolbar, "compact");
	EXPECT_EQ(theme.layouts[0].activation, "doubleClick");
	EXPECT_GT(theme.layouts[0].attract.idleSeconds, 0);
	EXPECT_FALSE(theme.layouts[0].moveSound.empty());
	EXPECT_TRUE(theme.layouts[0].rail.favouriteMark);
	for (const auto& layout : theme.layouts)
	{
		EXPECT_GT(layout.launchSeconds, 0);
		EXPECT_FALSE(layout.launchSound.empty());
		// The game's own disc art sits on the theme's disc, turning about the same point.
		const auto find = [&](const char *type, const char *source) {
			return std::find_if(layout.elements.begin(), layout.elements.end(), [&](const themes::Element& element) {
				return element.type == type && (element.type != "artwork" || element.source == source) && element.spinSeconds > 0;
			});
		};
		const auto plain = find("image", "");
		const auto art = find("artwork", "disc");
		ASSERT_NE(plain, layout.elements.end());
		ASSERT_NE(art, layout.elements.end());
		EXPECT_GT(art->zIndex, plain->zIndex);
		EXPECT_GE(art->spinHole, 0);
		EXPECT_FLOAT_EQ(art->spinCenterX, plain->spinCenterX);
		EXPECT_FLOAT_EQ(art->spinCenterY, plain->spinCenterY);
		EXPECT_FLOAT_EQ(art->spinRadius, plain->spinRadius);
		EXPECT_FLOAT_EQ(art->spinLaunchSeconds, plain->spinLaunchSeconds);
	}
	EXPECT_LT(theme.layouts[1].rail.center, 0.5f); // Portrait keeps the rail's gap small.
	EXPECT_LT(theme.layouts[1].toolbarLogoHeight, theme.layouts[0].toolbarLogoHeight);
	for (const auto& element : theme.layouts[0].elements)
		if (element.action == "favourite")
		{
			EXPECT_FALSE(element.buttonImage.empty());
			EXPECT_FALSE(element.buttonHoverImage.empty());
			EXPECT_FALSE(element.buttonSelectedImage.empty());
			EXPECT_FALSE(element.buttonSelectedHoverImage.empty());
		}
	for (const auto& layout : theme.layouts)
		EXPECT_TRUE(std::any_of(layout.elements.begin(), layout.elements.end(), [](const themes::Element& element) {
			return element.action == "favourite";
		})) << layout.id;
	ASSERT_EQ(theme.layouts[0].rail.badges.size(), 2u);
	EXPECT_EQ(theme.layouts[0].rail.badges[0], (std::vector<std::string>{"system"}));
	EXPECT_EQ(theme.fonts.size(), 2u);
	EXPECT_EQ(themes::responsiveLayout(theme, theme.layouts[0], 0.6f).id, "demo-disc-portrait");
}

TEST(ThemeParserTest, BundledPocketDimensionUsesGenericExtensionsAndLocalAssets)
{
	const auto root = fs::path(FLYCAST_TEST_FILES).parent_path().parent_path();
	const auto theme = themes::load((root / "themes" / "pocket-dimension").u8string());
	EXPECT_EQ(theme.id, "pocket-dimension");
	ASSERT_EQ(theme.layouts.size(), 2u);
	EXPECT_EQ(theme.layouts[0].toolbar, "compact");
	for (const auto& layout : theme.layouts)
	{
		EXPECT_EQ(layout.pixelArt.resolution, 128);
		EXPECT_EQ(layout.pixelArt.levels, 32);
		EXPECT_EQ(layout.artworkSource, "cover");
	}
	EXPECT_EQ(theme.fonts.size(), 2u);
	EXPECT_EQ(themes::responsiveLayout(theme, theme.layouts[0], 0.6f).id, "pocket-portrait");
	for (const auto& layout : theme.layouts)
		for (const char* media : {"cover", "screenshot", "miximage", "disc"})
			EXPECT_TRUE(std::any_of(layout.elements.begin(), layout.elements.end(), [&](const themes::Element& e) {
				return e.type == "button" && e.action == "artwork" && e.source == media;
			})) << layout.id << ": " << media;
	for (const char* id : {"preview-art-book-next", "preview-horizon-grid", "preview-cd-rack"})
	{
		const auto existing = themes::load((root / "themes" / id).u8string());
		for (const auto& layout : existing.layouts)
		{
			EXPECT_EQ(layout.toolbar, "standard");
			EXPECT_EQ(layout.activation, "doubleClick");
			EXPECT_EQ(layout.pixelArt.resolution, 0);
		}
	}
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

TEST(ThemeParserTest, LoadsAllBundledPackagesIncludingStarredPreviews)
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
	EXPECT_EQ(previews, 4);
}

TEST(ThemeParserTest, RackAllowsNarrowSpinesWithoutChangingGridLimits)
{
	TempDirectory package;
	fs::create_directories(package.path / "art");
	std::ofstream(package.path / "art" / "overlay.png", std::ios::binary) << "fixture";
	std::string json = replaceOnce(kManifest, "\"mode\": \"grid\"", "\"mode\": \"rack\"");
	json = replaceOnce(json, "\"columns\": 5", "\"columns\": 22");
	json = replaceOnce(json, "\"itemAspect\": 0.75", "\"itemAspect\": 0.13");
	const auto rack = parseFixture(json, package.path).layouts.front();
	EXPECT_EQ(rack.mode, "rack");
	EXPECT_EQ(rack.columns, 22);
	EXPECT_FLOAT_EQ(rack.itemAspect, 0.13f);
	expectRejected(replaceOnce(json, "\"columns\": 22", "\"columns\": 65"), "columns");
	expectRejected(replaceOnce(json, "\"itemAspect\": 0.13", "\"itemAspect\": 0.01"), "itemAspect");
}

TEST(ThemeParserTest, RejectsDuplicateLayoutIdsAndUnsupportedElementsOrBindings)
{
	expectRejected(replaceOnce(kManifest, "\"compact-list\"", "\"main-grid\""), "must be unique within the theme");
	expectRejected(replaceOnce(kManifest, "\"type\": \"panel\"", "\"type\": \"video\""), "must be panel, image, artwork, text, metadata, details, button, stat, or meter");
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

TEST(ThemeParserTest, PagedGridAndSelectionDecorationsAreOptionalAndValidated)
{
	const std::string json = R"({"formatVersion":1,"id":"paged","name":"Paged","layouts":[{
		"id":"main","name":"Main","mode":"grid","browser":[0,0,1,1],
		"pagedGrid":{"rows":4,"padding":3},"elements":[
		{"type":"panel","rect":[0.4,0.8,0.01,0.02],"anchor":"selectionTrail","onSelection":0.25},
		{"type":"text","rect":[0,0.3,1,0.1],"text":"New","when":"unplayed","onSelection":1.3},
		{"type":"stat","rect":[0,0,0.2,0.1],"source":"totalPlayHours","digits":7,"valueScale":1000},
		{"type":"meter","rect":[0,0.9,0.2,0.02],"source":"neglect","maximum":1},
		{"type":"button","rect":[0.7,0.8,0.2,0.1],"action":"nextPage"}]}]})";
	const auto layout = themes::parse(json, fs::temp_directory_path().u8string()).layouts.front();
	EXPECT_EQ(layout.pagedGrid.rows, 4);
	EXPECT_EQ(layout.elements[0].anchor, "selectionTrail");
	EXPECT_FLOAT_EQ(layout.elements[0].onSelection, .25f);
	EXPECT_EQ(layout.elements[2].digits, 7);
	EXPECT_FLOAT_EQ(layout.elements[2].valueScale, 1000);
	EXPECT_EQ(themes::Layout{}.pagedGrid.rows, 0);
	EXPECT_EQ(themes::Element{}.onSelection, 0);
	EXPECT_TRUE(themes::Element{}.anchor.empty());
	expectRejected(replaceOnce(json, "\"rows\":4", "\"rows\":0"), "pagedGrid.rows");
	expectRejected(replaceOnce(json, "\"rows\":4", "\"rows\":4.5"), "pagedGrid.rows");
	expectRejected(replaceOnce(json, "\"mode\":\"grid\"", "\"mode\":\"list\""), "requires grid");
	expectRejected(replaceOnce(json, "\"anchor\":\"selectionTrail\"", "\"anchor\":\"arbitrary\""), "anchor");
	expectRejected(replaceOnce(json, "\"onSelection\":0.25", "\"onSelection\":0"), "onSelection");
	expectRejected(replaceOnce(json, "\"source\":\"neglect\"", "\"source\":\"madeUp\""), "numeric statistic");
	expectRejected(replaceOnce(json, "\"maximum\":1", "\"maximum\":0"), "maximum");
	expectRejected(replaceOnce(json, "\"digits\":7", "\"digits\":100"), "digits");
	expectRejected(replaceOnce(json, "\"pagedGrid\":{\"rows\":4,\"padding\":3},", ""), "require pagedGrid");
	expectRejected(replaceOnce(json, "\"action\":\"nextPage\"", "\"action\":\"nextPage\",\"anchor\":\"selection\""), "interactive");
}

TEST(ThemeParserTest, TiledImagesKeepPackagePathAndMotionValidation)
{
	TempDirectory package;
	std::ofstream(package.path / "stars.png") << "fixture";
	const std::string json = R"({"formatVersion":1,"id":"stars","name":"Stars","layouts":[{
		"id":"main","name":"Main","mode":"grid","browser":[0,0,1,1],"elements":[
		{"type":"image","rect":[0,0,1,1],"source":"stars.png","tileSeconds":8}]}]})";
	EXPECT_EQ(themes::parse(json, package.path.u8string()).layouts[0].elements[0].tileSeconds, 8);
	EXPECT_THROW(themes::parse(replaceOnce(json, "\"tileSeconds\":8", "\"tileSeconds\":8,\"sweepSeconds\":2"), package.path.u8string()), std::runtime_error);
	EXPECT_THROW(themes::parse(replaceOnce(json, "stars.png", "../stars.png"), package.path.u8string()), std::runtime_error);
}

TEST(ThemeMediaRequirementsTest, BasicViewsDoNotRequestThemeExtras)
{
	const auto mask = themes::requiredMedia(themes::Layout{});
	EXPECT_EQ(mask, artwork::metadataMedia | artwork::mediaBit("cover")
		| artwork::mediaBit("miximage") | artwork::mediaBit("title"));
	for (const char* optional : {"case", "fanart", "screenshot", "banner", "titlescreen", "backcover", "spine"})
		EXPECT_EQ(mask & artwork::mediaBit(optional), 0u) << optional;
	EXPECT_EQ(mask & artwork::mediaBit("physical"), 0u);
	EXPECT_EQ(mask & artwork::mediaBit("boxtexture"), 0u);
}

TEST(ThemeMediaRequirementsTest, DerivesBindingsFallbacksAndHiddenAnimationDependencies)
{
	themes::Layout layout;
	layout.artworkSource = "spine";
	layout.requiredMedia = {"boxtexture", "backcover"};
	themes::Element image;
	image.type = "artwork"; image.source = "fanart"; image.fallback = "screenshot"; image.requiresArtwork = "title";
	themes::Element button; button.type = "button"; button.action = "artwork"; button.source = "disc";
	layout.elements = {image, button};
	const auto mask = themes::requiredMedia(layout);
	for (const char* source : {"spine", "boxtexture", "backcover", "fanart", "screenshot", "title", "physical"})
		EXPECT_NE(mask & artwork::mediaBit(source), 0u) << source;
	EXPECT_EQ(mask & artwork::mediaBit("case"), 0u);
}

TEST(ThemeMediaRequirementsTest, IncludesResponsiveLayoutAndRespectsIconOverride)
{
	themes::Theme theme;
	themes::Layout wide, narrow;
	wide.id = "wide"; wide.artworkSource = "spine"; wide.narrowLayout = "narrow";
	narrow.id = "narrow"; narrow.requiredMedia = {"banner"};
	theme.layouts = {wide, narrow};
	const auto mask = themes::requiredMedia(theme, wide, std::string("disc"));
	EXPECT_NE(mask & artwork::mediaBit("banner"), 0u);
	EXPECT_NE(mask & artwork::mediaBit("physical"), 0u);
	EXPECT_EQ(mask & artwork::mediaBit("spine"), 0u);
}
