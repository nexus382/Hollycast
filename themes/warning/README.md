# Warning!

A vertical arcade shooter with your library as the enemy formation. Cyan-framed
covers are recorded as played; red-framed covers are unplayed targets. The ship
tracks your selection, a short beam marks each lock-on, and an unplayed game gets
a brief striped **WARNING!!** banner. **Fire!** starts the selected game through
the normal Hollycast launch path, after a skippable 0.43-second sequence.

Install this folder with **Settings > General > UI Settings > Install Library
Theme Folder**, then select **Warning!** and its **Warning! / Arcade** layout.
The separately authored **Vertical** layout takes over in narrow windows.
It uses a compact HUD and four larger targets per wave, with a selected-game
panel and persistent Fire and Favourite controls below the formation.
This package requires the optional paged-grid, numeric-stat, and selection-effect
fields described in [the theme guide](../../docs/Themes.md).

- Each wave contains up to sixteen entries. Arrow keys / D-pad move the target;
  the wheel, Page Up / Page Down, and Previous / Next Wave move through pages.
  A single click selects; double-click, Enter / controller confirm, or Fire! launches.
- Score is recorded play hours in the current filtered view multiplied by 1,000.
  Hi-score uses the most-played game's hours, with the same multiplier. They are
  calculated from raw duration, not placeholder scores or parsed display text.
- Lives counts favourites. Bosses Left counts games with neither a recorded
  launch nor recorded play time. BIOS and device entries do not count as games,
  favourites, or bosses. An old library with no recorded history appears unplayed.
- Threat rises with days since the last recorded launch: `log2(days + 1) / 9`,
  clamped to 5–100%. Unplayed games show 100%. A duration without a launch date
  leaves the meter empty rather than inventing a date.
- Favourite persists through the shared library action and marks the grid item.
  All Games, Continue Playing, Favourites, Filter, and Settings stay in the toolbar;
  right-click / long-press retains the normal game menu.
- Covers and details use local library data and the existing artwork providers.
  The theme makes no network requests and contains no game artwork or ROMs.
- Selection and launch sounds follow the app's audio volume. The warning fades
  once; it does not strobe or repeat while resting on the same game.

All engine extensions are generic and opt-in. There are no theme-ID checks.

Rebuild the original images, synthesized PCM sounds and manifest with
`python3 source/generate_assets.py` (Pillow required). The generator uses the
existing bundled Hollycast logo without changing its proportions. The package
uses GPL-2.0-or-later, apart from its bundled fonts: [Audiowide](https://github.com/google/fonts/tree/main/ofl/audiowide)
and [Press Start 2P](https://github.com/google/fonts/tree/main/ofl/pressstart2p)
use the SIL Open Font License, included in `assets/` with Audiowide's FONTLOG.
