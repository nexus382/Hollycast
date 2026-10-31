/*
	Copyright 2022 flyinghead

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
#include "json.hpp"
#include "artwork_media.h"

#include <map>
#include <string>
#include <vector>
#include <functional>

using namespace nlohmann;

class TheGamesDb : public Scraper
{
public:
	using Transport = std::function<int(const std::string&, std::vector<u8>&, std::string&)>;
	explicit TheGamesDb(Transport transport = {});
	// Front/back covers, logos, banners, fanart, screenshots and title screens.
	inline static constexpr u32 supportedMedia = 1u | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 9);
	static bool needsMedia(const GameBoxart& item, u32 selection);
	bool initialize(const std::string& saveDirectory) override;
	bool ready() const override;
	void scrape(GameBoxart& item) override;
	void scrape(std::vector<GameBoxart>& items) override;

private:
	void fetchPlatforms();
	std::string makeUrl(const std::string& endpoint);
	json httpGet(const std::string& url);
	json findGame(const GameBoxart& item);
	void applyMetadata(GameBoxart& item, const json& game);
	std::string referenceNames(const json& game, const std::string& category);
	bool downloadMedia(const std::string& url, const std::string& path);

	int dreamcastPlatformId = 0;
	int arcadePlatformId = 0;
	u64 blackoutPeriod = 0;

	Transport transport;
	std::string directory;
	json references = json::object();
};
