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

The publisher copies ZIP artifacts from successful platform push workflows into
automatic prereleases tagged `ci-<branch>-<run-id>`, targeting the exact source
commit. It never extracts or executes artifacts. Archives retain GitHub's artifact
ZIP layout, so users must extract them. Published assets are not replaced and do
not expire with the original Actions artifact. Manual releases are not modified
or displaced as GitHub's latest release.

Only this repository's `master` and `dev` platform pushes are eligible. Failed,
pull-request and fork builds are excluded. New releases remain drafts until all
uploads succeed; retries reuse completed assets and remove only empty, unfinished
assets belonging to that draft before uploading again.

Updates run after releases and platform build completions, hourly, and on manual
dispatch. Non-workflow-run events backfill the latest successful build of each
platform on each branch from the most recent 100 successful runs per branch.
Build completions publish that specific run. Expired source artifacts cannot be
backfilled; the next successful build will provide fresh downloads. Open
pages reload the static catalog every five minutes, without using visitors'
GitHub API quotas. GitHub scheduling and deployments can add a short delay.

The catalog scans up to 1,000 releases and displays the latest 20 commits per
branch plus 20 releases per release channel. Published downloads are retained on
GitHub beyond this recent list. Development tags are recognized even when historical
releases were not marked as prereleases. CI changelogs compare an earlier public
build on the same branch, with up to 100 commit messages and a comparison link.
Without linear earlier history, the current commit message is shown explicitly.

Run `node --test tools/builds/catalog.test.mjs tools/builds/publish.test.mjs` and then
`node tools/builds/catalog.mjs` from the repository root to generate the catalog.
`GH_TOKEN` may be supplied for API access. Serve `docs/builds` with any static
HTTP server for local preview. Generated `builds.json` is not committed.

Publishing runs in a separate job with `contents: write` and `actions: read`;
catalog generation and deployment do not receive release write access. Both
privileged and catalog jobs check out trusted `master` code. Pull requests run
only the checks, with read permissions. Workflow concurrency serializes publishing
and deployment, avoiding competing draft releases and stale catalog deployments.
