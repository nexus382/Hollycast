<!-- Portions Copyright 2026 The Hollycast Authors -->

# Split texture sheets

For a game that stores several textures in one image, open **Settings > Advanced > Texture Dumping > Split Texture Sheets**. Enable **Texture Dumping** while collecting the game's textures. Hollycast saves the original sheets, cropped pieces, and texture-atlas.json; copy those files into the game's usual texture replacement folder. Keep each piece from a sheet at the same scale.

The option is off by default.

## Steps

1. Enable **Texture Dumping** and **Split Texture Sheets** in **Settings > Advanced > Texture Dumping**, then play through the area you want to edit. Check the game's dump folder for the original sheets, cropped pieces, and `texture-atlas.json`.
2. Edit the pieces without renaming them. Keep the original sheets at their original dimensions and scale each edited piece from the same sheet by the same factor.
3. Copy the original sheets, edited pieces, and mapping file into `data/textures/<game id>`. Enable **Load Custom Textures** and **Split Texture Sheets**, then start the game and revisit the edited area to check that the replacements appear.

[Source pull request #55](https://github.com/nexus382/Hollycast/pull/55)
