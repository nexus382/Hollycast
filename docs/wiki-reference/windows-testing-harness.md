<!-- Portions Copyright 2026 The Hollycast Authors -->

# Native Testing Harness

The opt-in harness supports structured live observations, controller input, UI inspection and actions, screenshots, and presentation captures. Enable it for a test build with `-DHOLLYCAST_TEST_HARNESS=ON`. Ordinary builds keep it disabled. Its Node client runs bounded playtest plans and cleans up input after a session.

## Steps

1. Build a standalone Windows or Linux executable with `-DHOLLYCAST_TEST_HARNESS=ON` and use Node.js 22 or later.
2. Use `node tools/testing/hollytest.mjs launch` with the executable, source revision, and a fresh output directory. Supply game and firmware paths for a game plan.
3. Run `doctor` and `verify` with `--run <run directory>` to check the live connection and input identities. Run `playtest` against that directory with `--plan tools/testing/playtest.example.json` after matching the plan's game identity to your session. Check the plan result and any captures requested by its steps.

[Source pull request #76](https://github.com/nexus382/Hollycast/pull/76)
