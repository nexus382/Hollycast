// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "ui/boxart/gamesdb.h"
#include "ui/game_scanner.h"
#include <stb_image_write.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
int reply(std::vector<u8>& bytes, const json& data)
{
	const auto body = json{{"code", 200}, {"data", data}}.dump();
	bytes.assign(body.begin(), body.end());
	return 200;
}
std::vector<u8> png()
{
	std::vector<u8> bytes;
	const u8 pixel[] = {100, 120, 140, 255};
	stbi_write_png_to_func([](void* context, void* data, int size) {
		auto& output = *static_cast<std::vector<u8>*>(context);
		const auto* first = static_cast<u8*>(data);
		output.insert(output.end(), first, first + size);
	}, &bytes, 1, 1, 4, pixel, 4);
	return bytes;
}

class TheGamesDbTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		root = std::filesystem::temp_directory_path() / ("hollycast-tgdb-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(root);
		game = {{"id", 42}, {"game_title", "Crazy Taxi"}, {"platform", 16}, {"release_date", "2000-02-01"},
			{"overview", "First line\\r\\nSecond line"}, {"players", 1}, {"developers", {10}}, {"publishers", {20}}, {"genres", {30}}};
		images = json::array();
		for (const char* type : {"boxart", "clearlogo", "banner", "fanart", "screenshot", "titlescreen"})
			images.push_back({{"type", type}, {"side", type == std::string("boxart") ? json("front") : json(nullptr)}, {"filename", std::string(type) + "/image.png"}});
		images.insert(images.begin(), json{{"type", "boxart"}, {"side", "back"}, {"filename", "boxart/back.png"}});
		item.fileName = "Crazy Taxi.chd"; item.name = item.searchName = "Crazy Taxi";
		item.gamePath = "/games/Crazy Taxi.chd"; item.tgdbId = 42;
		item.requestedMedia = artwork::allMedia;
	}
	void TearDown() override { std::filesystem::remove_all(root); }
	int respond(const std::string& url, std::vector<u8>& bytes, std::string& contentType)
	{
		++requests;
		if (url.find("cdn.example/") != std::string::npos)
		{
			downloads.push_back(url);
			if (missingImage) return 404;
			if (failImage && url.find("clearlogo/") != std::string::npos) return 503;
			contentType = "image/png"; bytes = png();
			return 200;
		}
		contentType = "application/json";
		if (url.find("Platforms/ByPlatformName") != std::string::npos)
			return reply(bytes, {{"platforms", json::array({{{"id", 16}, {"name", "Sega Dreamcast"}}, {{"id", 23}, {"name", "Arcade"}}})}});
		if (url.find("Games/Images") != std::string::npos)
			return reply(bytes, {{"base_url", {{"original", "https://cdn.example/original/"}, {"large", "https://cdn.example/large/"}, {"thumb", "https://cdn.example/thumb/"}}}, {"images", {{"42", images}}}});
		for (auto category : {"developers", "publishers", "genres"})
		{
			std::string endpoint = category; endpoint[0] = std::toupper(endpoint[0]);
			if (url.find("/" + endpoint + "?") != std::string::npos)
			{
				++referenceRequests;
				const int id = endpoint == "Developers" ? 10 : endpoint == "Publishers" ? 20 : 30;
				return reply(bytes, {{category, {{std::to_string(id), {{"id", id}, {"name", endpoint + " name"}}}}}});
			}
		}
		if (url.find("Games/") != std::string::npos) return reply(bytes, {{"games", ambiguous ? json::array({game, otherGame}) : json::array({game})}});
		ADD_FAILURE() << "Unexpected API endpoint";
		return 500;
	}
	TheGamesDb scraper()
	{
		return TheGamesDb([&](const std::string& url, std::vector<u8>& bytes, std::string& type) { return respond(url, bytes, type); });
	}
	std::filesystem::path root;
	GameBoxart item;
	json game, otherGame, images;
	std::vector<std::string> downloads;
	int requests = 0, referenceRequests = 0;
	bool failImage = false, missingImage = false, ambiguous = false;
};

TEST_F(TheGamesDbTest, OriginalImagesAndMetadataUseDistinctPersistentSlots)
{
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string())); source.scrape(item);
	ASSERT_EQ(item.tgdbMedia.size(), 7u);
	EXPECT_EQ(downloads.size(), 7u);
	for (const auto& url : downloads) EXPECT_NE(url.find("/original/"), std::string::npos);
	EXPECT_NE(item.tgdbMedia.at("cover"), item.tgdbMedia.at("backcover"));
	EXPECT_EQ(item.boxartPath, item.tgdbMedia.at("cover"));
	EXPECT_EQ(item.developer, "Developers name"); EXPECT_EQ(item.publisher, "Publishers name");
	EXPECT_EQ(item.genre, "Genres name"); EXPECT_EQ(item.players, "1");
	EXPECT_EQ(item.overview, "First line\nSecond line");
	EXPECT_EQ(referenceRequests, 3);
	const auto stored = item.to_json(root.string() + '/');
	EXPECT_EQ(stored.dump().find("apikey"), std::string::npos);
	const GameBoxart loaded(stored, root.string() + '/');
	EXPECT_EQ(loaded.tgdbId, 42u); EXPECT_EQ(loaded.tgdbMedia, item.tgdbMedia);
	EXPECT_EQ(loaded.tgdbCheckedMedia, TheGamesDb::supportedMedia);
	EXPECT_FALSE(TheGamesDb::needsMedia(loaded, artwork::allMedia));
	const int before = requests; source.scrape(item); EXPECT_EQ(requests, before);
}

TEST_F(TheGamesDbTest, AutomaticFetchDownloadsOnlyCoverAndManualRequestAddsExtras)
{
	item.requestedMedia = artwork::coverMedia;
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string())); source.scrape(item);
	ASSERT_EQ(downloads.size(), 1u); ASSERT_EQ(item.tgdbMedia.size(), 1u);
	const std::string cover = item.boxartPath;
	item.requestedMedia = artwork::allMedia;
	auto restarted = scraper(); ASSERT_TRUE(restarted.initialize(root.string())); restarted.scrape(item);
	EXPECT_EQ(item.tgdbMedia.size(), 7u); EXPECT_EQ(downloads.size(), 7u);
	EXPECT_EQ(item.boxartPath, cover);
	EXPECT_EQ(referenceRequests, 3) << "Reference names should be reused across launches";
}

TEST_F(TheGamesDbTest, MissingTypesAreRememberedAndDeletedDownloadsBecomePending)
{
	images.erase(images.begin() + 1, images.end()); // Only a real back-cover scan remains.
	item.boxartPath = "existing-art.png";
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string())); source.scrape(item);
	EXPECT_EQ(item.boxartPath, "existing-art.png");
	EXPECT_EQ(item.tgdbMedia.size(), 1u); EXPECT_EQ(item.tgdbMedia.count("cover"), 0u);
	EXPECT_FALSE(TheGamesDb::needsMedia(item, artwork::allMedia));
	std::filesystem::remove(item.tgdbMedia.at("backcover"));
	EXPECT_TRUE(TheGamesDb::needsMedia(item, artwork::allMedia));
	EXPECT_FALSE(TheGamesDb::needsMedia(item, (1u << 1) | (1u << 4) | (1u << 8) | (1u << 10)));
	missingImage = true;
	source.scrape(item);
	EXPECT_TRUE(item.tgdbMedia.empty());
	EXPECT_FALSE(TheGamesDb::needsMedia(item, artwork::allMedia)) << "A remote 404 should not create an endless retry";
}

TEST_F(TheGamesDbTest, FailureRetainsCompletedImagesAndDoesNotCacheFailedMedia)
{
	failImage = true;
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string()));
	EXPECT_THROW(source.scrape(item), std::runtime_error);
	ASSERT_EQ(item.tgdbMedia.count("cover"), 1u); EXPECT_EQ(item.tgdbMedia.count("title"), 0u);
	EXPECT_TRUE(item.tgdbCheckedMedia & artwork::coverMedia);
	EXPECT_FALSE(item.tgdbCheckedMedia & (1u << 2)); EXPECT_FALSE(source.ready());
	const auto cover = item.boxartPath;
	failImage = false;
	auto retry = scraper(); ASSERT_TRUE(retry.initialize(root.string())); retry.scrape(item);
	EXPECT_EQ(item.boxartPath, cover); EXPECT_EQ(item.tgdbMedia.size(), 7u);
}

TEST_F(TheGamesDbTest, AmbiguousSerialMatchesDoNotReplaceExistingArtwork)
{
	item.tgdbId = 0; item.uniqueId = "MK-51035"; item.boxartPath = "old-art.png";
	game["uids"] = json::array({{{"uid", item.uniqueId}}});
	otherGame = game; otherGame["id"] = 99; ambiguous = true;
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string())); source.scrape(item);
	EXPECT_EQ(item.boxartPath, "old-art.png"); EXPECT_EQ(item.tgdbId, 0u);
	EXPECT_TRUE(downloads.empty()); EXPECT_FALSE(TheGamesDb::needsMedia(item, artwork::allMedia));
}

TEST_F(TheGamesDbTest, PartialResponsesPreserveCachedMetadataAndOtherProviderImages)
{
	game = {{"id", 42}, {"game_title", "Crazy Taxi"}};
	item.developer = "Prior developer"; item.players = "2";
	item.screenScraperId = 123; item.screenScraperMedia["physical"] = "disc.png";
	auto source = scraper(); ASSERT_TRUE(source.initialize(root.string())); source.scrape(item);
	EXPECT_EQ(item.developer, "Prior developer"); EXPECT_EQ(item.players, "2");
	const GameBoxart loaded(item.to_json(root.string() + '/'), root.string() + '/');
	EXPECT_EQ(loaded.screenScraperId, 123u); EXPECT_EQ(std::filesystem::path(loaded.screenScraperMedia.at("physical")).lexically_normal(), (root / "disc.png").lexically_normal());
	EXPECT_EQ(loaded.tgdbMedia, item.tgdbMedia);
}
}

TEST_F(TheGamesDbTest, MetadataOnlySkipsImageCatalogueAndDownloads)
{
	int imageRequests = 0;
	TheGamesDb scraper([&](const std::string& url, std::vector<u8>& bytes, std::string& type) {
		if (url.find("Games/Images") != std::string::npos) ++imageRequests;
		return respond(url, bytes, type);
	});
	ASSERT_TRUE(scraper.initialize(root.u8string() + '/'));
	item.requestedMedia = artwork::metadataMedia;
	scraper.scrape(item); scraper.scrape(item);
	EXPECT_TRUE(item.tgdbSearched);
	EXPECT_EQ(item.players, "1");
	EXPECT_EQ(imageRequests, 0);
	EXPECT_TRUE(downloads.empty());
	EXPECT_EQ(item.tgdbCheckedMedia, 0u);
}
