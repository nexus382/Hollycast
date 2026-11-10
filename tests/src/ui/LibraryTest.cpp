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
