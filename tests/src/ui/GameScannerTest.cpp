// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/game_scanner.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

class GameScannerTest : public ::testing::Test
{
protected:
	void SetUp() override {
		oldPaths = config::ContentPath.get();
		oldGamelist = config::GameListPath.get();
		oldArtwork = config::BoxartPath.get();
		oldHideLegacy = config::HideLegacyNaomiRoms.get();
		directory = std::filesystem::temp_directory_path()
				/ ("hollycast-scan-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(directory);
		config::ContentPath = std::vector<std::string>{directory.string()};
		config::GameListPath = "";
		config::BoxartPath = "";
		config::HideLegacyNaomiRoms = true;
	}
	void TearDown() override {
		scanner.stop();
		config::ContentPath = oldPaths;
		config::GameListPath = oldGamelist;
		config::BoxartPath = oldArtwork;
		config::HideLegacyNaomiRoms = oldHideLegacy;
		std::filesystem::remove_all(directory);
	}
	std::vector<GameMedia> games() {
		std::lock_guard<std::mutex> lock(scanner.get_mutex());
		std::vector<GameMedia> result;
		for (const auto& game : scanner.get_game_list())
			if (!game.device && !game.path.empty()) result.push_back(game);
		return result;
	}
	GameScanner scanner;
	std::filesystem::path directory;
	std::vector<std::string> oldPaths;
	std::string oldGamelist, oldArtwork;
	bool oldHideLegacy;
};

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
	EXPECT_EQ(1u, scanner.get_generation());
}

TEST_F(GameScannerTest, RefreshKeepsThePreviousSnapshotUntilTheNewScanPublishes)
{
	std::ofstream(directory / "first.chd") << "fixture";
	scanner.fetch_game_list_sync();
	const auto original = games();
	std::ofstream(directory / "second.chd") << "fixture";
	scanner.refresh();
	{
		std::lock_guard<std::mutex> lock(scanner.get_mutex());
		scanner.fetch_game_list();
		EXPECT_EQ(1u, scanner.get_generation());
		EXPECT_EQ(original.front().path, scanner.get_game_list().back().path);
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (scanner.get_generation() == 1 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	EXPECT_EQ(2u, games().size());
	EXPECT_EQ(2u, scanner.get_generation());
}
