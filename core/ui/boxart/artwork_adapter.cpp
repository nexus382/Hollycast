// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "artwork_adapter.h"
#include "gamesdb.h"
#include "launchbox.h"
#include "screenscraper.h"
#include "stdclass.h"

ArtworkAdapter::ArtworkAdapter(int provider, std::unique_ptr<Scraper> primary,
  std::unique_ptr<Scraper> fallback)
 : provider(provider), primary(std::move(primary)), fallback(std::move(fallback))
{
 if (provider == 2 && !this->fallback) this->fallback = std::make_unique<TheGamesDb>();
}

bool ArtworkAdapter::initialize(const std::string& directory)
{
 return primary->initialize(directory) && (!fallback || fallback->initialize(directory));
}

bool ArtworkAdapter::ready() const
{
 std::lock_guard<std::mutex> lock(fallbackMutex);
 return primary->ready() && (!fallback || fallback->ready());
}

u32 ArtworkAdapter::supportedMedia(int provider)
{
 return provider == 1 ? artwork::allMedia : provider == 2 ? LaunchBox::supportedMedia : TheGamesDb::supportedMedia;
}

u32 ArtworkAdapter::fallbackMedia(const GameBoxart& item, int provider, u32 selection, bool unresolved)
{
 if (provider == 0) return 0;
 const u32 checked = provider == 1 ? item.screenScraperCheckedMedia : item.launchboxCheckedMedia;
 u32 missing = 0;
 const auto& media = item.mediaForProvider(provider);
 for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
 {
  const u32 bit = 1u << i;
  if (!(selection & TheGamesDb::supportedMedia & (unresolved ? artwork::allMedia : checked) & bit)) continue;
  const auto image = media.find(artwork::mediaTypes[i].name);
  if (image == media.end() || !file_exists(image->second)) missing |= bit;
 }
 // If the primary found no game, a metadata-only job may still use TGDB.
 if ((selection & artwork::metadataMedia) && (unresolved ||
   (provider == 1 ? item.screenScraperSearched && !item.screenScraperId : item.launchboxSearched && !item.launchboxId)))
  missing |= artwork::metadataMedia;
 return missing;
}

u32 ArtworkAdapter::stageMedia(const GameBoxart& item, int provider, u32 selection)
{
 u32 missing = selection & supportedMedia(provider);
 for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
 {
  const u32 bit = 1u << i;
  if (!(missing & bit)) continue;
  for (int cachedProvider : {1, 2, 0})
  {
   const auto& cached = item.mediaForProvider(cachedProvider);
   const auto found = cached.find(artwork::mediaTypes[i].name);
   if (found != cached.end() && file_exists(found->second)) { missing &= ~bit; break; }
  }
 }
 // A later image provider can fill absent fields, but needs no extra metadata
 // lookup when an earlier provider already matched the game.
 if ((selection & artwork::metadataMedia) && (provider == 1 ||
   (!item.screenScraperId && (provider == 2 || !item.launchboxId)))) missing |= artwork::metadataMedia;
 u32 pending = 0;
 for (u32 bits = missing; bits; bits &= bits - 1)
 {
  const u32 bit = bits & (~bits + 1u);
  const bool needed = provider == 1 ? screenscraper::needsMedia(item, bit)
    : provider == 2 ? LaunchBox::needsMedia(item, bit) : TheGamesDb::needsMedia(item, bit);
  if (needed) pending |= bit;
 }
 return pending;
}

int ArtworkAdapter::nextProvider(const GameBoxart& item, u32 selection)
{
 for (int provider : {1, 2, 0})
  if (stageMedia(item, provider, selection)) return provider;
 return -1;
}

bool ArtworkAdapter::needsMedia(const GameBoxart& item, int provider, u32 selection)
{
 if (provider == 1 && !item.arcade) return nextProvider(item, selection) != -1;
 const bool primaryPending = provider == 1 ? screenscraper::needsMedia(item, selection)
   : provider == 2 ? LaunchBox::needsMedia(item, selection) : TheGamesDb::needsMedia(item, selection);
 if (primaryPending) return true;
 const u32 missing = fallbackMedia(item, provider, selection);
 if (item.arcade) return (missing & ~item.tgdbCheckedMedia) != 0;
 return TheGamesDb::needsMedia(item, missing);
}

void ArtworkAdapter::scrape(GameBoxart& item)
{
 if (provider == 1) { primary->scrape(item); return; }
 std::exception_ptr primaryFailure;
 try { primary->scrape(item); } catch (...) { primaryFailure = std::current_exception(); }
 const u32 requested = item.requestedMedia ? item.requestedMedia : artwork::coverMedia;
 const u32 missing = fallbackMedia(item, provider, requested, primaryFailure != nullptr);
 if (!fallback || !missing) { if (primaryFailure) std::rethrow_exception(primaryFailure); return; }
 std::lock_guard<std::mutex> lock(fallbackMutex);
 // Keep richer primary metadata when TGDB only fills an unavailable image slot.
 const bool matched = provider == 1 ? item.screenScraperId != 0 : item.launchboxId != 0;
 const GameBoxart before = item;
 item.requestedMedia = missing;
 if (item.arcade && (missing & ~item.tgdbCheckedMedia)) item.scraped = false;
 auto restore = [&] {
  item.requestedMedia = before.requestedMedia;
  if (matched && !before.name.empty()) item.name = before.name;
  for (const auto field : {&GameBoxart::releaseDate, &GameBoxart::overview, &GameBoxart::developer,
    &GameBoxart::publisher, &GameBoxart::genre, &GameBoxart::players})
   if (!(before.*field).empty()) item.*field = before.*field;
 };
 try { fallback->scrape(item); }
 catch (...) { restore(); throw; }
 restore();
 if (primaryFailure) std::rethrow_exception(primaryFailure);
}
