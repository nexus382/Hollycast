# Hollycast implementation plans

Sixteen plans cover audit findings **2–17**. The numbers match the audit. Each plan is a standalone handoff written against `dev` at `ebf8fbb79` on 26 November 2026.

## Execution order and status

Start with the P1 plans. The order below reflects the audit priority. The dependency column and coordination notes identify shared work.

| Finding | Plan | Priority | Effort | Risk of change | Requires another plan | Status |
| --- | --- | --- | --- | --- | --- | --- |
| 2 | [Preserve the previous save slot](002-preserve-save-slots.md) | P1 | M | Medium | — | DONE |
| 3 | [Correct ALSA microphone frame accounting](003-bound-alsa-capture.md) | P1 | S | Low | — | DONE |
| 4 | [Back up legacy controller mappings](004-back-up-legacy-mappings.md) | P1 | S | Low | 2 | DONE |
| 5 | [Verify VMU card migrations](005-preserve-vmu-migrations.md) | P1 | M | Medium | — | DONE |
| 6 | [Stop after a partial state restore](006-stop-after-partial-state-load.md) | P1 | M | Medium | — | DONE |
| 7 | [Resolve quick-load against current folders](007-resolve-current-save-folders.md) | P1 | S | Low | — | DONE |
| 8 | [Validate Naomi ROM and BIOS reads](008-validate-naomi-rom-reads.md) | P1 | M | Medium | — | DONE |
| 9 | [Bound HTTP asset bodies during transfer](009-limit-http-response-bodies.md) | P1 | L | Medium | — | DONE |
| 10 | [Map mixed-width CUE tracks](010-map-mixed-width-cue-tracks.md) | P2 | M | Medium | — | DONE |
| 11 | [Propagate Crowdin workflow failures](011-propagate-crowdin-failures.md) | P2 | S | Low | — | DONE |
| 12 | [Verify known motion in MetalFX](012-verify-metalfx-motion.md) | P2 | M | Low | — | DONE |
| 13 | [Defer the artwork recovery image index](013-defer-artwork-recovery-index.md) | P2 | S | Low | — | DONE |
| 14 | [Reduce LaunchBox match-cache writes](014-reduce-launchbox-cache-rewrites.md) | P2 | M | Medium | — | DONE |
| 15 | [Recognize linked Apple worktrees](015-initialize-linked-apple-worktrees.md) | P2 | S | Low | — | DONE |
| 16 | [Pin the Switch SDK](016-pin-switch-sdk.md) | P3 | S | Low | — | DONE |
| 17 | [Correct Hikaru defaults in the README](017-correct-hikaru-readme.md) | P3 | S | Low | — | DONE |

**Effort** is hours for S, roughly a day for M, and multiple days for L. The HTTP plan is estimated at L after tracing the curl, WinINet, Apple, Android and UWP implementations. Save-slot provider support may also need additional time where an interrupted document replacement requires recovery.

**Status** starts at TODO. IN PROGRESS covers implementation and review. PLATFORM BUILD PENDING identifies a reviewed change awaiting an SDK build. DONE records the reviewed commit and the checks that passed. BLOCKED identifies a missing prerequisite, and REJECTED records why the change was retired.

## Independent review

The audit started at `ebf8fbb79`. The branch `reviewed-deep-audit-fixes` includes the subsequent Star Wars Racer fix from `dev` at `846b90b45`. Both the normal and profiled Hikaru engine-game tests passed on macOS after integration.

The full Google Test executable passed 1,088 tests on macOS 27.2.1 arm64 and 1,088 on Ubuntu 24.04 arm64 on the source tree at `767d8f78b`. The runs skipped 32 and 21 tests, respectively, with three disabled on each platform. All 248 tests in the affected suites passed on both hosts.

Both hosts passed all 47 enabled CTest cases, including the 23 Hollycast cases, and all seven offline Crowdin tests. CTest lists one additional disabled dependency test.

| Finding | Accepted commit and local branch | Verification |
| --- | --- | --- |
| 2 | `e3f9a7d6` on `review/f002-corrected` | On macOS and Ubuntu, a real emulator save survived a forced file-size limit with identical bytes and modification time, then reloaded and accepted the next save. Native checks covered access controls and a Linux setgid parent. Six libretro regressions passed twice on Windows x64 under Wine. Stock frontend callbacks recovered interrupted overwrites and could create a slot in a frontend-owned directory. The Android SDK built both APKs, 27 JVM tests passed, and all four production storage cases passed on Android 15 arm64 with the external storage document provider. Five Windows API-model checks under Wine covered access rules, neighboring files, errors 1176 and 1177, and a real writer process interrupted during journal creation. The Windows Store SDK built and packaged the application for Win32, x64 and ARM64. [UWP SDK build](https://github.com/nexus382/Hollycast/actions/runs/45910521617) |
| 3 | `e53aa8091` on `bound-alsa-capture-frames` | Four frame-accounting tests passed on macOS and Ubuntu arm64. On Ubuntu 24.04, the registered ALSA backend read all 17 known mono samples from a file-backed capture device at 8,000 Hz and 11,025 Hz. A physical microphone was unavailable. |
| 4 | `8ac4a52f` on `review/f004-corrected` | Seven mapping and provider regressions, plus two independent LoadMapping fixtures, passed on macOS and Ubuntu arm64. They compared legacy bytes across CRLF, a missing final newline and multiple copy buffers, repaired an empty backup, and preserved an unreadable or newly appearing historical backup. Stock Windows libretro callbacks under Wine recovered the old .cfg.save through the first exists, metadata and open calls. The Android 15 document-provider instrumentation also recovered a controller backup. |
| 5 | `e2355410` on `review/f005-corrected` and `ff7ac345d` on `fix-phone-vmu-standalone-compile` | On a mounted exFAT volume, a Maple block survived a restart through the writable legacy card after hard-link publication failed. Conflicting destinations and failed reopens returned a device write error and preserved memory. The VMU, path and GGPO regressions and all eight standalone phone-VMU cases passed on macOS and Ubuntu arm64. |
| 6 | `57aad7a8` on `review/f006-corrected` | The numbered-slot UI resumed a running synthetic guest after an invalid header and emitted Pause then Resume. A partial restore blocked start, returned to the library, emitted Terminate and preserved the old autosave bytes. A fresh session wrote and reloaded its own autosave. The regression passed on macOS and Ubuntu arm64. Teardown now joins the emulation work before clearing the failure flags. |
| 7 | `5c02b77dd` on `resolve-current-save-folders` | All 20 library savestate tests and six cache tests passed on macOS and Ubuntu arm64. Reordering roots and deleting the first candidate both changed the next read of the same slot. The writable path and missing-slot fallback also passed. |
| 8 | `f10d12e13` on `revise-naomi-rom-read-validation` | Ten synthetic archive tests and the Dreamcast header compatibility test passed on macOS and Ubuntu arm64. The tests cover both interleave lanes at the cartridge boundary, short required reads, optional EEPROMs, BIOS word order, publication after success, cleanup and retry. |
| 9 | `55a75b6c` on `review/f009-corrected` | Five body-limit tests, two timeout tests and the asset consumer suites passed on macOS and Ubuntu arm64. All seven HTTP tests also passed in a MinGW x64 executable using WinINet under Wine. The Android Java client passed 12 loopback cases with its pinned Apache libraries on a host JVM. The UWP implementation retains the original header timeout and unlimited default body time, and releases streams through C++/CX scope. The arm64 Android app and instrumentation APKs built with SDK 36.1 and NDK 29.0.14206865. The Windows Store SDK built and packaged the application for Win32, x64 and ARM64. [UWP SDK build](https://github.com/nexus382/Hollycast/actions/runs/45910521617) |
| 10 | `20abca75` on `review/f010-corrected` | Nine CUE cases, four GDI cases and an independent public Disc::ReadSectors test passed on macOS and Ubuntu arm64. Both stored audio pregap sectors returned all 2,352 expected bytes. Synthetic gaps and TOC positions passed. A MinGW x64 executable under Wine measured a 3,221,225,492-byte sparse file and read the marker at byte 3,221,225,489. The previous implementation returned a negative size and failed the seek. |
| 11 | `704876809` on `propagate-crowdin-failures` | Seven offline tests passed, including overlapping upload and download. Six additional subprocess checks covered success, exit code 27 and a signal through both real workflows. Crowdin CLI 3.15.0 validated the generated configuration offline. The template and translation header matched the baseline after the tests. |
| 12 | `226e00e4` on `review/f012-corrected` | On Apple M5 with macOS 27.2.1 and Metal API Validation enabled, both normal runs measured a mean motion vector of (−4.00, +3.00) within 0.75 input pixels. The composed marker moved (+14.2, −11.5) against an expected (+16, −12), within four output pixels. The reversed-vector run failed the intended direction assertion at both sampled blocks. |
| 13 | `0a84caa8a` on `defer-artwork-recovery-index` | All 57 box-art tests passed on macOS and Ubuntu arm64. On each platform, five runs of the 240-image fixture went from two recovery walks and one image-index build to one walk and zero builds. The duplicate database test still recovered the preferred record and its cover. [Measurements](013-defer-artwork-recovery-index.md#independent-review) |
| 14 | `78de0dda` on `review/f014-corrected` | The 123 offline artwork and queue checks passed on both macOS and Ubuntu arm64. A cached rescan performed zero writes, and the next provider completed compaction after an interrupted snapshot. Both platforms read the public metadata export. macOS also downloaded and reopened a real cover. A Windows x64 test under Wine replayed a journal with a 9,008-byte torn tail in a Japanese directory. [Measured record and byte counts](014-reduce-launchbox-cache-rewrites.md#independent-review) |
| 15 | `d0ddf2406` on `init-linked-apple-worktrees` | macOS Bash 3.2 passed the linked-worktree and archive fixtures, including the pin and repeat-run checks. The generator preserved staged and unstaged edits through a symlink and returned exit 23 on an injected update failure. A local-only archive bootstrap reached its expected commit. Damaged branch references and failed fetches stopped before reset. |
| 16 | `0f609276c` on `pin-switch-sdk-image` | Actionlint and the cache-key change check passed. A network-disabled linux/amd64 container at the pinned devkitA64 digest built and installed both Release variants with GCC 15.2.0. The original toolchain check produced a 64 MiB NRO and a 227 MiB libretro archive. The combined source also built both targets, producing a 64 MiB NRO and a 229 MiB libretro archive. |
| 17 | `7a4f3ba0c` on `docs/hikaru-build-defaults` | Compared the four default platforms and renderer choices with CMake and the compatibility guide. Clarified the build requirement and restored the reference to known graphics and audio limitations. The source, overview, tracked-link and single-paragraph scope checks passed on the reviewed commit. |

## Dependencies and coordination

- **Save and restore.** [002](002-preserve-save-slots.md) owns publication of the disk slot. [006](006-stop-after-partial-state-load.md) owns whether emulation may run after applying it. They touch different paths in `core/nullDC.cpp`. Reuse temporary profiles and storage fixtures where possible. Keep a pre-apply rejection distinct from a failure after the machine has changed.
- **Other persistence fixes.** [004](004-back-up-legacy-mappings.md) and [005](005-preserve-vmu-migrations.md) can reuse a checked storage operation from [002](002-preserve-save-slots.md) if it has landed. The controller test must compare original bytes, and the VMU test must reopen the destination before retiring the source. [007](007-resolve-current-save-folders.md) changes path lookup and can land separately.
- **Artwork.** [009](009-limit-http-response-bodies.md) and [014](014-reduce-launchbox-cache-rewrites.md) touch provider plumbing. Reconcile their interface and fixture changes when integrating. [013](013-defer-artwork-recovery-index.md) measures a populated directory walk, while [014](014-reduce-launchbox-cache-rewrites.md) measures match records and bytes written. Collect the baseline for each workload before its optimization.
- **Platform checks.** [003](003-bound-alsa-capture.md) needs a Linux build that actually includes ALSA. [012](012-verify-metalfx-motion.md) needs a Mac that runs the temporal MetalFX branch. [016](016-pin-switch-sdk.md) requires builds of both Switch variants using the same immutable SDK digest. [017](017-correct-hikaru-readme.md) can land as a documentation-only change.
- **Test registration.** Several C++ changes add entries to `tests/CMakeLists.txt`. Preserve every registration when integrating parallel work. The Linux unit preset runs Google Test through the Hollycast executable. The focused commands verify that the named regression was registered.

## Audit context

Two isolated checks provide useful starting fixtures. The legacy controller copy produced a zero-byte backup from 45 input bytes. Replacing the only configured save root left a second lookup of the same slot pointing at its old file. Both translation wrappers also exited successfully when their child stages returned failure.

The audit baseline passed 69 harness tests, three Metal image-comparison tests and 12 standalone Hikaru CTest suites. Each plan names the additional checks needed to establish its own implementation result.

For implementation review, a clean macOS arm64 build of `ebf8fbb79` passed 153 Google Tests across the affected suites and all 22 registered Hikaru and updater CTest cases. The opt-in live LaunchBox test skipped. The two HTTP timeout cases used a loopback server.

An Ubuntu 24.04 arm64 build at the same revision compiled `audiobackend_alsa.cpp` with `USE_ALSA`. Both Maple GGPO tests and both loopback HTTP timeout tests passed.

The existing Metal presentation test passed both the spatial and temporal MetalFX paths on an Apple M5 running macOS 27.2.1 with Metal API Validation enabled. Finding 12 adds a separate assertion for known motion.

## Findings considered and rejected

- A header path is a supported Crowdin argument. The preparation and translation scripts normalize it to its containing directory. The planned defect concerns propagation of child failures. [Preparation](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/crowdin_prep.py#L7) · [Translation](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/intl/crowdin_translate.py#L7)
- The standalone Hikaru host-test target is defined in the test subproject. Its implementation is assembled from the host test sources listed there. [CMake target](https://github.com/nexus382/Hollycast/blob/ebf8fbb79022b7ad85509759a5ff5ab074d9dbc7/tests/hikaru/CMakeLists.txt#L82)
