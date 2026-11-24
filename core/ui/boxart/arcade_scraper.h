/*
	Copyright 2025 flyinghead
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

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "scraper.h"
#include "oslib/http_client.h"
#include "stdclass.h"

#include <algorithm>

class ArcadeScraper : public Scraper
{
public:
	explicit ArcadeScraper(Transport transport = {}) : transport(std::move(transport)) {}
	using Scraper::scrape;
	bool initialize(const std::string& saveDirectory) override;
	void scrape(GameBoxart& item) override;
	static bool needsCover(const GameBoxart& item);

private:
	static std::array<std::string, 2> candidates(const GameBoxart& item);
	Transport transport;
};

inline bool ArcadeScraper::initialize(const std::string& saveDirectory)
{
	if (!Scraper::initialize(saveDirectory))
		return false;

	if (!transport)
		http::init();

	return true;
}

inline std::array<std::string, 2> ArcadeScraper::candidates(const GameBoxart& item)
{
	std::array<std::string, 2> result;
	const std::string extension = get_file_extension(item.fileName);
	if (extension == "zip" || extension == "7z")
		result[0] = get_file_basename(item.fileName);
	if (std::all_of(item.uniqueId.begin(), item.uniqueId.end(), [](unsigned char c) { return c >= ' ' && c <= '~'; }))
		result[1] = item.uniqueId;
	return result;
}

inline bool ArcadeScraper::needsCover(const GameBoxart& item)
{
	if (!item.arcade || item.fileName.empty() || (item.scraped && !item.boxartPath.empty())
			|| item.legacyArcadeRetryAfter > std::chrono::steady_clock::now())
		return false;
	const auto names = candidates(item);
	for (const auto& name : names)
		if (!name.empty() && std::find(item.legacyArcadeMissing.begin(), item.legacyArcadeMissing.end(), name) == item.legacyArcadeMissing.end())
			return true;
	return false;
}

inline void ArcadeScraper::scrape(GameBoxart& item)
{
	if (!needsCover(item))
		return;
	const auto names = candidates(item);
	const std::string filename = makeUniqueFilename("dummy.jpg");
	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i].empty() || (i == 1 && names[1] == names[0])
				|| std::find(item.legacyArcadeMissing.begin(), item.legacyArcadeMissing.end(), names[i]) != item.legacyArcadeMissing.end())
			continue;
		const std::string url = "https://flyinghead.github.io/flycast-content/arcade/jpg/" + http::urlEncode(names[i]) + ".jpg";
		try
		{
			if (downloadMediaImage(url, filename, transport))
			{
				item.setBoxartPath(filename);
				item.boxartUrl = url;
				item.scraped = true;
				item.legacyArcadeRetryAfter = {};
				return;
			}
			item.legacyArcadeMissing[i] = names[i];
		}
		catch (const std::exception& error)
		{
			item.legacyArcadeRetryAfter = std::chrono::steady_clock::now() + std::chrono::minutes(1);
			INFO_LOG(COMMON, "Arcade artwork: %s", error.what());
		}
	}
}
