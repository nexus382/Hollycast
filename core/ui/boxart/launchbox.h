/* Copyright 2026 The Hollycast Authors. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "scraper.h"
#include <chrono>
#include <functional>
#include <unordered_map>

// A compact index of the Dreamcast and arcade records in LaunchBox's export.
// Read the XML incrementally: the full export is much larger than this index.
class LaunchBoxCatalog
{
public:
	struct Cover {
		std::string file;
		std::string region;
		std::string type;
	};
	struct Game {
		int id = 0;
		std::string name;
		std::string platform;
		std::vector<std::string> aliases;
		std::vector<Cover> covers;
	};
	struct Match {
		int id = 0;
		std::string name;
		std::string url;
	};
	using Reader = std::function<size_t(char *, size_t)>;
	void read(const Reader& reader);
	json toJson() const;
	void loadJson(const json& data);
	Match match(const GameBoxart& item) const;
	static std::string identity(const GameBoxart& item);
	static std::string normalizeTitle(std::string title);

private:
	void indexNames();
	std::unordered_map<int, Game> games;
	std::unordered_map<std::string, std::vector<int>> names;
};

class LaunchBox : public Scraper
{
public:
	bool initialize(const std::string& saveDirectory) override;
	void scrape(GameBoxart& item) override;

private:
	void loadCatalog();
	void saveMatches();
	LaunchBoxCatalog catalog;
	bool catalogLoaded = false;
	std::string directory;
	json matches = json::object();
	std::chrono::steady_clock::time_point retryAfter{};
};
