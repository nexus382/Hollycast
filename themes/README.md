<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast theme collection

These are native Hollycast adaptations of three established ES-DE themes. Extract a theme folder, then use **Settings > Library > Library Themes > Install Library Theme Folder**. A package is self-contained; keep its assets and notices alongside `theme.json`.

| Theme | Original author | Style | Source and port notes |
| --- | --- | --- | --- |
| [Art Book Next](art-book-next/theme.json) | Anthony Caccese | Editorial lists and large selected-game media | [Source, mapping and license](art-book-next/SOURCE.md) |
| [Iconic](iconic/theme.json) | Siddy212 | Selected-game miximage backgrounds with front covers and list or grid browsing | [Source, mapping and notices](iconic/SOURCE.md) |
| [Linear](linear/theme.json) | ES-DE project, based on ESonline by Weestuarty | Dense text lists and restrained metadata panels | [Source, mapping and license](linear/SOURCE.md) |

The ports cover static game-library views. Hollycast's menus and toolbar remain its own interface, and its application font substitutes for ES-DE theme fonts. ES-DE system carousels, video playback, transitions and dynamic badges are not reproduced. Each package's `SOURCE.md` describes its specific adaptations and retained upstream assets. Art Book Next and Linear retain static Dreamcast branding. Iconic uses the highlighted game's media without a fixed platform logo.

Theme files retain their own upstream licenses. Art Book Next uses CC BY-NC-SA 2.0. Iconic's upstream README and LICENSE carry differing notices; both are included, and the port retains the README's attribution and noncommercial share-alike terms. Linear includes its MIT license and upstream asset credits. These theme assets are separate from Hollycast's GPL-licensed application code.

[Midnight](midnight/theme.json) is the original Hollycast example for authors. See the [theme author guide](../docs/Themes.md) to create or modify a package.

## Starred preview layouts

These six additional packages implement the Hollycast game-list mockups. Their names end in `*` so they are easy to find beside the existing ports. They require the renderer features documented in the current author guide. Existing packages keep their original IDs and files.

| Package | Media and layout |
| --- | --- |
| [Iconic Dark Grid *](preview-iconic-dark-grid/theme.json) | Selected miximage backdrop, independent front cover, dark details panel and right cover grid. |
| [Iconic Light List *](preview-iconic-light-list/theme.json) | Miximage backdrop with a pale gradient, centered front cover, white information cards and right list. |
| [Horizon Grid *](preview-horizon-grid/theme.json) | Blue-to-navy gradient, screenshot with overlapping front cover, right cover grid. |
| [Epic Noir List *](preview-epic-noir-list/theme.json) | Left list, large precomposed miximage and details beneath. |
| [Art Book Next *](preview-art-book-next/theme.json) | Selected cover background, left list, and a large miximage above a scraped front cover and details. |
| [Fanart & Disc *](preview-fanart-disc/theme.json) | Fanart backdrop, screenshot, front cover, disc and marquee, with a right thumbnail list. |

The previews use the user's configured ES-DE media and real game metadata. No generated game art or example text is bundled. Missing screenshots try miximages; decorative discs and marquees disappear when missing. These are native Hollycast interpretations of the requested concepts, not complete ES-DE ports. See [preview sources and scope](PREVIEW-NOTES.md).
