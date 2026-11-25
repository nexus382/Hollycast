# Paired Release comparison

Keep the pre-change probe and its adjacent `probe-build.json` together. For this
worktree the preserved baseline is `.cache/sh4-trace-baseline/Hollycast.app`;
`preserved-baseline.json` records its original and copied executable hashes.
Build the candidate from current source using the Release commands in
[CAPTURE.md](CAPTURE.md), choosing a fresh output such as
`.cache/sh4-trace-candidate-v3-probe/Hollycast.app`. Never overwrite the baseline.

The capture helper accepts a second app and an explicit ordered sequence. It
requires matching compiler, architecture, and recorded CMake settings. Both apps
run the same pinned replay with `Dynarec.Enabled = yes` and
`Dynarec.GuardedPollTrace = yes`; the pre-change app ignores the latter key.
Every sample records its app label and executable hash. The helper checks for
binary replacement before and after each replay. Finish all source changes,
compilation and other tests before collecting timing results.

```sh
baseline="$PWD/.cache/sh4-trace-baseline/Hollycast.app"
candidate="$PWD/.cache/sh4-trace-candidate-v3-probe/Hollycast.app"
python3 tools/sh4-profile/capture.py --app "$baseline" \
  --candidate-app "$candidate" --require-candidate-trace --sequence candidate:both \
  --output "$PWD/.cache/sh4-trace-smoke" --check-only
python3 tools/sh4-profile/capture.py --app "$baseline" \
  --candidate-app "$candidate" --require-candidate-trace --sequence candidate:both \
  --output "$PWD/.cache/sh4-trace-smoke"
```

Inspect that counted correctness smoke before proceeding: exact guest frame-cycle
sequence, profiler/probe cycle boundaries and screenshot must match the preserved
baseline evidence. Inspect the candidate's trace counters and captured code to
confirm the guarded path ran. A fast run with the guard never taken does not
measure the optimization. Analyze the counted guest blocks/edges to check logical
execution accounting separately from host sampling.

Use alternating runtime-off captures for the primary CPU comparison. The order
below is baseline, candidate, candidate, baseline, baseline, candidate, followed
by one candidate sampling-only run for a post-change profile estimate (the final
cached-budget capture used this seven-run sequence):

```sh
python3 tools/sh4-profile/capture.py --app "$baseline" \
  --candidate-app "$candidate" --require-candidate-trace \
  --sequence baseline:off,candidate:off,candidate:off,baseline:off,baseline:off,candidate:off,candidate:sample \
  --output "$PWD/.cache/sh4-trace-captures"
```

For a repeated post-change profile, append two more `candidate:sample` entries.
A single sampling capture gives a statistical estimate and does not establish
ranking stability across runs. The earlier uncached-budget experiment used three
candidate sampling captures and remains separate from the final build.

The completed `capture.json` separates summaries by app and profiling mode.
`application_comparison.off` reports baseline/candidate CPU medians, ranges,
percentage change and whether ranges overlap. Use scene process CPU for total
emulator cost and scene emulation-thread CPU to localize the change. Whole-replay
CPU includes preroll, compilation of guest blocks and profile serialization;
retain it as a separate measurement. Frame pacing can conceal CPU changes, so
also report FPS, p99, worst frame and every interval over 50 ms.

`guest_timing_validation` compares the actual guest-cycle column of every frame
trace, quantized capture boundaries and screenshots. For this Metal Slug scene,
the original 3,328-row guest-cycle sequence SHA-256 is
`ffd579fc390c38a07dac1329e785038e62d3b6fdfa379c6adbb3bac6fe6166c4`,
the profile bounds are `16000000128–19980003904`, the frame-probe start/terminal
cycles are `16002262976`/`20002576832`, and the pinned screenshot SHA-256 is
`c3dc072b81531e1ce3b7abbef28256c2062b4fb83ed4082b649f3c1aa01c980c`.
Do not claim preserved guest timing if these checks differ; investigate first.

The initial profiling study's sample-only baseline may be used to give historical
ranking context, labeled as a separate measurement. It is not a paired timing
baseline. If the new CPU comparison is marginal or ranges overlap, collect fresh
alternating baseline/candidate sampling-only runs before claiming a performance
result:

```sh
python3 tools/sh4-profile/capture.py --app "$baseline" \
  --candidate-app "$candidate" --require-candidate-trace \
  --sequence baseline:sample,candidate:sample,candidate:sample,baseline:sample,baseline:sample,candidate:sample \
  --output "$PWD/.cache/sh4-trace-sampling-comparison"
```

All captures acquire the same local timing lock as Hollybench. Keep other builds,
test binaries, profilers and timing processes stopped. The CPU sampler remains a
statistical estimate; see [CAPTURE.md](CAPTURE.md) for its attribution limits and
the runtime-off baseline's common compiled-in profiling cost.

After a successful paired capture, regenerate its focused machine-readable
comparison with the counted candidate smoke and the preserved counted baseline:

```sh
python3 tools/sh4-profile/compare.py /path/to/paired/capture.json \
  --candidate-counts /path/to/successful-candidate-smoke/sh4-profile.json \
  --baseline-counts /path/to/preserved-baseline/sh4-profile.json \
  --output guarded-trace-results.json
```

The tool retains every frame interval over the configured threshold, per-run FPS,
p99/worst frame, scene and whole-process CPU ranges, adjacent A/B changes, normalized
logical guest-count checks, timing fingerprints, positive guarded-entry counts and
post-change sampled cycle attribution. Fused native code in B contains guest PCs
from both A and B; compare their cycle union. Trace-emitted state materialization
bypasses the allocator span hooks, so a smaller allocator-span category alone is
not a measurement of eliminated synchronization cost.

The ARM64 execution regressions require `ENABLE_CTEST=ON`, `BUILD_TESTING=ON` and
`TEST_AUTOMATION=ON`. Run the absolute test binary with an ignored runtime directory
as its working directory (the emulator can write private VMU/flash files):

```sh
mkdir -p .cache/sh4-trace-tests/runtime
cd .cache/sh4-trace-tests/runtime
../Hollycast.app/Contents/MacOS/Hollycast \
  --gtest_filter='GuardedPollTraceTest.*:Sh4InterpreterTest.*:MmuTest.*:AssemblyDebuggerTest.*:TimerTest.*'
```

The eight trace tests execute real SH-4 instructions against the generic JIT
reference and require positive guarded-path entries for eligible programs. They
cover both exits, signed bytes, scheduler-phase/negative-budget boundaries,
interrupt/RTE banked state, opcode/literal/branch-island invalidation, the actual
two-hop branch chain, cache resets, and rejected MMU/non-RAM/wrapping-stack layouts.

For the manual fixed-work emitted-JIT benchmark, build the same Release test target,
then run only the disabled benchmark in its private runtime directory:

```sh
benchmark_binary="$PWD/.cache/sh4-trace-tests/Hollycast.app/Contents/MacOS/Hollycast"
benchmark_output="$PWD/.cache/sh4-trace-tests/fixed-work-benchmark.json"
mkdir -p .cache/sh4-trace-tests/runtime
cd .cache/sh4-trace-tests/runtime
HOLLYCAST_SH4_TRACE_BENCHMARK_OUTPUT="$benchmark_output" "$benchmark_binary" \
  --gtest_also_run_disabled_tests \
  --gtest_filter='GuardedPollTraceTest.DISABLED_ActualGameTraceBenchmark'
```

This is an optional measurement, excluded from the normal suite and without a
speed threshold. It uses the actual two-hop guarded SH-4 program in one binary,
configuration off/on, seven samples per mode with alternating order, one discarded
full run per mode, and a continuous untimed warm phase in every sample. Precompile
runs with `CpuRunning=true`, then actual untimed execution resolves the generic
link stubs. No cache/context reset separates warmup and timing. A thread CPU clock
and steady wall clock cover an equal guest-cycle interval; normal device/scheduler
work remains, while audio output, renderer and pacing are inactive. The benchmark
compares final architectural/memory state and a scheduler digest and requires
positive trace entries. Its result measures this fixed-work workload, not gameplay
FPS or a guaranteed whole-app improvement.
