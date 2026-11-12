// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/boxart/boxart.h"
#include "ui/game_scanner.h"
#include "ui/library.h"
#include "ui/library_savestates.h"

#include <chrono>
#include <thread>

TEST(LibrarySortTest, ScannerColumnsUseVisibleRowsAndKeepTiesInViewOrder)
{
	using Column = library::SortColumn;
	std::vector<GameMedia> games(6);
	games[0].path = "never-visible.chd";
	games[1].path = "one.GDI";
	games[1].size = 100;
	games[2].path = "two.cdi";
	games[2].size = 9;
	games[3].path = "three.CDI";
	games[3].size = 7;
	games[4].path = "drive";
	games[4].device = true;
	// Index 5 represents the BIOS (empty path).
	Boxart boxart;
	const auto oldFavourites = config::LibraryFavourites.get();
	config::LibraryFavourites = std::vector<std::string>{games[2].path, games[4].path, ""};
	auto sorted = [&](Column column, bool descending) {
		std::vector<size_t> rows = {3, 5, 1, 4, 2};
		library::sortRows(rows, games, column, descending, std::locale::classic(), boxart, nullptr);
		return rows;
	};
	const auto revision = boxart.metadataRevision().database;
	EXPECT_EQ(sorted(Column::Format, false), (std::vector<size_t>{5, 4, 3, 2, 1}));
	EXPECT_EQ(sorted(Column::Format, true), (std::vector<size_t>{1, 3, 2, 4, 5}));
	EXPECT_EQ(sorted(Column::Size, false), (std::vector<size_t>{5, 4, 3, 2, 1}));
	EXPECT_EQ(sorted(Column::Size, true), (std::vector<size_t>{1, 2, 3, 5, 4}));
	EXPECT_EQ(sorted(Column::Favourite, false), (std::vector<size_t>{3, 5, 1, 4, 2}));
	EXPECT_EQ(sorted(Column::Favourite, true), (std::vector<size_t>{2, 3, 5, 1, 4}));
	EXPECT_EQ(boxart.metadataRevision().database, revision);
	config::LibraryFavourites = oldFavourites;
}

TEST(LibrarySortTest, ContinuePlayingUsesSaveDatesAndRestoresItsOwnViewOrder)
{
	std::vector<GameMedia> games(3);
	games[0].path = "old.chd";
	games[1].path = "new.chd";
	games[2].path = "never.chd";
	Boxart boxart;
	LibrarySavestateCache saves;
	saves.reset([](const std::string& path, const std::string&) {
		return SavestateInfo{path, path == "old.chd" ? 10 : 20};
	});
	const auto revision = library::sortRevisions(library::SortColumn::Date, boxart, &saves);
	saves.setScanGames({{games[0].path, {}}, {games[1].path, {}}});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (saves.peek(games[1].path).creationDate == 0 && std::chrono::steady_clock::now() < deadline)
	{
		saves.advance();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_NE(library::sortRevisions(library::SortColumn::Date, boxart, &saves), revision);
	std::vector<library::Entry> entries;
	for (const auto& game : games)
		entries.push_back({game.path, game.path, saves.peek(game.path).creationDate});
	const auto view = library::selectGames(entries, library::View::ContinuePlaying, {});
	ASSERT_EQ(view, (std::vector<size_t>{1, 0}));
	auto rows = view;
	library::sortRows(rows, games, library::SortColumn::Date, false, std::locale::classic(), boxart, &saves);
	EXPECT_EQ(rows, (std::vector<size_t>{0, 1}));
	library::sortRows(rows, games, library::SortColumn::Date, true, std::locale::classic(), boxart, &saves);
	EXPECT_EQ(rows, view);
	library::sortRows(rows, games, library::SortColumn::Size, true, std::locale::classic(), boxart, &saves);
	EXPECT_EQ(rows, view);
}

TEST(LibrarySortTest, EmptyAndSingleRowsDoNotReadMetadata)
{
	Boxart boxart;
	const std::vector<GameMedia> games(1);
	std::vector<size_t> rows;
	const auto revision = boxart.metadataRevision().database;
	library::sortRows(rows, games, library::SortColumn::Title, false, std::locale::classic(), boxart, nullptr);
	EXPECT_TRUE(rows.empty());
	rows.push_back(0);
	library::sortRows(rows, games, library::SortColumn::TimePlayed, true, std::locale::classic(), boxart, nullptr);
	EXPECT_EQ(rows, (std::vector<size_t>{0}));
	EXPECT_EQ(boxart.metadataRevision().database, revision);
}
