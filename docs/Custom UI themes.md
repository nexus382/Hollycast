# Custom UI themes

Hollycast ships six immutable built-in presets. The **Create Theme** control duplicates the selected preset or custom theme into a new custom theme that you can edit. The **Edit Selected Theme** control edits custom themes; to customize a built-in preset, use **Create Theme**. The editor previews changes while the selected theme remains saved; **Save** writes a custom theme and selects it, while **Cancel** restores the selected theme. Editing a custom theme updates that file only after an explicit Save.

The editor exposes ImGui colors, semantic and widget role colors, common style controls, layout controls, font and image references, background fit/opacity, and export. Font, background and icon paths are relative to the custom theme folder. Copy files there before entering paths; Save reports missing or unsafe references instead of writing a theme that silently drops them. Import and export copy referenced files alongside the JSON and rewrite imported references to a theme-specific `assets/` folder. Export refuses to overwrite existing files. Import chooses a unique theme filename if the same name already exists. Import/export use the app's file browser for the import source and a destination folder path for export.

When a custom theme is selected, **Create Theme** starts a new copy of it; saving uses a new filename and leaves the original intact. The color tab puts the main semantic colors first. Check a color to override the default, or uncheck it to use the base behavior again. Advanced interface colors can be searched by name, and widget-specific colors are grouped separately.

The editor's file format remains the documented JSON format below, so themes can also be edited or shared manually. Import validates the JSON before writing and reports missing assets or unsupported fields. Top-level extension fields are retained when an existing theme is edited; unknown properties inside supported sections remain validation errors so misspelled controls do not silently appear to work. Export preserves supported theme fields and includes referenced files; it is a folder of JSON plus assets rather than a single archive. Relative paths are portable across platforms. Copying only the JSON without its referenced assets leaves those references unresolved, so share the complete exported folder.

Older Hollycast versions stored editable built-in overrides under `themes/builtin/`. Those files are preserved. At startup, Hollycast copies every changed legacy file and its referenced assets into a custom theme. If the active preset had edits, Hollycast selects its custom copy in memory and keeps a pending record until the normal settings save is observed on a later startup. If settings were not saved, the next startup reapplies the custom copy. An explicit theme selection finalizes the migration record before changing the selection. Other migrated themes remain available in the custom theme list. Presets load directly from bundled resources, and selecting a preset later does not apply its old override.

Put additional `.json` files directly in `themes/` to add custom themes. The folder is shown under **Settings → General → UI Settings → Custom Theme Folder**; hover or focus the row to see the full path in the details area. On macOS this is normally `~/Library/Application Support/Flycast/themes/` (or `~/.flycast/themes/` if that older folder exists). Select the file under **UI Theme**. The file name is the custom theme name. Copy [the Aurora example](examples/aurora-theme.json) into that folder to get started.

The theme list refreshes while settings are open. After editing a selected file, select it again to reload it. The selection is saved by filename, so adding or removing other themes does not change it. A missing or invalid selected file falls back to Dark and retains the filename, allowing it to work again when restored. An invalid custom file selected in settings displays an error and leaves the current theme selected. If a file becomes invalid between sessions, details are also written to the application log.

## Arcade Afterglow showcase

The [Arcade Afterglow example](examples/Arcade%20Afterglow.json) demonstrates semantic and widget colors, custom body and bold fonts, a library background and settings icon, and all four layout controls. To install it manually, copy the JSON into `themes/` and copy the accompanying `examples/assets/arcade-afterglow/` folder into `themes/assets/arcade-afterglow/`. The font files come from Hollycast's existing bundled fonts.

Select **Arcade Afterglow** in **UI Theme**, then choose **Edit Selected Theme** to see and adjust its values. The library shows the background and custom settings icon; the grid view shows the cover spacing and rounded artwork when games have artwork. Settings show the wider sidebar, density, and custom fonts. Media status colors appear in game details when media is found or missing; settings feedback colors appear when the relevant warning or error occurs. Try Save and Cancel with the live preview, and export the theme to an empty folder to include its assets. Import the exported JSON to test the round trip.

## Format

A theme is a JSON object with any of `colors`, `style`, `widgets`, `fonts`, `assets`, or `layout`. Names are case-sensitive. Only listed properties override the base. Omitted properties retain the base appearance. `widgets` provides optional widget-specific semantic role overrides.

Custom files in `themes/` accept an optional `base` that defaults to `Dark`. The supported names are `Dark`, `Light`, `Dreamcast`, `High Contrast`, `Nintendo`, and `Aqua Chill`. Custom themes inherit immutable bundled presets. Editing a custom theme that names a preset never changes the preset itself.

Built-in presets are loaded directly from bundled resources and have no editable files. To customize a preset, create a custom theme based on it.

Colors accept `#RRGGBB`, `#RRGGBBAA`, or four normalized floating-point RGBA components such as `[0.15, 0.35, 0.55, 1.0]`. Alpha is last. Each numeric component must be finite and between 0 and 1. The bundled files use floating-point values to preserve the original ImGui colors without hexadecimal rounding.

```json
{
  "base": "Dark",
  "colors": {"WindowBg": "#10152A", "Text": "#F1F6FF", "Button": "#285D76",
    "accent": "#48B4AD", "success": "#35C875", "warning": "#F0B64A", "error": "#E55B64"},
  "widgets": {"media.status": {"success": "#7DE8CA"}},
  "style": {"FrameRounding": 7, "ItemSpacing": [10, 10]},
  "layout": {"density": 1.0, "settingsSidebarWidth": 225,
    "libraryCoverSpacing": 8, "artworkCornerRadius": 0}
}
```

The semantic color names are `accent`, `success`, `warning`, and `error`. They use the same hex or RGBA syntax as ImGui colors. `accent` derives shared button, header, check mark, and slider colors, including hover and active states. An explicit ImGui color in the same theme layer takes precedence over the accent-derived value.

### Fonts and images

Font and image paths are relative to the directory containing the JSON file. Put assets beside custom themes under `themes/`. Paths are local only; absolute paths and `..` traversal are ignored for that asset. Missing or unsupported assets fall back to the bundled font or current icon. Font files must be valid TrueType/OpenType SFNT files and are limited to 32 MiB. Images are limited to 16 MiB and 4096 × 4096 pixels.

- `fonts.body` selects regular UI text, library labels, and settings headings and values. `fonts.bold` optionally selects the bold font. Fonts for CJK languages, emoji, Font Awesome, and other specialized UI elements continue to use Hollycast's existing fallback fonts.
- `assets.background` draws an image behind the library content. `backgroundFit` is `cover` (default), `contain`, or `stretch`; `backgroundOpacity` is from 0 to 1 (default 0.22). A dark translucent wash remains over the image for text contrast.
- `assets.icons.settings` replaces the Settings gear in the library toolbar. If the image cannot be loaded, the existing gear glyph is shown. Other named icon paths are accepted for future UI roles.

For example, save this in a custom theme JSON, then put your own files at the corresponding paths next to it:

```json
{
  "base": "Dark",
  "fonts": {"body": "fonts/Inter-Regular.ttf", "bold": "fonts/Inter-Bold.ttf"},
  "assets": {
    "background": "images/library.jpg",
    "backgroundFit": "cover",
    "backgroundOpacity": 0.28,
    "icons": {"settings": "images/settings.png"}
  }
}
```

Selecting the theme again reloads its files. The font atlas is rebuilt at the next frame boundary; image textures are refreshed when a theme is applied. Custom theme assets are rooted beside that custom JSON file.

### Layout controls

`layout` properties are optional and bounded. Their dimensions are logical pixels and follow Hollycast's UI scaling, including the user's scaling preference. Values omitted by a theme retain the existing layout.

- `density` controls vertical spacing and control padding in settings and library grids. It accepts 0.75–1.5; 1.0 keeps the usual density.
- `settingsSidebarWidth` sets the preferred settings navigation width from 136–310 logical pixels. The UI still clamps it to fit narrow windows and preserve readable navigation labels.
- `libraryCoverSpacing` sets horizontal spacing between cover cards from 0–48 logical pixels. If omitted, the existing spacing is retained.
- `artworkCornerRadius` rounds library artwork from 0–32 logical pixels; 0 keeps square covers. If omitted, the existing image shape is retained.

The theme controls affect the settings navigation/content layout and library grid. Explicit `style` values such as `FramePadding` and `ItemSpacing` remain applied; density scales vertical settings spacing and the library grid spacing around those authored values.

`widgets` maps stable widget keys to any of the four semantic roles. For example, `widgets.media.status.success` overrides the global `colors.success` only for media status indicators. Role lookup order is widget-specific role, global role, then the existing theme-derived fallback. The selected custom theme overlays its bundled preset. Semantic roles are reset before each theme application. Themes without semantic entries keep the prior appearance and fallback behavior. Themes are parsed and validated as a whole; malformed role data rejects the file, and themes do not recursively inherit from other files.

### Color names

Any of these Dear ImGui colors can be overridden:

- `Text`, `TextDisabled`, `WindowBg`, `ChildBg`, `PopupBg`, `Border`
- `BorderShadow`, `FrameBg`, `FrameBgHovered`, `FrameBgActive`, `TitleBg`, `TitleBgActive`
- `TitleBgCollapsed`, `MenuBarBg`, `ScrollbarBg`, `ScrollbarGrab`, `ScrollbarGrabHovered`, `ScrollbarGrabActive`
- `CheckMark`, `SliderGrab`, `SliderGrabActive`, `Button`, `ButtonHovered`, `ButtonActive`
- `Header`, `HeaderHovered`, `HeaderActive`, `Separator`, `SeparatorHovered`, `SeparatorActive`
- `ResizeGrip`, `ResizeGripHovered`, `ResizeGripActive`, `InputTextCursor`, `TabHovered`, `Tab`
- `TabSelected`, `TabSelectedOverline`, `TabDimmed`, `TabDimmedSelected`, `TabDimmedSelectedOverline`, `PlotLines`
- `PlotLinesHovered`, `PlotHistogram`, `PlotHistogramHovered`, `TableHeaderBg`, `TableBorderStrong`, `TableBorderLight`
- `TableRowBg`, `TableRowBgAlt`, `TextLink`, `TextSelectedBg`, `TreeLines`, `DragDropTarget`
- `DragDropTargetBg`, `UnsavedMarker`, `NavCursor`, `NavWindowingHighlight`, `NavWindowingDimBg`, `ModalWindowDimBg`

### Style properties

Numbers are logical sizes before UI scaling. Most size values accept 0–200; border sizes accept 0–20. Global `Alpha` accepts 0.2–1; `DisabledAlpha` accepts 0–1. Pair values use `[horizontal, vertical]`. Alignment pairs accept 0–1 per component.

- Opacity: `Alpha`, `DisabledAlpha`.
- Windows: `WindowPadding`, `WindowMinSize`, `WindowTitleAlign`, `WindowRounding`, `WindowBorderSize`.
- Children and popups: `ChildRounding`, `ChildBorderSize`, `PopupRounding`, `PopupBorderSize`.
- Controls: `FramePadding`, `FrameRounding`, `FrameBorderSize`, `GrabMinSize`, `GrabRounding`, `ButtonTextAlign`, `SelectableTextAlign`.
- Layout: `ItemSpacing`, `ItemInnerSpacing`, `CellPadding`, `TouchExtraPadding` (0–20 per component), `IndentSpacing`, `SeparatorTextBorderSize`, `SeparatorTextAlign`, `SeparatorTextPadding`.
- Scrollbars: `ScrollbarSize`, `ScrollbarRounding`, `ScrollbarPadding`.
- Tabs: `TabRounding`, `TabBorderSize`, `TabMinWidthBase`, `TabMinWidthShrink`, `TabBarBorderSize`, `TabBarOverlineSize`.

Hollycast resets the style before applying a theme, then applies platform touch padding, bundled values, and file overrides. UI scaling is applied once afterward. Bundled themes omit `TouchExtraPadding` so mobile defaults remain in effect.

Some custom widgets and platform-specific UI use their own layout or colors, so not every element follows every property. Global `style.Alpha` must be at least 0.2 so the interface remains visible. Low-contrast colors can still make controls hard to use; select a built-in preset in settings to recover.
