<!-- Portions Copyright 2026 The Hollycast Authors -->
# Pocket Dimension

An original Hollycast theme that turns a VMU into the library browser. Existing
scraped covers become detailed green LCD images; the selected game's full-color artwork sits in
a recessed sage display beside the device, with scrollable game details including
the description, publisher, players, region, product ID, and play history when available.
The toolbar gives Hollycast's existing logo its own proportionate space. A portrait layout is selected automatically in
narrow windows.

Both VMU layouts use cover art by default, rendered at up to 128 pixels with
32 green tones and smooth filtering. This preserves recognizable artwork while
keeping the sage LCD palette. The conversion is cached in memory and leaves the
downloaded images untouched; no new scrape is needed. Missing covers still use
the local LCD placeholder and title initials.

Install this complete folder with **Settings > Library > Library Themes > Install
Library Theme Folder**, select **Pocket Dimension**, and select **Pocket library**.
This package requires a Hollycast build with the optional presentation extensions
documented in [the theme author guide](../../docs/Themes.md).

- Click or focus an LCD tile to preview a game. Double-click it, press Enter or the
  controller's confirm button, or use **Play** to launch it.
- The device's left and right buttons move the selection, including through larger
  libraries. The LCD browser also scrolls and supports keyboard/controller navigation.
- **A** displays the cover and **B** displays a screenshot. Missing named artwork
  disables its choice and explains its availability on hover; it does not show the
  cover as a screenshot. **Mix** shows the downloaded miximage; **Disc** shows physical-media art. The cover is selected initially.
- The outlined-star **Favourite** button marks the selected game. Its filled star,
  sage fill, and **Favourited** label confirm the saved state; press it again to remove.
  The state is saved and used by the normal Favourites library view.
- Favourites and Continue Playing use the normal library filters. Right-click or
  long-press a tile for its usual game actions. **Filter** expands the search field.

All behavior comes from generic, opt-in manifest fields. There are no package-ID
checks in the renderer. Existing themes keep the standard toolbar, full-color
artwork, normal grid sizing, and single-click activation unless their authors opt in.

The device skin was generated with imagegen from the approved [visual target](../../docs/design/pocket-dimension.md).
It is a transparent static skin; the LCD tiles and every control remain live widgets.
Regenerate the supporting paper, display frame, placeholders, and manifest with
`python3 source/generate_assets.py` (Pillow required). The generator retains the
device skin unchanged. Space Mono is
bundled locally under the SIL Open Font License; see [assets/OFL.txt](assets/OFL.txt).
The remaining package and generator use Hollycast's GPL-2.0-or-later license.
No game artwork, ROMs, remote resources, or scripts executed by the application are
included in the package.

Artwork uses the shared library cache for **TheGamesDB** and **ScreenScraper**, with configured local artwork taking priority.
Use **Settings > Library > Artwork Downloads > Download All Required Media** to fetch the selected provider's supported categories.
Both providers supply covers and screenshots. ScreenScraper also supplies miximages and disc scans; those choices stay disabled under TheGamesDB unless matching media is already cached or supplied locally.
Basic automatic downloads fetch covers only, and selecting the theme never starts a bulk scrape.
TheGamesDB uses Hollycast's existing API access; ScreenScraper requires its provisioned application credentials.
Downloaded game metadata fills the details panel while configured gamelist values retain priority.

LaunchBox is available in the shared Artwork Provider selector. After Download All Required Media, its native disc scans feed the Disc control; front/back covers, 3D cases and spines use the same reusable fields as ScreenScraper. TheGamesDB automatically fills its supported missing categories for either primary provider.

Favourited games show a passive LCD star beside their VMU-grid initials in both layouts. This uses the shared opt-in `showFavouriteMarker` layout option and updates with the existing Favourite button.
