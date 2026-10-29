<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast themes

Hollycast theme packages are JSON descriptions of the app palette and game-library layouts. This is Hollycast's own **theme format version 1**; an ES-DE theme or theme pack is not compatible.

## Install a theme

A theme folder contains `theme.json` at its root. Its folder name must match the manifest's `id` exactly. For example:

```text
midnight/
└── theme.json
```

To install from Hollycast, open **Settings > Library > Library Themes**, click **Install Library Theme Folder**, then select the theme folder. The app validates and copies it into the themes directory shown below the controls. Install does not overwrite a package with the same ID. Packages may contain at most 256 entries, 64 MiB of files, and eight nested directory levels. The installer rejects symbolic links. To replace or remove an installed package, edit or delete its folder, then use **Reload Library Themes**.

You can also copy a package folder directly into the themes directory shown in settings. Choose it under **Library Theme**, then choose one of its layouts under **Library Layout**. **Reload Library Themes** rescans the directory after manual changes. Selecting **App UI Theme** restores the selected UI theme and the default library layout.

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

The [theme collection](../themes/README.md) contains six starred examples using grids, lists, selected-game artwork and metadata. Each package includes its source notes.

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
| `layouts` | Yes | Array of 1–16 layouts. |

Base palette numbers are `0` Dark, `1` Light, `2` Dreamcast, `3` High Contrast, `4` Nintendo, and `5` Aqua Chill. A custom theme starts from this palette, then applies each entry in `colors`.

Supported `colors` names are: `Text`, `TextDisabled`, `WindowBg`, `ChildBg`, `PopupBg`, `Border`, `BorderShadow`, `FrameBg`, `FrameBgHovered`, `FrameBgActive`, `TitleBg`, `TitleBgActive`, `TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`, `ScrollbarGrab`, `ScrollbarGrabHovered`, `ScrollbarGrabActive`, `CheckMark`, `SliderGrab`, `SliderGrabActive`, `Button`, `ButtonHovered`, `ButtonActive`, `Header`, `HeaderHovered`, `HeaderActive`, `Separator`, `SeparatorHovered`, `SeparatorActive`, `ResizeGrip`, `ResizeGripHovered`, `ResizeGripActive`, `InputTextCursor`, `TabHovered`, `Tab`, `TabSelected`, `TabSelectedOverline`, `TabDimmed`, `TabDimmedSelected`, `TabDimmedSelectedOverline`, `PlotLines`, `PlotLinesHovered`, `PlotHistogram`, `PlotHistogramHovered`, `TableHeaderBg`, `TableBorderStrong`, `TableBorderLight`, `TableRowBg`, `TableRowBgAlt`, `TextLink`, `TextSelectedBg`, `TreeLines`, `DragDropTarget`, `DragDropTargetBg`, `UnsavedMarker`, `NavCursor`, `NavWindowingHighlight`, `NavWindowingDimBg`, and `ModalWindowDimBg`.

## Layouts

Each layout is an object. Unknown fields are errors. Required fields are `id`, `name`, `mode`, and `browser`.

| Field | Meaning and bounds |
| --- | --- |
| `id` | Unique within the package; 1–64 lowercase ASCII letters, digits, hyphens, or underscores. |
| `name` | Display label used by **Library Layout**. |
| `mode` | `grid` or `list`. |
| `browser` | `[x, y, width, height]` rectangle for the interactive game browser. Coordinates are fractions of the library content area. Position and size must fit wholly inside 0–1; width and height must each be at least `0.1`. |
| `columns` | Grid column count, integer 1–12. Default `4`; list mode ignores it. |
| `itemAspect` | Grid cell width divided by height, from `0.3` to `3`. Default `0.8`; list mode ignores it. |
| `spacing` | Logical UI pixels between browser items, from `0` to `48`. Default `12`. |
| `fontSize` | Optional browser text height as a fraction of canvas height, from `0.01` to `0.08`; clamped to 10–96 screen pixels. Omit to inherit the application font size. |
| `rowHeight` | Optional list row height as a fraction of canvas height, from `0.02` to `0.2`. Rows stay large enough to fit their text and padding. Omit for automatic sizing. |
| `textColor` | Optional browser title color, as 6- or 8-digit hex. Defaults to the app text color. |
| `selectedTextColor` | Optional selected/hovered browser title color. Defaults to `textColor`. |
| `selectorColor` | Optional browser selection color. Defaults to the app selection color. |
| `showTitles` | Whether grid cells show their title, default `true`. List rows always show the game title. |
| `background` | Library canvas color, 6- or 8-digit hex; default `151820ff`. |
| `elements` | Optional array of up to 64 decorative or selected-game elements. |
| `artworkSource` | Browser artwork: empty (default library media), `cover`, `miximage`, `case`, `screenshot`, `fanart`, `title`, `physical`, or `titlescreen`. Missing media falls back to the library artwork. |
| `rounding` | Browser selection/image corner radius in logical UI pixels, 0–48; default `0`. |
| `inactiveOpacity` | Unselected grid artwork opacity, 0.1–1; default `1`. |
| `selectorBorderColor` | Optional selection outline color, separate from the selection fill. |
| `separatorColor` | Optional list-row divider color. |
| `showArtwork` | Show square artwork thumbnails beside list titles; default `false`. |
| `compactControls` | Show favourite buttons only on hovered/focused items or saved favourites; default `false`. Context-menu actions remain available. |
| `showInfoTooltip` | Show the app's hover information popup; default `true`. Layouts with their own details can disable it. |

Grid cells show `artworkSource` and optionally a title. With `showTitles: false`, artwork uses the entire cell; an item with no artwork still displays its title. List titles stay on one line and use an ellipsis when necessary, with room reserved for favourite controls. The browser is the interactive region. Elements are decorative or display data for the currently selected item; they do not create interactive controls.

## Elements

Every element is an object with required `type` and `rect`. Element rectangles use the same normalized coordinate system as `browser`, must fit inside 0–1, and must be at least `0.01` wide and high. Panels allow dimensions down to `0.001` for thin rules. Unknown fields are errors.

| Field | Meaning and bounds |
| --- | --- |
| `type` | `panel`, `image`, `artwork`, `text`, or `metadata`. |
| `rect` | `[x, y, width, height]` normalized to the full library content area. |
| `source` | Required for `image`; a library artwork name for `artwork`; a metadata field for `metadata`. |
| `text` | Literal string for `text`. |
| `color` | 6- or 8-digit hexadecimal string; default `ffffffff`. Panel/text color or image/artwork tint, including alpha. |
| `fit` | `contain` (default), `cover`, or `stretch`. Images and artwork either fit inside the rectangle, crop centrally to fill it, or stretch to its dimensions. |
| `align` | Text alignment: `left` (default), `center`, or `right`. Applies to the wrapped text block. |
| `fontSize` | Text height as a fraction of the library canvas height, from `0.01` to `0.15`; default `0.035`. At render time it is clamped to 10–96 UI pixels. Uses the app font; custom fonts are not supported. |
| `zIndex` | Integer from `-100` to `100`; default `0`. Negative elements draw behind the browser. Zero or positive elements draw above it and can obscure its visuals. Elements are not interactive. |
| `endColor` | Optional second panel color, including alpha, for a smooth gradient from `color`. |
| `gradient` | Panel gradient direction: `vertical` (top to bottom, default) or `horizontal` (left to right). |
| `rounding` | Panel/image/artwork corner radius in logical UI pixels, 0–48; default `0`. |
| `bold` | Use the app's bold font for text or metadata; default `false`. |
| `wrap` | Wrap text into the rectangle, default `true`. Set `false` for one line with an ellipsis. Wrapped text clips at complete lines. |
| `fallback` | Missing artwork: `default` uses the game's library art; `none` leaves the element empty. A named artwork source tries that source before default art. Defaults to `default`. |
| `requiresArtwork` | Optional artwork source that must be present before drawing this artwork element. For example, require `screenshot` on cover/disc/logo overlays so they disappear when a hero uses a precomposed miximage instead. Empty (default) has no condition. |

Element types:

- **`panel`** fills its rectangle with `color`, optionally grading into `endColor`, with rounded corners when requested.
- **`image`** draws a package image using `fit` and `color`. Its `source` is a relative file path inside the package, such as `images/panel.png`. Use PNG or JPEG; SVG and WebP must be rasterized or decoded first. Absolute paths, `..`, and paths escaping through symlinks are rejected. The referenced file must exist.
- **`artwork`** draws the selected game's artwork using `fit`. `source` can be empty (default artwork), `cover`, `miximage`, `case` (3D box), `screenshot`, `fanart`, `title` (marquee/wheel), `physical` (disc/cartridge), or `titlescreen`. Named sources use local library media and obey `fallback`. For decorative marquees and discs, use `fallback: "none"` to avoid duplicated covers. `miximage` selects ES-DE mixed artwork independently of the library cover-media setting. Background and detail artwork follow mouse hover and keyboard/controller focus. If no applicable media is available, the rectangle is empty.
- **`text`** draws the literal `text` value.
- **`metadata`** draws the selected game's value for `source`; a missing value draws nothing. Supported fields: `title`, `description`, `developer`, `publisher`, `genre`, `players`, `releaseDate`, `productId`, `region`, `format`, `playTime`, `lastPlayed`, and `size`.

The current selected item is shown in detail areas outside the browser rectangle. Put background panels there at a negative `zIndex`, and keep text and artwork in that area at `zIndex: 0` or greater. Elements above the browser are not clipped to the browser and can obscure its items.

Themes do not run scripts, load remote resources, or supply fonts. Keep a package self-contained and use only relative image paths for static assets.

## Troubleshooting

- Check that the folder name and manifest `id` match exactly, including lowercase spelling.
- Keep the manifest under 256 KiB and use only documented fields and values; the parser reports the invalid field in Settings.
- Ensure each layout ID is unique and every normalized rectangle stays within the canvas.
- Verify referenced image files exist inside the theme folder. Avoid symlinks that point outside it.
- After editing or copying a package manually, press **Reload Library Themes**. A failed package appears with its validation error and is skipped.
