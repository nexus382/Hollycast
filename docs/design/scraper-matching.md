# Interactive scraper matching design

## Recommendation

Keep automatic acceptance unchanged: ScreenScraper still auto-matches only
one exact normalized full title. Store bounded candidate records from search
responses on each unmatched Dreamcast game, then let the user explicitly choose
a provider game ID from that saved list. This design leaves runtime
matching and persistence as they are today.

## Candidate persistence

`ScreenScraper::findGame()` builds title and filename queries, then up to two
significant whole-word queries (`core/ui/boxart/screenscraper.cpp:572-605`).
Each `jeuRecherche.php` response is checked for a valid Dreamcast game ID and
system (`screenscraper.cpp:610-614`); exact normalized matches are accepted,
while unrelated results are skipped (`screenscraper.cpp:627-648`). The response
objects currently live only in this function. When no exact match is returned,
`ScreenScraper::scrape()` marks the search complete and returns without storing
those objects (`screenscraper.cpp:651-677`).

The smallest persistence seam is the existing per-game `GameBoxart` record:
provider IDs, searched flags and media maps are already serialized by
`GameBoxart::to_json()` and loaded by its JSON constructor
(`core/ui/boxart/scraper.h:41-57`, `core/ui/boxart/scraper.cpp:80-105,
118-172`). `Boxart::saveDatabase()` writes those records as one atomic JSON
array (`core/ui/boxart/boxart.cpp:1882-1925`), and
`completeArtworkWork()` publishes the completed scraper record back into that
database (`boxart.cpp:729-765`).

The proposed format stores up to 20 distinct candidates per unmatched game, each
with `provider_game_id`, `title`, `system_id`, the originating `query`, and a
Unix UTC `timestamp`. Store these sanitized fields only. A confirmed
automatic match clears its candidates. Refresh the list during the existing
explicit retry path (`boxart.cpp:1006-1053`). Preserve successful IDs and
downloaded files. Leave the matcher version unchanged until a change in
actual matching behavior warrants the existing negative-cache retry policy
(`scraper.cpp:69, 146-170`). A storage experiment can test synthetic
candidate round trips without collecting candidates in live lookups.

For a full picker, manual confirmation should set the chosen provider game ID
and use the existing game-info request to obtain authoritative metadata and
media (`screenscraper.cpp:564-570`). A manual choice must be recorded as such
so future automatic retries never replace it silently. Reset-to-automatic
clears that override and returns the record to the normal exact matcher.

## Picker UX

Add **Match artwork…** to the selected game's library actions. The themed
library already provides a per-item context menu
(`core/ui/gui.cpp:3130-3164`). Expose the same action in the stock
library so the picker is reachable with either view. Disable it for BIOS/device rows and when no candidate list
exists; offer **Search again** only through the existing explicit retry path.

The picker should show each candidate's title, system, provider game ID, and
the query that found it. If the response includes region or year, show those
as secondary information, clearly labeled as provider data. Do not rank a
candidate as correct or preselect the first result. Confirmation names the
selected title and asks the user to confirm it before downloading media.

Provide three distinct actions:

- **Use this match** stores the chosen provider ID and queues normal requested
  media through the existing artwork queue.
- **Unmatch** clears the manual provider ID and its provider-owned metadata and
  media references. It keeps local/custom artwork and leaves cached files on
  disk; deleting files immediately is unsafe because provider images can be
  shared by multiple game records.
- **Reset to automatic** removes the manual override and re-runs the current
  exact matcher. It must not infer a replacement from the candidate order.

On re-match, detach the previous provider's cached media paths from this game
before recording the new provider result. Keep the old files until a later
garbage-collection pass can prove that no database record references them.
Preserve unrelated provider media, local ES-DE artwork, and custom artwork.
The user can then unmatch or reset without destroying those sources.

## Serial and product ID path

The offline disc scraper reads Dreamcast IP.BIN `product_number` into
`GameBoxart::uniqueId` and reads `software_name` into `searchName`
(`core/ui/boxart/scraper.cpp:298-324`). For supported non-archive ROM files it
also reads a 32-byte identifier at offset `0x30` (`scraper.cpp:352-381`). The
library metadata payload exposes this value as `productId`
(`core/ui/gui.cpp:2804`, `docs/Themes.md:232`).

ScreenScraper lookup currently does not submit `uniqueId`: it uses only the
three title/name inputs and whole-word fallbacks (`screenscraper.cpp:571-603`).
The in-repository guide explicitly says serial/hash matching is not implemented
(`docs/screenscraper.md:140-142`), and the code refuses to send a compressed
CHD hash as a disc hash (`screenscraper.cpp:571`). Whether the ScreenScraper API
accepts Dreamcast product IDs for deterministic matching is **an operator
question**; this repository does not establish that API contract. Until
verified, keep ID matching out of the automatic and picker-confirmation paths.
If supported, test it as a separate deterministic lookup before title search;
never use it for CHD hashes.

## Arcade titles and other providers

Candidate storage and the first picker should be limited to ScreenScraper's
Dreamcast system-23 path. Arcade records use a narrower routing path:
`ArtworkAdapter::needsMedia()` treats non-arcade ScreenScraper work through
the provider chain, while arcade fallback checks TGDB state separately
(`core/ui/boxart/artwork_adapter.cpp:104-119`). The existing scraper guide
also identifies the separate arcade cover source (`docs/screenscraper.md:7`).
Do not show Dreamcast candidates for Naomi or Atomiswave entries. A later
provider-neutral picker can add arcade candidates only after each provider's
system IDs, match rules, and reversible media ownership are designed.

TheGamesDB and LaunchBox could use the same user-facing interaction, but their
candidate searches, IDs, and caches differ (`artwork_adapter.cpp:75-108`,
`core/ui/boxart/launchbox.h:21`). Generalize the UI contract after a
provider-specific design; do not extend their matchers in this plan.

## Quota and cache rules

Opening the picker and viewing saved candidates makes no API request. Choosing
a candidate reuses its persisted game ID, so it avoids another title-search
request; confirming may require one game-info request, followed by requests
only for missing requested media. This is bounded and lower than repeating
the original search sequence. Candidate discovery itself consumes only the
requests already made by the original automatic search.

The implementation must retain the shared artwork rules: **“Artwork providers
share the check for a missing cached image. A completed search can legitimately
return no image.”** (`docs/design/shared-emulator-rules.md:15`). A chosen match
with no cover is still a completed match; keep its ID and metadata, leave the
missing image unavailable, and allow the normal explicit retry. Preserve
successful cached images and local/custom media when a search or download
fails, consistent with `docs/screenscraper.md:138-162`.

## Open questions

- Does ScreenScraper accept Dreamcast product IDs, and what exact request field
  and format does it require?
- Which candidate details are consistently present in search responses
  (localized title, region, year), versus requiring a game-info request?
- Should candidate lists expire, or remain until a matcher-version change or
  explicit retry?
- Should manual matches be pinned across provider changes, and how should the
  UI explain a match that no longer returns metadata?
- Can a future provider-neutral design represent arcade candidate systems
  without weakening each provider's own acceptance rules?

## Follow-on estimate

| Phase | Work | Estimate |
| --- | --- | --- |
| 1 | Persist bounded ScreenScraper candidates and cover retry/upgrade behavior | **S** |
| 2 | Add picker data/actions, explicit override state, unmatch/reset, and safe media detachment | **M–L** |
| 3 | Add context-menu and picker UI, accessibility/input handling, localization, and end-to-end artwork tests | **M** |
| 4 | Verify product-ID API support and, only if supported, add a deterministic lookup design | **S–M**, pending API confirmation |
