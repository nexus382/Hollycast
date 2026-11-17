<!-- Portions Copyright 2026 The Hollycast Authors -->
# Hollycast native testing harness

This is the shared entry point for Hollycast testing. Use the narrowest workflow
that answers the question:

- [C++ unit and integration tests](../../tests/) run with CTest; configure with
  `-DENABLE_CTEST=ON`. The [C/C++ workflow](../../.github/workflows/c-cpp.yml)
  runs the repository checks in CI.
- [Focused regression checks](../../tests/regression/README.md), [VMU tests](../../tests/vmu/README.md),
  [phone VMU tests](../../tests/phone_vmu/README.md), [theme HTTP tests](../../tests/theme_repository_http/README.md),
  and [updater tests](../../tests/updater/CMakeLists.txt) cover specific components.
- [Platform build instructions](../../README.md) describe the standalone and
  frontend targets. Device, libretro, and store-build acceptance requires those
  platform workflows, including the [Android](../../.github/workflows/android.yml),
  [Switch](../../.github/workflows/switch.yml), and [UWP](../../.github/workflows/uwp.yml)
  checks; it is not established by the native live harness below.
- The native live harness is for structured interactive observations and bounded
  input plans. It runs only in standalone Windows/Linux builds enabled with
  `HOLLYCAST_TEST_HARNESS=ON`; it does not control Android, libretro, Switch, UWP,
  or macOS applications.

The harness reports exact build/game identities, fresh live telemetry, effective
settings, controller state, version-bound memory watches, and native UI items.
Screenshots are an opt-in visual fallback. Watch mappings are supplied and
version-bound; a mapping does not prove the meaning of a field.

There is no deterministic replay integration in this checkout's harness. A live
benchmark is a single-run measurement with `comparison_verdict: NOT_EVALUATED`,
not an A/B comparison or proof of full-game correctness.

[Muse CLI setup](MUSE.md) describes optional bridge handoffs. Planet Harriers
scenarios and the included Hikaru mappings require `USE_HIKARU=ON`. Hikaru's
read-only CPU/RAM diagnostics also require `HOLLYCAST_TEST_HARNESS=ON`; they are
excluded from ordinary release builds.

## Current support boundaries

| Area | Supported or verified scope | Not established |
| --- | --- | --- |
| Host | Standalone Windows and Linux native builds | Android, libretro, Switch, UWP, macOS, and device acceptance |
| Systems | Generic SH-4 telemetry for Dreamcast, Naomi, Naomi 2, Atomiswave, and System SP; bounded dual-CPU/RAM diagnostics for Hikaru-enabled builds | A platform-wide or all-title gameplay pass |
| Title semantics | A game-specific claim requires a mapping verified against that title and build | Generic telemetry does not establish what a mapped value means |
| Windows settings UI | Standalone Windows exposes native controls for General, Library, Video, OSD, Audio, Controls, Network, Advanced, and About | Hardware/network prerequisites, read-only state, apply/restart behavior, and per-control acceptance still require separate verification |
| Rendering | Crazy Taxi live-control/capture smoke on Dreamcast OpenGL/DX11/Vulkan and Naomi OpenGL | Visual parity, comparative performance, or full-title playthrough |
| Guest controls | Native controller API drives emulated gamepad state | Guest keyboard, mouse, lightgun, operator, or JVS controls; native UI keys operate the host emulator UI, not a guest keyboard |
| Media identity | The selected primary game file and explicitly seeded originals are hashed | Companion tracks/images, parent ROMs, or which BIOS/archive member the loader selected unless separately verified |

Tested title smoke: Crazy Taxi on Dreamcast (OpenGL, DX11, Vulkan) and Naomi
(OpenGL), with live controller input and capture. Naomi 2, Atomiswave, and System
SP were architecture-only in this validation and were not live-tested.

Treat unsupported systems, titles, input devices, and rendering backends as
untested. A successful harness session proves only the observations and actions
recorded for that exact build, media, and configuration.

## Current-game single-run benchmark

The general benchmark uses the `current` scenario and external input by default:

```powershell
node tools/testing/hollytest.mjs benchmark --exe C:/path/to/Hollycast.exe --game C:/path/to/game-image --out D:/tmp/hollytest-run-001 --scenario current --control external --seconds 60 --revision EXACT_SOURCE_AND_DIRTY_PATCH_ID
```

This creates a disposable session, launches the selected game, waits for a live
native sample, warms up, and records host presentation intervals and telemetry.
It does not drive game-specific controls or assert that the game reached a
particular scene. Use a plan or explicit controller commands to establish and
check the requested gameplay state. Only the instrumented standalone build is
covered; use platform-specific builds and tests for frontend/device coverage.

## Optional Hikaru benchmark: Planet Harriers

This scenario is unavailable in current dev builds. Use it only after building a
branch that includes Hikaru support.

```powershell
node tools/testing/hollytest.mjs benchmark --exe D:/tmp/hollytest-build/Hollycast.exe --game C:/path/to/planet-harriers.zip --firmware-dir C:/path/to/hikaru-firmware --out D:/tmp/planet-benchmark-001 --scenario planet-harriers --seconds 60 --revision EXACT_SOURCE_AND_DIRTY_PATCH_ID
```

This creates an isolated run, boots Planet Harriers, inserts a coin, presses START and confirms
the character, waits for active stage gameplay, warms up for five seconds, and
records sixty seconds of movement and firing. Boot, selection, and introduction
are excluded from timing. The default preset uses OpenGL, 480-line internal
rendering, VSync, and the OSD collectors. `--profile SETTINGS_JSON` overrides
those settings, `--warmup SECONDS` changes warm-up, and `--state-profile JSON`
selects another verified mapping for this scenario. The shipped mapping must
match the game's archive and assembled ROM identities.

The output directory contains:

- **report.html**: readable results, timelines, conditions, and evidence links.
- **benchmark.json**: native presentation intervals, completed guest-frame
  boundaries, OSD summaries, gameplay state, hashes, validity, and cleanup.
- **benchmark-telemetry.jsonl**: original monitored samples with timestamps.
- **benchmark-status.json**: an atomic status snapshot for Codex or Muse.
- **events.jsonl**: the native controller/command audit.

The OSD updates approximately once per second; the benchmark polls at that rate,
keeps distinct fresh collector updates, and leaves unsupported metrics null.
Native guest-frame counts span the complete monotonic capture window. OSD
averages summarize the sampled collector updates; they are not per-frame GPU
timings. Raw presentation intervals supply mean/median/p95/p99/max and counts
over 33.33, 50, and 100 ms. Monitoring and native controller overhead are included.
The scripted input pattern repeats by elapsed time; this is an instrumented
play test, not a deterministic replay or an unobserved A/B benchmark.
The report also records sampled changes to output dimensions and emulator
settings. A complete timing capture can still have `conditions_stable: false`;
its warning identifies a run with mixed conditions that should not be used as
a fixed-condition performance comparison. The raw measurements remain available.

Read status without adding another emulator query:

```powershell
node tools/testing/hollytest.mjs status --run D:/tmp/planet-benchmark-001
```

For an agent-driven session, launch/configure a disposable session first, then:

```powershell
node tools/testing/hollytest.mjs benchmark --run D:/tmp/hollytest-session-001 --scenario current --control external --seconds 60
```

That mode records the running game while Codex/Muse uses `hold`, `controller`,
and `release` from another command. Clients wait for transport ownership without
stealing locks or replaying uncertain commands. `current` validates running
emulation, not a game's semantic phase; the agent must establish gameplay.
The optional Planet Harriers scenario checks its mapped active stage mode and
positive player energy throughout the sampled window. A pause, scene mismatch,
interruption, failed identity check, or incomplete capture produces an invalid
report and CLI exit code 2. Ctrl+C retains partial evidence and releases input.

Each session permits one benchmark attempt; use a new output directory for a
repeat. Fresh launches stop after reporting. Attached sessions pause by default;
`--after stop|pause|leave` changes that. `--capture true` requests one native
fallback image after timing ends. No screenshots or UI inspection run inside
the measured window. Single-run comparisons remain `NOT_EVALUATED`.

## Build and launch

Use Node.js 22+; there are no npm dependencies. Build standalone Windows/Linux
with `-DHOLLYCAST_TEST_HARNESS=ON`.
The bridge is off in ordinary builds and dormant in enabled builds until a
session is supplied. Unit-test, libretro, mobile, and legacy automation builds
cannot enable it.

Use the project's configured CMake toolchain and a fresh build directory to build
the standalone `Hollycast` target. Example launch:

```powershell
node tools/testing/hollytest.mjs launch --exe D:/tmp/hollytest-build/Hollycast.exe --out D:/tmp/hollytest-session-001 --game C:/path/to/game-image --profile C:/path/to/settings.json --revision EXACT_SOURCE_AND_DIRTY_PATCH_ID
```

Output must be a new directory. The executable and primary game archive/image are
hashed. Configuration and save roots are redirected before configuration loads.
Auto state saving/loading, networking, crash uploads, scripts, and boxart fetching
are disabled in the disposable profile. Original user configuration is not copied.
The launcher has an independent process lifetime after readiness.

`--firmware-dir` accepts only named seed files: fixed-size Dreamcast
`dc_boot.bin`, `dc_flash.bin`, `dc_nvmem.bin`, and VMU saves; optional Hikaru/MIE
profile seeds and archives; and Naomi BIOS archives named `airlbios`, `awbios`,
`f355bios`, `f355dlx`, `hod2bios`, `naomi`, `naomidev`, `naomigd`, `naomi2`, or
`segasp` with `.zip` or `.7z` extensions (nonempty, at most 16 MiB). Archive
contents are not inspected; unrelated names and credentials are excluded.
`verify` rehashes the original executable, selected primary media, and each
explicitly seeded source file. That verifies source-file bytes, not the BIOS
variant or archive member the emulator selected. Companion tracks/images and
parent ROM files are not bound unless selected or separately verified. Session
directories must support atomic rename and hard links for exclusive publication
of complete command/evidence files.

The optional Hikaru build locates its BIOS/MIE archives beside the game or in
configured BIOS locations, uses isolated NVRAM, and publishes assembled
BIOS/program/data CRC32 identities. Those identities must match before a Hikaru
watch profile is accepted. Planet Harriers NVRAM seeds and Hikaru archives are
only relevant on a Hikaru-enabled branch.

A settings profile contains only allowlisted numeric/boolean values:

```json
{
  "config": {
    "pvr.rend": 0,
    "rend.Resolution": 480,
    "rend.vsync": true
  },
  "rend": {
    "ResourceMonitorMode": 2,
    "OverlayShowOutputFPS": true
  }
}
```

OSD metric values use their existing collection and validity gates. Disabled,
unsupported, or not-yet-sampled metrics are null. Enabling an OSD key in a profile
does not establish that a particular build implements that collector.

## Live data first

```powershell
node tools/testing/hollytest.mjs live --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs watch --run D:/tmp/hollytest-session-001 --milliseconds 10000 --interval 100
```

`watch` emits JSONL and retains it in `live.jsonl`. Samples contain execution
sequence, sample age, stale/available flags, effective controls/settings, raw OSD
metrics with units in field names, and mapped game values/change events. Platform-
specific CPU telemetry appears only when the running build supports it. Default
polling is 10 Hz. A stalled source is reported as unavailable rather than replaced
with a guessed state. Polling observes sampled transitions; it is not an
instruction-level event trace.

SH-4 watches are read on the CPU's synchronous VBlank boundary. Hikaru
observations use their serialized frame owner and raw RAM buffers when available.
Neither path reads MMIO or writes guest memory. UI-only snapshots and graphics
readback are separate, explicitly intrusive operations.

```javascript
const { HollycastHarness } = await import('file:///PATH/TO/tools/testing/hollytest.mjs');
const h = await HollycastHarness.attach('D:/tmp/hollytest-session-001');
for await (const sample of h.watch({ durationMs: 10000, intervalMs: 100 })) {
  // Feed sample.state and sample.telemetry to the model or a test policy.
}
```

An agent whose JavaScript runtime restricts session-directory access can run the
same CLI through its authorized command tool and read JSONL output. The harness
does not require a desktop-control fallback for this loop.

## Controller input

```powershell
node tools/testing/hollytest.mjs press --run D:/tmp/hollytest-session-001 --button START
node tools/testing/hollytest.mjs press --run D:/tmp/hollytest-session-001 --button D
node tools/testing/hollytest.mjs menu --run D:/tmp/hollytest-session-001
```

Buttons and axes follow the selected game's existing input mapping; do not assume
cabinet bindings across games. The live native controller arrays expose the input
state consumed by the emulator.
For simultaneous controls:

```javascript
await h.hold({ port: 0, buttons: ['A'], axes: [16000, -8000, 0, 0],
  triggers: [0, 45000] }, 3000);
```

The CLI exposes the same simultaneous controls without a JavaScript wrapper:

```powershell
node tools/testing/hollytest.mjs hold --run D:/tmp/hollytest-session-001 --state '{"buttons":["A"],"axes":[16000,-8000,0,0],"triggers":[0,45000]}' --milliseconds 3000
node tools/testing/hollytest.mjs controller --run D:/tmp/hollytest-session-001 --state '{"buttons":[],"axes":[0,0,0,0]}'
node tools/testing/hollytest.mjs press --run D:/tmp/hollytest-session-001 --button A --port 0
```

Disposable sessions own controller and UI input exclusively; physical gamepad,
keyboard, and pointer events cannot overwrite native commands or open a menu.
Use native `ui` commands for interaction; the operating-system window Close
button still works. Game controls do not also navigate the menu bar during
gameplay. `press` accepts an optional `--port` (default 0). Ports are 0..3, stick axes
-32767..32767, and triggers 0..65535. Each request is a
complete snapshot. Leases are bounded to 1..2000 ms; `hold` renews them and releases
in `finally`. Expired input releases on the next emulator UI tick. A completely
stalled owner cannot service input until it resumes. This tests the emulator's mapping/consumption path;
physical USB/Bluetooth latency requires separate hardware measurements.

## Rewind control

Native builds that advertise the `rewind` ready capability accept one input
command (the complete native op is `rewind` with exactly `{held: boolean}`):

```powershell
node tools/testing/hollytest.mjs rewind --run D:/tmp/hollytest-session-001 --held true
node tools/testing/hollytest.mjs observe --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs live --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs rewind --run D:/tmp/hollytest-session-001 --held false
```

Enable Rewind in a disposable profile with the `config` keys
`Dreamcast.RewindEnable`, `Dreamcast.RewindInterval`,
`Dreamcast.RewindCompression` (0 Fast, 1 Balanced, 2 Maximum), and
`Dreamcast.RewindDuration`, then launch the game. Hollycast automatically
uses shared threading when Rewind is available. Holds stay active
until released (there is no lease expiry), and every `release`, `quit`, menu,
or restricted-session path also drops the harness hold. Holding fails with
the emulator's unavailable reason when Rewind cannot run; releasing always
succeeds, even if the session became restricted (hardcore, online, or
multiboard). Holding during a performance capture marks that capture invalid
with reason `rewind_restore`.

Each rewind reply and `state.rewind` in `observe`/`live` report
`enabled`, `available`, `unavailable_reason`, `can_step`, `requested`,
`held_by_harness`, `snapshots`, `memory_bytes`,
`interval_ms`, `duration_s`, and `compression`. While the game loader is
applying its settings, the four configuration fields are `null`. `unavailable_reason` is
user-facing text and may change; assert on `available`/`can_step` instead.
To prove time moved backwards, compare the sampled `sh4_cycles` string in
`live` telemetry before and after holding (it is sampled on the CPU's VBlank
boundary, never read live). A falling snapshot count alone, or the monotonic
completed-frame counter, does not prove a restore.

## Native UI inspection and Windows settings

The native UI inspector is the primary way to inspect settings and widget state.
It reports item IDs, exact and semantic labels, visible/enabled state, typed
values where the native widget exposes them, apply requirements, active windows,
scroll offsets, and focused text-input metadata. It exports the current typed
value of non-password InputText controls when available, but never the internal
ImGui editing buffer or a password value. It does not infer controls from screenshots or
accessibility trees, and it does not expand collapsed sections or expose clipped
offscreen widgets. A truncated inspection is marked explicitly.

```powershell
node tools/testing/hollytest.mjs settings --run D:/tmp/hollytest-session-001 --page video
node tools/testing/hollytest.mjs observe --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs click --run D:/tmp/hollytest-session-001 --semantic-id EXACT_SEMANTIC_ID
node tools/testing/hollytest.mjs set-toggle --run D:/tmp/hollytest-session-001 --target '{"semantic_id":"EXACT_SEMANTIC_ID"}' --value false
node tools/testing/hollytest.mjs expect-ui --run D:/tmp/hollytest-session-001 --target '{"semantic_id":"EXACT_SEMANTIC_ID"}' --expected '{"requires_apply":true}'
node tools/testing/hollytest.mjs fill --run D:/tmp/hollytest-session-001 --target '{"label":"EXACT_VISIBLE_INPUT_LABEL"}' --text-file D:/tmp/hollytest-value.txt --commit false
node tools/testing/hollytest.mjs scroll-window --run D:/tmp/hollytest-session-001 --window EXACT_OBSERVED_WINDOW_NAME --delta-y 1
```

`settings --page` accepts `general`, `library`, `video`, `osd`, `audio`,
`controls`, `network`, `advanced`, or `about`; it requests that page and checks
fresh page readback when the native snapshot exposes it. Select one visible,
enabled item by a unique exact label, semantic ID, or current item ID. A label
can match its friendly semantic label or displayed label; if more than one item
matches, add a `--window` selector or use an item ID from a fresh inspection.
`click --input` accepts native click modifiers (`Ctrl`, `Shift`, `Alt`), mouse
button, and click count. `fill --modifiers '["Ctrl"]'` supplies a modifier for an
unannotated numeric input; supported slider/range widgets add Ctrl automatically.
`fill` selects the exact field, verifies native focus,
replaces the field with Ctrl+A and text (or Backspace for an empty value), and
optionally presses Enter. It rejects read-only inputs. An exported non-password
typed value is checked before commit; when available, a fresh post-commit value
is checked too. If Enter closes the field, the result reports
`native_value_before_commit`; follow it with `expect_ui` for a separate
post-effect assertion. Raw ImGui inputs may report `text_buffer_not_exported`.
Password fields are writable but write-only. Use dummy values for form-entry
tests; perform real-account sign-in only when it is part of the requested
scenario. Text-action arguments are redacted in saved events and copied plans.
Ordinary non-password values remain readable in structured UI state and may
appear in its evidence. Password contents are never exported.

`set-toggle` reads the current value and clicks only when it differs from the
requested boolean, then verifies a fresh value. `expect-ui` compares only
declared scalar or 2/4-number vector fields: missing typed state is
INCONCLUSIVE, while a mismatch is FAIL. Password values are never assertable.
`scroll-window` uses the selected visible window's freshly inspected inner bounds
and reports the new scroll offset.

Mapping controls use a synthetic device only after explicit creation on Settings
> Controls. Start the app's normal detect-input flow before sending a button or
axis code. Mapping input is not evidence of the final binding: axis baseline
samples can be delivered before a binding is accepted. Confirm a binding using
fresh Settings UI readback. Cleanup is reported only after native cancellation
is acknowledged and detection is confirmed stopped. The synthetic device is never physical or rumble
hardware. Mapping cancellation and plan cleanup stop detection.

The UI also supports exact-window `click`, native `key`/`text`/`scroll`/`drag`,
and opt-in screenshots. Native UI keys operate the emulator UI; they do not send
guest keyboard input. Guest keyboard, mouse, lightgun, operator, and JVS APIs
are not exposed by this harness. Settings pages and controls remain subject to
their hardware, network, read-only, apply, and restart requirements. The command
surface does not certify that every setting can be changed or that a changed
setting takes effect in every renderer.

Inspect native telemetry and UI state before requesting a screenshot. Screenshots
are a visual fallback for a specific question, not the first observation.
Do not inspect, capture, or use UI controls during a quiet performance window.

## Version-bound game watches

A watch profile declares version 1, id, platform, game_id, the session
`content_sha256`, and 1..64 watches. The SH-4 platform IDs `dreamcast`, `naomi`,
`naomi2`, `atomiswave`, and `systemsp` use `main_ram`. The optional Hikaru profile
uses `master_ram`, `slave_ram`, `command_ram`, or `bram`, and also binds
`rom_crc32` in BIOS/program/data order. Each watch declares a unique name, region,
byte offset, and type (`u8`, `s8`, `u16`, `s16`, `u32`, `s32`, `f32`). Optional
scale, bias, unit, and raw-value labels decode known fields. Identity mismatch,
invalid ranges, and nonfinite floats do not become valid values.
Platform labels do not guarantee that a particular build runs that system or
publishes a matching identity; the live feed must verify both.

```powershell
node tools/testing/hollytest.mjs profile --run D:/tmp/hollytest-session-001 --file C:/path/to/verified-watch-profile.json
node tools/testing/hollytest.mjs memory --run D:/tmp/hollytest-session-001 --region main_ram --offset 0 --length 32
```

Planet Harriers profiles are optional and require a matching Hikaru-enabled
build and the exact content/ROM identity. See [the profile notes](profiles/README.md)
before using those mappings.

Raw snapshots are bounded to 4096 bytes. SH-4 snapshots pause/resume the CPU;
Hikaru reads use their existing frame lock. Manual snapshots invalidate overlapping
performance captures. No memory mutation API is exposed.

## Optional screenshots and timing reports

```powershell
node tools/testing/hollytest.mjs screenshot --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs record --run D:/tmp/hollytest-session-001 --milliseconds 30000
node tools/testing/hollytest.mjs verify --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs stop --run D:/tmp/hollytest-session-001
```

Screenshots read the composited GPU target, including UI and OSD, before present.
OpenGL, DX11, and supported Vulkan swapchains have native readers. Unsupported
formats/surfaces reject the request; they are not silently replaced with a game-only
image or a desktop screenshot. `h.observe({ screenshot: true })` saves one on demand.

Performance captures retain actual monotonic host presentation intervals and
mean/median/p95/p99/max. These are not GPU execution times, guest frame counts,
or physical input/audio latency. UI inspection/actions, menus, reset/renderer
changes, fast-forward, manual RAM reads, and sample overflow invalidate a capture.
Live telemetry queries are counted as observation overhead. Quiet benchmark
windows should omit polling, screenshots, and UI activity.

Reports retain raw samples and `comparison_verdict: NOT_EVALUATED`. A single
trace does not establish a speedup, regression, or full game correctness.
Incomplete captures retain invalid evidence and the CLI exits 2. This harness
does not provide deterministic replay or an A/B comparison runner.

## Verification batch

Finish implementation and review before running checks together:

```powershell
node --test tools/testing/hollytest.test.mjs tools/testing/playtest.test.mjs
cmake -S tools/testing -B D:/tmp/hollytest-protocol-build
cmake --build D:/tmp/hollytest-protocol-build --config Release
ctest --test-dir D:/tmp/hollytest-protocol-build -C Release --output-on-failure
```

Then build the instrumented emulator and run live integration against the selected
game. Fixture checks alone do not verify real gameplay. Test fixtures use the
system temporary directory (override `HOLLYTEST_TEST_ROOT`); original game/save files are preserved.
Artifacts stay local in the session directory. The bridge opens no network listener.

## Declarative native playtest plans

The existing `hollytest` CLI can execute a bounded JSON plan in an isolated native
session. The runner uses the session's `doctor`, source verification, telemetry,
controller, UI, and screenshot APIs; it does not start or stop an attached emulator.
Build a standalone Windows/Linux app with `HOLLYCAST_TEST_HARNESS=ON`. Node.js 22+
is required. The harness is off
by default and unsupported in mobile, libretro, unit-test, and automation builds.

Create a fresh session with the exact executable, user-authorized game/firmware
paths, and a new output directory. Pass the commit and dirty-patch identity to
`--revision`; the manifest records this declaration and independently hashes the
executable, game, and copied seeds.

```powershell
node tools/testing/hollytest.mjs launch --exe D:/tmp/hollytest-build/Hollycast.exe --out D:/tmp/hollytest-session-001 --game C:/path/to/game-image --firmware-dir C:/path/to/firmware --revision "EXACT_SHA_PLUS_DIRTY_PATCH_ID"
node tools/testing/hollytest.mjs doctor --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs verify --run D:/tmp/hollytest-session-001
node tools/testing/hollytest.mjs playtest --run D:/tmp/hollytest-session-001 --plan tools/testing/playtest.example.json
node tools/testing/hollytest.mjs verify --run D:/tmp/hollytest-session-001
```

`doctor` performs the live PID/session handshake and reports capabilities and
identity. `verify` re-hashes the executable, game and seed inputs. The plan
command repeats both checks before any game action. A `status` command reads a
benchmark status file; use `doctor` to establish an interactive session.

Plans use schema `hollytest.playtest-plan.v1`, a short unique `id`, an optional
`scope` (`game` by default or `ui`), an `identity`, a global `timeout_ms`, and
1..64 ordered steps. Game scope requires the exact `game_id` copied from
`doctor` or `live`, and may also bind `profile_id`, `game_sha256`, and
`executable_sha256`. UI scope requires the exact `executable_sha256`; it can run
without a game or while emulation is paused. Optional UI-scope game/content IDs
and hashes still have to match. Both scopes run `doctor` and verify original
executable/media/seed hashes before operations. Only game scope requires fresh
SH-4 gameplay telemetry.

The example below checks a generic live game session. Mapped assertions
must use `telemetry.game_state.values.<name>.value`, name the expected
`profile_id`, and pass only when the native mapping is identity-matched and the
field is valid. Supported comparisons are `eq`, `ne`, `gt`, `gte`, `lt`, `lte`,
`is_true`, and `is_false`.

A UI-only plan uses the exact executable hash from `doctor` and can navigate
settings while paused without asserting gameplay telemetry:

```json
{
  "schema": "hollytest.playtest-plan.v1",
  "id": "video-settings-inspection",
  "scope": "ui",
  "identity": { "executable_sha256": "EXACT_SHA256_FROM_DOCTOR" },
  "timeout_ms": 10000,
  "steps": [
    { "id": "open-video", "op": "settings", "page": "video" },
    { "id": "check-control-kind", "op": "expect_ui",
      "target": { "semantic_id": "EXACT_ID_FROM_FRESH_INSPECTION" },
      "expected": { "kind": "toggle" } }
  ]
}
```

```json
{
  "schema": "hollytest.playtest-plan.v1",
  "id": "current-game-smoke",
  "identity": { "game_id": "EXACT_GAME_ID_FROM_LIVE" },
  "timeout_ms": 5000,
  "steps": [{
    "id": "running-and-fresh",
    "op": "observe",
    "assertions": [
      { "path": ["state", "running"], "op": "is_true" },
      { "path": ["telemetry", "sample_available"], "op": "is_true" },
      { "path": ["telemetry", "stale"], "op": "is_false" }
    ]
  }]
}
```

Game plans support `observe` with optional assertions, `wait_for` with assertions
and bounded timeout/interval, `press`, `hold`, `controller`, `release`, `click`,
native `ui`, and opt-in `screenshot`. Both scopes support `settings`, `mapping`,
`fill`, `set_toggle`, `expect_ui`, and `scroll_window`. Click selectors can use
`label`, `item_id`, or `semantic_id`, with an optional `window`; click modifiers,
mouse button, and click count are bounded native fields. Mapping button/axis
operations report delivery separately from binding acceptance; use UI readback
to assert the actual setting.
Plans are fully validated before attaching or issuing input. There is no shell,
evaluation, arbitrary output path, automatic retries, or implicit process stop.
The runner stops after the first failed step, marks later steps not-run, and
releases controller input on success, failure, timeout, or Ctrl+C. Reports and
step JSONL are written to a unique directory inside the session; a non-PASS
result exits with code 2. Missing, stale, or unmapped telemetry is inconclusive,
never a pass.

The generic game plan establishes a fresh running emulator and the selected game
identity; it does not establish any game-specific behavior. A UI-scope plan makes
no gameplay or telemetry claim. It can navigate settings without CPU freshness,
but still binds the tested executable and any optional game identity. The runner
makes tests repeatable in structure, not deterministic. Assert only fields whose
meanings and identities are verified. The interactive benchmark reports a
single-run measurement, not a comparison.

For the optional Hikaru branch, `playtest.planet-harriers.example.json` shows a
game-specific mapping plan. It must be used with a Hikaru-enabled build and the
matching included profile; it cannot run on current dev.

## Optional Windows build and compiler-cache helpers

```powershell
./tools/testing/build-windows.ps1 -BuildDirectory C:/path/to/new-build -CacheMode Auto
./tools/testing/measure-build.ps1 -MeasurementDirectory C:/path/to/new-comparison -SccachePath C:/path/to/sccache.exe -SccacheDirectory C:/path/to/new-empty-cache
```

The build helper discovers Visual Studio and Windows Kits. `-PortableBundle`
selects an explicit NuGet bundle; explicit path parameters override discovery.
`-CacheMode Auto` uses sccache only when the local enablement marker matches the
installed executable hash. Set `-EnableHarness:$false` for an ordinary build.
Keep every build and measurement directory new and outside the checkout. A cache
comparison is meaningful only when every phase completes with matching source
fingerprints and fixed options; it does not clear existing user caches.

The first CMake configure of bundled libchdr/zlib can rename its tracked
`zconf.h` to `zconf.h.included`. The helper correctly rejects that source identity
drift. Inspect and review the dependency change, prepare that dependency state,
then use a new build directory. Do not reset unrelated submodule changes.
