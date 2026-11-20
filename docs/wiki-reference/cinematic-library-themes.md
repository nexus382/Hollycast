<!-- Portions Copyright 2026 The Hollycast Authors -->

# Cinematic library themes and DCnet

Theme authors can build full-window libraries with their own navigation, game actions, save-state previews, and explicit Resume and Start Fresh buttons. A theme can also style a DCnet page around the native dashboard while keeping its search and network settings available.

## Preview a cinematic theme

1. Use a standalone build with cinematic theme support. Save a state in a game to give the layout a preview to display.
2. Open **Settings → Library → Library Themes**, then choose a theme and its **Library Layout**. For a theme you are building, place its package in `library-themes/<id>/` in the app data folder, match the folder name to the ID in `theme.json`, and restart Hollycast. See the [theme format guide](../Themes.md).
3. Open the saved game's entry. Layouts with a save-state preview show the saved point. Choose **Resume** to continue or **Start Fresh** to boot the game.
4. If the theme includes a **DCnet** page, open it to search games and access network settings within the theme.

[Source pull request #69](https://github.com/nexus382/Hollycast/pull/69)
