// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/game_scanner.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>

class GameScannerTest : public ::testing::Test
{
protected:
	void SetUp() override {
		oldPaths = config::ContentPath.get();
		oldGamelist = config::GameListPath.get();
		oldArtwork = config::BoxartPath.get();
		oldHideLegacy = config::HideLegacyNaomiRoms.get();
		oldHideDrives = config::loadStr("config", "HideCdromDrives");
		directory = std::filesystem::temp_directory_path()
				/ ("hollycast-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(directory);
		config::ContentPath = std::vector<std::string>{directory.string()};
		config::GameListPath = "";
		config::BoxartPath = "";
		config::HideLegacyNaomiRoms = true;
		config::saveBool("config", "HideCdromDrives", false);
	}
	void TearDown() override {
		scanner.stop();
		config::ContentPath = oldPaths;
		config::GameListPath = oldGamelist;
		config::BoxartPath = oldArtwork;
		config::HideLegacyNaomiRoms = oldHideLegacy;
		if (oldHideDrives.empty()) config::deleteEntry("config", "HideCdromDrives");
		else config::saveStr("config", "HideCdromDrives", oldHideDrives);
		std::filesystem::remove_all(directory);
	}
	std::vector<GameMedia> games() {
		std::lock_guard<std::mutex> lock(scanner.get_mutex());
		std::vector<GameMedia> result;
		for (const auto& game : scanner.get_game_list())
			if (!game.device && !game.path.empty()) result.push_back(game);
		return result;
	}
	GameScanner scanner{[] { return std::vector<std::string>{}; }};
	std::filesystem::path directory;
	std::vector<std::string> oldPaths;
	std::string oldGamelist, oldArtwork, oldHideDrives;
	bool oldHideLegacy;
};

#ifdef USE_HIKARU
TEST_F(GameScannerTest, EnrichesHikaruArchivesAndRetainsTheArcadeSystem)
{
	std::ofstream(directory / "PHARRIER.zip", std::ios::binary) << "fixture";
	std::ofstream(directory / "gamelist.xml") << "<gameList><game><path>PHARRIER.zip</path>"
			"<name>My Planet Harriers</name></game></gameList>";
	config::GameListPath = (directory / "gamelist.xml").string();
	scanner.fetch_game_list_sync();
	const auto scanned = games();
	ASSERT_EQ(1u, scanned.size());
	const auto& game = scanned.front();
	EXPECT_EQ("My Planet Harriers", game.name);
	EXPECT_EQ("PHARRIER.zip", game.fileName);
	EXPECT_EQ("Planet Harriers", game.gameName);
	EXPECT_EQ("Hikaru", game.system);
	EXPECT_TRUE(game.arcade);
	EXPECT_EQ(7u, game.size);
}
#endif

TEST_F(GameScannerTest, RefreshFindsNewAndRemovedGamesEvenWhenTheCountIsUnchanged)
{
	std::ofstream(directory / "Alpha.chd") << "fixture";
	std::ofstream(directory / "Zulu.cdi") << "fixture";
	scanner.fetch_game_list_sync();
	const auto original = games();
	ASSERT_EQ(2u, original.size());
	EXPECT_EQ("Alpha.chd", original.front().fileName);
	const auto generation = scanner.get_generation();
	std::filesystem::remove(directory / "Alpha.chd");
	std::ofstream(directory / "Beta.gdi") << "fixture";
	scanner.fetch_game_list_sync();
	const auto refreshed = games();
	ASSERT_EQ(2u, refreshed.size());
	EXPECT_EQ("Beta.gdi", refreshed.front().fileName);
	EXPECT_GT(scanner.get_generation(), generation);
}

TEST_F(GameScannerTest, LargeNestedLibraryPublishesEveryGameAndACompletedGeneration)
{
	constexpr int count = 10000;
	for (int folder = 0; folder < 10; ++folder)
		std::filesystem::create_directories(directory / std::to_string(folder));
	for (int i = count - 1; i >= 0; --i)
		std::ofstream(directory / std::to_string(i % 10) / ("Game" + std::to_string(i) + ".chd")) << "fixture";
	std::ofstream(directory / "track.bin") << "hidden legacy track";
	std::ofstream(directory / "readme.txt") << "not a game";
	const auto start = std::chrono::steady_clock::now();
	scanner.fetch_game_list_sync();
	RecordProperty("scan_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
	const auto result = games();
	ASSERT_EQ(count, result.size());
	std::set<std::string> unique;
	for (const auto& game : result) unique.insert(game.path);
	EXPECT_EQ(count, unique.size());
	EXPECT_EQ(2u, scanner.get_generation());
	EXPECT_FALSE(scanner.is_scanning());
}

TEST_F(GameScannerTest, RefreshKeepsThePreviousSnapshotUntilTheNewScanPublishes)
{
	std::ofstream(directory / "first.chd") << "fixture";
	scanner.fetch_game_list_sync();
	const auto original = games();
	const auto generation = scanner.get_generation();
	std::ofstream(directory / "second.chd") << "fixture";
	scanner.refresh();
	{
		std::lock_guard<std::mutex> lock(scanner.get_mutex());
		scanner.fetch_game_list();
		EXPECT_EQ(generation, scanner.get_generation());
		EXPECT_EQ(original.front().path, scanner.get_game_list().back().path);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (scanner.get_generation() == generation && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	EXPECT_EQ(2u, games().size());
	EXPECT_EQ(generation + 1, scanner.get_generation());
}

TEST_F(GameScannerTest, ImportedConfiguredGamesPublishBeforeSlowDriveDiscoveryAfterAnEmptyScan)
{
	std::promise<void> discoveringDrives, releaseDrives;
	auto started = discoveringDrives.get_future();
	auto released = releaseDrives.get_future().share();
	int enumerations = 0;
	GameScanner startupScanner([&] {
		if (++enumerations == 1)
			return std::vector<std::string>{};
		discoveringDrives.set_value();
		released.wait();
		return std::vector<std::string>{"test-cdrom"};
	});
	startupScanner.fetch_game_list_sync();
	ASSERT_EQ(1u, startupScanner.get_generation());
	ASSERT_FALSE(startupScanner.is_scanning());
	const auto importedDirectory = directory / "imported";
	std::filesystem::create_directories(importedDirectory);
	std::ofstream(importedDirectory / "game.chd") << "fixture";
	std::ofstream(importedDirectory / "gamelist.xml") << "<gameList><game><path>game.chd</path>"
			"<name>Imported title</name></game></gameList>";
	config::ContentPath = std::vector<std::string>{importedDirectory.string()};
	config::GameListPath = (importedDirectory / "gamelist.xml").string();
	startupScanner.refresh();
	startupScanner.fetch_game_list();
	const auto discoveryStatus = started.wait_for(std::chrono::seconds(5));
	// Release the worker even after a failed expectation so its destructor can join.
	EXPECT_EQ(std::future_status::ready, discoveryStatus);
	{
		std::lock_guard<std::mutex> lock(startupScanner.get_mutex());
		const auto& pending = startupScanner.get_game_list();
		EXPECT_EQ(1u, pending.size());
		if (!pending.empty())
		{
			EXPECT_EQ((importedDirectory / "game.chd").string(), pending.front().path);
			EXPECT_EQ("game.chd", pending.front().name);
		}
		EXPECT_EQ(2u, startupScanner.get_generation());
		EXPECT_TRUE(startupScanner.is_scanning());
	}
	releaseDrives.set_value();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (startupScanner.is_scanning() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	EXPECT_FALSE(startupScanner.is_scanning());
	{
		std::lock_guard<std::mutex> lock(startupScanner.get_mutex());
		const auto& complete = startupScanner.get_game_list();
		auto game = std::find_if(complete.begin(), complete.end(), [](const GameMedia& media) {
			return media.fileName == "game.chd";
		});
		ASSERT_NE(complete.end(), game);
		EXPECT_EQ("Imported title", game->name);
		EXPECT_TRUE(std::any_of(complete.begin(), complete.end(), [](const GameMedia& media) {
			return media.device && media.path == "test-cdrom";
		}));
		EXPECT_EQ(3u, startupScanner.get_generation());
	}
}
