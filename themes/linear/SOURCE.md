# Linear source and adaptation notes

This package adapts the official ES-DE bundled theme `themes/linear-es-de` into Hollycast theme format 1. The original XML is retained under `source/` as attribution and conversion reference; Hollycast loads only `theme.json` and its PNG asset.

- Original upstream: [ES-DE Linear theme](https://gitlab.com/es-de/emulationstation-de/-/tree/master/themes/linear-es-de)
- Source mirror used to retrieve the files: [RetroDECK/ES-DE](https://github.com/RetroDECK/ES-DE/tree/15e1b236eafd2864bd99ffcc9853d2efa2a3916c/themes/linear-es-de)
- Retrieved mirror commit: `15e1b236eafd2864bd99ffcc9853d2efa2a3916c`
- Retrieved files: `theme.xml`, `colors.xml`, `aspect-ratio-16-9.xml`, `README.md`, `CREDITS.md`, `LICENSE`, `system/logos/dreamcast.svg`, and `system/systemart/dreamcast.webp`.

## Layout mapping

- **Textlist** uses the source text list rectangle from `theme.xml`: `[0.02, 0.174, 0.37, 0.735]`. The still-image variant in `aspect-ratio-16-9.xml` sets `gameImage` to position `[0.592, 0.417]`, maximum size `[0.377, 0.485]`; the component origin is centered at that position, so the native selected-game screenshot rectangle is `[0.4035, 0.1745, 0.377, 0.485]`. Hollycast also displays the available description at source position `[0.405, 0.674]`, size `[0.374, 0.268]`.
- The source `Textlist` metadata cards are centered at `x=0.88`, width `0.17`, height `0.08`. The package places corresponding Hollycast panel/label/value elements in their source vertical positions: release date `0.257`, players `0.340`, genre `0.423`, developer `0.5055`, publisher `0.5885`, last played `0.6715`, and play time `0.7545`. Values map to Hollycast `releaseDate`, `players`, `genre`, `developer`, `publisher`, `lastPlayed`, and `playTime`. Labels are fixed English strings because package elements do not support translated literals. The rating card is omitted because Hollycast has no rating metadata field.
- **Simple Textlist** uses the same source list rectangle. Its `systemLogo` maximum-size box `[0.03, 0.026, 0.18, 0.12]` follows `<pos>0.03 0.146</pos>` with `<origin>0 1</origin>`. Its still-image component uses `<pos>0.66 0.174</pos>`, `<maxSize>0.5 0.69</maxSize>`, and `<origin>0.5 0</origin>`; the native screenshot rectangle `[0.41, 0.174, 0.5, 0.69]` preserves that horizontal center anchor while staying within the canvas.
- **Textlist (OLED black)** keeps the detailed Textlist geometry with the upstream OLED background and metadata panel colors.
- The Textlist `systemWatermarkLogo` source box is centered at `[0.88, 0.887]` with maximum size `[0.145, 0.07]`; its native image box is `[0.8075, 0.852, 0.145, 0.07]`.

Hollycast does not have ES-DE's dynamic `${system.fullName}` / `${system.theme}` elements, gradients, video playback, ratings, or translated static labels. The source system-name header uses the static label “Sega Dreamcast” at its medium-font position `[0.03, 0.05]`; the system-specific secondary label is omitted. Like the static logo, this header stays the same for arcade games. The native header box is taller to accommodate Hollycast's application font. Metadata labels and values use the source medium font size `0.03` and centered alignment, with the description at `0.032`. Screenshot artwork falls back to the current available game artwork when a screenshot is missing.

## Palette and list mapping

The default palette follows `colors.xml` `dark`: background `373737`, primary `FAFAFA`, secondary and tertiary `C2C2C2`, selected list `FFFFFF`, unselected primary `777777`, unselected secondary `BBBBBB`, metadata panel `303030`, label `ABABAB`, and watermark `545454`. The OLED layout uses source background/panel `000000`. Native list styling uses source text size `0.035`, primary list text `777777`, selected text `FAFAFA`, and row height `0.053` (approximately `0.035 × 1.498`, the source line spacing). Source's white narrow selector stripe is represented by a translucent white full-row highlight because Hollycast cannot draw the source's narrow stripe. ES-DE Light, NSO, LIVE, and PSN color schemes cannot be switched independently from layouts in this package format; their original values remain in `source/colors.xml`.

## Source assets

The original Dreamcast SVG logo and system-art WebP are preserved byte-for-byte in `source/original-assets/`. The SVG logo was rasterized to `assets/dreamcast-logo.png` with macOS AppKit (`NSImage`); its output dimensions are 264 × 45 RGBA. The converted logo is used at the source `systemLogo` position in Simple Textlist and the source `systemWatermarkLogo` position in Textlist. ES-DE resolves those image paths from `${system.theme}`; Hollycast image paths are static, so this Dreamcast logo appears for every system, including arcade platforms. The theme uses selected-game screenshots/artwork in the main preview area.

SHA-256:

- `dreamcast.svg`: `663d4d820c99eec39d781534a59a8d50d9766a10e6ce5c52d269465249e04166`
- `dreamcast.webp`: `47327077da9ddd40f97c6658abcbaaf8be66d8b6b37226d11f66bcbf6acaf686`
- `assets/dreamcast-logo.png` (AppKit rasterization of the SVG): `61290bef6d5c37d9e800a0e861396bcab4698801e4f251826139f43af21a9c98`

Font substitution is intentional: upstream Gilroy/Oxygen assets are not bundled, and Hollycast uses its application font. Transitions, carousel layouts, badges, custom video/image variants and localized labels are outside this native package format. `LICENSE`, `CREDITS.md`, and the complete upstream `README.md` are preserved with the XML source.
