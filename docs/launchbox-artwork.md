# LaunchBox artwork

In **Settings → Library**, select **LaunchBox** under **Online Artwork Source**
and enable **Fetch Box Art**. This works with **Original Box Art** and as the
fallback for **Custom Boxart**. **Physical Media** continues to use disc artwork.

The first lookup downloads LaunchBox's public `Metadata.zip` catalog (currently
about 103 MiB). Hollycast reads the XML incrementally and saves a smaller local
index containing only Dreamcast and arcade games, alternate names and covers.
When the index is over a week old, a lookup that needs the catalog attempts to
update it. A failed update keeps the existing index usable.

Matching uses the disc's internal title first, then its library name and ROM
filename, restricted to the Dreamcast or arcade platform. Punctuation, case and
trailing ROM-set annotations are normalized. Alternate names are included, and
ambiguous matches are skipped. Covers for the detected region are preferred,
then worldwide artwork, then other regions. Within each region preference,
original front covers take priority over reconstructed covers and arcade flyers.

Selected covers are downloaded at their original CDN resolution. Both the
LaunchBox database ID and the downloaded file are cached locally. TheGamesDB
remains the default provider; changing providers reopens the online lookup while
keeping existing artwork visible until a replacement is available.

## Renaming games

For Dreamcast discs with a valid product ID, the persistent LaunchBox match is
keyed by that ID and the disc's region. Renaming or moving the ROM does not alter
this identity. The next scan reads the same disc header and reuses the saved
match and cover, including after restarting Hollycast. With no valid product ID,
Hollycast uses the internal title and region when available.

Keep multi-file disc references valid when renaming a CUE/GDI and its tracks.
Unreadable discs cannot supply their internal identity. Arbitrarily renamed
arcade ZIP/7z files remain subject to the existing arcade scanner, which expects
recognized ROM-set filenames.

## Implementation and verification

For a local macOS preview, run `./script/build_and_run.sh` or use the Codex
**Run** action. This builds a native OpenGL development app in
`build-launchbox-app/Hollycast.app`. Add `--build-only` to build without opening
it, or `--verify` to check that it remains running after launch.

The provider uses the catalog's database IDs and image filenames. Website page
IDs are a separate identifier and are not used. Metadata and match caches live
beside the normal artwork database as `launchbox-catalog.json` and
`launchbox-matches.json`. Cached covers and matches can be reused offline.

The metadata format and image-download method are described in the
[LaunchBox developer discussion](https://forums.launchbox-app.com/topic/54163-is-there-a-public-way-to-get-images-from-the-launchbox-games-database/).

The normal `LaunchBox*` and `Boxart*` tests exercise region/platform selection,
alternate names, partial downloads, ambiguous matches, persistence, provider
changes and renamed discs. Optional checks:

- Set `HOLLYCAST_LAUNCHBOX_EXPORT` to a local `Metadata.zip` to run
  `LaunchBoxCatalogTest.LocalExportSmokeCheck` against the full export.
- Set `HOLLYCAST_LAUNCHBOX_LIVE=1` to run
  `LaunchBoxCacheTest.LiveProviderSmokeCheck`, which downloads the public catalog
  and one cover into a temporary directory, then verifies cache reuse.
