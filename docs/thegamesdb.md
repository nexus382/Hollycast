<!-- Copyright 2026 The Hollycast Authors -->
# TheGamesDB library media

Select **Settings > Library > Artwork Downloads > Artwork Provider > TheGamesDB**.
Hollycast uses its existing TheGamesDB API access; no ScreenScraper login or application credentials are needed.

All three providers share image slots, manual selections, Download All Required Media,
Download Selected Media, Retry Unmatched / Unavailable Media, and queue progress.
The media popup lists only types supported by the current provider. Selections
use stable shared bits, so a screenshot remains a screenshot after switching providers.
The existing `ScreenScraperMedia` configuration key is retained for compatibility.

TheGamesDB supplies these categories:

| Library slot | TheGamesDB image |
| --- | --- |
| Cover | Front boxart |
| Back Cover | Back boxart |
| Screenshot | Screenshot |
| Fan Art | Fanart |
| Wheel / Marquee | Clearlogo |
| Banner | Banner |
| Title Screen | Titlescreen |

The service does not advertise separate miximages, physical-media scans, 3D boxes,
or spines. Hollycast keeps those categories empty unless another provider or
local artwork supplies them. It does not manufacture those images from covers.
Not every game has every supported category. One image per category is cached.

Automatic browsing and completed content scans request covers only. The first
TheGamesDB request upgrades an old thumbnail-only record into the new categorized
cache. Manual downloads fetch the chosen categories for the Dreamcast library;
choosing a theme never starts an expanded scrape. Download All Required Media
requests all seven categories for TheGamesDB all ten for LaunchBox, and all eleven for ScreenScraper.

Downloads prefer the API's original image size, then large, medium, small and
thumbnail sizes when larger variants are absent. Separate images are saved under
`boxart/thegamesdb/`; provider IDs, completed/unavailable categories, and local
paths persist in the artwork database. Developer, publisher and genre dictionaries
are cached there too, and resolved names, player count, overview, title and release
date feed the same details panels as ScreenScraper. Configured gamelist metadata
keeps priority. Partial responses do not erase existing metadata.

Local artwork has first priority. Otherwise, the selected provider's cached
category is preferred, with the other provider's existing cache as a fallback.
Switching providers does not delete images or require downloading them again.
Retry clears only the selected provider's unsuccessful attempts, preserving
successful files. Missing downloaded files become pending again. Unmatched or
ambiguous titles keep their existing artwork. Serial matches use an exact disc ID;
name matches require a single exact normalized title within the game platform.

Images are decoded and validated before temporary files are published. Failed
requests keep completed images and pause further TheGamesDB requests for a minute.
API keys and complete request URLs are not written into the artwork database or
scraper logs.

Provider reference: [TheGamesDB API specification](https://github.com/TheGamesDB/TheGamesDBv2/blob/master/API/spec.yaml).

TGDB is also the default automatic fallback for LaunchBox and ScreenScraper. It fills only missing supported categories within the current request scope; existing primary artwork and metadata keep priority. A primary failure may download fallback art while preserving the primary retry.
