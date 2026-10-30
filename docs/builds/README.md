<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast build downloads

Published at https://nexus382.github.io/Hollycast/ by `.github/workflows/builds-pages.yml`.
The repository's Pages publishing source must be **GitHub Actions**.

Automatic `master` and `dev` builds are the default view. Platform downloads for
the same branch and commit share one compact, expandable row, titled with its
short commit ID. Master and Dev builds have independent columns, newest first;
the columns stack on narrow screens. Each row expands to show public downloads
and its changelog. Downloads sit in labelled device columns inside each build;
the page explains the Windows, Android and Xbox choices. Visitors do not need a
GitHub account.
Ordinary stable and development releases remain available through the filters.

The six download columns match Flycast's CI page: Android APK, standalone Windows
x64 MinGW ZIP, Linux AppImage, macOS app ZIP, standalone Switch NRO and Xbox UWP
x64. MSVC, 32-bit, ARM64 UWP, AAB and libretro alternatives are not substituted.
The page lists a build only when all six matching platform downloads are uploaded.
Automatic builds are grouped by branch and commit before checking completeness;
downloads from another branch or commit cannot fill a missing platform. Incomplete
automatic builds and tagged releases stay hidden until their downloads are ready.
Actions artifact packages may add an outer ZIP around the matching build. Additional
formats remain available on the linked GitHub release.

Reference: [Flycast's CI page implementation](https://github.com/flyinghead/flycast-builds/blob/master/builds.js)
and [its desktop build matrix](https://github.com/flyinghead/flycast/blob/master/.github/workflows/c-cpp.yml).

The publisher copies ZIP artifacts from successful platform push workflows into
automatic prereleases tagged `ci-<branch>-<run-id>`, targeting the exact source
commit. It never extracts or executes artifacts. Archives retain GitHub's artifact
ZIP layout, so users must extract them. Published assets are not replaced and do
not expire with the original Actions artifact. Manual releases are not modified
or displaced as GitHub's latest release.

The [Latest Development Build](https://github.com/nexus382/Hollycast/releases/tag/latest-dev)
is a rolling prerelease sourced exclusively from `dev`. It uses the same six-platform,
same-commit completeness rule as the catalog. Newer incomplete commits, master builds,
drafts and unfinished assets cannot fill missing platforms or displace the current
complete dev build. Older completions cannot roll it back.

The publisher copies the six selected CI release archives into a separate draft using
consistent filenames, verifies their SHA256 digests, and adds `SHA256SUMS.txt`. Existing
downloads stay available during staging; retries reuse verified draft assets. Once
all seven assets are verified, the previous rolling release is retained under
`latest-dev-<commit>`, the `latest-dev` Git tag moves to the new source commit, and the
draft is promoted. A failed promotion attempts to restore the previous release and
tag; retry the workflow if GitHub is unavailable during recovery. Rolling releases
and their archives are excluded from the catalog to avoid duplicating CI history.
Notes include the source commit, build date and changelog. ZIP contents are copied
without extraction, including any existing outer artifact ZIP. Stable releases
keep GitHub's official Latest designation.

Only this repository's `master` and `dev` platform pushes are eligible. Failed,
pull-request and fork builds are excluded. New releases remain drafts until all
uploads succeed; retries reuse completed assets and remove only empty, unfinished
assets belonging to that draft before uploading again.

Updates run after releases and platform build completions, hourly, and on manual
dispatch. Publishing runs are serialized, and waiting build completions are queued
instead of replacing one another. Non-workflow-run events backfill the latest
successful build of each platform from the most recent 100 successful runs per branch.
Build completions publish that specific run. Expired source artifacts cannot be
backfilled; the next successful build will provide fresh downloads. Open
pages reload the static catalog every five minutes, without using visitors'
GitHub API quotas. GitHub scheduling and deployments can add a short delay.

The catalog scans up to 1,000 releases and displays the latest 20 complete commits
per branch plus 20 complete releases per release channel. Published downloads are retained on
GitHub beyond this recent list. Development tags are recognized even when historical
releases were not marked as prereleases. CI changelogs compare an earlier public
build on the same branch, with up to 100 commit messages and a comparison link.
Without linear earlier history, the current commit message is shown explicitly.

Run `node --test tools/builds/*.test.mjs` and then
`node tools/builds/catalog.mjs` from the repository root to generate the catalog.
`GH_TOKEN` may be supplied for API access. Serve `docs/builds` with any static
HTTP server for local preview. Generated `builds.json` is not committed.

Publishing runs in a separate job with `contents: write` and `actions: read`;
catalog generation and deployment do not receive release write access. Both
privileged and catalog jobs check out trusted `master` code. Pull requests run
only the checks, with read permissions. Workflow concurrency serializes publishing
and deployment, avoiding competing draft releases and stale catalog deployments.
