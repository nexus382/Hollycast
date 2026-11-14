// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/library.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include <algorithm>

using namespace library;

TEST(LibraryTest, AllPreservesOrderingAndContinuePlayingSortsSavedGames)
{
	const std::vector<Entry> entries = {
		{"/games/z.cdi", "Zulu", 40},
		{"/bios/dc_boot.bin", "BIOS", 900, true},
		{"/games/a.cdi", "Alpha", 40},
		{"/games/old.cdi", "Old", 10},
		{"/games/never.cdi", "Never", 0},
		{"", "Missing path", 50},
		{"/games/alpha.cdi", "Alpha", 40},
	};
	EXPECT_EQ(selectGames(entries, View::All, {}), (std::vector<size_t>{0, 1, 2, 3, 4, 5, 6}));
	EXPECT_EQ(selectGames(entries, View::ContinuePlaying, {}),
			(std::vector<size_t>{2, 6, 0, 3}));
}

TEST(LibraryTest, FavouritesUseFullOpaquePathIdentityAndPreserveLibraryOrder)
{
	const std::vector<Entry> entries = {
		{"/one/game.cdi", "Same title", 1},
		{"/two/game.cdi", "Same title", 2},
		{"content://provider/tree/7/document/game.cdi", "Remote", 3},
		{"/bios/game.cdi", "Favourite device", 4, true},
		{"", "No path", 5},
	};
	const std::vector<std::string> favourites = {
		"/two/game.cdi", "/bios/game.cdi", "content://provider/tree/7/document/game.cdi", ""
	};
	EXPECT_EQ(selectGames(entries, View::Favourites, favourites), (std::vector<size_t>{1, 2}));
	EXPECT_TRUE(isFavourite("/one/game.cdi", std::vector<std::string>{"/one/game.cdi"}));
	EXPECT_FALSE(isFavourite("game.cdi", favourites));
}

TEST(LibraryTest, SettingFavouriteDeduplicatesRemovesAllCopiesAndIgnoresEmptyPaths)
{
	std::vector<std::string> favourites = {"/games/a.cdi", "/games/a.cdi", "/games/b.cdi"};
	setFavourite("/games/a.cdi", true, favourites);
	EXPECT_EQ(favourites, (std::vector<std::string>{"/games/a.cdi", "/games/b.cdi"}));
	setFavourite("/games/c.cdi", true, favourites);
	EXPECT_EQ(favourites.back(), "/games/c.cdi");
	setFavourite("", true, favourites);
	EXPECT_EQ(favourites.size(), 3u);
	setFavourite("/games/a.cdi", false, favourites);
	EXPECT_EQ(favourites, (std::vector<std::string>{"/games/b.cdi", "/games/c.cdi"}));
	setFavourite("", false, favourites);
	EXPECT_EQ(favourites.size(), 2u);
}

TEST(LibraryTest, ConfiguredFavouritesCanBeSavedAndLoaded)
{
	const std::string key = "LibraryFavourites";
	const auto originalOption = config::LibraryFavourites.get();
	const auto originalEntries = config::getEntries("ui");
	const bool hadOriginalEntry = std::find(originalEntries.begin(), originalEntries.end(), key) != originalEntries.end();
	const std::string originalConfigValue = config::loadStr("ui", key);
	const bool originalWasTransient = config::isTransient("ui", key);
	struct RestoreConfig {
		std::vector<std::string> option;
		bool hadEntry;
		std::string value;
		bool transient;
		~RestoreConfig() {
			config::setAutoSave(false);
			if (hadEntry) {
				if (transient)
					config::setTransient("ui", "LibraryFavourites", value);
				else
					config::saveStr("ui", "LibraryFavourites", value);
			}
			else
				config::deleteEntry("ui", "LibraryFavourites");
			config::LibraryFavourites = option;
			config::setAutoSave(true);
		}
	} restore{originalOption, hadOriginalEntry, originalConfigValue, originalWasTransient};

	const std::vector<std::string> expected = {
		"/games/a.cdi", "content://provider/tree/7/document/game.cdi",
		"/games/odd; \"é遊戲\".cdi"
	};
	config::setAutoSave(false);
	config::LibraryFavourites = expected;
	config::LibraryFavourites.save();
	config::LibraryFavourites = {};
	config::LibraryFavourites.load();
	EXPECT_EQ(config::LibraryFavourites.get(), expected);
}

TEST(LibraryTest, ExplicitLaunchChoicesOverrideAutomaticLoadAndRequireAllowedState)
{
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, true, true, false), AutoLoadAction::Load);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Fresh, true, true, true), AutoLoadAction::StartFresh);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Default, true, true, false), AutoLoadAction::Ask);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Default, true, true, true), AutoLoadAction::Load);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, false, true, true), AutoLoadAction::RejectResume);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, true, false, true), AutoLoadAction::RejectResume);
}

TEST(LibraryTest, ResumeUsesLoadedEligibilityAndCannotFallThroughToNetworkStartup)
{
	const bool previousAchievements = config::EnableAchievements;
	const bool previousHardcore = config::AchievementsHardcoreMode;
	const bool previousRollback = config::GGPOEnable;
	config::EnableAchievements = true;
	config::AchievementsHardcoreMode = true;
	config::GGPOEnable = true;
	// Global preferences must not override the selected title's effective eligibility.
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, true, true, false), AutoLoadAction::Load);
	EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, true, false, false), AutoLoadAction::RejectResume);
	config::EnableAchievements = previousAchievements;
	config::AchievementsHardcoreMode = previousHardcore;
	config::GGPOEnable = previousRollback;

	for (bool automaticLoad : {false, true})
	{
		EXPECT_EQ(initialAutoLoadAction(LaunchMode::Resume, true, true, automaticLoad, true), AutoLoadAction::RejectResume);
		EXPECT_EQ(initialAutoLoadAction(LaunchMode::Default, true, true, automaticLoad, true), AutoLoadAction::StartNetwork);
		EXPECT_EQ(initialAutoLoadAction(LaunchMode::Fresh, true, true, automaticLoad, true), AutoLoadAction::StartNetwork);
	}
}

TEST(LibraryTest, ContinuePlayingIncludesUnplayedSavedGamesAndLeavesOtherViewsUnchanged)
{
	const std::vector<Entry> entries = {
		{"/games/played.cdi", "Played without a state", 0},
		{"/games/manual.cdi", "Unplayed with manual state", 300},
		{"/games/auto.cdi", "Unplayed with autosave", 400},
		{"/games/old.cdi", "Older saved game", 100},
		{"/device", "Device", 500, true},
	};
	EXPECT_EQ(selectGames(entries, View::ContinuePlaying, {}), (std::vector<size_t>{2, 1, 3}));
	EXPECT_EQ(selectGames(entries, View::All, {}), (std::vector<size_t>{0, 1, 2, 3, 4}));
	EXPECT_EQ(selectGames(entries, View::Favourites, {"/games/played.cdi", "/games/manual.cdi"}),
			(std::vector<size_t>{0, 1}));
}

TEST(LibraryTest, FilteredContinuePlayingChecksEachDiscoveredGameOnlyOnce)
{
	std::vector<Entry> entries;
	for (size_t i = 0; i < 10000; ++i)
		entries.push_back({"/games/" + std::to_string(i) + ".cdi", "Game", 0});

	FilteredGames filtered;
	size_t checks = 0;
	const auto matches = [&](size_t index) {
		++checks;
		return index % 3 == 0;
	};
	filtered.rebuild(entries, View::ContinuePlaying, {}, matches);
	EXPECT_EQ(checks, 0u);
	EXPECT_TRUE(filtered.getRows().empty());

	for (size_t start = 0; start < entries.size(); start += 32)
	{
		std::vector<std::string> changedPaths;
		for (size_t i = start; i < std::min(start + 32, entries.size()); ++i)
		{
			entries[i].latestStateDate = static_cast<time_t>(i + 1);
			changedPaths.push_back(entries[i].path);
		}
		// Artwork and save discovery can report the same source in one frame.
		changedPaths.push_back(changedPaths.front());
		filtered.update(entries, changedPaths, matches);
	}

	EXPECT_EQ(checks, entries.size());
	std::vector<size_t> expected;
	for (size_t i = entries.size(); i-- > 0;)
		if (i % 3 == 0)
			expected.push_back(i);
	EXPECT_EQ(filtered.getRows(), expected);
}

TEST(LibraryTest, FilteredSaveDiscoveryPreservesTiesAndDuplicateSourceRows)
{
	std::vector<Entry> entries = {
		{"/games/shared.cdi", "Same", 0},
		{"/games/z.cdi", "Zulu", 100},
		{"/games/a.cdi", "Alpha", 100},
		{"/games/shared.cdi", "Same", 0},
		{"/device", "Device", 100, true},
		{"", "Missing path", 100},
	};
	FilteredGames filtered;
	size_t checks = 0;
	const auto matches = [&](size_t) { ++checks; return true; };
	filtered.rebuild(entries, View::ContinuePlaying, {}, matches);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{2, 1}));
	checks = 0;
	entries[0].latestStateDate = entries[3].latestStateDate = 100;
	filtered.update(entries, {entries[0].path, entries[0].path, "/device"}, matches);
	EXPECT_EQ(checks, 2u);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{2, 0, 3, 1}));
}

TEST(LibraryTest, FilteredArtworkUpdatesUseFullPathsAndAddOrRemoveEverySourceRow)
{
	const std::vector<Entry> entries = {
		{"/one/game.cdi", "Game", 0},
		{"/two/game.cdi", "Game", 0},
		{"/one/game.cdi", "Other row", 0},
	};
	std::vector<bool> artworkMatches(entries.size(), false);
	size_t checks = 0;
	const auto matches = [&](size_t index) { ++checks; return artworkMatches[index]; };
	FilteredGames filtered;
	filtered.rebuild(entries, View::All, {}, matches);
	EXPECT_TRUE(filtered.getRows().empty());
	checks = 0;
	artworkMatches[0] = artworkMatches[2] = true;
	filtered.update(entries, {"/one/game.cdi", "/one/game.cdi", "game.cdi"}, matches);
	EXPECT_EQ(checks, 2u);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{0, 2}));

	checks = 0;
	artworkMatches[0] = artworkMatches[2] = false;
	artworkMatches[1] = true;
	filtered.update(entries, {"/one/game.cdi", "/two/game.cdi"}, matches);
	EXPECT_EQ(checks, 3u);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{1}));
}

TEST(LibraryTest, FilteredViewsRebuildAfterInvalidationAndScannerReplacement)
{
	std::vector<Entry> entries = {
		{"/games/a.cdi", "Alpha", 10},
		{"/games/b.cdi", "Beta", 20},
		{"/device", "Device", 30, true},
		{"", "BIOS", 0, true},
	};
	size_t checks = 0;
	const auto matches = [&](size_t) { ++checks; return true; };
	FilteredGames filtered;
	filtered.rebuild(entries, View::Favourites, {"/games/b.cdi", "/device"}, matches);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{1}));
	checks = 0;
	filtered.update(entries, {"/games/a.cdi", "/device"}, matches);
	EXPECT_EQ(checks, 0u);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{1}));

	filtered.rebuild(entries, View::All, {}, matches);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{0, 1, 2, 3}));
	filtered.rebuild(entries, View::ContinuePlaying, {}, matches);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{1, 0}));
	entries[0].latestStateDate = entries[1].latestStateDate = 0;
	filtered.rebuild(entries, View::ContinuePlaying, {}, matches);
	EXPECT_TRUE(filtered.getRows().empty());

	entries = {{"/new/game.cdi", "New", 50}};
	filtered.rebuild(entries, View::All, {}, matches);
	EXPECT_EQ(filtered.getRows(), (std::vector<size_t>{0}));
	checks = 0;
	filtered.update(entries, {"/games/a.cdi", "/games/b.cdi"}, matches);
	EXPECT_EQ(checks, 0u);
	filtered.rebuild(entries, View::All, {}, [](size_t) { return false; });
	EXPECT_TRUE(filtered.getRows().empty());
}
