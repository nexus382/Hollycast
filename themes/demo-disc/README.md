# Demo Disc

An original Hollycast theme styled after the disc taped to the front of a games
magazine: brushed silver, a spinning rainbow disc, and loud type. Everything it says
comes from your library.

Install this complete folder with **Settings > General > UI Settings > Install
Library Theme Folder**, then select **Demo Disc** and its **Demo Disc** layout.
This package requires a Hollycast build with the `rail` browser mode and the other
optional extensions documented in [the theme author guide](../../docs/Themes.md).

- The tilted rail keeps the selected game in the center. Click a row, scroll, drag,
  or use the arrow keys or D-pad to move along it.
- Double-click a row, press Enter or the controller's confirm button, or use
  **Play** to launch a game. **Favourite** marks the selected game with a star on its
  row and adds it to the Favourites view; **Favourited** removes it again.
- Each row is tagged with the system the game runs on, such as Dreamcast or Naomi,
  plus its genre, or its region or file format when no genre is known. The panel below the artwork lists only the details known for the
  selected game. A `gamelist.xml` adds developer, publisher, genre, and more.
- The ticker scrolls the selected game's description, then its publisher, developer,
  genre, players, release date, system, region, and product ID, skipping anything unknown.
- Moving to another game plays a short whoosh at your audio volume setting. Attract mode
  stays silent.
- Launching a game plays a confirm chime and a rising swell while the disc spins up to a
  blur, a bright band sweeps the cover, and the screen flashes to white. Any press during
  that second skips to the game.
- The spinning disc wears the selected game's own disc art when there is any: your
  physical media artwork first, otherwise the label stored on the Dreamcast disc itself.
  Games without either keep the plain rainbow disc.
- Leave the library alone for 20 seconds and attract mode lights up and steps through
  the disc. Any input hands control straight back.
- All Games, Continue Playing, Favourites, Filter, and Settings stay in the compact
  toolbar. Right-click or long-press a row for its usual game actions.
- A portrait layout is selected automatically in narrow windows.

All behavior comes from generic, opt-in manifest fields. There are no package-ID
checks in the renderer.

Regenerate the artwork and manifest with `python3 source/generate_assets.py` (Pillow
required). Orbitron is bundled locally under the SIL Open Font License; see
[assets/OFL.txt](assets/OFL.txt). The remaining package and generator use Hollycast's
GPL-2.0-or-later license. No game artwork, ROMs, remote resources, or scripts executed
by the application are included in the package.
