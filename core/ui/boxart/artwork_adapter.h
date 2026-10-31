// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "scraper.h"
#include <mutex>

// Provider-independent slots, completion and fallback policy. Themes consume
// GameBoxart media without depending on a service's category names or API.
class ArtworkAdapter : public Scraper
{
public:
 ArtworkAdapter(int provider, std::unique_ptr<Scraper> primary,
   std::unique_ptr<Scraper> fallback = {});
 bool initialize(const std::string& directory) override;
 bool ready() const override;
 void scrape(GameBoxart& item) override;
 static u32 supportedMedia(int provider);
 // For the ordered ScreenScraper -> LaunchBox -> TGDB pipeline. -1 means complete.
 static int nextProvider(const GameBoxart& item, u32 selection);
 static u32 stageMedia(const GameBoxart& item, int provider, u32 selection);
 static bool needsMedia(const GameBoxart& item, int provider, u32 selection);
 static u32 fallbackMedia(const GameBoxart& item, int provider, u32 selection, bool unresolved = false);

private:
 int provider;
 std::unique_ptr<Scraper> primary, fallback;
 // The optional legacy-provider fallback runs serially.
 mutable std::mutex fallbackMutex;
};
