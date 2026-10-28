# Art Book Next port source and notices

This Hollycast package adapts **Art Book Next (ES-DE Edition)** by Anthony Caccese. It is based on upstream commit [`d772d07109701d9bd7c9fda305bfef6601105ab8`](https://github.com/anthonycaccese/art-book-next-es-de/tree/d772d07109701d9bd7c9fda305bfef6601105ab8). The upstream README is retained verbatim in [UPSTREAM.md](UPSTREAM.md), including its credits and project notes.

## License

The upstream README identifies the theme as Creative Commons **Attribution-NonCommercial-ShareAlike 2.0 Generic (CC BY-NC-SA 2.0)**: <https://creativecommons.org/licenses/by-nc-sa/2.0/>. This derivative port and its included upstream artwork are distributed under the same license terms. The official license text is included as [CC-BY-NC-SA-2.0.html](CC-BY-NC-SA-2.0.html). This theme is not relicensed under Hollycast's software license.

Upstream credits retained from the README include Dan Patrick's console logo work and Eduardo Tunni's ChangaOne typeface. Hollycast uses its built-in app font because theme packages cannot load custom fonts; it does not bundle those fonts or the upstream logos for every system.

## Upstream source files and adaptation

Source files were fetched from the pinned commit above:

| Upstream file | SHA-256 | Port use |
| --- | --- | --- |
| `theme.xml` | `ed558c03e2903a47231458df136806a0cfe824f51239678f7056f126e4691668` | Establishes the Art Book Next view structure and list/grid styling. |
| `aspect-ratio-16-9.xml` | `c6373f2c35c8e6e842436a7eed93c384eb75da32e84b22b32d93cb7679a924d6` | Source for the 16:9 header, list, and selected-artwork positions. |
| `colors.xml` | `58341b6ce3bad4e3999fac8c5ef3384b54e6620bb827ff46f83b53333b3cb758` | Source for dark-scheme page, list, and selection colors. |
| `README.md` | `46ef5cd5ca8ff49be9a0d4fc7f9e1987e51515fc9eb92c56c1d021fccd549200` | Included verbatim as `UPSTREAM.md`. |
| `_inc/systems/logos/dreamcast.svg` | `663d4d820c99eec39d781534a59a8d50d9766a10e6ce5c52d269465249e04166` | Preserved byte-for-byte in `assets/_inc/systems/logos/dreamcast.svg`; rasterized for Hollycast's static image loader. |

The dark palette copies the source values: `#222222` page, `#111111` list surface, `#444444` selection, white selected text, and white at 33% opacity for unselected text. List variants use the upstream 16:9 text origin x≈0.0410, y≈0.2604 (35×125 on the source 853×480 canvas), medium text size 0.0375, and 1.944 line spacing, represented by Hollycast's row height of about 0.073.

In the list layouts, the upstream selected-art center is x≈0.6876, y≈0.6354. Its box-art maximum size is 0.5551×0.5625, represented as a rectangle from x≈0.4100 to 0.9651 and y≈0.0729 to 0.6354. The screenshot layout uses the source crop width 0.6249 and height 0.6354, ending at y≈0.6354; the small cover is placed at x≈0.8069, y≈0.3333, with size 0.17585×0.3333. The description and metadata elements use their source positions in `aspect-ratio-16-9.xml`.

The grid layout maps the upstream `gamelist-grid-cover` variant: the black grid begins at y≈0.1167; the dark header bar is 0.1167 canvas high; the Dreamcast logo starts at x≈0.0211 and is centered on y=0.0625 with maximum size 0.1524×0.05; and the selected game title is centered at x=0.5, y≈0.062. Hollycast uses six 2:3 cover columns, following the source's standard box-art grid. Grid text size is 0.03, as in the upstream XML.

The source header logo is a system-specific `${system.theme}` asset in ES-DE. Hollycast theme packages use static paths, so this port uses the authentic Dreamcast wordmark in its header for every system. On the grid layout, the static wordmark and current game title follow the upstream grid header anchors; the list layouts retain that header while the selected game title also appears in the list/detail view. The package does not bundle the upstream custom fonts, every system logo, video playback, badges, or help overlay. Those theme-engine elements have no equivalent in Hollycast's static layout format. Dynamic selected cover, screenshot, description, and metadata use each game's local library data.

## Included upstream logo asset

| Packaged path | Upstream path | SHA-256 | Notes |
| --- | --- | --- | --- |
| `assets/_inc/systems/logos/dreamcast.svg` | `_inc/systems/logos/dreamcast.svg` | `663d4d820c99eec39d781534a59a8d50d9766a10e6ce5c52d269465249e04166` | Original white 264×45 SVG, copied byte-for-byte. |
| `assets/dreamcast-logo.png` | Raster of the SVG above | `61290bef6d5c37d9e800a0e861396bcab4698801e4f251826139f43af21a9c98` | 264×45 PNG rasterized with macOS AppKit (`NSImage`) to preserve the SVG viewBox. Used for the header because Hollycast's static image loader does not render SVG. |

To reproduce the PNG on macOS, save the following as `svg_to_png.swift` and run `swift -module-cache-path /tmp/swift-module-cache svg_to_png.swift input.svg output.png`:

```swift
import AppKit
import Foundation
let input = URL(fileURLWithPath: CommandLine.arguments[1])
let output = URL(fileURLWithPath: CommandLine.arguments[2])
guard let image = NSImage(contentsOf: input), let rep = NSBitmapImageRep(data: image.tiffRepresentation!) else { fatalError("could not rasterize SVG") }
guard let data = rep.representation(using: .png, properties: [:]) else { fatalError("could not encode PNG") }
try data.write(to: output)
```

AppKit's SVG raster output can vary by macOS version; the packaged PNG hash above identifies the exact included raster.
