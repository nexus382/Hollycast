// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/library.h"

TEST(LibrarySortTest, TitleSortsBothDirectionsWithoutReorderingEqualTitles)
{
	const std::vector<library::SortEntry> entries = {
		{"/z", "Zulu"}, {"/a-new", "Alpha"},
		{"/a-old", "Alpha"}, {"/b", "Bravo"}
	};
	std::vector<size_t> rows = {0, 1, 2, 3};
	library::sortRows(rows, entries, library::SortColumn::Title, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{1, 2, 3, 0}));
	library::sortRows(rows, entries, library::SortColumn::Title, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0, 3, 1, 2}));
}

TEST(LibrarySortTest, TitleSortOnlyChangesSelectedRows)
{
	const std::vector<library::SortEntry> entries = {
		{"/z", "Zulu"}, {"/a", "Alpha"}, {"/b", "Bravo"}
	};
	std::vector<size_t> rows = {0, 2};
	library::sortRows(rows, entries, library::SortColumn::Title, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{2, 0}));
	library::sortRows(rows, entries, library::SortColumn::Title, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0, 2}));
}

TEST(LibrarySortTest, TitleSortPreservesContinuePlayingOrderForEqualTitles)
{
	const std::vector<library::Entry> entries = {
		{"/z", "Zulu", 300}, {"/a-old", "Alpha", 100},
		{"/a-new", "Alpha", 200}, {"/never", "Never", 0}
	};
	std::vector<library::SortEntry> metadata(entries.size());
	for (size_t i = 0; i < entries.size(); ++i)
		metadata[i].title = entries[i].name;
	auto rows = library::selectGames(entries, library::View::ContinuePlaying, {});
	library::sortRows(rows, metadata, library::SortColumn::Title, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{2, 1, 0}));
	EXPECT_EQ(library::selectGames(entries, library::View::ContinuePlaying, {}),
			(std::vector<size_t>{0, 2, 1}));
}

TEST(LibrarySortTest, EmptyAndSingleRowsRemainValid)
{
	std::vector<size_t> rows;
	library::sortRows(rows, {}, library::SortColumn::Title, false, std::locale::classic());
	EXPECT_TRUE(rows.empty());
	const std::vector<library::SortEntry> entries = {{"/a", "Alpha"}};
	rows.push_back(0);
	library::sortRows(rows, entries, library::SortColumn::Title, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0}));
}

TEST(LibrarySortTest, EveryMetadataColumnSortsBothDirectionsWithStableTies)
{
	using Column = library::SortColumn;
	std::vector<library::SortEntry> entries(3);
	entries[0] = {"Z", "Zulu", "US", "gdi", 100, 100, 100, true};
	entries[1] = {"A", "Alpha", "EU", "chd", 9, 9, 9, false};
	entries[2] = entries[1];
	for (Column column : {Column::ProductId, Column::Title, Column::Region, Column::Format,
			Column::TimePlayed, Column::Date, Column::Size, Column::Favourite})
	{
		std::vector<size_t> rows = {0, 2, 1};
		library::sortRows(rows, entries, column, false, std::locale::classic());
		EXPECT_EQ(rows, (std::vector<size_t>{2, 1, 0}));
		library::sortRows(rows, entries, column, true, std::locale::classic());
		EXPECT_EQ(rows, (std::vector<size_t>{0, 2, 1}));
	}
}

TEST(LibrarySortTest, MissingPlaytimeRemainsDistinctFromZeroAndLargeValues)
{
	std::vector<library::SortEntry> entries(4);
	entries[0].playTimeSeconds = UINT64_MAX;
	entries[1].playTimeSeconds = 0;
	entries[2].playTimeSeconds = std::nullopt;
	entries[3].playTimeSeconds = 60;
	std::vector<size_t> rows = {0, 1, 2, 3};
	library::sortRows(rows, entries, library::SortColumn::TimePlayed, false, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{2, 1, 3, 0}));
	library::sortRows(rows, entries, library::SortColumn::TimePlayed, true, std::locale::classic());
	EXPECT_EQ(rows, (std::vector<size_t>{0, 3, 1, 2}));
}
