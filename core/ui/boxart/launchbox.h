/* Copyright 2026 The Hollycast Authors. SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "scraper.h"
#include "artwork_media.h"
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

// A compact index of Dreamcast/arcade metadata and categorized original images.
// The full XML export is read incrementally.
class LaunchBoxCatalog
{
public:
 struct Image { std::string file, region, type; };
 struct Game {
  int id = 0;
  std::string name, platform;
  std::vector<std::string> aliases;
  std::vector<Image> images;
  json metadata = json::object();
 };
 struct Match {
  int id = 0;
  std::string name, url; // url retains the cover interface used by older clients.
  std::map<std::string, std::string> media;
  json metadata = json::object();
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
	struct MatchCacheMetrics {
		size_t recordsSerialized = 0;
		size_t bytesWritten = 0;
		size_t physicalWrites = 0;
		std::chrono::nanoseconds elapsed{};
	};
 explicit LaunchBox(Transport transport = {}) : transport(std::move(transport)) {}
 ~LaunchBox() override;
 // LaunchBox has no composite miximage category. Other slots use native images.
 inline static constexpr u32 supportedMedia = artwork::allMedia & ~(1u << 1);
 static bool needsMedia(const GameBoxart& item, u32 selection);
 bool initialize(const std::string& saveDirectory) override;
 bool ready() const override;
 void beginBulk() override;
 void endBulk() override;
 void scrape(GameBoxart& item) override;
	MatchCacheMetrics matchCacheMetrics() const { return metrics; }

private:
	struct MatchCacheState;
	static std::shared_ptr<MatchCacheState> sharedCacheState(const std::string& directory);
 void loadCatalog();
	void saveMatches(const std::string& key, const json& match);
 LaunchBoxCatalog catalog;
 bool catalogLoaded = false;
 std::string directory, imageDirectory;
	std::shared_ptr<MatchCacheState> matchCache;
	bool bulkActive = false;
	MatchCacheMetrics metrics;
 Transport transport;
 std::chrono::steady_clock::time_point retryAfter{};
};
