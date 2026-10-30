<!-- Portions Copyright 2026 The Hollycast Authors -->

# ScreenScraper library artwork

ScreenScraper is the default online source for Dreamcast library covers.
The basic list and icon views automatically fetch missing front covers when
content is added or browsed, following the existing artwork-source rules.
Choosing extra media or switching themes never silently starts a full scrape.
Existing ES-DE media folders, cached artwork and disc-art fallbacks remain usable.
The separate arcade cover source and the explicit TheGamesDB option are retained.

ScreenScraper requires Hollycast's approved application credentials. Official
builds receive `HOLLYCAST_SCREENSCRAPER_DEV_ID` and
`HOLLYCAST_SCREENSCRAPER_DEV_PASSWORD` from the build environment when CMake
configures the application. CMake generates a private header in the build
directory; credential values are not stored in CMakeCache, compiler arguments
or tracked source files. Users of a provisioned build only enter their optional
personal login in Library settings.

For local development, the same pair of environment variables can override the
application credentials at runtime. Both must be provided together; a partial
override disables application access instead of mixing credentials. Personal
accounts are never included in a build. Without application credentials the
media choices remain usable and Library settings report that access is not
configured; no automatic fallback to TheGamesDB occurs.

Personal login is optional. Guest access connects automatically when online
artwork is enabled and developer access is configured. Guest restrictions still
apply, including periods when ScreenScraper closes guest access. Sign in with a
personal account to use that account's reported thread and quota allowances.
Signing out returns to guest access. Reconnecting waits for old in-flight calls
to finish so the two sessions cannot exceed the same account's thread allowance.

The username is saved in settings. The password is masked and kept only for the
current session; it is never written to the configuration or artwork database.
Users sign in again after restarting; browsing can otherwise use guest access.
Downloads use HTTPS; credential-bearing request URLs and server error bodies
are not logged or cached.

## Media choices

Open **Settings → Library → Artwork Downloads → Images to Download** for the
checkbox popup. It uses the same themed dialog as the other settings options;
the settings row shows how many types are selected. Tick several types without
closing the popup, then choose **Apply** to save or **Cancel** to discard changes.
Escape, controller Back, and clicking outside also discard unsaved selections.

All supported image types are initially selected for manual downloads. The popup offers Covers + Banners,
Select All and Clear Selection presets. Every supported Custom Boxart image
category is available, using the same names:

| Checkbox | ScreenScraper media | Library/theme slot |
| --- | --- | --- |
| Cover | `box-2D` | `cover` |
| Mix Image | `mixrbv2`, then `mixrbv1` | `miximage` |
| Wheel / Marquee | `wheel-hd`, then `wheel` | `title` |
| Banner | `screenmarquee` | `banner` |
| Physical Media | `support-2D` | `physical` |
| Fan Art | `fanart` | `fanart` |
| Screenshot | `ss` | `screenshot` |
| Title Screen | `sstitle` | `titlescreen` |
| Case / 3D Box | `box-3D` | `case` |
| Back Cover | `box-2D-back` | `backcover` |
| Spine | `box-2D-side` | `spine` |

Mixes are downloaded from ScreenScraper, not generated locally. Videos and
manuals are outside this image-download feature. Not every game has every type.
The disc's region is preferred, followed by world/US/Europe/Japan alternatives.
English descriptions and available title/date/developer/publisher/genre/player
metadata are also cached and shown in themes, hover details and the information
page; configured gamelist metadata keeps priority. Missing fields in a later
provider response do not erase previously downloaded metadata.

Fetch Box Art must be enabled and the source must allow online artwork
(Original Box Art or Custom Boxart). The controls have separate purposes:

- **Automatic cover fetching** requests only Cover for the basic library.
- **Download All Required Media** queues all eleven image types in the loaded
  Dreamcast library for hover details, expanded themes, backgrounds and icons.
- **Images to Download** selects types for **Download Selected Media**. Those
  checkboxes do not change automatic cover fetching or the all-media action.
- **Retry Unmatched / Unavailable Media** retries the selected types while
  preserving successful downloads.

Settings remain responsive while batches run. Each queued game retains its
requested media types. A full-media request made while a cover job is running
is merged and completed without dropping types or fetching the cover twice.
Completed content scans queue basic covers for the whole library, including
games outside the visible list. Disc-header workers publish each finished game
to the online queue immediately; network downloads do not wait for the scan to
finish. A persistent worker pool drains that queue without UI-frame refill gaps
or waiting for the slowest game in a fixed-size batch. Covers are completed
across the requested library before the selected extra images are fetched.
Lookup metadata is reused between those phases with credential-bearing media
URLs removed, then released when the job finishes. Database saves are coalesced
away from download workers. Settings show remaining games and covers separately
from active API requests. Changing provider or disabling downloads stops further
queue claims; an HTTP request already in progress can finish.
Bulk requests stay queued through temporary rate-limit pauses. Adding another
content directory later still requests covers only. Unchecking a type does not
delete already-downloaded files.

Downloads are stored beneath Hollycast's writable `boxart/screenscraper/`
directory with local paths recorded in the artwork database. Existing custom
ES-DE images keep priority and are never overwritten. Missing custom categories
fall back to downloaded ScreenScraper categories; existing default artwork
remains the final cover fallback. Theme Icon Media includes Banner, Back Cover
and Spine. Local spine folders can be named `spines`, `spine`, `box-2d-side` or
`box2dside`. CD Rack uses real spine scans where available and vertical game
titles otherwise; it never crops front covers into substitute spine artwork.

## Matching, limits and retrying

This first implementation targets Dreamcast (system 23). It uses system-scoped
title searches from disc metadata and library/ROM names, accepts one exact
normalized title, and caches ScreenScraper's game ID for subsequent requests.
Punctuation-distinct filename searches are retained because the server treats
them differently. When full-title queries fail, up to two significant whole
words retrieve possible aliases; a result still needs an exact normalized full
title match. Similar sequels and editions are not accepted merely because they
appear first. Older cached unmatched results retry once after a matcher upgrade;
successful matches and downloaded files remain valid.
It does not submit a CHD container hash as a disc hash or guess between ambiguous
matches. Unmatched games keep their previous artwork. Serial/hash matching and
an interactive match picker are not implemented here.

The guest session or signed-in account supplies its thread count, download-speed allowance,
per-minute and daily request caps, and daily unmatched-game cap. The shared
request gate reserves requests before dispatch, limits simultaneous calls,
and paces aggregate completed bytes using the reported KB/s allowance. Up to
64 workers are supported, always within the reported allowance. Other clients
using the same account can still consume its quota; updated replies and API
limit responses are authoritative.

Rate limits and temporary errors back off for a minute. Authentication,
application-access and daily-quota failures stop new requests and explain the
reason in Library settings. Reconnect after service/daily allowance recovery.
Signing out cancels personal-account requests and reconnects as a guest. Closing
Hollycast cancels waiting requests. An HTTP request already in progress finishes
before its worker exits. Cached files and completed progress remain available.

Successful images are validated and published from temporary files. Partial
batch progress is retained after an error. Unavailable types and unmatched
titles are remembered to avoid repeat calls on every frame. Retry Unmatched /
Unavailable Media clears those attempts while preserving successful downloads.

API reference: [ScreenScraper API v2](https://www.screenscraper.fr/webapi2.php).
