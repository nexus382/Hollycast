// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
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
 explicit StubScraper(std::function<void(GameBoxart&)> work) : work(std::move(work)) {}
 void scrape(GameBoxart& item) override { work(item); }
 std::function<void(GameBoxart&)> work;
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

TEST_F(ArtworkAdapterTest, UnmatchedPrimaryUsesCoverOnlyFallbackUntilExplicitlyExpanded)
{
 std::vector<u32> requests;
 ArtworkAdapter adapter(1, std::make_unique<StubScraper>([](GameBoxart& item) {
  item.screenScraperCheckedMedia = artwork::allMedia; item.screenScraperSearched = true;
 }), std::make_unique<StubScraper>([&](GameBoxart& item) {
  requests.push_back(item.requestedMedia); item.tgdbCheckedMedia |= item.requestedMedia;
 }));
 GameBoxart item; item.requestedMedia = artwork::coverMedia;
 EXPECT_TRUE(ArtworkAdapter::needsMedia(item, 1, item.requestedMedia));
 adapter.scrape(item);
 ASSERT_EQ(1u, requests.size()); EXPECT_EQ(artwork::coverMedia, requests[0]);
 EXPECT_FALSE(ArtworkAdapter::needsMedia(item, 1, artwork::coverMedia));
 EXPECT_TRUE(ArtworkAdapter::needsMedia(item, 1, artwork::allMedia));
 item.requestedMedia = artwork::allMedia; adapter.scrape(item);
 EXPECT_EQ(TheGamesDb::supportedMedia, requests.back());
 EXPECT_FALSE(ArtworkAdapter::needsMedia(item, 1, artwork::allMedia));
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
