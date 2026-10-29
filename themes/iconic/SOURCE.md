<!-- Portions Copyright 2026 The Hollycast Authors -->
# Iconic theme source and port notes

This Hollycast package is a static adaptation of **Iconic (ES-DE Version)** by Siddy212. The source repository was pinned to the commit returned by GitHub for `main`: `0a549ffbe5a392bd818ee4c11a7175b2372591e4`. Source files were read through raw URLs using this SHA; no repository clone is included.

- Repository: https://github.com/Siddy212/iconic-es-de
- Upstream layout references: `aspect-ratio-16-9-detailed.xml`, `theme-detailed.xml`, `theme-simple.xml`, `colors.xml`, and `capabilities.xml` at the pinned commit.
- Upstream preview references: the README's Retro Gray textlist preview and Dark Modern grid preview show the selected game's media and metadata on the left, over system character art, with the interactive list or grid on the right.
- Upstream license/credits files are retained unmodified as `LICENSE` and `README.md` in this package. The upstream `LICENSE` contains CC0 1.0 text, while the upstream README describes the theme as CC-BY-NC-SA 2.0 and provides artist and inspiration credits. Those notices differ; both are preserved here. This port follows the attribution and share-alike terms stated in the README and retains its credits. No legal conclusion is made about the conflict or the rights in individual artwork.

## Port mapping

- `Textlist` keeps the selected game's front cover and details on the left, with the interactive list on the right.
- `Grid: Boxart` keeps the same details beside a three-column grid. The grid uses the user's selected library artwork.
- Both layouts replace the fixed Dreamcast character background with the highlighted game's local ES-DE miximage. Mouse hover and keyboard/controller navigation update the backdrop and details together. Missing miximages fall back to current library artwork.
- The front cover remains a separate `cover` binding. A dark translucent panel separates the title, release date, player count, developer, and description from the backdrop. Platform branding and the lower-left Dreamcast wordmark are omitted.
- Colors retain Iconic's dark-modern palette and cyan accent. Hollycast uses its application font; ES-DE custom fonts, video playback, carousels, and dynamic aspect/color scheme switching are not reproduced.

## Media and attribution

Game media comes from the user's configured custom artwork directory. The former static Dreamcast character and wordmark assets are no longer bundled or referenced by this adaptation.

The upstream, unmodified `README.md` and `LICENSE` retain their notices and credits. These Hollycast layout changes do not replace those notices.
