<!-- Copyright 2026 The Hollycast Authors -->
# LaunchBox artwork and shared provider adapter

Select **Settings > Library > Artwork Downloads > Artwork Provider > LaunchBox**.
LaunchBox uses the same image choices, download actions, retry control, progress,
metadata and theme bindings as ScreenScraper and TheGamesDB. The separate PR-era
Online Artwork Source selector has been folded into this shared provider selector.
No LaunchBox login or ScreenScraper key is required.

| Shared slot | LaunchBox category |
| --- | --- |
| `cover` | Box - Front, then reconstructed front or advertisement flyer |
| `physical` | Disc scan (cartridge fronts for arcade records) |
| `case` | Box - 3D |
| `backcover` | Box - Back, then reconstructed back or advertisement flyer |
| `spine` | Box - Spine |
| `screenshot` | Screenshot - Gameplay |
| `titlescreen` | Screenshot - Game Title |
| `title` | Clear Logo, or Arcade - Marquee |
| `banner` | Banner |
| `fanart` | Fanart - Background |

These are distinct, provider-independent fields. Any theme may opt into them with
an artwork element's `source`, browser `artworkSource`, or artwork button. Missing
discs and box scans are never substituted with a front cover. LaunchBox has no
native composite miximage category. An existing local/other-provider mix can still
be displayed. One image per category is selected, with matching regions preferred
before worldwide and other-region artwork. Original image files are downloaded.

TheGamesDB is the automatic fallback for supported categories missing from the
primary provider, unmatched titles, and failed primary lookups. Existing primary
images and metadata retain priority. Fallback follows the current request mask:
automatic browsing is cover-only; expanded categories require a manual action.
TGDB cannot supply discs, 3D cases, spines or miximages. Failed primary work remains
retryable while any successfully downloaded fallback art stays visible.

The public catalog (about 104 MiB compressed) is read incrementally. A compact
Dreamcast/arcade index is cached for a week; a failed refresh retains a usable
expanded index. The index also stores overview, release date, developer,
publisher, genre and player count. Configured gamelist values retain priority.

Matches use the internal title and platform, including alternate names. Product
ID plus region, or internal title when no valid ID exists, persists the identity
across renamed/moved images. Ambiguous matches do not replace artwork. Matches,
metadata, separate media paths and checked/unavailable category bits survive
restarts. The old cover-only match cache can be read and upgraded. Manual expanded
downloads upgrade the older catalog format. Downloads are decoded and validated,
then atomically published; partial successes remain cached after a failure.

The optional tests use `HOLLYCAST_LAUNCHBOX_EXPORT` for an existing `Metadata.zip`,
and `HOLLYCAST_LAUNCHBOX_LIVE=1` for a public network smoke check.
