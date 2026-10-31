<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast themes

Hollycast theme packages are JSON descriptions of the app palette and game-library layouts. This is Hollycast's own **theme format version 1**; an ES-DE theme or theme pack is not compatible.

## Included library themes

Every standalone download includes all eight library themes and their assets inside
the executable or app. On startup Hollycast restores missing packages to its data
folder and updates bundled assets when their content version changes. No theme
installation or reload action is required. Open **Settings > Library > Library
Themes**, choose **Library Theme**, then **Library Layout**. **App UI Theme** returns
to the standard library. This packaging is shared by Windows, Linux, macOS,
Android, iOS and Switch; libretro uses its frontend's library.

New settings default to **Demo Disc**. The first-run checklist lets players keep
that theme, choose another bundled theme, or use the original icon or list view.
Existing saved selections are preserved. Every library style centers the same
**Add Game Folder** action in its game browser when no content folder is configured,
keeping the theme background visible without a separate prompt panel.

Every bundled layout provides a visible **Play** button. Touching a game only
selects it so its artwork and information can be inspected, including on a second
tap. Swipe anywhere inside a list, grid, shelf or details pane to scroll it;
orbit swipes turn the discs and paged-grid swipes change pages. Controller
directions (D-pad or left stick) browse games and controls, confirm activates the focused item/button,
and cancel leaves a details pane or popup. Mouse double-click and keyboard confirm
remain available. The original icon and table views keep their activation rules.

For theme development, an additional package folder may still be placed under
`library-themes/` in the app data directory and loaded by restarting the app. Its
folder name must match the manifest's `id`. Bundled IDs are maintained by Hollycast;
use a separate ID for local modifications.

Library packages live under `library-themes/` in the app data directory, separately from the UI editor's JSON theme files. Selecting or saving a UI theme clears the library package selection. Opening the UI editor temporarily previews that UI theme; cancelling restores the library package.

With a library theme active, **Theme Icon Media** beside the layout selector chooses the scraped media used in its grid or list thumbnails. Choices include covers, miximages, 3D boxes, screenshots, marquees, discs, fanart and title screens. **Theme Default** restores the package's choice. Selecting a source also enables thumbnails in text-list layouts. This setting affects browser icons independently of the selected game's background and detail images. Explicit media bindings prefer the matching ES-DE folders; missing files fall back to the existing library artwork. Canonical folders such as `covers`, `miximages` and `marquees` take priority over aliases and legacy `images` files.

The app loads only immediate child folders of the library themes directory. A broken package is reported in theme settings and is not selectable. If a selected theme or layout is unavailable, Hollycast falls back to the selected UI theme or the first layout in the selected package. The installed-theme folder is user data; keep its `id` and containing folder name identical.

## Minimal package

This is the smallest useful `theme.json`:

```json
{
  "formatVersion": 1,
  "id": "sample",
  "name": "Sample",
  "layouts": [
    {
      "id": "grid",
      "name": "Grid",
      "mode": "grid",
      "browser": [0.05, 0.05, 0.9, 0.9]
    }
  ]
}
```

The [theme collection](../themes/README.md) contains original themes and starred examples using grids, lists, CD racks, selected-game artwork and metadata. Each package includes its source notes.

## Manifest reference

The root must be a JSON object. Unknown fields are errors. The UTF-8 `theme.json` file must be no larger than **256 KiB**.

| Field | Required | Meaning |
| --- | --- | --- |
| `formatVersion` | Yes | Must be integer `1`. |
| `id` | Yes | 1–64 lowercase ASCII letters, digits, hyphens, or underscores. Must equal the package folder name. |
| `name` | Yes | Display name. |
| `author` | No | Author label. |
| `description` | No | Short description shown in settings. |
| `baseTheme` | No | Base palette before `colors` overrides it. Integer 0–5; defaults to `0`. See below. |
| `colors` | No | Object of ImGui color names and 6- or 8-digit hexadecimal strings. Six digits imply opaque alpha. Names omit the `ImGuiCol_` prefix. |
| `fonts` | No | Optional `body` and `bold` package-relative TrueType/OpenType font paths. Omit to retain the application fonts. Each file must have a valid SFNT directory and be at most 32 MiB. The normal font loader validates the font and retains existing language/emoji fallbacks. |
| `layouts` | Yes | Array of 1–16 layouts. |

Base palette numbers are `0` Dark, `1` Light, `2` Dreamcast, `3` High Contrast, `4` Nintendo, and `5` Aqua Chill. A custom theme starts from this palette, then applies each entry in `colors`.

Supported `colors` names are: `Text`, `TextDisabled`, `WindowBg`, `ChildBg`, `PopupBg`, `Border`, `BorderShadow`, `FrameBg`, `FrameBgHovered`, `FrameBgActive`, `TitleBg`, `TitleBgActive`, `TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`, `ScrollbarGrab`, `ScrollbarGrabHovered`, `ScrollbarGrabActive`, `CheckMark`, `SliderGrab`, `SliderGrabActive`, `Button`, `ButtonHovered`, `ButtonActive`, `Header`, `HeaderHovered`, `HeaderActive`, `Separator`, `SeparatorHovered`, `SeparatorActive`, `ResizeGrip`, `ResizeGripHovered`, `ResizeGripActive`, `InputTextCursor`, `TabHovered`, `Tab`, `TabSelected`, `TabSelectedOverline`, `TabDimmed`, `TabDimmedSelected`, `TabDimmedSelectedOverline`, `PlotLines`, `PlotLinesHovered`, `PlotHistogram`, `PlotHistogramHovered`, `TableHeaderBg`, `TableBorderStrong`, `TableBorderLight`, `TableRowBg`, `TableRowBgAlt`, `TextLink`, `TextSelectedBg`, `TreeLines`, `DragDropTarget`, `DragDropTargetBg`, `UnsavedMarker`, `NavCursor`, `NavWindowingHighlight`, `NavWindowingDimBg`, and `ModalWindowDimBg`.

## Layouts

Each layout is an object. Unknown fields are errors. Required fields are `id`, `name`, `mode`, and `browser`.

The library uses a General UI Scaling baseline of 100%, retaining platform/DPI
scaling. Library Scale (also labelled Icon Size in the toolbar) controls browser
artwork and the opposite cover/information column. That column scrolls when
enlarged content exceeds the viewport; full-canvas backgrounds stay fixed.
Rack cases remain bounded to fit a complete case and shelf on screen.

| Field | Meaning and bounds |
| --- | --- |
| `id` | Unique within the package; 1–64 lowercase ASCII letters, digits, hyphens, or underscores. |
| `name` | Display label used by **Library Layout**. |
| `mode` | `grid`, `list`, `rack` (vertical jewel-case spines on scrollable shelves), `rail`, or `orbit` (upright circular disc carousel). See [Rail mode](#rail-mode). |
| `browser` | `[x, y, width, height]` rectangle for the interactive game browser. Coordinates are fractions of the library content area. Position and size must fit wholly inside 0–1; width and height must each be at least `0.1`. |
| `columns` | Grid column count, integer 1–12, or 1–64 for racks. Default `4`; list mode ignores it. The library Icon Size control adjusts the displayed column count to resize artwork. |
| `itemAspect` | Cell width divided by height, from `0.3` to `3`, or `0.03` to `3` for racks. Default `0.8`; list mode ignores it. Rack heights are bounded to keep spines readable. |
| `spacing` | Logical UI pixels between browser items, from `0` to `48`. Default `12`. |
| `fontSize` | Optional browser text height as a fraction of canvas height, from `0.01` to `0.08`; clamped to 10–96 screen pixels. Omit to inherit the application font size. |
| `rowHeight` | Optional list row height as a fraction of canvas height, from `0.02` to `0.2`. Rows stay large enough to fit their text and padding. Omit for automatic sizing. |
| `textColor` | Optional browser title color, as 6- or 8-digit hex. Defaults to the app text color. |
| `selectedTextColor` | Optional selected/hovered browser title color. Defaults to `textColor`. |
| `selectorColor` | Optional browser selection color. Defaults to the app selection color. |
| `showTitles` | Whether grid cells show their title, default `true`. List rows always show the game title. |
| `background` | Library canvas color, 6- or 8-digit hex; default `151820ff`. |
| `elements` | Optional array of up to 64 decorative or selected-game elements. |
| `artworkSource` | Browser artwork: empty (default library media), `cover`, `miximage`, `case`, `screenshot`, `fanart`, `title`, `physical`, `titlescreen`, `banner`, `backcover`, `spine`, `disc`, or `boxtexture`. Missing media falls back to library artwork, except racks, which show a vertical title label. Rack spines fill the case height with proportional widths and wrap onto shelves; horizontal scans rotate to fit. |
| `rounding` | Browser selection/image corner radius in logical UI pixels, 0–48; default `0`. |
| `inactiveOpacity` | Unselected grid artwork opacity, 0.1–1; default `1`. |
| `selectorBorderColor` | Optional selection outline color, separate from the selection fill. |
| `separatorColor` | Optional list-row divider or rack shelf-edge color. |
| `showArtwork` | Show square artwork thumbnails beside list titles; default `false`. |
| `compactControls` | Show favourite buttons only on hovered/focused items or saved favourites; default `false`. Context-menu actions remain available. |
| `showInfoTooltip` | Show the app's hover information popup; default `true`. Layouts with their own details can disable it. |
| `canvasAspect` | Optional fixed canvas width/height ratio, `0.4`–`3`. The canvas fits and centers within the content area, preserving authored asset proportions. Omit to use the complete content area as before. |
| `toolbar` | `standard` (default) or `compact`. Compact uses smaller shared library controls and a **Filter** button that expands search. All Games, Continue Playing, Favourites, and Settings remain available. |
| `toolbarSelectedTextColor` | Optional foreground for the selected compact-toolbar view. Omit to inherit app text. The selected fill uses the app palette's `ButtonActive`. |
| `toolbarLogo` | Optional package-relative image replacing the compact toolbar's text brand. It retains its aspect ratio at the toolbar control height. Omit to keep text branding. |
| `toolbarLogoHeight` | Optional logo height in logical UI pixels, `16`–`96`. Requires `toolbarLogo`; its original aspect ratio is preserved. Omit to match the control height. |
| `activation` | Mouse activation: `singleClick` (default) or `doubleClick`. In double-click mode, a single click selects/previews; double-click, Enter/controller confirm, or an `activate` button launches the game. Touch always selects/previews, so every layout must provide a visible `activate` button. |
| `minItemWidth` | Minimum grid-cell width in logical UI pixels, `32`–`256`; defaults to `96`. This controls when the renderer reduces the requested column count. |
| `titleStyle` | `full` (default) or `initials`. Initials shorten grid labels only; titles used by metadata, search, and game actions remain complete. Whole UTF-8 characters are retained. |
| `selectorStyle` | `fill` (default) or `outline`. Outline preserves artwork contrast on low-color displays. |
| `pixelArt` | Optional object with required integer `resolution` (`4`–`128`) and optional `dark`/`light` hex colors. The browser converts artwork to four tones at that maximum pixel dimension and samples it with nearest-neighbor filtering. The effect has its own texture-cache key; it never changes the original image or other theme views. Omit for original artwork. |
| `placeholder` | Optional package-relative image used by grid items with no artwork. Omit for the existing empty-artwork behavior. |
| `showFavouriteMarker` | Optional passive star for favourited browser items, default `false`. Uses the layout text color, or a dot-grid star in the `pixelArt` palette. Adds no interactive control. |
| `showItemActions` | Whether to draw favourite buttons on tiles, default `true`. Disabling them retains context-menu and long-press game actions. |
| `hoverDetails` | Whether to show the standard game-details tooltip, default `true`. Themes with their own details panel can disable it. |
| `previewSource` | Initial named artwork for `source: "preview"` elements; defaults to `cover`. The shared artwork buttons can switch it. |
| `narrowLayout` | Optional ID of another layout in the same package, used automatically when the available content width/height falls below `narrowAspect`. It does not change the saved layout selection. The fallback is resolved once; it does not recursively follow other fallbacks. |
| `narrowAspect` | Threshold for `narrowLayout`, `0.4`–`3`; defaults to `1.1`. Requires `narrowLayout`. The compact toolbar uses the fallback's logo settings too. |
| `rail` | Optional appearance of a `rail` browser. Only valid in rail mode. See [Rail mode](#rail-mode). |
| `sounds` | Optional object of package WAV files: `move` plays when the player selects another game, by any input, and `launch` plays at the start of a `launch` sequence. Each must be 44.1 kHz 16-bit PCM, mono or stereo, and at most 1 MiB. Sounds follow the app's audio volume and never play in attract mode or during a game. A launch sound is cut off when the game starts, so keep it no longer than the sequence. |
| `launch` | Optional object with required `seconds` (`0.1`–`5`): a short sequence between choosing a game and starting it, for elements with `when: "launching"` to play in. Nothing else can be chosen meanwhile, and any fresh press skips straight to the game. Omit to start games at once. |
| `attract` | Optional object with required `idleSeconds` (`1`–`3600`) and optional `intervalSeconds` (`0.5`–`600`, default `3.5`). After that long without player input, the selection steps through the library and wraps at the end, in any browser mode. Any input, open menu, or text field stops it at once. Omit to disable. |

Grid cells use the selected browser artwork source and optional titles. List rows retain native navigation and ellipsized titles. Optional button elements provide explicit shared library actions.

## Rail mode

`"mode": "rail"` draws a perspective list that keeps the selected game at the center of the `browser` rectangle. Rows select when pressed or focused, never on hover, so a row sliding under a resting pointer cannot change the selection. Clicks, the wheel, and drags move along the rail, stopping at each end. Up and Down, on the keyboard or D-pad, step one game at a time and only leave the rail at its ends, so controls above and below stay reachable. Right moves to the layout's first enabled button, and returning to the rail never changes the selection. `rowHeight` sets the center row's height; omit it to fit `visible` rows on each side. `fontSize`, `textColor`, `selectedTextColor`, `selectorColor` (the selected row's fill), and `activation` work as they do for lists.

| `rail` field | Meaning and bounds |
| --- | --- |
| `visible` | Rows drawn on each side of the selection, `1`–`8`; default `4`. |
| `center` | Where the selected row sits, as a fraction of browser height, `0.1`–`0.9`; default `0.5`. A value nearer the top keeps a short library from leaving a gap above it. |
| `tilt` | Row inset per row of distance, as a fraction of browser width, `0`–`0.1`; default `0.03`. |
| `skew` | Right-edge slant of each row, as a fraction of browser width, `0`–`0.1`; default `0.03`. |
| `falloff` | Scale lost per row of distance, `0`–`0.1`; default `0.05`. |
| `fade` | Opacity lost per row of distance, `0`–`0.25`; default `0.14`. |
| `rowColor` | Fill of unselected rows. Omit to use the app palette's `FrameBg`. |
| `numbers` | Whether rows show their position in the current view; default `true`. |
| `favouriteMark` | Whether to draw a star on the rows of favourite games; default `false`. |
| `badges` | Optional array of 1–4 tags for each row, drawn left to right. Each tag is an array of metadata fields and shows the first with a value, such as `[["system"], ["genre", "region", "format"]]`. A tag with no value is left out, and a long value is clipped so the title keeps most of the row. Omit for no tags. |

## Elements

Every element is an object with required `type` and `rect`. Element rectangles use the same normalized coordinate system as `browser`, must fit inside 0–1, and must be at least `0.01` wide and high. Panels allow dimensions down to `0.001` for thin rules. Unknown fields are errors.

| Field | Meaning and bounds |
| --- | --- |
| `type` | `panel`, `image`, `artwork`, `text`, `metadata`, `details`, or `button`. |
| `rect` | `[x, y, width, height]` normalized to the full library content area. |
| `source` | Required for `image`; a library artwork name for `artwork`; a metadata field for `metadata`. |
| `text` | Literal string for `text`; optional fallback for a `metadata` field that is absent or empty. Omit to leave missing metadata blank. |
| `fallbackImage` | Optional package-relative image for `artwork` when the requested library media path is empty. Omit to preserve an empty artwork region. Uses the same fit/tint and path validation as other package images. |
| `color` | 6- or 8-digit hexadecimal string; default `ffffffff`. Panel/text color or image/artwork tint, including alpha. |
| `fit` | `contain` (default), `cover`, or `stretch`. Images and artwork either fit inside the rectangle, crop centrally to fill it, or stretch to its dimensions. |
| `align` | Text alignment: `left` (default), `center`, or `right`. Applies to the wrapped text block. |
| `endColor` | Optional second panel color, including alpha, for a smooth gradient from `color`. |
| `gradient` | Panel gradient direction: `vertical` (top to bottom, default) or `horizontal` (left to right). |
| `bold` | Use the app's bold font for text or metadata; default `false`. |
| `wrap` | Wrap text into the rectangle, default `true`. Set `false` for one line with an ellipsis. Wrapped text clips at complete lines. |
| `fallback` | Missing artwork: `default` uses the game's library art; `none` leaves the element empty. A named artwork source tries that source before default art. Defaults to `default`. |
| `requiresArtwork` | Optional artwork source that must be present before drawing this artwork element. For example, require `screenshot` on cover/disc/logo overlays so they disappear when a hero uses a precomposed miximage instead. Empty (default) has no condition. |
| `fontSize` | Text height as a fraction of the library canvas height, from `0.01` to `0.15`; default `0.035`. At render time it is clamped to 10–96 UI pixels. Uses the application font, including optional package fonts. |
| `zIndex` | Integer from `-100` to `100`; default `0`. Negative elements draw behind the browser. Zero or positive decorative elements draw above it and can obscure its visuals. Buttons draw after decoration. |
| `action` | Required only for `button`: `previous`, `next`, `up`, `down`, `activate`, `artwork`, or `favourite`. Buttons require a nonnegative `zIndex` and render as independent input regions above decoration. |
| `background` | Optional button fill color. `00000000` permits a transparent hit region over a package image, such as a physical button. Omit to use the selected app palette. `color` controls button text. |
| `selectedColor`, `selectedBackground` | Optional text/fill overrides for a selected artwork choice or a game marked as favourite. They allow shared-state controls without a theme-specific widget. |
| `launchFadeFrom` | Requires `when: "launching"`. Fades the element in from this fraction of the launch sequence to its end, `0`–`0.99`. |
| `when` | Limits a decorative element to one state: `attract` while attract mode is running, `interactive` otherwise, or `launching` during a `launch` sequence. Launching elements with a nonnegative `zIndex` draw over everything, including buttons. Not valid on `button` or `details`. Omit to always draw it. |
| `bleed` | When `true`, `rect` and `spin` use the whole library area instead of the `canvasAspect` canvas, so backgrounds and bands can reach the window edges. Not valid on `button` or `details`. Defaults to `false`. |
| `slant` | Text and metadata only. Shears the text like an italic, as horizontal shift per unit of height, `0`–`0.5`. Useful because bundled fonts may have no italic. |
| `marqueeSeconds` | Text and metadata only. Scrolls one line from right to left, taking this many seconds to travel the rectangle's width, `0.5`–`600`, then repeats. Any length of text moves at the same speed, and new text starts again from the right edge. |
| `sweepSeconds` | Images only. Moves the image from above to below its rectangle in this many seconds, `0.5`–`600`, then repeats, clipped to the rectangle. The image keeps its aspect ratio at the rectangle's width. |
| `spin` | Images and artwork; not combinable with `sweepSeconds`. An object with required `seconds` per turn (`0.5`–`600`), optional `launchSeconds` (`0.02`–`600`), the period it accelerates to during a `launch` sequence, `center` as `[x, y]` canvas fractions (`-1`–`2`, so a disc can hang off the canvas edge), `radius` as a fraction of canvas width (`0.01`–`2`), and optional `hole` (`0`–`0.9`), which cuts the image to a round disc with that fraction of its radius left open, so square art still spins as a disc. The rectangle clips the result. Spinning artwork with no image and no `fallback` draws nothing, so an image beneath it can stand in. |
| `fields` | `details` (required) or `metadata`. On `metadata`, replaces `source` with an ordered array of metadata fields whose values are joined; fields with no value are skipped. If none has a value, `text` is shown instead. |
| `separator` | `metadata` with `fields` only. The text placed between values; default `  /  `. |
| `image`, `hoverImage`, `selectedImage`, `selectedHoverImage` | Buttons only. Package images that replace the button's drawn background, stretched to its rectangle. `image` is required for the others to apply. Hover images also show on keyboard or controller focus, and each state falls back to the closest one given. A disabled button dims its image. |
| `selectedText` | Optional visible label for a selected artwork/favourite button. The control keeps its stable widget ID when this text changes. |
| `icon`, `selectedIcon` | Optional vector button icon, `star` (outline) or `star-filled`. `selectedIcon` overrides `icon` for a selected artwork/favourite button. Icons inherit the text color, sit beside the label, and require no icon font. Icon and label scale together to fit a narrow button. |
| `borderColor`, `selectedBorderColor` | Optional button outline and selected-state override. Uses a one-logical-pixel border and the button's rounding. Outlined controls with an explicit fill also get tinted hover feedback. Omit to retain the app's existing border style. |
| `rounding` | For panels/images/artwork: logical-pixel corner radius, `0`–`48`, default `0`. For buttons: optional fraction of the smaller dimension, `0`–`0.5`; omit to inherit the app style. |
| `fields` | Required for `details`: a nonempty array of unique supported metadata field names, in display order. Absent or empty game values are hidden. Description wraps below the other fields. Only valid on `details`. |
| `columns` | Optional number of label/value pairs per `details` row, integer `1`–`3`; default `1`. Only valid on `details`. |
| `autoFit` | Details only. `true` compacts label/value spacing and fits the complete metadata and description into the rectangle without scrolling. Reflow is cached until the text, font or viewport changes. Default `false` keeps normal scrolling, including finger dragging and controller scrolling. |

Element types:

- **`panel`** fills its rectangle with `color`, optionally grading into `endColor`, with rounded corners when requested.
- **`image`** draws a package image using `fit` and `color`. Its `source` is a relative file path inside the package, such as `images/panel.png`. Use PNG or JPEG; SVG and WebP must be rasterized or decoded first. Absolute paths, `..`, and paths escaping through symlinks are rejected. The referenced file must exist.
- **`artwork`** draws selected-game media using `fit`: default art, `cover`, `miximage`, `case`, `screenshot`, `fanart`, `title`, `physical`, `titlescreen`, `banner`, `backcover`, `spine`, `disc`, or `boxtexture`. Named slots prefer matching local media, then the selected provider and TGDB fallback, and obey `fallback`. `preview` follows the selected artwork button; unavailable non-cover choices remain empty instead of borrowing cover art. A `fallbackImage` package asset can fill an empty region. Backgrounds and details follow mouse and keyboard/controller selection.
- **`text`** draws the literal `text` value.
- **`metadata`** draws the selected game's value for `source`; a missing value draws nothing. Supported fields: `title`, `description`, `developer`, `publisher`, `genre`, `players`, `releaseDate`, `productId`, `region`, `format`, `playTime`, `lastPlayed`, `size`, and `system`, which names the hardware a game runs on: `Dreamcast`, `Naomi`, `Naomi 2`, `Atomiswave`, or `System SP`.
- **`details`** displays selected-game metadata with translated field labels in its own scrollable rectangle. `fields` chooses the content and order; `columns` controls density. Empty fields are omitted, long values wrap, and the panel scrolls back to the top when the selected game changes. It requires a nonnegative `zIndex`, and retains normal mouse, keyboard, and controller scrolling. Other themes have no details panel unless they add this element.
- **`button`** uses the same normalized rectangles as other elements. `previous` and `next` wrap through filtered games; `up` and `down` move by the current grid column count (one row in a list). Navigation scrolls the selected game into view. `activate` launches it using the normal library path. An `artwork` button requires a named artwork `source` and switches `preview` elements to that source; unavailable choices are disabled with an explanatory tooltip. `favourite` toggles the selected game's persistent favourite state, including in the Favourites view. BIOS/device entries disable that action. Empty libraries disable the controls. `text` may be empty for an asset-backed hit region. Keyboard/controller activation and focus remain native ImGui widgets.

Explicit `details` elements retain their own authored scrollable regions; layouts without them use the shared scaled cover/information column. The current selected item is shown in detail areas outside the browser rectangle. Put background panels there at a negative `zIndex`, and keep text and artwork in that area at `zIndex: 0` or greater. Elements above the browser are not clipped to the browser and can obscure its items.

Themes do not run scripts or load remote resources. Keep a package self-contained and use only package-relative paths for images and fonts. Every presentation extension above is optional; existing manifests keep their previous behavior. [Pocket Dimension](../themes/pocket-dimension/theme.json) demonstrates them together without any package-specific renderer code.

Artwork availability comes from the shared library. LaunchBox, ScreenScraper and TheGamesDB use the same named theme bindings, while keeping their provider caches separate. Matching local ES-DE artwork keeps priority, followed by the selected provider and TheGamesDB fallback; existing images from another provider remain usable. Basic views request covers, hover miximages, title logos and metadata. Selecting a theme queues its additional manifest requirements and shows progress. **Settings > Library > Artwork Downloads > Download All Media for All Themes** prepares the union used by installed layouts. Disc scans use `physical`; front box artwork uses `cover`, 3D boxes use `case`, and back covers and spines use `backcover` and `spine`. These bindings are reusable and opt-in for every theme. LaunchBox supplies ten categories including discs and box artwork; ScreenScraper also supplies `miximage`. ScreenScraper completes first; LaunchBox then fills only missing requested types, followed by TheGamesDB after the LaunchBox pass completes. Earlier artwork and metadata retain priority. See [LaunchBox artwork](launchbox-artwork.md), [TheGamesDB media](thegamesdb.md) and [ScreenScraper setup](screenscraper.md).

## Troubleshooting

- Check that the folder name and manifest `id` match exactly, including lowercase spelling.
- Keep the manifest under 256 KiB and use only documented fields and values; the parser reports the invalid field in Settings.
- Ensure each layout ID is unique and every normalized rectangle stays within the canvas.
- Verify referenced image files exist inside the theme folder. Avoid symlinks that point outside it.
- After editing or copying a package manually, press **Reload Library Themes**. A failed package appears with its validation error and is skipped.

## Paged grids, play-history stats, and selection effects

[Warning!](../themes/warning/theme.json) combines these optional primitives. Existing
layouts retain their scrolling browser and decorative behavior when they omit them.

A `grid` layout can add `pagedGrid: {"rows": 4, "padding": 4}`. This fits a fixed
number of rows into the browser and submits only the selected page, keeping large
libraries bounded. The normal adaptive `columns`, `spacing`, and library icon scale
still apply. `rows` is an integer from 1–12, and `padding` is 0–24 logical pixels
inside each cell. Optional `frame` and `unplayedFrame` are package image overlays;
the latter applies only to games without recorded play history. Device entries
use the regular frame. Both paths use the standard package path validation.

The `previousPage` and `nextPage` button actions require `pagedGrid`. They stop at
the first and last pages. Wheel and Page Up / Page Down page the browser; arrows
and controller navigation can cross a page boundary. Click selects, and activation
still follows `activation`, including double-click and controller confirm.

Two decorative element types, `stat` and `meter`, require a numeric `source`:

| Source | Value |
| --- | --- |
| `gameCount`, `favouriteCount`, `unplayedCount` | Counts in the current filtered view, excluding BIOS/devices. Unplayed means no recorded duration or launch. |
| `totalPlayHours`, `maxPlayHours` | Total and greatest recorded game duration, in hours. |
| `playHours` | Selected game's recorded hours; absent for a device. |
| `neglect` | Selected game's `log2(days since launch + 1) / 9`, clamped to 0.05–1; 1 for an unplayed game, absent when a played game's launch date is unknown. Future dates clamp to zero days. |
| `position`, `page`, `pageCount` | One-based entry/page position, and total pages (including device entries). Empty views return zero. Pages use the actual adaptive grid capacity. |

A `stat` shows the integer part of its value times optional `valueScale`
(0.001–1,000,000, default 1), with optional `digits` zero padding (0–12, default 0).
It accepts normal text size, color, alignment, wrapping, and bold styling. `text`
is the fallback for an absent selected-game value. A `meter` fills its rectangle
horizontally by `value * valueScale / maximum`, clipped to 0–1. `maximum` is
0.001–1,000,000,000, default 1. A panel beneath can provide its empty track.
Numbers come from raw play history rather than localized metadata strings.

Decorative elements can add `anchor: "selection"` to center their rectangle on
the selected item, or `"selectionX"` to follow its horizontal center while keeping
the authored vertical position. `"selectionTrail"` uses the authored width and
vertical start as a narrow strip from the selected item's bottom to that start.
These elements have no input region, hide when the selected item has no visible
rectangle, and cannot combine with `bleed`. They work with any browser mode.

`onSelection` (0.05–5 seconds) limits a decoration to a brief period after the
selection changes, with a fade during its final quarter. It retriggers on a new
selection, not every frame. New `when` conditions `played` and `unplayed` limit
it to the selected game's recorded history and exclude devices. Combine them
for event banners. Interactive `button` and `details` elements cannot use these
visibility or anchor fields.

An image can add `tileSeconds` (0.5–600) to scroll repeated copies down its
rectangle seamlessly, one image-height per period, preserving its aspect ratio.
It cannot combine with `spin` or `sweepSeconds`. This uses package artwork and
shares the usual asset validation; themes still execute no scripts.

## Disc orbit mode

`mode: "orbit"` arranges up to seven upright, circular game discs around a perspective ring. Left/Right and controller navigation rotate through the full filtered library; the mouse wheel also steps, click selects, and double-click or confirm plays. The selected disc rises from the ring. `artworkSource: "disc"` prefers physical media and embedded labels with existing artwork as fallback. Library Icon Size scales the discs within the browser viewport. Other layout elements provide the selected miximage, title and metadata. This mode uses the shared draw-list renderer on standalone platforms.

## Automatic media requirements

The two standard library views prepare only covers, miximages and title logos for hover details,
and game metadata. Disc labels are read locally from the game; no extra image
categories are scraped until a theme or a manual download action needs them.
Selecting a theme queues its missing media for the whole library and shows progress
in the library and theme picker. Downloads continue while settings are open.

Requirements come from `artworkSource`, artwork element `source`, named `fallback`,
`requiresArtwork`, preview media and artwork-selection buttons. Responsive fallback
layouts are included. For assets consumed only by an animation, a layout may add
`"requiredMedia": ["boxtexture", "backcover"]`, as CD Rack does. Source names use
the same provider-independent media registry as all artwork bindings. New themes
must describe media here; do not start scraping from a renderer or branch on a
package ID. Adding a media category to the registry automatically exposes its bit
to requirement collection, provider queues and progress reporting.

**Download All Media for All Themes** prepares the union of media referenced by all
valid installed layouts. **Download Selected Media** remains available for explicit
category choices. Existing ES-DE images and provider caches are reused; switching
themes does not redownload them. Metadata-only lookup is independent of image
requests, so local covers do not prevent game information being fetched.

Progress counts ready, pending and unavailable image/metadata units, rather than
HTTP requests. The hover media picker shows missing baseline categories and optional
media already on disk; it never treats every supported image type as required.
A missing or unavailable title logo falls back to the game title in text.
Unavailable includes unsupported provider categories and unmatched
games; it is never reported as a successful download. Provider quotas, paused
sessions and missing credentials can leave work pending. Choose ScreenScraper for
miximages; other providers do not supply them. The existing retry action can retry
unmatched or unavailable media without deleting successful images.

The library inventories and enqueues media in bounded UI slices after the local
index is ready. The queue holds extra-media jobs until all covers have been queued;
provider workers then refill independently. Only pending categories are polled.
An unreadable configured local folder pauses inventory instead of treating unknown
files as absent; fix the path and refresh the artwork index to resume.
Repeated requests merge into the existing source job, and concurrent ScreenScraper
disc entries share each game/type/region cache file rather than downloading it twice.
Keep these rules in the shared queue when adding themes; never add per-frame scrape
requests or a second artwork cache in a theme renderer.

## Ordered scrape passes

ScreenScraper is the default initial source. It completes all queued covers,
additional requested images and game lookups before any LaunchBox fallback starts.
LaunchBox completes its missing-media pass before TheGamesDB is tried. Backoff,
quota pauses and in-flight jobs do not count as completion. New primary work added
while a fallback is running takes priority before further fallback jobs begin.
Only missing requested categories are passed on; unsupported categories are skipped.
Successful files and unavailable-category checks persist across restarts, and later
providers fill gaps without overwriting earlier provider metadata.
