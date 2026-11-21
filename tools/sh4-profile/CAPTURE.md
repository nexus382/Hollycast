# Reproducible Release gameplay capture

The local `hollycast-benchmark` skill supplies the gameplay probe and the pinned
`metal-slug` profile. The normal benchmark CLI removes unrelated environment
variables, so use `capture.py` to pass the SH-4 profiler settings to the app.
It reuses that runner's media hashes, real-BIOS checks, replay, frame analysis,
and exact final screenshot validation. It explicitly enables `Dynarec.Enabled`.

On this machine the profile boots the real Dreamcast BIOS, selects Metal Slug 6,
and holds Right + Fire in Mission 1. The capture interval is guest cycles
16,000,000,000 through 19,980,000,000: guest seconds 80.0 through 99.9 at the
unchanged 200 MHz SH-4 clock. The null audio backend paces normal-speed gameplay.
The replay ends after 100 guest seconds. Do not alter replay inputs, SH-4 clock,
audio pacing, frame skipping, renderer, or resolution between modes.

From the checkout, with its build dependencies initialized:

```sh
build="$PWD/.cache/sh4-profile-build"
probe="$HOME/.codex/skills/hollycast-benchmark/probe/build.py"
app="$PWD/.cache/sh4-profile-probe/Hollycast.app"
cmake -S . -B "$build" -G Ninja \
  -DCMAKE_PROJECT_INCLUDE="$PWD/script/macos-build.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DBUILD_TESTING=OFF -DTEST_AUTOMATION=ON -DUSE_METAL=ON \
  -DUSE_VULKAN=OFF -DUSE_BREAKPAD=OFF -DUSE_DISCORD=OFF \
  -DUSE_OPENMP=OFF -DFT_DISABLE_PNG=ON -DENABLE_SH4_LOOP_PROFILER=ON
cmake --build "$build" --target Hollycast
python3 "$probe" --build-dir "$build" --output-app "$app" --check-only
python3 "$probe" --build-dir "$build" --output-app "$app"
hollybench gameplay show metal-slug
python3 tools/sh4-profile/capture.py --app "$app" \
  --output "$PWD/.cache/sh4-profile-captures" \
  --modes off,both,sample,sample,both,off,off,both,sample
```

Each fresh process has a fresh runtime, BIOS copy, and VMU. The game remains at
its original media location. The output contains `capture.json`, per-run frame
traces and screenshots, emulator logs, and `sh4-profile.json` for enabled runs.
A failed validation leaves an error report and its raw outputs; exclude it from
measurements. Keep `probe-build.json` beside the app: capture refuses a build
without a Release provenance manifest.

The completed report compares the actual `guest_cycle` column from every frame
trace, separately from host timestamps. `guest_timing_validation` records exact
sequence hashes, equality results, actual profiler/probe cycle boundaries and
screenshot hashes. Inspect these checks before claiming guest timing was
preserved; equal final screenshots alone do not establish identical timing.

`--modes` accepts `off`, `counts`, `sample`, and `both`. Use the same executable
for these runs. The primary command captures three runs of each of `off`, `both`,
and `sample`, alternating their order. A quick smoke capture is `--modes both`;
use at least three baseline and three enabled runs for overhead. For more detail, repeat an
alternating series for each enabled mode, such as
`--modes off,counts,counts,off,off,counts` and
`--modes off,sample,sample,off,off,sample`. Run one timing process at a time and
record thermal/load changes. Do not run builds or other profilers concurrently.

The helper records process user/system CPU with `wait4` for the app child only.
This covers the whole replay, including its approximately 80-second preroll,
BIOS boot and shutdown; label overhead derived from it as whole-replay overhead.
Prefer the profiler's scene CPU boundaries when comparing the profiling window.
Report baseline and enabled medians, ranges, and the percentage change
`100 * (enabled / baseline - 1)`. Frame pacing can hide CPU overhead, so retain
CPU measurements as well as FPS, p99, worst frame, stutters, guest-cycle bounds,
and screenshot hashes. Treat small differences with overlapping ranges as
inconclusive. Count instrumentation and sampling can perturb the measured loop
weights: compare sample-only and combined runs when making an optimization
recommendation.

The registered profile's local files are under
`~/.local/share/hollybench/profiles/metal-slug`; `hollybench gameplay show`
verifies them before capture. For another game, register and visually pin a
reproducible gameplay profile through `hollybench gameplay setup/capture/pin`
first, then pass `--profile NAME` to the helper. No commercial media or firmware
belongs in the repository or report.

## Sampling and timing boundaries

The native CPU sampler currently requires ARM64 macOS. It observes its own
emulation thread with Mach at approximately 997 Hz, records PC/LR and a bounded
frame-pointer walk, and maps those native addresses through timestamped JIT
block lifetimes. It does not attach to another process or require external
profiler privileges. Mach `TH_STATE_RUNNING` includes runnable threads, so
runnable observations and derived CPU attribution are statistical estimates.
The sampling interval is deliberately offset from a 30/60 Hz cadence. Sampling
can still perturb scheduling and power management. Unmapped samples stay in
the denominator. Samples inside C helpers require a valid JIT return address;
missing stacks and unmapped code are not silently redistributed.

The requested guest-cycle interval is quantized to the first scheduler tick at
or beyond each bound. The JSON records the actual start/end cycles, monotonic
host boundaries, and emulation-thread/process CPU-clock differences. Compare
these emitted boundaries across modes; do not describe requested boundaries
as cycle-exact captures. CPU clock differences measure CPU consumed within the
actual boundaries; sampled loop shares and any conversion of those shares to
seconds remain estimates. `missed_sample_deadlines` counts wake misses across
the whole replay, including inactive preroll; `sample_attempts` counts only
active-window observations.

The runtime `off` baseline uses the same compiled executable as the enabled
modes. This measures incremental runtime profiling overhead. Its common
compiled-in scheduler gate and CPU-clock boundary cost are excluded from that
overhead comparison. `ENABLE_SH4_LOOP_PROFILER` defaults to OFF; a normal build
removes profiling hooks at compile time. A comparison against a separately
compiled option-OFF build would be needed to quantify the common compiled-in
cost. Do not infer a small speedup or slowdown from overlapping CPU ranges.

Normal shutdown flushes through `bm_Term()`. Metal `TEST_AUTOMATION` exits via
`std::_Exit` after screenshot readback, so its explicit profiling flush occurs
immediately before that exit. Serialization follows the captured interval and
does not contaminate scene CPU clocks. Whole-process `wait4` CPU includes it.

## Analyze the sample-only/counts pair

The sample-only replay leaves JIT block execution unchanged by block counters.
Use a counted replay of the same pinned scene for execution counts and CFG,
then join on guest instruction addresses and opcodes. The analysis labels the
separate-run provenance and excludes unmatched code. For the first pair from
the nine-run command, replace `CAPTURE` below with the printed capture directory:

```sh
python3 tools/sh4-profile/analyze.py \
  "$CAPTURE/sample-03-sample/sh4-profile.json" \
  --counts "$CAPTURE/sample-02-both/sh4-profile.json" \
  --json "$CAPTURE/loops-pair-1.json" \
  --markdown "$CAPTURE/loops-pair-1.md"
```

Repeat with sample 04/counts 05 and sample 09/counts 08 to check ranking stability.
Captured disassembly annotations may contain compilation-time register values;
use opcode bytes to identify code and avoid treating those annotations as values
at every execution. See [README.md](README.md) for exclusive nested-loop
ownership, interprocedural exclusions, unresolved code, and attribution limits.
