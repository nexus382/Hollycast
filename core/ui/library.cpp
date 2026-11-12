// Portions Copyright 2026 The Hollycast Authors
#include "library.h"
#include "boxart/boxart.h"
#include "boxart/vmu_icon.h"
#include "game_scanner.h"
#include "library_savestates.h"
#include "stdclass.h"

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace library {

std::string regionLabel(uint32_t region)
{
	if (region == 0)
		return "Unknown";

	std::string value;
	if (region & GameBoxart::JAPAN)
		value += "JP";
	if (region & GameBoxart::USA)
	{
		if (!value.empty())
			value += "/";
		value += "US";
	}
	if (region & GameBoxart::EUROPE)
	{
		if (!value.empty())
			value += "/";
		value += "EU";
	}
	return value.empty() ? "Unknown" : value;
}

std::string mediaFormat(const GameMedia& game)
{
	if (game.path.empty())
		return "BIOS";
	if (game.device)
		return "Device";
	const std::string extension = get_file_extension(game.path);
	return extension.empty() ? "Unknown" : extension;
}

std::array<uint64_t, 2> sortRevisions(SortColumn column, Boxart& boxart, const LibrarySavestateCache* latestSaves)
{
	switch (column)
	{
	case SortColumn::ProductId:
	case SortColumn::Title:
	case SortColumn::Region:
		return {boxart.metadataRevision().database, 0};
	case SortColumn::TimePlayed: {
		const auto revision = boxart.metadataRevision();
		return {revision.database, revision.playtime};
	}
	case SortColumn::Date:
		if (latestSaves != nullptr)
			return {0, latestSaves->getRevision()};
		return {boxart.metadataRevision().database, getLibraryBootHistoryRevision()};
	default:
		return {};
	}
}

// Resolve the selected key once per source row. Scanner indices keep their
// identity when filtering or clipping, and equal keys keep the view's order.
template<typename KeyForRow, typename Less = std::less<>>
static void sortByKey(std::vector<size_t>& rows, bool descending, KeyForRow keyForRow, Less less = {})
{
	using Key = std::decay_t<decltype(keyForRow(size_t{}))>;
	std::vector<std::pair<Key, size_t>> keyedRows;
	keyedRows.reserve(rows.size());
	for (size_t index : rows)
		keyedRows.emplace_back(keyForRow(index), index);
	std::stable_sort(keyedRows.begin(), keyedRows.end(), [&](const auto& left, const auto& right) {
		return descending ? less(right.first, left.first) : less(left.first, right.first);
	});
	for (size_t i = 0; i < rows.size(); ++i)
		rows[i] = keyedRows[i].second;
}

void sortRows(std::vector<size_t>& rows, const std::vector<GameMedia>& games, SortColumn column,
		bool descending, const std::locale& locale, Boxart& boxart, const LibrarySavestateCache* latestSaves)
{
	if (rows.size() < 2)
		return;
	const auto mode = static_cast<BoxartSourceMode>(config::BoxartSourceMode.get());
	auto metadata = [&](size_t i) {
		return games[i].device ? GameBoxart{} : boxart.getCachedBoxart(games[i], mode);
	};
	switch (column)
	{
	case SortColumn::ProductId:
		sortByKey(rows, descending, [&](size_t i) { return metadata(i).uniqueId; }, locale);
		break;
	case SortColumn::Title:
		sortByKey(rows, descending, [&](size_t i) {
			std::string title = metadata(i).name;
			return title.empty() ? games[i].name : std::move(title);
		}, locale);
		break;
	case SortColumn::Region:
		sortByKey(rows, descending, [&](size_t i) { return regionLabel(metadata(i).region); }, locale);
		break;
	case SortColumn::Format:
		sortByKey(rows, descending, [&](size_t i) { return mediaFormat(games[i]); }, locale);
		break;
	case SortColumn::TimePlayed:
		sortByKey(rows, descending, [&](size_t i) { return boxart.getLibraryPlaytime(metadata(i)); });
		break;
	case SortColumn::Date:
		sortByKey(rows, descending, [&](size_t i) {
			return latestSaves != nullptr ? latestSaves->peek(games[i].path).creationDate
					: getLibraryGameLastBooted(games[i], metadata(i).uniqueId);
		});
		break;
	case SortColumn::Size:
		sortByKey(rows, descending, [&](size_t i) { return games[i].size; });
		break;
	case SortColumn::Favourite: {
		const auto& paths = config::LibraryFavourites.get();
		const std::unordered_set<std::string_view> favourites(paths.begin(), paths.end());
		std::stable_partition(rows.begin(), rows.end(), [&](size_t i) {
			const auto& game = games[i];
			const bool favourite = !game.device && !game.path.empty() && favourites.count(game.path) != 0;
			return favourite == descending;
		});
		break;
	}
	}
}

} // namespace library
