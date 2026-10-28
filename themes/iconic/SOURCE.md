# Iconic theme source and port notes

This Hollycast package is a static adaptation of **Iconic (ES-DE Version)** by Siddy212. The source repository was pinned to the commit returned by GitHub for `main`: `0a549ffbe5a392bd818ee4c11a7175b2372591e4`. Source files were read through raw URLs using this SHA; no repository clone is included.

- Repository: https://github.com/Siddy212/iconic-es-de
- Upstream layout references: `aspect-ratio-16-9-detailed.xml`, `theme-detailed.xml`, `theme-simple.xml`, `colors.xml`, and `capabilities.xml` at the pinned commit.
- Upstream preview references: the README's Retro Gray textlist preview and Dark Modern grid preview show the selected game's media and metadata on the left, over system character art, with the interactive list or grid on the right.
- Upstream license/credits files are retained unmodified as `LICENSE` and `README.md` in this package. The upstream `LICENSE` contains CC0 1.0 text, while the upstream README describes the theme as CC-BY-NC-SA 2.0 and provides artist and inspiration credits. Those notices differ; both are preserved here. This port follows the attribution and share-alike terms stated in the README and retains its credits. No legal conclusion is made about the conflict or the rights in individual artwork.

## Port mapping

- `Textlist` follows the detailed 16:9 `textlist` variant: selected game cover art and metadata are on the left, with the game list on a tall dark translucent panel on the right. The selected cover uses Hollycast's dynamic `cover` artwork element; title, genre, release date, developer, and description use selected-game metadata elements.
- `Grid: Boxart` translates the detailed `grid-boxart` variant: selected game cover and metadata stay on the left while the interactive cover grid fills the right panel.
- Both layouts use the full Dreamcast character background without cropping at the source's 16:9 aspect ratio. They add the original Dreamcast wordmark at the lower left. The wordmark uses `cover` fitting to crop the carousel texture's transparent vertical padding while retaining its full visible width. Alpha overlays keep the source character art visible while providing contrast behind game media and list/grid content.
- The colors follow Iconic's dark-modern scheme in `colors.xml`: near-black `090909` background, `cccccc` primary text, `bbbbbb` metadata, white selected text, gray `777777` list selector, and the source cyan `00C3E3` accent.
- Hollycast's static manifest cannot reproduce ES-DE's dynamic aspect/color scheme switching, custom fonts, badges, video/miximage selection, carousel, or per-system asset loading. It does support the selected cover artwork and metadata used here. The Dreamcast character background and Dreamcast wordmark are static assets for this package.

## Original assets included

The WebP originals below are copied byte-for-byte from the pinned upstream repository. Each is decoded to a PNG for Hollycast's current image loader; the source WebPs are retained beside the PNGs. The PNGs were produced with Pillow 12.3.0 using `Image.open(source).save(destination, format="PNG", optimize=False, compress_level=9)`; no artwork edits or resampling were applied. Source URLs use the pinned commit rather than the moving `main` branch.

| Package path | Upstream path / derivation | Source URL | SHA-256 |
| --- | --- | --- | --- |
| `assets/dreamcast-character.webp` | `_inc/systems/alternate-backgrounds/dreamcast.webp` (byte-for-byte original) | https://raw.githubusercontent.com/Siddy212/iconic-es-de/0a549ffbe5a392bd818ee4c11a7175b2372591e4/_inc/systems/alternate-backgrounds/dreamcast.webp | `5cc2b40b7e762109a884d0b847b77cb36a96b398b005f51e4b2170622f199c4a` |
| `assets/dreamcast-character.png` | PNG decoded from the WebP above | same as above | `9aa3e6f52625e3179b8effd3660041b29b232b31c93f3a7825820202e9ce1149` |
| `assets/dreamcast-logo.webp` | `_inc/systems/carousel-icons/dreamcast.webp` (byte-for-byte original) | https://raw.githubusercontent.com/Siddy212/iconic-es-de/0a549ffbe5a392bd818ee4c11a7175b2372591e4/_inc/systems/carousel-icons/dreamcast.webp | `e00ac12499ff4ff5ba1fd52d8ceee03a39249aaedb12fcab8d820214e621ebcc` |
| `assets/dreamcast-logo.png` | PNG decoded from the WebP above | same as above | `ecd73d03af9cd9ae7b84aeb2c072531337b1d0257b874f8f65edb91c97c63f29` |

All upstream credits remain in the included, unmodified `README.md`. The README identifies broad artwork contributions and notes that other publicly available character art is credited to its original creators; no more specific creator attribution for the selected Dreamcast files was identified in the source files inspected.
