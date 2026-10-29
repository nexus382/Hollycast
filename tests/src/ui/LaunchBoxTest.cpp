/* Copyright 2026 The Hollycast Authors. SPDX-License-Identifier: GPL-2.0-or-later */
#include "gtest/gtest.h"
#include "ui/boxart/launchbox.h"
#include "archive/ZipArchive.h"
#include "oslib/oslib.h"
#include <stb_image.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
const std::string fixture = R"xml(<?xml version="1.0"?><LaunchBox>
<Game><Name>Sonic Adventure</Name><DatabaseID>2416</DatabaseID><Platform>Sega Dreamcast</Platform></Game>
<Game><Name>Sonic Adventure</Name><DatabaseID>9000</DatabaseID><Platform>Windows</Platform></Game>
<Game><Name>Sonic Adventure</Name><DatabaseID>100</DatabaseID><Platform>Arcade</Platform></Game>
<Game><Name>Virtua Tennis</Name><DatabaseID>5894</DatabaseID><Platform>Sega Dreamcast</Platform></Game>
<Game><Name>Foo &amp; Bar</Name><DatabaseID>200</DatabaseID><Platform>Sega Dreamcast</Platform></Game>
<GameAlternateName><AlternateName>Power Smash</AlternateName><DatabaseID>5894</DatabaseID></GameAlternateName>
<GameAlternateName><AlternateName>&#x30BD;&#12491;&#x30C3;&#12463;</AlternateName><DatabaseID>2416</DatabaseID></GameAlternateName>
<GameAlternateName><AlternateName><![CDATA[Foo & Bar: Special]]></AlternateName><DatabaseID>200</DatabaseID></GameAlternateName>
<GameImage><DatabaseID>2416</DatabaseID><FileName>us.png</FileName><Type>Box - Front</Type><Region>North America</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>jp.jpg</FileName><Type>Box - Front</Type><Region>Japan</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>eu.png</FileName><Type>Box - Front</Type><Region>Europe</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>back.jpg</FileName><Type>Box - Back</Type><Region>Japan</Region></GameImage>
<GameImage><DatabaseID>100</DatabaseID><FileName>arcade.png</FileName><Type>Advertisement Flyer - Front</Type><Region /></GameImage>
<GameImage><DatabaseID>5894</DatabaseID><FileName>tennis.png</FileName><Type>Box - Front</Type><Region>World</Region></GameImage>
<GameImage><DatabaseID>200</DatabaseID><FileName>foo.png</FileName><Type>Box - Front</Type><Region>World</Region></GameImage>
</LaunchBox>)xml";

void readXml(LaunchBoxCatalog& catalog, const std::string& xml, size_t chunkSize = 65536)
{
	size_t position = 0;
	catalog.read([&](char *buffer, size_t capacity) {
		const size_t size = std::min({capacity, chunkSize, xml.size() - position});
		std::memcpy(buffer, xml.data() + position, size);
		position += size;
		return size;
	});
}

GameBoxart sonic()
{
	GameBoxart item;
	item.searchName = "SONIC ADVENTURE";
	item.uniqueId = "HDR-0001";
	item.fileName = "Someone renamed this.chd";
	item.gamePath = "/games/Someone renamed this.chd";
	item.region = GameBoxart::JAPAN;
	return item;
}
} // namespace

TEST(LaunchBoxCatalogTest, StreamsTheExportAndChoosesRegionalFrontCovers)
{
	LaunchBoxCatalog catalog;
	readXml(catalog, fixture, 7);
	GameBoxart item = sonic();
	EXPECT_EQ(2416, catalog.match(item).id);
	EXPECT_EQ("https://images.launchbox-app.com/jp.jpg", catalog.match(item).url);
	item.region = GameBoxart::EUROPE;
	EXPECT_EQ("https://images.launchbox-app.com/eu.png", catalog.match(item).url);
	item.region = GameBoxart::USA;
	EXPECT_EQ("https://images.launchbox-app.com/us.png", catalog.match(item).url);
	item.arcade = true;
	EXPECT_EQ(100, catalog.match(item).id);
}

TEST(LaunchBoxCatalogTest, UsesAlternateNamesUnicodeAndCdata)
{
	LaunchBoxCatalog catalog;
	readXml(catalog, fixture);
	GameBoxart item = sonic();
	item.searchName = "Power Smash";
	EXPECT_EQ(5894, catalog.match(item).id);
	item.searchName = "ソニック";
	EXPECT_EQ(2416, catalog.match(item).id);
	item.searchName = "Foo & Bar: Special";
	EXPECT_EQ(200, catalog.match(item).id);
}

TEST(LaunchBoxCatalogTest, NormalizesRomSuffixesAndFallsBackToFilename)
{
	LaunchBoxCatalog catalog;
	readXml(catalog, fixture);
	GameBoxart item;
	item.fileName = "Sonic Adventure (Europe) (Disc 1).chd";
	EXPECT_EQ(2416, catalog.match(item).id);
	item.searchName = "Unknown header";
	EXPECT_EQ(2416, catalog.match(item).id);
	item.fileName = "Unknown.chd";
	EXPECT_EQ(0, catalog.match(item).id);
}

TEST(LaunchBoxCatalogTest, RejectsAmbiguousMatchesAndUnsafeImagePaths)
{
	LaunchBoxCatalog catalog;
	std::string xml = fixture;
	xml.insert(xml.find("<GameAlternateName>"),
		"<Game><Name>Sonic-Adventure</Name><DatabaseID>2</DatabaseID><Platform>Sega Dreamcast</Platform></Game>");
	readXml(catalog, xml);
	EXPECT_EQ(0, catalog.match(sonic()).id);
	xml = fixture;
	xml.replace(xml.find("jp.jpg"), 6, "../private.jpg");
	readXml(catalog, xml);
	EXPECT_NE("https://images.launchbox-app.com/../private.jpg", catalog.match(sonic()).url);
}

TEST(LaunchBoxCatalogTest, KeepsCatalogOnTruncatedDownloadAndRoundTripsCache)
{
	LaunchBoxCatalog catalog;
	readXml(catalog, fixture);
	EXPECT_THROW(readXml(catalog, fixture.substr(0, fixture.size() - 20)), std::runtime_error);
	EXPECT_EQ(2416, catalog.match(sonic()).id);
	LaunchBoxCatalog loaded;
	loaded.loadJson(catalog.toJson());
	EXPECT_EQ(catalog.match(sonic()).url, loaded.match(sonic()).url);
}

TEST(LaunchBoxCatalogTest, PrefersMatchingRegionToAnotherRegionsOriginalScan)
{
	LaunchBoxCatalog catalog;
	std::string xml = fixture;
	const size_t jp = xml.find("<FileName>jp.jpg</FileName><Type>Box - Front</Type>");
	xml.replace(jp, std::string("<FileName>jp.jpg</FileName><Type>Box - Front</Type>").size(),
		"<FileName>jp.jpg</FileName><Type>Box - Front - Reconstructed</Type>");
	readXml(catalog, xml);
	EXPECT_EQ("https://images.launchbox-app.com/jp.jpg", catalog.match(sonic()).url);
}

TEST(LaunchBoxCatalogTest, IdentitySurvivesRenameAndDistinguishesRegionAndPlatform)
{
	GameBoxart item = sonic();
	const std::string identity = LaunchBoxCatalog::identity(item);
	item.fileName = "Completely different.chd";
	item.gamePath = "/another/directory/Completely different.chd";
	item.name = "Different display name";
	EXPECT_EQ(identity, LaunchBoxCatalog::identity(item));
	item.region = GameBoxart::EUROPE;
	EXPECT_NE(identity, LaunchBoxCatalog::identity(item));
	item.region = GameBoxart::JAPAN;
	item.arcade = true;
	EXPECT_NE(identity, LaunchBoxCatalog::identity(item));
}

TEST(LaunchBoxCacheTest, RestoresCoverForRenamedDiscWithoutCatalogOrNetwork)
{
	const auto directory = std::filesystem::temp_directory_path()
		/ ("hollycast-launchbox-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const std::string image = (directory / "cover.png").string();
	std::ofstream(image) << "cached artwork";
	GameBoxart original = sonic();
	const json matches = {{LaunchBoxCatalog::identity(original), {
		{"id", 2416}, {"name", "Sonic Adventure"}, {"url", "https://images.launchbox-app.com/jp.jpg"}, {"path", image}}}};
	std::ofstream(directory / "launchbox-matches.json") << json({{"version", 1}, {"matches", matches}}).dump();
	LaunchBox scraper;
	ASSERT_TRUE(scraper.initialize(directory.string() + "/"));
	GameBoxart renamed = original;
	renamed.fileName = "Renamed.chd";
	renamed.gamePath = "/somewhere/else/Renamed.chd";
	scraper.scrape(renamed);
	EXPECT_TRUE(renamed.scraped);
	EXPECT_EQ(2416, renamed.launchboxId);
	EXPECT_EQ(image, renamed.boxartPath);
	EXPECT_FALSE(std::filesystem::exists(directory / "launchbox-catalog.json"));
	std::filesystem::remove_all(directory);
}

// Optional real-export check, using a user-supplied local copy, with no network.
TEST(LaunchBoxCatalogTest, LocalExportSmokeCheck)
{
	const char *path = std::getenv("HOLLYCAST_LAUNCHBOX_EXPORT");
	if (path == nullptr) GTEST_SKIP() << "Set HOLLYCAST_LAUNCHBOX_EXPORT to Metadata.zip";
	std::ifstream file(path, std::ios::binary);
	std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	ZipArchive zip;
	ASSERT_TRUE(zip.Open(bytes.data(), bytes.size()));
	std::unique_ptr<ArchiveFile> xml(zip.OpenFile("Metadata.xml"));
	ASSERT_NE(nullptr, xml);
	LaunchBoxCatalog catalog;
	catalog.read([&](char *buffer, size_t size) { return xml->Read(buffer, static_cast<u32>(size)); });
	GameBoxart item = sonic();
	const auto match = catalog.match(item);
	EXPECT_NE(0, match.id);
	EXPECT_EQ("Sonic Adventure", match.name);
	EXPECT_FALSE(match.url.empty());
}

// Explicitly opt in: this downloads the current public catalog and one cover.
TEST(LaunchBoxCacheTest, LiveProviderSmokeCheck)
{
	if (std::getenv("HOLLYCAST_LAUNCHBOX_LIVE") == nullptr)
		GTEST_SKIP() << "Set HOLLYCAST_LAUNCHBOX_LIVE to exercise public downloads";
	const auto directory = std::filesystem::temp_directory_path()
		/ ("hollycast-launchbox-live-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	GameBoxart item = sonic();
	{
		LaunchBox scraper;
		ASSERT_TRUE(scraper.initialize(directory.string() + "/"));
		ASSERT_NO_THROW(scraper.scrape(item));
	}
	ASSERT_TRUE(item.scraped);
	ASSERT_NE(0, item.launchboxId);
	int width = 0, height = 0, channels = 0;
	ASSERT_TRUE(stbi_info(item.boxartPath.c_str(), &width, &height, &channels));
	EXPECT_GT(width, 500);
	EXPECT_GT(height, 500);
	EXPECT_TRUE(std::filesystem::exists(directory / "launchbox-catalog.json"));
	// Removing the catalog proves the restarted provider can restore its match
	// directly, without scanning metadata or relying on the renamed filename.
	std::filesystem::remove(directory / "launchbox-catalog.json");
	GameBoxart renamed = sonic();
	renamed.fileName = "Totally different.chd";
	renamed.gamePath = "/moved/Totally different.chd";
	LaunchBox restarted;
	ASSERT_TRUE(restarted.initialize(directory.string() + "/"));
	ASSERT_NO_THROW(restarted.scrape(renamed));
	EXPECT_EQ(item.launchboxId, renamed.launchboxId);
	EXPECT_EQ(item.boxartPath, renamed.boxartPath);
	EXPECT_FALSE(std::filesystem::exists(directory / "launchbox-catalog.json"));
	std::filesystem::remove_all(directory);
}
