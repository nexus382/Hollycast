// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "ui/boxart/arcade_scraper.h"
#include "ui/boxart/artwork_adapter.h"
#include "ui/boxart/artwork_media.h"
#include "ui/boxart/gamesdb.h"
#include "ui/boxart/launchbox.h"
#include <filesystem>
#include <fstream>
#include <chrono>

namespace {
class StubScraper : public Scraper {
public:
 explicit StubScraper(std::function<void(GameBoxart&)> work, std::function<void()> end = {})
  : work(std::move(work)), end(std::move(end)) {}
 void scrape(GameBoxart& item) override { work(item); }
 void endBulk() override { if (end) end(); }
 std::function<void(GameBoxart&)> work;
 std::function<void()> end;
};
class ArtworkAdapterTest : public ::testing::Test {
protected:
 void SetUp() override {
  root = std::filesystem::temp_directory_path() / ("hollycast-adapter-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
 }
 void TearDown() override { std::filesystem::remove_all(root); }
 std::string image(const std::string& name) {
  const auto path = root / (name + ".png"); std::ofstream(path) << "cached"; return path.string();
 }
 std::filesystem::path root;
};

TEST_F(ArtworkAdapterTest, TgdbFillsOnlyMissingSupportedSlotsAndKeepsPrimaryMetadata)
{
 u32 requested = 0;
 ArtworkAdapter adapter(2, std::make_unique<StubScraper>([&](GameBoxart& item) {
  item.launchboxId = 12; item.launchboxCheckedMedia = LaunchBox::supportedMedia;
  item.name = "LaunchBox name"; item.developer = "Primary developer";
  for (const char* kind : {"cover", "physical", "case", "backcover", "spine"}) item.launchboxMedia[kind] = image(kind);
 }), std::make_unique<StubScraper>([&](GameBoxart& item) {
  requested = item.requestedMedia; item.tgdbCheckedMedia |= requested;
  item.name = "Fallback name"; item.developer = "Fallback developer"; item.publisher = "Fallback publisher";
  item.tgdbMedia["screenshot"] = image("fallback-screen");
 }));
 ASSERT_TRUE(adapter.initialize(root.string()));
 GameBoxart item; item.requestedMedia = artwork::allMedia;
 adapter.scrape(item);
 EXPECT_EQ(TheGamesDb::supportedMedia & ~(artwork::coverMedia | (1u << 9)), requested);
 EXPECT_EQ("LaunchBox name", item.name); EXPECT_EQ("Primary developer", item.developer);
 EXPECT_EQ("Fallback publisher", item.publisher);
 EXPECT_TRUE(item.launchboxMedia.count("physical")); EXPECT_TRUE(item.tgdbMedia.count("screenshot"));
 EXPECT_EQ(artwork::allMedia, item.requestedMedia);
 EXPECT_FALSE(ArtworkAdapter::needsMedia(item, 2, artwork::allMedia));
}

TEST_F(ArtworkAdapterTest, PausedFallbackKeepsLaunchBoxAvailableForOtherGames)
{
 auto authorization = std::make_shared<TheGamesDbAuthorization>();
 int calls = 0;
 auto fallback = std::make_unique<TheGamesDb>([&](const std::string&, std::vector<u8>&, std::string&) {
  ++calls;
  return 403;
 }, authorization);
 ArtworkAdapter adapter(2, std::make_unique<StubScraper>([&](GameBoxart& item) {
  item.launchboxSearched = true;
  item.launchboxCheckedMedia |= artwork::coverMedia;
  if (item.name == "Matched") {
   item.launchboxId = 1;
   item.launchboxMedia["cover"] = image("other-cover");
  }
 }), std::move(fallback));
 ASSERT_TRUE(adapter.initialize(root.string()));
 GameBoxart missing;
 missing.requestedMedia = artwork::coverMedia;
 EXPECT_THROW(adapter.scrape(missing), std::runtime_error);
 EXPECT_TRUE(adapter.ready());
 EXPECT_FALSE(adapter.fallbackReady());
 EXPECT_TRUE(ArtworkAdapter::needsMedia(missing, 2, missing.requestedMedia));
 GameBoxart matched;
 matched.name = "Matched";
 matched.requestedMedia = artwork::coverMedia;
 EXPECT_NO_THROW(adapter.scrape(matched));
 EXPECT_FALSE(ArtworkAdapter::needsMedia(matched, 2, matched.requestedMedia));
 EXPECT_EQ(calls, 1);
 authorization->reset();
 EXPECT_TRUE(adapter.fallbackReady());
}

TEST_F(ArtworkAdapterTest, LegacyArcadeMissSurvivesRestartWithoutSuppressingTgdb)
{
 int calls = 0;
 ArcadeScraper scraper([&](const std::string&, std::vector<u8>&, std::string&) {
  ++calls;
  return 404;
 });
 ASSERT_TRUE(scraper.initialize(root.string() + "/"));
 GameBoxart item;
 item.arcade = true;
 item.fileName = "sample.zip";
 scraper.scrape(item);
 EXPECT_EQ(1, calls);
 EXPECT_FALSE(ArcadeScraper::needsCover(item));

 GameBoxart loaded(item.to_json(root.string()), root.string());
 EXPECT_FALSE(ArcadeScraper::needsCover(loaded));
 EXPECT_TRUE(TheGamesDb::needsMedia(loaded, artwork::coverMedia));
 loaded.uniqueId = "sample";
 EXPECT_FALSE(ArcadeScraper::needsCover(loaded));
 loaded.uniqueId = "Another ID";
 EXPECT_TRUE(ArcadeScraper::needsCover(loaded));
 scraper.scrape(loaded);
 EXPECT_EQ(2, calls);
 EXPECT_FALSE(ArcadeScraper::needsCover(loaded));
}

TEST_F(ArtworkAdapterTest, LegacyArcadeRetriesInvalidImagesAndRetainsDefinitiveMisses)
{
 std::vector<std::string> requests;
 ArcadeScraper scraper([&](const std::string& url, std::vector<u8>& bytes, std::string& type) {
  requests.push_back(url);
  if (url.find("/sample.jpg") != std::string::npos) return 404;
  bytes = {1, 2, 3};
  type = "image/jpeg";
  return 200;
 });
 ASSERT_TRUE(scraper.initialize(root.string() + "/"));
 GameBoxart item;
 item.arcade = true;
 item.fileName = "sample.zip";
 item.uniqueId = "another";
 scraper.scrape(item);
 ASSERT_EQ(2u, requests.size());
 EXPECT_FALSE(ArcadeScraper::needsCover(item));
 EXPECT_TRUE(item.boxartPath.empty());
 EXPECT_TRUE(std::filesystem::is_empty(root));
 scraper.scrape(item);
 EXPECT_EQ(2u, requests.size());

 GameBoxart loaded(item.to_json(root.string()), root.string());
 EXPECT_TRUE(ArcadeScraper::needsCover(loaded));
 scraper.scrape(loaded);
 ASSERT_EQ(3u, requests.size());
 EXPECT_EQ(requests[1], requests[2]);
}

TEST_F(ArtworkAdapterTest, ScreenScraperDefersFallbackAndPlansOnlyMissingRequestedMedia)
{
 int fallbackCalls = 0;
 ArtworkAdapter adapter(1, std::make_unique<StubScraper>([](GameBoxart& item) {
  item.screenScraperCheckedMedia = artwork::allMedia; item.screenScraperSearched = true;
 }), std::make_unique<StubScraper>([&](GameBoxart&) { ++fallbackCalls; }));
 GameBoxart item; item.requestedMedia = artwork::coverMedia;
 adapter.scrape(item);
 EXPECT_EQ(fallbackCalls, 0);
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, item.requestedMedia), 2);
 EXPECT_EQ(ArtworkAdapter::stageMedia(item, 2, item.requestedMedia), artwork::coverMedia);
 item.launchboxCheckedMedia = LaunchBox::supportedMedia; item.launchboxSearched = true;
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, item.requestedMedia), 0);
 item.tgdbCheckedMedia = TheGamesDb::supportedMedia; item.tgdbSearched = true;
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, item.requestedMedia), -1);
 EXPECT_FALSE(ArtworkAdapter::needsMedia(item, 1, item.requestedMedia));
}

TEST_F(ArtworkAdapterTest, PipelineReusesImagesFromAnyProviderAndSkipsUnsupportedTypes)
{
 GameBoxart item; item.screenScraperSearched = true;
 item.screenScraperCheckedMedia = artwork::allMedia;
 item.launchboxMedia["cover"] = image("cover");
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, artwork::coverMedia), -1);
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, artwork::mediaBit("miximage")), -1);
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, artwork::mediaBit("spine")), 2);
 item.launchboxCheckedMedia = LaunchBox::supportedMedia;
 EXPECT_EQ(ArtworkAdapter::nextProvider(item, artwork::mediaBit("spine")), -1);
}

TEST_F(ArtworkAdapterTest, DiscAndBoxRequestsNeverUseAnUnrelatedFallbackImage)
{
 int fallbackCalls = 0;
 ArtworkAdapter adapter(2, std::make_unique<StubScraper>([](GameBoxart& item) {
  item.launchboxCheckedMedia = LaunchBox::supportedMedia;
 }), std::make_unique<StubScraper>([&](GameBoxart&) { ++fallbackCalls; }));
 GameBoxart item; item.requestedMedia = (1u << 4) | (1u << 8) | (1u << 10);
 adapter.scrape(item);
 EXPECT_EQ(0, fallbackCalls); EXPECT_FALSE(ArtworkAdapter::needsMedia(item, 2, item.requestedMedia));
 EXPECT_TRUE(item.launchboxMedia.empty()); EXPECT_TRUE(item.tgdbMedia.empty());
}

TEST_F(ArtworkAdapterTest, EndsFallbackBulkEvenWhenPrimaryCompactionFails)
{
	int fallbackEnds = 0;
	ArtworkAdapter adapter(2,
		std::make_unique<StubScraper>([](GameBoxart&) {}, [] { throw std::runtime_error("snapshot failure"); }),
		std::make_unique<StubScraper>([](GameBoxart&) {}, [&] { ++fallbackEnds; }));
	EXPECT_THROW(adapter.endBulk(), std::runtime_error);
	EXPECT_EQ(1, fallbackEnds);
}

TEST_F(ArtworkAdapterTest, FailedFallbackPreservesPrimaryAndTheOriginalRequest)
{
 ArtworkAdapter adapter(2, std::make_unique<StubScraper>([&](GameBoxart& item) {
  item.launchboxId = 12; item.launchboxCheckedMedia = LaunchBox::supportedMedia;
  item.launchboxMedia["cover"] = image("primary"); item.developer = "Primary";
 }), std::make_unique<StubScraper>([](GameBoxart& item) {
  item.developer = "Fallback"; throw std::runtime_error("temporary error");
 }));
 GameBoxart item; item.requestedMedia = artwork::allMedia;
 EXPECT_THROW(adapter.scrape(item), std::runtime_error);
 EXPECT_EQ(artwork::allMedia, item.requestedMedia); EXPECT_EQ("Primary", item.developer);
 EXPECT_TRUE(item.launchboxMedia.count("cover")); EXPECT_TRUE(ArtworkAdapter::needsMedia(item, 2, artwork::allMedia));
}
TEST_F(ArtworkAdapterTest, UnavailablePrimaryStillDownloadsFallbackAndRetainsRetryState)
{
 ArtworkAdapter adapter(2, std::make_unique<StubScraper>([](GameBoxart&) {
  throw std::runtime_error("primary temporarily unavailable");
 }), std::make_unique<StubScraper>([&](GameBoxart& item) {
  EXPECT_EQ(artwork::coverMedia, item.requestedMedia);
  item.tgdbMedia["cover"] = image("fallback-cover"); item.tgdbCheckedMedia |= artwork::coverMedia;
 }));
 GameBoxart item; item.requestedMedia = artwork::coverMedia;
 EXPECT_THROW(adapter.scrape(item), std::runtime_error);
 EXPECT_TRUE(item.tgdbMedia.count("cover")); EXPECT_EQ(0u, item.launchboxCheckedMedia);
 EXPECT_TRUE(ArtworkAdapter::needsMedia(item, 2, artwork::coverMedia));
}

}
