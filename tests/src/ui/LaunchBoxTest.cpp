/* Copyright 2026 The Hollycast Authors. SPDX-License-Identifier: GPL-2.0-or-later */
#include "gtest/gtest.h"
#include "ui/boxart/launchbox.h"
#include "archive/ZipArchive.h"
#include "oslib/oslib.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
const std::string fixture = R"xml(<?xml version="1.0"?><LaunchBox>
<Game><Name>Sonic Adventure</Name><DatabaseID>2416</DatabaseID><Platform>Sega Dreamcast</Platform><Developer>Sonic Team</Developer><Publisher>Sega</Publisher><Genres>Platform</Genres><MaxPlayers>1</MaxPlayers><Overview>Fixture description</Overview><ReleaseDate>1998-12-23</ReleaseDate></Game>
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
<GameImage><DatabaseID>2416</DatabaseID><FileName>disc.png</FileName><Type>Disc</Type><Region>Japan</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>case.png</FileName><Type>Box - 3D</Type><Region>Japan</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>spine.png</FileName><Type>Box - Spine</Type><Region>Japan</Region></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>screen.png</FileName><Type>Screenshot - Gameplay</Type></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>title.png</FileName><Type>Screenshot - Game Title</Type></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>logo.png</FileName><Type>Clear Logo</Type></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>banner.png</FileName><Type>Banner</Type></GameImage>
<GameImage><DatabaseID>2416</DatabaseID><FileName>fanart.png</FileName><Type>Fanart - Background</Type></GameImage>
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
 EXPECT_TRUE(match.media.count("physical"));
 EXPECT_TRUE(match.media.count("case"));
 EXPECT_TRUE(match.media.count("spine"));
 if (const char *output = std::getenv("HOLLYCAST_LAUNCHBOX_EXPORT_INDEX"))
 {
  json data = catalog.toJson(); data["downloaded_at"] = std::time(nullptr);
  std::ofstream(output) << data.dump();
 }
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

namespace {
class LaunchBoxMediaTest : public ::testing::Test {
protected:
 void SetUp() override {
  root = std::filesystem::temp_directory_path() / ("hollycast-launchbox-media-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  LaunchBoxCatalog catalog; readXml(catalog, fixture);
  auto data = catalog.toJson(); data["downloaded_at"] = std::time(nullptr);
  std::ofstream(root / "launchbox-catalog.json") << data.dump();
  item = sonic();
 }
 void TearDown() override { std::filesystem::remove_all(root); }
 LaunchBox source() {
  return LaunchBox([&](const std::string& url, std::vector<u8>& bytes, std::string& type) {
   ++downloads;
   if (url.find("Metadata.zip") != std::string::npos) { ADD_FAILURE() << "The cached catalog should be reused"; return 500; }
   if (failLogo && url.find("logo.png") != std::string::npos) return 503;
   type = "image/png"; const u8 pixel[] = {1, 2, 3, 255};
   stbi_write_png_to_func([](void* context, void* data, int size) {
    auto& bytes = *static_cast<std::vector<u8>*>(context);
    const auto *first = static_cast<u8*>(data); bytes.insert(bytes.end(), first, first + size);
   }, &bytes, 1, 1, 4, pixel, 4);
   return 200;
  });
 }
 std::filesystem::path root; GameBoxart item; int downloads = 0; bool failLogo = false;
};

TEST_F(LaunchBoxMediaTest, CoversStayAutomaticAndManualRequestsFillDistinctSlots)
{
 auto scraper = source(); ASSERT_TRUE(scraper.initialize(root.string()));
 scraper.scrape(item);
 ASSERT_EQ(1, downloads); ASSERT_EQ(1u, item.launchboxMedia.size());
 const auto cover = item.boxartPath;
 item.requestedMedia = artwork::allMedia; scraper.scrape(item);
 EXPECT_EQ(10, downloads); EXPECT_EQ(10u, item.launchboxMedia.size());
 EXPECT_EQ(cover, item.boxartPath);
 EXPECT_NE(item.launchboxMedia.at("physical"), item.launchboxMedia.at("case"));
 EXPECT_NE(item.launchboxMedia.at("backcover"), item.launchboxMedia.at("spine"));
 EXPECT_EQ("Sonic Team", item.developer); EXPECT_EQ("1", item.players);
 EXPECT_EQ("Fixture description", item.overview);
 GameBoxart restored(item.to_json(root.string() + '/'), root.string() + '/');
 EXPECT_EQ(item.launchboxMedia, restored.launchboxMedia);
 EXPECT_EQ(LaunchBox::supportedMedia, restored.launchboxCheckedMedia);
 EXPECT_FALSE(LaunchBox::needsMedia(restored, artwork::allMedia));
 // Rename and remove the catalog: the expanded match, images and metadata survive.
 std::filesystem::remove(root / "launchbox-catalog.json");
 GameBoxart renamed = sonic(); renamed.fileName = "Renamed.chd"; renamed.requestedMedia = artwork::allMedia;
 auto restarted = source(); ASSERT_TRUE(restarted.initialize(root.string())); restarted.scrape(renamed);
 EXPECT_EQ(item.launchboxMedia, renamed.launchboxMedia);
 EXPECT_EQ(item.developer, renamed.developer); EXPECT_EQ(10, downloads);
}

TEST_F(LaunchBoxMediaTest, PartialFailureKeepsCompletedMediaAndRetriesOnlyPendingTypes)
{
 failLogo = true; item.requestedMedia = artwork::allMedia;
 auto scraper = source(); ASSERT_TRUE(scraper.initialize(root.string()));
 EXPECT_THROW(scraper.scrape(item), std::runtime_error);
 EXPECT_EQ(1u, item.launchboxMedia.count("cover"));
 EXPECT_FALSE(item.launchboxCheckedMedia & (1u << 2)); EXPECT_FALSE(scraper.ready());
 failLogo = false; auto retry = source(); ASSERT_TRUE(retry.initialize(root.string())); retry.scrape(item);
 EXPECT_EQ(10u, item.launchboxMedia.size()); EXPECT_EQ(11, downloads);
}

TEST(LaunchBoxCatalogTest, SharedSlotsHaveDiscAndBoxImagesAndKeepMixUnavailable)
{
 LaunchBoxCatalog catalog; readXml(catalog, fixture, 17);
 const auto match = catalog.match(sonic());
 EXPECT_EQ(10u, match.media.size());
 EXPECT_EQ("https://images.launchbox-app.com/disc.png", match.media.at("physical"));
 EXPECT_EQ("https://images.launchbox-app.com/case.png", match.media.at("case"));
 EXPECT_EQ("https://images.launchbox-app.com/back.jpg", match.media.at("backcover"));
 EXPECT_EQ("https://images.launchbox-app.com/spine.png", match.media.at("spine"));
 EXPECT_EQ(0u, match.media.count("miximage"));
}
}
