# Hollycast themes

Community theme catalogue for Hollycast, maintained by TnT Studio's.
In Hollycast, open **Settings → Library → Browse themes** to see screenshots,
read about each creator, and download and install a theme. Installed themes are
marked automatically. Choose **Use theme** to apply one.

Catalogue: `https://raw.githubusercontent.com/TnT-Studio-s/Hollycast-themes/main/catalog.json`

## Contribute a theme

1. Create `themes/<theme-id>/` containing a valid Hollycast `theme.json` and its
   assets. Keep all asset paths relative to that folder. Include licences and
   attribution for everything you distribute.
2. Put one to eight real screenshots under `screenshots/<theme-id>/`. PNG and
   JPEG are recommended; each image must be at most 8 MiB, 8192 pixels on either
   side and 16 million pixels total. Avoid personal information in screenshots.
3. Add `themes/<theme-id>/listing.json`:

   ```json
   {
     "version": "1.0.0",
     "screenshots": ["screenshots/my-theme/library.png"]
   }
   ```

   Name, creator and description come from `name`, `author` and `description` in
   `theme.json`; all three are required for catalogue listings. The folder name
   must match the manifest's `id`.
4. Run `python3 build_catalog.py` and commit the source theme, screenshots,
   generated `packages/` ZIP and `catalog.json`. Open a pull request.

Packages contain `theme.json` at the ZIP root. Each package is limited to 256
files, eight nested folders and 64 MiB unpacked. Symlinks are not supported.
The publishing script checks paths and package limits. Also install the theme
locally in Hollycast to validate its layouts, fonts and asset references.

The catalogue starts empty until a theme is contributed. The application does
not need Git, GitHub authentication or a separate installer to use this repo.
Theme authors retain their copyrights; each theme must include its own licence.
The catalogue builder is licensed under GPL-2.0-or-later; see `LICENSE`.

## Maintainers

Catalogue format version 1 has a `themes` array. Each entry contains `id`,
`name`, `creator`, `description`, `version`, `package`, and `screenshots`.
Package and screenshot paths are relative to `catalog.json`; HTTPS URLs are
also supported by the client. Publish the catalogue and packages together.
The validation workflow checks that generated files match the source themes.

Hollycast deliberately never overwrites an installed theme. Users keep their
existing package when a newer version appears in this catalogue.
