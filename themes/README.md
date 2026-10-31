<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast theme collection

These packages provide Hollycast game-list layouts. To install one, use **Settings > Library > Library Themes > Install Library Theme Folder**. Keep its source notice alongside `theme.json`. See the [theme author guide](../docs/Themes.md) for the format.

| Package | Media and layout |
| --- | --- |
| [Disc Orbit *](disc-orbit/theme.json) | Upright physical-disc carousel, raised selection, central miximage and game information below. |
| [Horizon Grid *](preview-horizon-grid/theme.json) | Blue-to-navy gradient, screenshot with overlapping front cover, right cover grid. |
| [Art Book Next *](preview-art-book-next/theme.json) | Selected cover background, left list, and a large miximage above a scraped front cover and details. |
| [Fanart & Disc *](preview-fanart-disc/theme.json) | Fanart backdrop with compact artwork and game details; physical-disc thumbnails in the right list. |
| [CD Rack *](preview-cd-rack/theme.json) | Scrollable jewel-case spines on shaded shelves, selected miximage and front cover on the right, with title metadata. Missing spines have vertical title labels. |

- [Demo Disc](demo-disc/) — a magazine cover disc with a tilted game rail, attract mode, and a disc that wears each game's own art.
- [Pocket Dimension](pocket-dimension/) — a tactile VMU library with live LCD tiles, artwork controls, and a portrait layout.
- [Warning!](warning/) — a vertical arcade shooter with sixteen-game waves, a tracking ship, play-history HUD, and unplayed-target alerts.

The previews use the user's configured ES-DE media and real game metadata. No generated game art or example text is bundled. Missing screenshots try miximages; decorative discs and marquees disappear when missing. These are native Hollycast interpretations of the requested concepts, not complete ES-DE ports. See [preview sources and scope](PREVIEW-NOTES.md).
