/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <locale>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace library {

enum class View { All, ContinuePlaying, Favourites };
enum class LaunchMode { Default, Resume, Fresh };
enum class AutoLoadAction { StartFresh, Load, Ask, StartNetwork, RejectResume };

inline AutoLoadAction initialAutoLoadAction(LaunchMode mode, bool stateAvailable,
		bool stateAllowed, bool automaticLoad, bool networkStartup = false)
{
	// Only the loaded game's effective settings can decide Resume eligibility.
	if (mode == LaunchMode::Resume)
		return stateAllowed && stateAvailable && !networkStartup ? AutoLoadAction::Load : AutoLoadAction::RejectResume;
	if (networkStartup)
		return AutoLoadAction::StartNetwork;
	if (!stateAllowed || !stateAvailable || mode == LaunchMode::Fresh)
		return AutoLoadAction::StartFresh;
	if (automaticLoad)
		return AutoLoadAction::Load;
	return AutoLoadAction::Ask;
}

struct Entry {
	std::string path;
	std::string name;
	time_t latestStateDate = 0;
	bool device = false;
};

inline bool isFavourite(const std::string& path, const std::vector<std::string>& favourites)
{
	return !path.empty() && std::find(favourites.begin(), favourites.end(), path) != favourites.end();
}

inline void setFavourite(const std::string& path, bool value, std::vector<std::string>& favourites)
{
	if (path.empty())
		return;
	auto first = std::find(favourites.begin(), favourites.end(), path);
	if (value)
	{
		if (first == favourites.end())
			favourites.push_back(path);
		else
			favourites.erase(std::remove(first + 1, favourites.end(), path), favourites.end());
	}
	else if (first != favourites.end())
	{
		favourites.erase(std::remove(favourites.begin(), favourites.end(), path), favourites.end());
	}
}

inline bool matchesView(const Entry& entry, View view, const std::vector<std::string>& favourites)
{
	return view == View::All || (!entry.device && !entry.path.empty()
			&& (view == View::ContinuePlaying ? entry.latestStateDate > 0 : isFavourite(entry.path, favourites)));
}

inline bool entryBefore(size_t left, size_t right, const std::vector<Entry>& entries, View view)
{
	if (view != View::ContinuePlaying)
		return left < right;
	const Entry& a = entries[left];
	const Entry& b = entries[right];
	if (a.latestStateDate != b.latestStateDate)
		return a.latestStateDate > b.latestStateDate;
	if (a.name != b.name)
		return a.name < b.name;
	if (a.path != b.path)
		return a.path < b.path;
	// Equal metadata keeps the scanner's original order during insertion too.
	return left < right;
}

inline std::vector<size_t> selectGames(const std::vector<Entry>& entries, View view,
		const std::vector<std::string>& favourites)
{
	std::vector<size_t> result;
	for (size_t i = 0; i < entries.size(); i++)
		if (matchesView(entries[i], view, favourites))
			result.push_back(i);
	if (view == View::ContinuePlaying)
		std::sort(result.begin(), result.end(), [&entries, view](size_t left, size_t right) {
			return entryBefore(left, right, entries, view);
		});
	return result;
}

enum class SortColumn { ProductId = 1, Title, Region, Format, TimePlayed, Date, Size, Favourite };

// A metadata-only snapshot keeps sorting independent of UI formatting and disk access.
struct SortEntry {
	std::string productId, title, region, format;
	std::optional<uint64_t> playTimeSeconds;
	time_t date = 0;
	size_t size = 0;
	bool favourite = false;
};

inline void sortRows(std::vector<size_t>& rows, const std::vector<SortEntry>& entries,
		SortColumn column, bool descending, const std::locale& locale)
{
	// Swap operands rather than reversing the result so equal keys retain view order.
	std::stable_sort(rows.begin(), rows.end(), [&](size_t left, size_t right) {
		const auto& a = entries[descending ? right : left];
		const auto& b = entries[descending ? left : right];
		switch (column)
		{
		case SortColumn::ProductId: return locale(a.productId, b.productId);
		case SortColumn::Title: return locale(a.title, b.title);
		case SortColumn::Region: return locale(a.region, b.region);
		case SortColumn::Format: return locale(a.format, b.format);
		case SortColumn::TimePlayed: return a.playTimeSeconds < b.playTimeSeconds;
		case SortColumn::Date: return a.date < b.date;
		case SortColumn::Size: return a.size < b.size;
		case SortColumn::Favourite: return a.favourite < b.favourite;
		}
		return false;
	});
}

// Cache scanner indices, which remain stable as save discovery adds visible games.
// Updates accept new save dates and matcher changes for the listed source paths.
// Rebuild when the scanner, view, filter or existing save dates are invalidated.
class FilteredGames
{
public:
	template<typename Matcher>
	void rebuild(const std::vector<Entry>& entries, View view,
			const std::vector<std::string>& favourites, const Matcher& matches)
	{
		this->view = view;
		this->favourites = favourites;
		matched.assign(entries.size(), false);
		indicesByPath.clear();
		for (size_t i = 0; i < entries.size(); ++i)
			if (!entries[i].path.empty())
				indicesByPath[entries[i].path].push_back(i);
		rows = selectGames(entries, view, favourites);
		rows.erase(std::remove_if(rows.begin(), rows.end(), [&](size_t index) {
			matched[index] = matches(index);
			return !matched[index];
		}), rows.end());
	}

	template<typename Matcher>
	void update(const std::vector<Entry>& entries, const std::vector<std::string>& changedPaths,
			const Matcher& matches)
	{
		std::unordered_set<std::string> updated;
		for (const std::string& path : changedPaths)
		{
			if (!updated.insert(path).second)
				continue;
			auto found = indicesByPath.find(path);
			if (found == indicesByPath.end())
				continue;
			for (size_t index : found->second)
			{
				const bool match = matchesView(entries[index], view, favourites) && matches(index);
				if (match == matched[index])
					continue;
				auto row = std::lower_bound(rows.begin(), rows.end(), index, [&](size_t left, size_t right) {
					return entryBefore(left, right, entries, view);
				});
				if (match)
					rows.insert(row, index);
				else if (row != rows.end() && *row == index)
					rows.erase(row);
				matched[index] = match;
			}
		}
	}

	const std::vector<size_t>& getRows() const { return rows; }

private:
	View view = View::All;
	std::vector<std::string> favourites;
	std::vector<bool> matched;
	std::vector<size_t> rows;
	std::unordered_map<std::string, std::vector<size_t>> indicesByPath;
};

} // namespace library
