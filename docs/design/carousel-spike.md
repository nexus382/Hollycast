# Carousel layout spike

## Sources

- Official catalogue package: Signal Bloom 1.0.0, `themes/signal-bloom/theme.json`, manifest blob `55a64dbb00df5ec5f2a44b648883dc94357fe3ab` ([source](https://github.com/TnT-Studio-s/Hollycast-themes/blob/main/themes/signal-bloom/theme.json)). The catalogue README describes it as a cover carousel with selected-game fanart, cyan selection and coral play controls, and lists the carousel-enabled development build as a compatibility requirement.
- Existing local implementation evidence: `origin/codex/play-archive-signal-bloom` at `1e5ec822b36c1095c76b54ea4214db6b3c81e3d2`. It includes the earlier generic `flatCarousel` implementation in `core/ui/theme_library.cpp` and parser/input tests. That branch was inspected read-only; its broader changes were not imported.
- Renderer capability constraint from [`pocket-dimension.md`](pocket-dimension.md): “Theme-system capabilities must remain optional and package-independent. Images, fonts, colors, geometry, and actions belong in the package; the renderer supplies generic behavior.”

## Semantics from the package

| Question | Evidence and conclusion |
| --- | --- |
| Arrangement | `signal-gallery` sets `mode: "carousel"`, `browser: [0.04, 0.205, 0.92, 0.43]`, `itemAspect: 0.78`, `columns: 5`, and `showTitles: false`. Its description calls the layout a cover carousel. This establishes a cover-oriented carousel and its browser viewport; the exact item positions are not encoded in the manifest. |
| Selection appearance | `inactiveOpacity: 0.94`, `selectorStyle: "outline"`, and cyan `selectorBorderColor` accompany the catalogue README's cyan-selection description. `artworkSource: "cover"` and the full-viewport background/fanart elements establish shared artwork and element behavior around the browser. |
| Selection and scroll | The manifest has no carousel-specific scroll or page fields. `columns` is shared grid configuration; the prior renderer treats the carousel as continuous selection through the filtered item list, independent of a page index. It centers the selected item and wraps the selection index. This is renderer precedent rather than an explicit catalogue guarantee. |
| Motion and controls | The manifest does not specify animation, keyboard, gamepad, wheel, touch, click, or wrap behavior. The inspected implementation animates selection toward a centered horizontal row; Left/Right and controller direction step through items, confirm activates, the wheel steps, touch swipes step, and mouse clicks select. These are prior renderer behaviors adopted for compatibility, not claims made by Signal Bloom's manifest. |
| `browser` and `navigation` | `browser` is the carousel's normalized interactive viewport. `signal-gallery` does not provide `navigation`; the optional `navigation` field is shared app-chrome placement for layouts with hidden toolbars, not a carousel control or content rectangle. |
| Carousel-specific fields | The manifest uses only `mode: "carousel"` to select the mode. `browser`, `itemAspect`, `columns`, `spacing`, `inactiveOpacity`, `artworkSource`, selector fields, elements, responsive layouts, activation and media requirements are shared layout/package fields. No carousel-only manifest field is needed for the documented layout. |

## Parser compatibility review

The Signal Bloom manifest fields used by its carousel layout are recognized by the current parser, including nested `elements` properties such as `bleed`, `zIndex`, `fallback`, `fit`, and `action`, and shared `list`, toolbar, responsive-layout, and media fields in its alternate layouts. The former incompatibility was the `carousel` mode downgrade to `grid`; the parser now preserves `carousel`. The carousel renderer uses the generic browser rectangle, artwork source, item aspect, spacing, inactive opacity, rounding, selector colors and item keys. It does not branch on the package ID or bundle Signal Bloom assets.

`columns` caps the visible covers and `spacing` sets the gap within the
browser width. Signal Bloom requests five covers. The renderer keeps the
selection centered and uses the same filtered list for wrapping.
The official Signal Bloom 1.0.0 package rendered its background and five
cover slots in a macOS arm64 debug app with 96 test entries. Left and Right
wrapped between the first and last entry and returned the selection to the
center. The fixtures exercised title fallbacks for missing artwork. Physical
controller input, touch devices, and launching a playable game remain unverified.

## Open points

The catalogue file does not define whether future carousel themes expect snapping, wrap, gesture, or animation details beyond the `mode` name and common layout fields. This implementation follows existing branch behavior for those shared interactions. A future catalogue version that adds carousel-specific fields should require a separately scoped parser and renderer review.
