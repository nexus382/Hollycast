<!-- Portions Copyright 2026 The Hollycast Authors -->

# Windows Settings Test Automation

In an opt-in harness build, Windows test plans can navigate all nine settings pages, inspect and operate controls, and use native file pickers. The automation follows existing widgets and callbacks. Password values stay unreadable, and ordinary builds do not include the harness.

## Steps

1. Use a Windows standalone build with `HOLLYCAST_TEST_HARNESS=ON` and Node.js 22 or later. Start an isolated `hollytest` session and run `doctor` against its run directory.
2. Open a page with `node tools/testing/hollytest.mjs settings --run <run directory> --page library`. Check that the returned `page` and the observed settings page agree.
3. Inspect controls with `observe`, operate a visible test control with `click` or `set-toggle`, and confirm the new value with `expect-ui`. Use the isolated session's test files for any native picker action.

[Source pull request #78](https://github.com/nexus382/Hollycast/pull/78)
