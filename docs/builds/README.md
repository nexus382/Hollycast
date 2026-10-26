<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast build downloads

Published at https://nexus382.github.io/Hollycast/ by `.github/workflows/builds-pages.yml`.
The repository's Pages publishing source must be **GitHub Actions**.

The catalog includes the latest 100 releases and eligible builds from the latest
300 Actions artifacts. Development tags are recognized even when a historical
release was not marked as a prerelease. Only successful pushes to this
repository's `master` and `dev` branches appear as CI builds. Expired artifacts,
pull requests and fork builds are excluded. CI ZIP downloads require GitHub
sign-in; release assets are public downloads.

Updates run after releases and platform build completions, hourly (including
artifact expiry and later release asset uploads), and on manual dispatch. Open
pages reload the static catalog every five minutes, without using visitors'
GitHub API quotas. GitHub scheduling and deployments can add a short delay.

Each release shows its release notes as plain text and links to the rendered
GitHub notes. CI changelogs compare the previous available build from the same
workflow and branch, with up to 100 commit messages and a full comparison link.
Without an earlier build, the current commit message is shown explicitly.

Run `node --test tools/builds/catalog.test.mjs` and then
`node tools/builds/catalog.mjs` from the repository root to generate the catalog.
`GH_TOKEN` may be supplied for API access. Serve `docs/builds` with any static
HTTP server for local preview. Generated `builds.json` is not committed.
