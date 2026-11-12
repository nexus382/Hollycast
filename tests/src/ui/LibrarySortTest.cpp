// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/library.h"

TEST(LibrarySortTest, TitleSortsBothDirectionsWithoutReorderingEqualTitles)
{
	const std::vector<library::Entry> entries = {
		{"/z", "Zulu"}, {"/a-new", "Alpha", 200},
		{"/a-old", "Alpha", 100}, {"/b", "Bravo"}
	};
	std::vector<size_t> rows = {0, 1, 2, 3};
	library::sortByTitle(rows, entries, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{1, 2, 3, 0}));
	library::sortByTitle(rows, entries, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0, 3, 1, 2}));
}

TEST(LibrarySortTest, TitleSortOnlyChangesSelectedRows)
{
	const std::vector<library::Entry> entries = {
		{"/z", "Zulu"}, {"/a", "Alpha"}, {"/b", "Bravo"}
	};
	std::vector<size_t> rows = {0, 2};
	library::sortByTitle(rows, entries, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{2, 0}));
	library::sortByTitle(rows, entries, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0, 2}));
}

TEST(LibrarySortTest, TitleSortPreservesContinuePlayingOrderForEqualTitles)
{
	const std::vector<library::Entry> entries = {
		{"/z", "Zulu", 300}, {"/a-old", "Alpha", 100},
		{"/a-new", "Alpha", 200}, {"/never", "Never", 0}
	};
	auto rows = library::selectGames(entries, library::View::ContinuePlaying, {});
	library::sortByTitle(rows, entries, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{2, 1, 0}));
	EXPECT_EQ(library::selectGames(entries, library::View::ContinuePlaying, {}),
			(std::vector<size_t>{0, 2, 1}));
}

TEST(LibrarySortTest, EmptyAndSingleRowsRemainValid)
{
	std::vector<size_t> rows;
	library::sortByTitle(rows, {}, false, std::locale::classic());
	EXPECT_TRUE(rows.empty());
	const std::vector<library::Entry> entries = {{"/a", "Alpha"}};
	rows.push_back(0);
	library::sortByTitle(rows, entries, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0}));
}
