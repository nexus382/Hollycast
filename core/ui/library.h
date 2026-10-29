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
#include <ctime>
#include <string>
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

inline std::vector<size_t> selectGames(const std::vector<Entry>& entries, View view,
		const std::vector<std::string>& favourites)
{
	std::vector<size_t> result;
	for (size_t i = 0; i < entries.size(); i++)
	{
		const Entry& entry = entries[i];
		if (view == View::All || (!entry.device && !entry.path.empty() &&
				(view == View::ContinuePlaying ? entry.latestStateDate > 0 : isFavourite(entry.path, favourites))))
			result.push_back(i);
	}
	if (view == View::ContinuePlaying)
		std::stable_sort(result.begin(), result.end(), [&entries](size_t left, size_t right) {
			const Entry& a = entries[left];
			const Entry& b = entries[right];
			if (a.latestStateDate != b.latestStateDate)
				return a.latestStateDate > b.latestStateDate;
			if (a.name != b.name)
				return a.name < b.name;
			return a.path < b.path;
		});
	return result;
}

} // namespace library
