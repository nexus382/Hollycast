/*
	Copyright 2022 flyinghead
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

#include "types.h"
#include "json.hpp"

#include <optional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace nlohmann;

enum class OfflineArtState { Pending, Complete, NoArtwork, Failed };

struct GameMedia;

struct GameBoxart
{
	std::string fileName;
	std::string name;
	std::string uniqueId;
	std::string searchName;
	u32 region = 0;
	std::string releaseDate;
	std::string overview;
	std::string developer, publisher, genre, players;
	u32 screenScraperId = 0;
	u32 screenScraperCheckedMedia = 0;
	bool screenScraperSearched = false;
	std::map<std::string, std::string> screenScraperMedia;

	std::string gamePath;
	std::string boxartPath;
	std::string boxartUrl;
	// Populated from the read-only Library play-time database; never persisted in the boxart database.
	std::optional<u64> playTimeSeconds;

	bool arcade = false;
	OfflineArtState offlineArtState = OfflineArtState::Pending;
	u64 workRevision = 0;
	// Per-job ScreenScraper bits; never persisted or inferred from the current theme.
	u32 requestedMedia = 0;
	// Sanitized lookup data shared between cover and extra-media phases; never persisted.
	std::shared_ptr<const json> screenScraperDetails;
	bool scraped = false;
	bool busy = false;

	enum Region { JAPAN = 1, USA = 2, EUROPE = 4 };

	json to_json(const std::string& baseArtPath) const;
	// Fill display metadata without overwriting configured gamelist values.
	void applyMetadata(GameMedia& media) const;

	template<typename T>
	static void loadProperty(T& i, const json& j, const std::string& propName)
	{
		try {
			i = j.at(propName).get<T>();
		} catch (const json::exception& e) {
			i = T();
		}
	}

	GameBoxart() = default;

	GameBoxart(const json& j, const std::string& baseArtPath);

	void setBoxartPath(const std::string& path) {
		boxartPath = path;
	}

	std::string sourceKey() const {
		return gamePath.empty() ? "legacy:" + fileName : (arcade ? "arcade:" : "disc:") + gamePath;
	}

	bool isValid() const {
		return !fileName.empty();
	}
};

class Scraper
{
public:
	virtual bool initialize(const std::string& saveDirectory) {
		this->saveDirectory = saveDirectory;
		return true;
	}

	virtual void scrape(GameBoxart& item) = 0;

	virtual void scrape(std::vector<GameBoxart>& items) {
		for (GameBoxart& item : items)
			scrape(item);
	}

	virtual ~Scraper() = default;

protected:
	bool downloadImage(const std::string& url, const std::string& localName, bool mute = false);
	std::string makeUniqueFilename(const std::string& url);

private:
	std::string saveDirectory;
};

class OfflineScraper : public Scraper
{
public:
	void scrape(GameBoxart& item) override;
};
