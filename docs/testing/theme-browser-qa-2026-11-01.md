# Theme browser verification — 1 November 2026

Changes under test: `theme-download-browser`, based on `01330d784`, including
the uncommitted browser, repository client and installer changes.

## Automated checks

Native macOS Debug app and test builds both linked successfully using Ninja and
the existing macOS build setup. The targeted test run passed **191 tests across
20 suites**, including nine new `ThemeRepositoryTest` cases:

```sh
Hollycast --gtest_filter='Theme*.*:CustomTheme*.*:UiSound*.*:SettingsPopupTest.*'
```

The new cases exercise catalogue metadata and relative asset resolution,
invalid IDs and duplicate entries, unsupported versions, missing screenshots,
insecure/escaping URLs, HTTP failures, installation, duplicate protection,
archive traversal, symlinks, invalid manifests and ZIPs, package size/file-count
limits, and validation of downloaded screenshots before saving.

The Python publisher passed an integration smoke check that generated a package,
verified its catalogue and ZIP contents, verified reproducibility, and rejected a
symlink. The empty production catalogue passed `build_catalog.py --check`.
The companion repository's main-branch GitHub validation workflow passed.

## Live macOS app

Used the freshly built app with a separate QA bundle identifier and temporary
configuration/data directory. LLDB redirected those directories before app
initialization. Another running Hollycast game instance was left running.

Verified through the real UI with keyboard navigation:

- Settings → Library exposes **Browse themes**.
- The production HTTPS catalogue loads and displays its empty state.
- A temporary repository branch containing two synthetic listings loads preview
  images, names, creator, version and description.
- An entry matching a bundled theme is recognised as installed; **Use theme**
  selects the existing package. It then shows **Current theme** and disables the
  redundant action.
- **Not installed** filters out that entry.
- **Download and install** fetches the other package from GitHub, installs it in
  the temporary profile, displays success, and replaces the download action with
  **Installed** and **Use theme**.
- **Use theme** on the downloaded package closes the browser and selects that
  package and its layout in Library settings.
- After the keyboard-navigation improvement, Tab crosses directly into card
  actions, without separate activation of nested child panels.
- Resizing from a 1200 × 750 window to 600 × 750 changes the gallery to one column;
  text wraps and Close remains visible. Results scroll independently.

The synthetic listings were published only on a temporary QA branch. That branch
was removed after verification; the production catalogue remains empty and ready
for real contributions. The QA app was closed afterward.

These checks cover the macOS desktop build. Windows, Linux, Android, iOS, Switch
and hardware-controller interaction were not exercised in this pass.

## Follow-up: fixed repository

The editable repository URL and its saved configuration option were subsequently
removed. The browser now always fetches the official main-branch catalogue.
The temporary catalogue selection described above was used only during the
initial QA pass.

## Follow-up: Christmas Morning contribution

Reviewed the Christmas theme chat's source, catalogue metadata, native wide and
portrait screenshots, asset licences and ZIP. Published Christmas Morning 1.0.0
through [theme repository PR #1](https://github.com/TnT-Studio-s/Hollycast-themes/pull/1),
merged as `add08095a1d6233f9bf25016bec4f9cdd03e71f2` after catalogue CI passed.
The listing identifies Hollycast contributors as creator and states its renderer
requirement. The ZIP has `theme.json` at its root, 20 files, 6,240,160 unpacked
bytes and 5,627,971 compressed bytes. Both screenshots meet the image limits.

The package needs optional list badges and selection behavior introduced by the
theme work. Integrated those shared parser and renderer changes into the browser
branch, without embedding the theme. This avoids a bundled copy replacing the
downloaded package. Added parser coverage and retained the theme author's badge
and resting-pointer keyboard-selection regression tests. Fresh macOS app and
test builds passed; **194 tests across 20 suites** passed using the filter above.

In a new isolated macOS QA profile, the official fixed main-branch catalogue
displayed Christmas Morning's creator, description, version and both screenshots.
Keyboard activation of Next image loaded the portrait preview. Download and
install fetched the production ZIP, reported success, marked the entry Installed
and exposed Use theme. All 20 installed files matched the published package
byte-for-byte; the current native parser validated both layouts (18 wide and 17
portrait live elements). Christmas Morning was not embedded in the QA build.

## PR integration against dev

Prepared `browse-downloadable-themes` on top of dev `451028376`. The shutdown
conflict was resolved by retaining both DCnet cleanup and theme-download cleanup.
Fresh native macOS Debug app and test builds passed after the rebase. With
network access available for the HTTP fixtures, **204 tests across 22 suites**
passed using:

```sh
Hollycast --gtest_filter='Theme*.*:CustomTheme*.*:UiSound*.*:SettingsPopupTest.*:Http*.*'
```

The publisher scaffold also passed `python3 tools/theme-repository/build_catalog.py
--check`, and the final patch passed `git diff --check`.
