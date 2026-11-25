# Guarded SH-4 trace — final v3 measurements

Implemented the guarded ARM64 trace for `0x8c05b3a4 ↔ 0x8c05b3d0`, including cached registers/T and a private cached cycle budget. In the final Release comparison, **whole-replay app CPU fell 16.20% at the median**, with non-overlapping three-run ranges. The continuous fixed-work poll workload used **39.77% less thread CPU** with seven samples per mode.

The measured gameplay scene’s app CPU median fell **15.44%**, but its ranges overlap: **that scene-specific improvement remains inconclusive**. The game remains paced at approximately 30 FPS; no FPS or power-consumption gain is claimed.

## Final Release gameplay comparison

Apple M5, arm64, macOS 26.6, Apple Clang 21; matching Release probe settings, Metal renderer, real BIOS, SH-4 JIT enabled. Existing pinned Metal Slug 6 Mission 1 replay: Right + Fire from guest seconds 80.0–99.9. Three baseline and three final-candidate runs, ordered **A B B A A B**, with runtime block counters and CPU sampling off. Both binaries retain the same compiled profiling/frame-probe infrastructure; candidate activation accounting is included, not subtracted.

| Scope | Baseline CPU seconds, median [range] | Final v3 CPU seconds, median [range] | Median change | Interpretation |
|---|---:|---:|---:|---|
| Whole replay: launch, BIOS, gameplay, teardown | 20.186 [19.516–20.431] | 16.915 [15.939–16.946] | -16.20% | Observed reduction; ranges disjoint |
| Measured gameplay: all app threads | 4.310 [3.944–4.500] | 3.645 [3.345–4.148] | -15.44% | Overlapping ranges; inconclusive |
| Measured gameplay: emulation thread | 4.155 [3.810–4.366] | 3.509 [3.200–4.015] | -15.56% | Overlapping ranges; inconclusive |

The whole-replay result includes preroll and compilation; it is not a measurement confined to the 19.9-second gameplay window. Three samples per app provide an observed comparison, not a formal confidence guarantee. Frame pacing keeps replay wall time near 100.5 seconds in both builds.

| Off-mode run | Scene app CPU s | Whole-replay CPU s | Average FPS | p99 frame ms | Worst frame ms | Intervals >50 ms |
|---|---:|---:|---:|---:|---:|---:|
| 1: baseline | 3.944 | 20.186 | 29.9654 | 37.871 | 38.200 | 0 |
| 2: candidate | 3.345 | 16.915 | 29.9677 | 37.907 | 39.024 | 0 |
| 3: candidate | 3.645 | 15.939 | 29.9640 | 37.924 | 39.059 | 0 |
| 4: baseline | 4.500 | 20.431 | 29.9681 | 38.150 | 39.321 | 0 |
| 5: baseline | 4.310 | 19.516 | 29.9686 | 37.783 | 40.264 | 0 |
| 6: candidate | 4.148 | 16.946 | 29.9682 | 38.071 | 39.273 | 0 |

Every interval over 50 ms is retained in the JSON event arrays: **all are empty**, including the additional candidate sampling run. Baseline/candidate p99 medians are **37.871/37.924 ms**; worst observed frames across the off-mode runs are **40.264/39.273 ms**. Their ranges overlap. These are submitted game-texture delivery intervals, not physical display/GPU-completion timing.

## Guest timing and actual activation

- All seven final captures have the **same 3,328-row guest-frame-cycle sequence**, equal to the original baseline; SHA256 `ffd579fc390c38a07dac1329e785038e62d3b6fdfa379c6adbb3bac6fe6166c4`.
- Every final image exactly matches the pinned screenshot SHA256 `c3dc072b81531e1ce3b7abbef28256c2062b4fb83ed4082b649f3c1aa01c980c`.
- Actual profiler bounds are `16000000128–19980003904`; frame-probe start/terminal cycles are `16002262976`/`20002576832`, unchanged across binaries.
- The final counted correctness smoke has **8,221 executed guest-code groups / 458,330,648 block entries**, exactly equal to the preserved baseline. Normalized opcode/address block counts and all observed edge counts plus structural continuation hints match. Every block’s non-estimated exit count equals its entry count.
- Every final candidate capture has **6,748,762 guarded trace entries within the measurement window**. This counts bounded trace invocations, not guest loop iterations. The underlying guest cycle remains 188,904,658 forward and 188,904,061 reverse optimized-block edge traversals, with 597 exits from each block.

Baseline executable SHA256: `6d83ae5b0f85dff60c09f9e279059d167de19a345f6edd4c3cee813cec97b953`. Final v3 executable SHA256: `3c819006592c7251a459f8245a21e167b636fa13e638cb897bfe0368070e6c23`. Source/compiler/build fingerprints and raw-data hashes are retained in [guarded-trace-results.json](guarded-trace-results.json).

## What changed and what is guarded

The specialization enters at B and executes B→A→B internally. A’s six opcodes and B’s three opcodes must match, as must A’s full cached IR, both blocks’ branch metadata, protected main-RAM page, literal and eligible layout. MMU, unprotected/temporary blocks, unsafe pointers, misaligned/wrapping stacks or changed code use the original B implementation. The loop continues to reread the poll byte, stack byte and stack flag; it does not skip idle guest time or remove polling loads.

The actual game’s folded return path contains **two** branch islands: `0x8c05b3d6: a15a,0009 → 0x8c05b68e: ae89,0009 → 0x8c05b3a4`. A bounded walker checks up to five BRA+NOP islands on the same 4 KiB guest page within a protected host RAM page (16 KiB on this M5) and guards each island on entry. It reuses the original decoded A/B cycle charges, **10/6 cycles**, including the existing SSA branch folding.

R0–R3, T and the private cycle budget stay in host registers inside the trace. Every original per-block signed budget check and subtraction remains. All cached state, including the cycle budget, is committed before side exits, scheduler/interrupt callbacks and ordinary dispatch. A negative pending-B budget uses the local original B prologue; a negative pending-A budget dispatches A. There are no guest writes or host calls inside the fast loop. This is a narrow guarded specialization, not a general tracing JIT.

To opt out before launch, set `[config] Dynarec.GuardedPollTrace = no`. Changing the setting on an already compiled stopped session requires a JIT cache reset/restart. Other CPU backends retain their generic path.

## Independent execution tests and fixed work

**43 regression tests pass, including eight guarded-trace tests.** They compare actual emitted generic and guarded JIT execution: full general/banked/FPU register snapshots, SR/FPSCR/exception state, PC, selected stack/poll/literal/opcode memory regions, cycle budgets and scheduler snapshots. Positive fixtures require actual guarded entries. Cases cover fast-A signed-byte comparisons and both exits, the real two-hop layout/address encoding, negative and boundary budgets, actual IRQ/RTE handler execution, literal/opcode/island mutations, cache reset and rejection of MMU/non-RAM/wrapping-stack layouts.

The disabled manual benchmark uses the same test binary with configuration off/on, actual two-hop opcodes, one discarded run per mode, and seven alternating measured runs per mode. `CpuRunning=true` precedes compilation; actual untimed execution resolves the normal generic direct-link stubs. A continuous warm phase of at least 0.5 guest seconds precedes timing, with no context/cache reset between warmup and measurement. Audio output, renderer and pacing are inactive; normal scheduler/AICA/PVR work is retained. Guest interrupts are masked only in this sustained-poll fixture; real interrupt delivery is separately exercised by the regression test.

| Fixed-work mode | Thread CPU ms, median [range] | Samples |
|---|---:|---:|
| generic | 50.424 [48.460–50.962] | 7 |
| guarded | 30.373 [29.887–34.290] | 7 |

Every measured run covers **400,007,104 guest cycles / 48,830 measurement callbacks**, with scheduler digest `18081345606337134654` and an identical full final register snapshot and checked memory regions (stack/poll/literal/opcode). Guarded runs have 892,873 trace entries, generic runs zero. The **39.77%** CPU reduction has disjoint ranges in this fixed-work workload. It does not establish the same percentage improvement in the complete game. Test binary/source/CMake hashes are fingerprinted separately in the JSON benchmark provenance.

## Post-change profile — statistical estimate

One final candidate sample-only capture attributes **543/4253 samples = 12.77%** of emulation-thread CPU residence to the exclusive A/B cycle union (approximately 494.9 ms after thread-clock scaling). It is one statistical capture, not three-run confidence evidence. The original separate profiling study estimated 27.89% across three baseline sample runs; that historical comparison is context, not a paired CPU-time speedup measurement.

The fused native range for B now contains guest instructions from both A and B. Counting A alone would manufacture an apparent speedup by moving its work into B; this analysis sums their cycle union. Manually emitted trace commits/reloads bypass the allocator-span hooks, so a drop in allocator-span samples is not interpreted as an exact reduction in total synchronization cost. Unmapped native work remains in the CPU-share denominator.

## Calibration and reproducibility

The earlier v2 trace was also measured rather than selected from a favorable run: fixed-work CPU fell 26.91%, while its gameplay scene median rose 14.17% and whole-replay median fell 10.24%, both with overlapping ranges. That gameplay result was inconclusive. v3 added only cached private-budget materialization, passed the same correctness checks, then produced the stronger fixed-work result and received a **fresh** complete gameplay comparison. Both v2 datasets remain archived in the final JSON calibration section.

Raw final comparison: [capture.json](../../.cache/sh4-trace-v3-comparison/sh4-7sddj_2x/capture.json). Counted final smoke and normalized equality proof: [capture.json](../../.cache/sh4-trace-v3-smoke/sh4-x8dbd3dv/capture.json), [logical-count-comparison.json](../../.cache/sh4-trace-v3-smoke/sh4-x8dbd3dv/logical-count-comparison.json). Fixed-work data: [v3 JSON](../../.cache/sh4-trace-tests/fixed-work-benchmark-v3.json). The original top-ten/disassembly report remains unchanged at [RESULTS.md](RESULTS.md).

Exact capture/test/benchmark instructions are in [COMPARE.md](COMPARE.md). Regenerate this report’s machine-readable data with:

```sh
python3 tools/sh4-profile/compare.py \
  .cache/sh4-trace-v3-comparison/sh4-7sddj_2x/capture.json \
  --candidate-counts .cache/sh4-trace-v3-smoke/sh4-x8dbd3dv/sample-01-candidate-both/sh4-profile.json \
  --baseline-counts .cache/sh4-profile-captures/sh4-lghycjxt/sample-02-both/sh4-profile.json \
  --test-log .cache/sh4-trace-tests/v3-regression-tests.log \
  --fixed-work .cache/sh4-trace-tests/fixed-work-benchmark-v3.json \
  --fixed-work-provenance .cache/sh4-trace-tests/fixed-work-benchmark-v3-provenance.json \
  --exploratory-comparison .cache/sh4-trace-comparison/sh4-h3l397ln/v2-analysis.json \
  --exploratory-fixed-work .cache/sh4-trace-tests/fixed-work-benchmark-v2.json \
  --output tools/sh4-profile/guarded-trace-results.json
```
