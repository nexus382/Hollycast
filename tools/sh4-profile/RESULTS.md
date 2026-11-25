# Measured SH-4 gameplay loops — 15 November 2026

The hottest observed guest cycle is **0x8c05b3a4 ↔ 0x8c05b3d0**: **27.89% of emulation-thread CPU** by pooled exclusive samples (three-run range **26.40–29.05%**), or an estimated **25.13% of whole-app CPU**. It repeatedly polls a byte/compares a stack byte, then tests a stack flag. The strongest first optimization is a guarded two-block JIT trace with shared register allocation; no optimization was implemented.

Keep every guest polling load, original per-block cycle charge, scheduler check, interrupt boundary and required flush/reload on exits/runtime events. Target the unconditional block-boundary register cleanup/preload: `compileBlock` calls `regalloc.Cleanup()` before `RelinkBlock` (`core/rec-ARM64/rec_arm64.cpp:1005`), and `Cleanup()` calls `FlushAllRegs(true)` (`core/hw/sh4/dyna/ssa_regalloc.h:247`). The hot cycle has **1,098 allocator-load/store samples**, **29.74% of its sampled CPU residence / 8.29% of all emulation-thread samples**. That identifies an optimization opportunity, not recoverable savings or a promised FPS gain. The title is paced at 30 FPS. Removing guest reads or skipping idle guest time needs separate correctness evidence.

## Capture and evidence

- Scene: Metal Slug 6 Dreamcast conversion, game ID `T0000M`, Mission 1; Marco holds Right + Fire. Existing pinned `hollybench` replay, real BIOS and fresh VMU, native-resolution Metal, normal-speed pacing.
- Machine: Apple M5, arm64, macOS 26.6; Apple Clang 21; CMake **Release**, SH-4 JIT explicitly enabled, `ENABLE_SH4_LOOP_PROFILER=ON`. Executable SHA256: `6d83ae5b0f85dff60c09f9e279059d167de19a345f6edd4c3cee813cec97b953`.
- Requested window: guest cycles `16,000,000,000–19,980,000,000` (80.0–99.9 guest seconds). Actual scheduler-aligned window: `16,000,000,128–19,980,003,904`, identical in all nine runs.
- Order: `off,both,sample,sample,both,off,off,both,sample`; three runtime-off baselines, three counts+samples runs and three sample-only runs. CPU ranking uses **sample-only** runs; CFG/counts come from paired deterministic counted replays: `03/02`, `04/05`, `09/08`.
- All nine final screenshots exactly match the pinned reference SHA256 `c3dc072b81531e1ce3b7abbef28256c2062b4fb83ed4082b649f3c1aa01c980c`. All nine 3,328-row submitted-frame guest-cycle sequences and measurement bounds match; every run has zero intervals over 50 ms.
- Each counted replay has **458,330,648 block entries**, and every block’s non-estimated outgoing-edge count equals its entry count. Normalized per-code block counts and edge counts are identical across all three counted replays; hashes are retained in `results.json`.
- Each sample/count join matches guest instruction addresses/opcodes with zero unmatched sampled code. Native range/lifecycle generations are resolved before cross-run joining.

Full local capture: [capture.json](../../.cache/sh4-profile-captures/sh4-lghycjxt/capture.json). Raw profiles, frames, logs and screenshots remain under that ignored directory. [results.json](results.json) retains source/executable/profile hashes, exact counters, individual run estimates and every top-ten block’s captured disassembly. Rebuild/capture instructions: [CAPTURE.md](CAPTURE.md). Validation: 10 analyzer tests pass; the Release build reports no remaining work; all five touched C++/Objective-C++ translation units pass profiling-OFF syntax checks.

## Top ten loops

Percentages below estimate **exclusive emulation-thread CPU**, using all 13,237 runnable-state thread observations, including unmapped observations, as denominator. Milliseconds use each sample run’s measured thread CPU clock scaled by its sample share. Counts are exact observations from separate matched replays, identical across all three counted runs. Closely ranked small loops have few samples; their ordinal order is uncertain.

| Rank | SH-4 address | Kind | CPU % estimate | Run range % | CPU ms/run estimate, range | Entries/run | Measured backedge/edge traversals per run |
|---:|---|---|---:|---:|---:|---:|---|
| 1 | `0x8c05b3a4 ↔ 0x8c05b3d0` | two_block_cycle | 27.892 | 26.398–29.054 | 958.8–1326.7 | 188,905,255 / 188,904,658 | 188,904,658 / 188,904,061 |
| 2 | `0x8c0cdca8` | natural | 0.997 | 0.919–1.112 | 33.4–45.4 | 1,704,365 | 1,678,144 |
| 3 | `0x8c03c750` | natural | 0.725 | 0.485–0.895 | 17.6–40.9 | 2,849,738 | 2,826,932 |
| 4 | `0x8c0b3112` | natural | 0.612 | 0.562–0.671 | 20.4–30.7 | 1,941,978 | 1,941,504 |
| 5 | `0x8c05e7c8` | natural | 0.393 | 0.272–0.509 | 11.1–23.2 | 1,222,059 | 1,221,462 |
| 6 | `0x8c01255c` | natural | 0.317 | 0.255–0.366 | 9.3–16.7 | 431,763 | 287,842 |
| 7 | `0x8c0d60e6` | natural | 0.317 | 0.203–0.587 | 8.3–21.3 | 764,160 | 740,280 |
| 8 | `0x8c072dfc` | natural | 0.295 | 0.128–0.387 | 4.6–17.7 | 264,676 | 132,338 |
| 9 | `0x8c0c9f84` | natural | 0.287 | 0.204–0.366 | 7.4–16.7 | 35,820 | 28,656 |
| 10 | `0x8c06d33c` | natural | 0.257 | 0.182–0.326 | 7.4–14.9 | 968,738 | 484,369 |

The first row is a confirmed observed reciprocal two-block cycle inside a larger multi-entry SCC; it is **not** claimed to have a natural-loop dominance header. Its edges have 188,904,658 forward and 188,904,061 reverse traversals per replay, with **597 exits from each block**. The smaller edge count is only an upper bound on uninterrupted completed two-edge cycles; marginal counts do not preserve full traversal order.

Natural loops come from observed CFG dominance backedges. Observed two-block cycles participate in the same innermost ownership scheme. Each sample has one owner; parent loop/SCC rows retain only residual samples. Shared guest callees are separate regions, so their cost is not duplicated in every caller. Estimated call-continuation edges preserve intraprocedural structure but are not measured returns. None of the ten listed backedge/cycle counts depends on an estimated continuation.

## Fallback and state-synchronization costs

No listed loop has a decoded interpreter-fallback site. The high-cost alternatives here are allocator state traffic and native memory/fault helpers. Allocator site counts multiplied by block entries are **upper-bound estimates**, because exceptions can bypass later instructions. Their counts do not themselves measure CPU cost.

- Hot poll cycle: 777 load-span +321 store-span samples. Per replay the static-site × entries upper bounds are 377,809,913 allocator loads and 1,322,335,591 stores. No interpreter fallback site was observed.
- `0x8c03c750`: 67/96 loop samples are inferred native-callee attribution: `addrspace::write64` 26, `__mprotect` 22, `_sigtramp` 18, `VramLockedWriteOffset` 1. Its five-opcode floating-point copy/store loop therefore has measured protection/fault-path expense, although it is much smaller than the poll cycle.
- `0x8c01255c`: store-queue setup/translation/write helpers account for named samples (`setSqwHandler` 6, `mmu_TranslateSQW` 3, `WriteMemBlock_nommu_sq` 2); nine additional native samples lack a symbol.

## Coverage and estimates

| Exclusive accounting bucket | Samples | % of all thread observations |
|---|---:|---:|
| Observed two-block cycles | 3,746 | 28.299 |
| Natural loops | 908 | 6.860 |
| Irreducible SCC residuals | 70 | 0.529 |
| Unknown-entry cyclic residuals | 3 | 0.023 |
| Mapped outside identified cyclic regions | 5,840 | 44.119 |
| Ambiguous overlapping cycles | 5 | 0.038 |
| Unmapped native observations | 2,665 | 20.133 |

These buckets sum to 13,237 with no double-counting. **10,572 samples (79.87%) map to a guest block; 7,080 (53.49% of all observations) map further to a captured guest instruction span.** The remaining mapped observations are block-level only. Unmapped observations include real non-JIT work such as AICA audio, scheduler/native execution and unavailable/truncated native stacks, not a presumed missing guest-loop contribution. Pooled unmapped leaves include `AICA_Sample` 541 and `CalcPlfo` 464 samples.

Native leaf PC maps directly to live RX JIT ranges. A matching LR/frame-pointer return address instead infers the calling JIT block (`return_address−1`); this attribution is less direct, especially across signal paths. Live generation lifetimes prevent reused host addresses from mapping to retired code. The sampler takes ~997 Hz wall-clock observations conditional on Mach `RUNNING`, which includes runnable threads; race/scheduling effects and incomplete stacks make CPU residence statistical estimates. The three sample-only runs have zero sampler errors/drops, and 75/4/20 whole-replay missed deadlines respectively (including pre-scene sampling-thread wakeups). CPU milliseconds use thread-clock scaling rather than pretending nominal sample periods are exact consumed CPU time.

Guest CFG targets with missing/ambiguous captured code versions exclude 133 traversals per counted run (399 pooled). Residual SCCs are not presented as single confirmed loops; short-cycle overlaps retain an explicit ambiguous bucket. Paths not exercised in this scene may change loop structure. CPU and counts were captured on separate deterministic replays, so their join is an estimate of per-loop cost rather than a simultaneous exact trace.

## Measured profiling overhead

All modes use the **same profiling-capable Release executable**. This measures enabling runtime sampling/counters; it does not measure the common compile-time gate or frame-probe cost against a fully uninstrumented binary. Profiling-OFF compilation was syntax-checked, not gameplay-timed as a separate full binary.

| Runtime mode, 3 runs each | Scene EMU-thread CPU s, median [range] | Scene whole-app CPU s, median [range] | Whole-app scene CPU change vs off |
|---|---:|---:|---:|
| off | 4.323 [4.024–4.657] | 4.451 [4.147–4.793] | baseline |
| sample-only | 4.084 [3.632–4.566] | 4.535 [4.046–5.049] | +1.91% |
| counts + samples | 4.521 [4.356–4.955] | 4.976 [4.796–5.439] | +11.80% |

Sample-only process CPU is **+1.91%** at the scene median, with overlapping ranges; the exact sampling overhead is unresolved amid host variation. Its thread-only median is −5.55%, which is not evidence that profiling speeds up emulation. Counts+samples process CPU is **+11.80%** at the scene median (thread CPU +4.57%); whole-replay process CPU changes are +2.54% sample-only and +16.83% combined. Combined counters are therefore used for counts/CFG, not final CPU ranking. Sample-only observations avoid their execution-cost bias.

Every mode stays at 29.966–29.970 delivered game FPS with zero >50 ms intervals. Identical frame guest-cycle sequences, screenshots, exact counted paths and unchanged cycle deductions/scheduler constants support preserved guest timing. They do not establish hardware display/GPU-completion timing.

## Captured disassembly and per-block counts

The following lists every captured decoded guest instruction for each top-ten region, including member blocks whose samples belong to nested children. Per-block samples are inclusive raw block residence; only the region’s **exclusive** table share is ranked. Register-value comments are omitted for readability: the disassembler’s original comments in `results.json` reflect compilation-time state, not a runtime register trace. GBR-relative textual resolutions can likewise depend on compilation-time state; instruction addresses/opcodes are authoritative. Existing JIT branch-target folding may bypass a standalone branch in its optimized block CFG; reported edge targets are the executed JIT block transitions.

### 1. 0x8c05b3a4 (two_block_cycle)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 24.0, "jit_self/register_load": 777.0, "jit_self/register_store": 321.0, "jit_self/unclassified": 2570.0}`

Native leaf samples, inferred caller attribution: `{"unknown native leaf": 24.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 377809913, "allocator_stores_x_block_entries_upper_bound": 1322335591}`

```text
block 0x8c05b3a4: entries/replay=188,905,255; pooled raw block samples=2343
  0x8c05b3a4  d309  mov.l @(0x8C05B3CC[PC]),R3                 ; mapped instruction samples=0
  0x8c05b3a6  6130  mov.b @R3,R1                               ; mapped instruction samples=7
  0x8c05b3a8  e013  mov #0x13,R0                               ; mapped instruction samples=6
  0x8c05b3aa  02fc  mov.b @(R0,R15),R2                         ; mapped instruction samples=1048
  0x8c05b3ac  3122  cmp/hs R2,R1                               ; mapped instruction samples=4
  0x8c05b3ae  8b0f  bf 0x8C05B3D0                              ; mapped instruction samples=0
block 0x8c05b3d0: entries/replay=188,904,658; pooled raw block samples=1349
  0x8c05b3d0  53f3  mov.l @(0xC,R15),R3                        ; mapped instruction samples=357
  0x8c05b3d2  2338  tst R3,R3                                  ; mapped instruction samples=294
  0x8c05b3d4  8901  bt 0x8C05B3DA                              ; mapped instruction samples=0
```

### 2. 0x8c0cdca8 (natural)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 1.0, "jit_self/register_load": 3.0, "jit_self/register_store": 16.0, "jit_self/unclassified": 112.0}`

Native leaf samples, inferred caller attribution: `{"unknown native leaf": 1.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 9081218, "allocator_stores_x_block_entries_upper_bound": 29122575}`

```text
block 0x8c0cdca8: entries/replay=1,704,365; pooled raw block samples=16
  0x8c0cdca8  50f4  mov.l @(0x10,R15),R0                       ; mapped instruction samples=2
  0x8c0cdcaa  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0cdcac  88fe  cmp/eq #0xFFFFFFFE,R0                      ; mapped instruction samples=0
  0x8c0cdcae  8903  bt 0x8C0CDCB8                              ; mapped instruction samples=0
block 0x8c0cdcb0: entries/replay=1,704,365; pooled raw block samples=10
  0x8c0cdcb0  88ff  cmp/eq #0xFFFFFFFF,R0                      ; mapped instruction samples=0
  0x8c0cdcb2  8903  bt 0x8C0CDCBC                              ; mapped instruction samples=0
block 0x8c0cdcc2: entries/replay=1,678,144; pooled raw block samples=16
  0x8c0cdcc2  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=1
  0x8c0cdcc4  52f9  mov.l @(0x24,R15),R2                       ; mapped instruction samples=1
  0x8c0cdcc6  5133  mov.l @(0xC,R3),R1                         ; mapped instruction samples=5
  0x8c0cdcc8  3120  cmp/eq R2,R1                               ; mapped instruction samples=0
  0x8c0cdcca  8901  bt 0x8C0CDCD0                              ; mapped instruction samples=0
block 0x8c0cdcd0: entries/replay=561,408; pooled raw block samples=3
  0x8c0cdcd0  e300  mov #0x0,R3                                ; mapped instruction samples=0
  0x8c0cdcd2  2f32  mov.l R3,@R15                              ; mapped instruction samples=1
  0x8c0cdcd4  a069  bra 0x8C0CDDAA                             ; mapped instruction samples=0
  0x8c0cdcd6  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cdcd8: entries/replay=561,408; pooled raw block samples=31
  0x8c0cdcd8  d213  mov.l @(0x8C0CDD28[PC]),R2                 ; mapped instruction samples=0
  0x8c0cdcda  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cdcdc  415a  lds R1,FPUL                                ; mapped instruction samples=0
  0x8c0cdcde  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0cdce0  63f2  mov.l @R15,R3                              ; mapped instruction samples=3
  0x8c0cdce2  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0cdce4  f22d  float FPUL,FR2                             ; mapped instruction samples=0
  0x8c0cdce6  f322  fmul FR2,FR3                               ; mapped instruction samples=0
  0x8c0cdce8  c710  mova @(0x8C0CDD2C[PC]),R0                  ; mapped instruction samples=0
  0x8c0cdcea  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0cdcec  f322  fmul FR2,FR3                               ; mapped instruction samples=0
  0x8c0cdcee  f33d  ftrc FR3, FPUL                             ; mapped instruction samples=3
  0x8c0cdcf0  035a  sts FPUL,R3                                ; mapped instruction samples=0
  0x8c0cdcf2  1f31  mov.l R3,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c0cdcf4  51f4  mov.l @(0x10,R15),R1                       ; mapped instruction samples=0
  0x8c0cdcf6  53f1  mov.l @(0x4,R15),R3                        ; mapped instruction samples=0
  0x8c0cdcf8  5111  mov.l @(0x4,R1),R1                         ; mapped instruction samples=0
  0x8c0cdcfa  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cdcfc  d20c  mov.l @(0x8C0CDD30[PC]),R2                 ; mapped instruction samples=1
  0x8c0cdcfe  6022  mov.l @R2,R0                               ; mapped instruction samples=0
  0x8c0cdd00  3108  sub R0,R1                                  ; mapped instruction samples=0
  0x8c0cdd02  415a  lds R1,FPUL                                ; mapped instruction samples=0
  0x8c0cdd04  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0cdd06  53f9  mov.l @(0x24,R15),R3                       ; mapped instruction samples=1
  0x8c0cdd08  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0cdd0a  d00a  mov.l @(0x8C0CDD34[PC]),R0                 ; mapped instruction samples=1
  0x8c0cdd0c  f136  fmov.s @(R0,R3),FR1                        ; mapped instruction samples=0
  0x8c0cdd0e  f311  fsub FR1,FR3                               ; mapped instruction samples=0
  0x8c0cdd10  f19d  fldi1 FR1                                  ; mapped instruction samples=0
  0x8c0cdd12  f110  fadd FR1,FR1                               ; mapped instruction samples=2
  0x8c0cdd14  f312  fmul FR1,FR3                               ; mapped instruction samples=0
  0x8c0cdd16  f33d  ftrc FR3, FPUL                             ; mapped instruction samples=0
  0x8c0cdd18  035a  sts FPUL,R3                                ; mapped instruction samples=0
  0x8c0cdd1a  1f33  mov.l R3,@(0xC,R15)                        ; mapped instruction samples=1
  0x8c0cdd1c  51f6  mov.l @(0x18,R15),R1                       ; mapped instruction samples=2
  0x8c0cdd1e  3310  cmp/eq R1,R3                               ; mapped instruction samples=2
  0x8c0cdd20  890a  bt 0x8C0CDD38                              ; mapped instruction samples=0
block 0x8c0cdd38: entries/replay=52,872; pooled raw block samples=4
  0x8c0cdd38  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=0
  0x8c0cdd3a  5232  mov.l @(0x8,R3),R2                         ; mapped instruction samples=0
  0x8c0cdd3c  622b  neg R2,R2                                  ; mapped instruction samples=0
  0x8c0cdd3e  d11e  mov.l @(0x8C0CDDB8[PC]),R1                 ; mapped instruction samples=0
  0x8c0cdd40  6312  mov.l @R1,R3                               ; mapped instruction samples=0
  0x8c0cdd42  3238  sub R3,R2                                  ; mapped instruction samples=0
  0x8c0cdd44  425a  lds R2,FPUL                                ; mapped instruction samples=0
  0x8c0cdd46  f32d  float FPUL,FR3                             ; mapped instruction samples=1
  0x8c0cdd48  50f9  mov.l @(0x24,R15),R0                       ; mapped instruction samples=0
  0x8c0cdd4a  4008  shll2 R0                                   ; mapped instruction samples=0
  0x8c0cdd4c  d21b  mov.l @(0x8C0CDDBC[PC]),R2                 ; mapped instruction samples=0
  0x8c0cdd4e  f226  fmov.s @(R0,R2),FR2                        ; mapped instruction samples=0
  0x8c0cdd50  f321  fsub FR2,FR3                               ; mapped instruction samples=0
  0x8c0cdd52  f29d  fldi1 FR2                                  ; mapped instruction samples=0
  0x8c0cdd54  f220  fadd FR2,FR2                               ; mapped instruction samples=0
  0x8c0cdd56  f322  fmul FR2,FR3                               ; mapped instruction samples=0
  0x8c0cdd58  f33d  ftrc FR3, FPUL                             ; mapped instruction samples=0
  0x8c0cdd5a  025a  sts FPUL,R2                                ; mapped instruction samples=0
  0x8c0cdd5c  1f22  mov.l R2,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0cdd5e  53fa  mov.l @(0x28,R15),R3                       ; mapped instruction samples=0
  0x8c0cdd60  3230  cmp/eq R3,R2                               ; mapped instruction samples=0
  0x8c0cdd62  8901  bt 0x8C0CDD68                              ; mapped instruction samples=0
block 0x8c0cdd68: entries/replay=17,696; pooled raw block samples=1
  0x8c0cdd68  52f7  mov.l @(0x1C,R15),R2                       ; mapped instruction samples=0
  0x8c0cdd6a  e3ff  mov #0xFFFFFFFF,R3                         ; mapped instruction samples=0
  0x8c0cdd6c  2232  mov.l R3,@R2                               ; mapped instruction samples=0
  0x8c0cdd6e  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0cdd70  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c0cdd72  2338  tst R3,R3                                  ; mapped instruction samples=0
  0x8c0cdd74  8916  bt 0x8C0CDDA4                              ; mapped instruction samples=0
block 0x8c0cdd76: entries/replay=584; pooled raw block samples=0
  0x8c0cdd76  e348  mov #0x48,R3                               ; mapped instruction samples=0
  0x8c0cdd78  d211  mov.l @(0x8C0CDDC0[PC]),R2                 ; mapped instruction samples=0
  0x8c0cdd7a  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cdd7c  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c0cdd7e  8911  bt 0x8C0CDDA4                              ; mapped instruction samples=0
block 0x8c0cdd80: entries/replay=584; pooled raw block samples=0
  0x8c0cdd80  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0cdd82  e100  mov #0x0,R1                                ; mapped instruction samples=0
  0x8c0cdd84  2212  mov.l R1,@R2                               ; mapped instruction samples=0
  0x8c0cdd86  d00e  mov.l @(0x8C0CDDC0[PC]),R0                 ; mapped instruction samples=0
  0x8c0cdd88  6202  mov.l @R0,R2                               ; mapped instruction samples=0
  0x8c0cdd8a  4208  shll2 R2                                   ; mapped instruction samples=0
  0x8c0cdd8c  d00d  mov.l @(0x8C0CDDC4[PC]),R0                 ; mapped instruction samples=0
  0x8c0cdd8e  51f8  mov.l @(0x20,R15),R1                       ; mapped instruction samples=0
  0x8c0cdd90  0216  mov.l R1,@(R0,R2)                          ; mapped instruction samples=0
  0x8c0cdd92  d00d  mov.l @(0x8C0CDDC8[PC]),R0                 ; mapped instruction samples=0
  0x8c0cdd94  d20a  mov.l @(0x8C0CDDC0[PC]),R2                 ; mapped instruction samples=0
  0x8c0cdd96  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cdd98  7101  add #1,R1                                  ; mapped instruction samples=0
  0x8c0cdd9a  2212  mov.l R1,@R2                               ; mapped instruction samples=0
  0x8c0cdd9c  71ff  add #-1,R1                                 ; mapped instruction samples=0
  0x8c0cdd9e  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cdda0  52f5  mov.l @(0x14,R15),R2                       ; mapped instruction samples=0
  0x8c0cdda2  0126  mov.l R2,@(R0,R1)                          ; mapped instruction samples=0
  0x8c0cdda4  63f2  mov.l @R15,R3                              ; mapped instruction samples=0
  0x8c0cdda6  7301  add #1,R3                                  ; mapped instruction samples=0
  0x8c0cdda8  2f32  mov.l R3,@R15                              ; mapped instruction samples=0
  0x8c0cddaa  d308  mov.l @(0x8C0CDDCC[PC]),R3                 ; mapped instruction samples=0
  0x8c0cddac  6232  mov.l @R3,R2                               ; mapped instruction samples=0
  0x8c0cddae  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0cddb0  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0cddb2  8b91  bf 0x8C0CDCD8                              ; mapped instruction samples=0
block 0x8c0cdda4: entries/replay=560,824; pooled raw block samples=10
  0x8c0cdda4  63f2  mov.l @R15,R3                              ; mapped instruction samples=2
  0x8c0cdda6  7301  add #1,R3                                  ; mapped instruction samples=0
  0x8c0cdda8  2f32  mov.l R3,@R15                              ; mapped instruction samples=1
  0x8c0cddaa  d308  mov.l @(0x8C0CDDCC[PC]),R3                 ; mapped instruction samples=0
  0x8c0cddac  6232  mov.l @R3,R2                               ; mapped instruction samples=1
  0x8c0cddae  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0cddb0  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0cddb2  8b91  bf 0x8C0CDCD8                              ; mapped instruction samples=0
block 0x8c0cddaa: entries/replay=561,408; pooled raw block samples=6
  0x8c0cddaa  d308  mov.l @(0x8C0CDDCC[PC]),R3                 ; mapped instruction samples=0
  0x8c0cddac  6232  mov.l @R3,R2                               ; mapped instruction samples=0
  0x8c0cddae  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0cddb0  3123  cmp/ge R2,R1                               ; mapped instruction samples=1
  0x8c0cddb2  8b91  bf 0x8C0CDCD8                              ; mapped instruction samples=0
block 0x8c0cddd0: entries/replay=1,678,144; pooled raw block samples=35
  0x8c0cddd0  52f5  mov.l @(0x14,R15),R2                       ; mapped instruction samples=0
  0x8c0cddd2  7201  add #1,R2                                  ; mapped instruction samples=0
  0x8c0cddd4  1f25  mov.l R2,@(0x14,R15)                       ; mapped instruction samples=0
  0x8c0cddd6  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=1
  0x8c0cddd8  7310  add #16,R3                                 ; mapped instruction samples=4
  0x8c0cddda  1f34  mov.l R3,@(0x10,R15)                       ; mapped instruction samples=3
  0x8c0cdddc  e248  mov #0x48,R2                               ; mapped instruction samples=0
  0x8c0cddde  51f5  mov.l @(0x14,R15),R1                       ; mapped instruction samples=1
  0x8c0cdde0  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0cdde2  8901  bt 0x8C0CDDE8                              ; mapped instruction samples=0
```

### 3. 0x8c03c750 (natural)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 67.0, "jit_self/register_load": 2.0, "jit_self/register_store": 10.0, "jit_self/unclassified": 17.0}`

Native leaf samples, inferred caller attribution: `{"VramLockedWriteOffset(unsigned long)": 1.0, "__mprotect": 22.0, "_sigtramp": 18.0, "addrspace::write64(unsigned int, unsigned long long)": 26.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 8549214, "allocator_stores_x_block_entries_upper_bound": 19948166}`

```text
block 0x8c03c750: entries/replay=2,849,738; pooled raw block samples=96
  0x8c03c750  f059  fmov.s @R5+,FR0                            ; mapped instruction samples=5
  0x8c03c752  4610  dt R6                                      ; mapped instruction samples=9
  0x8c03c754  f40a  fmov.s FR0,@R4                             ; mapped instruction samples=76
  0x8c03c756  8ffb  bf.s 0x8C03C750                            ; mapped instruction samples=0
  0x8c03c758  7408  add #8,R4                                  ; mapped instruction samples=0
```

### 4. 0x8c0b3112 (natural)

Exclusive sample categories: `{"jit_self/register_store": 11.0, "jit_self/unclassified": 70.0}`

Native leaf samples, inferred caller attribution: `{}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 3884814, "allocator_stores_x_block_entries_upper_bound": 13597956}`

```text
block 0x8c0b2d8c: entries/replay=1,941,504; pooled raw block samples=46
  0x8c0b2d8c  51f6  mov.l @(0x18,R15),R1                       ; mapped instruction samples=1
  0x8c0b2d8e  6312  mov.l @R1,R3                               ; mapped instruction samples=0
  0x8c0b2d90  1f34  mov.l R3,@(0x10,R15)                       ; mapped instruction samples=5
  0x8c0b2d92  52f6  mov.l @(0x18,R15),R2                       ; mapped instruction samples=3
  0x8c0b2d94  5321  mov.l @(0x4,R2),R3                         ; mapped instruction samples=5
  0x8c0b2d96  1f33  mov.l R3,@(0xC,R15)                        ; mapped instruction samples=1
  0x8c0b2d98  52f3  mov.l @(0xC,R15),R2                        ; mapped instruction samples=5
  0x8c0b2d9a  2228  tst R2,R2                                  ; mapped instruction samples=1
  0x8c0b2d9c  8b01  bf 0x8C0B2DA2                              ; mapped instruction samples=0
block 0x8c0b2da2: entries/replay=227; pooled raw block samples=0
  0x8c0b2da2  51f4  mov.l @(0x10,R15),R1                       ; mapped instruction samples=0
  0x8c0b2da4  902b  mov.w @(0x8C0B2DFE[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2da6  031d  mov.w @(R0,R1),R3                          ; mapped instruction samples=0
  0x8c0b2da8  633d  extu.w R3,R3                               ; mapped instruction samples=0
  0x8c0b2daa  9229  mov.w @(0x8C0B2E00[PC]),R2                 ; mapped instruction samples=0
  0x8c0b2dac  2329  and R2,R3                                  ; mapped instruction samples=0
  0x8c0b2dae  85fb  mov.w @(0x16,R15),R0                       ; mapped instruction samples=0
  0x8c0b2db0  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0b2db2  3300  cmp/eq R0,R3                               ; mapped instruction samples=0
  0x8c0b2db4  8901  bt 0x8C0B2DBA                              ; mapped instruction samples=0
block 0x8c0b2dba: entries/replay=227; pooled raw block samples=0
  0x8c0b2dba  53f3  mov.l @(0xC,R15),R3                        ; mapped instruction samples=0
  0x8c0b2dbc  8532  mov.w @(0x4,R3),R0                         ; mapped instruction samples=0
  0x8c0b2dbe  1f02  mov.l R0,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2dc0  53f3  mov.l @(0xC,R15),R3                        ; mapped instruction samples=0
  0x8c0b2dc2  8534  mov.w @(0x8,R3),R0                         ; mapped instruction samples=0
  0x8c0b2dc4  1f01  mov.l R0,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c0b2dc6  50f4  mov.l @(0x10,R15),R0                       ; mapped instruction samples=0
  0x8c0b2dc8  e124  mov #0x24,R1                               ; mapped instruction samples=0
  0x8c0b2dca  001c  mov.b @(R0,R1),R0                          ; mapped instruction samples=0
  0x8c0b2dcc  600c  extu.b R0,R0                               ; mapped instruction samples=0
  0x8c0b2dce  c801  tst #0x1,R0                                ; mapped instruction samples=0
  0x8c0b2dd0  8905  bt 0x8C0B2DDE                              ; mapped instruction samples=0
block 0x8c0b2dde: entries/replay=227; pooled raw block samples=0
  0x8c0b2dde  53f2  mov.l @(0x8,R15),R3                        ; mapped instruction samples=0
  0x8c0b2de0  4328  shll16 R3                                  ; mapped instruction samples=0
  0x8c0b2de2  1f32  mov.l R3,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2de4  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0b2de6  900c  mov.w @(0x8C0B2E02[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2de8  032e  mov.l @(R0,R2),R3                          ; mapped instruction samples=0
  0x8c0b2dea  5335  mov.l @(0x14,R3),R3                        ; mapped instruction samples=0
  0x8c0b2dec  51f2  mov.l @(0x8,R15),R1                        ; mapped instruction samples=0
  0x8c0b2dee  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0b2df0  1f12  mov.l R1,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2df2  52f9  mov.l @(0x24,R15),R2                       ; mapped instruction samples=0
  0x8c0b2df4  3218  sub R1,R2                                  ; mapped instruction samples=0
  0x8c0b2df6  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2df8  8904  bt 0x8C0B2E04                              ; mapped instruction samples=0
block 0x8c0b2e04: entries/replay=125; pooled raw block samples=0
  0x8c0b2e04  53f1  mov.l @(0x4,R15),R3                        ; mapped instruction samples=0
  0x8c0b2e06  4328  shll16 R3                                  ; mapped instruction samples=0
  0x8c0b2e08  52f2  mov.l @(0x8,R15),R2                        ; mapped instruction samples=0
  0x8c0b2e0a  323c  add R3,R2                                  ; mapped instruction samples=0
  0x8c0b2e0c  1f22  mov.l R2,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2e0e  53f9  mov.l @(0x24,R15),R3                       ; mapped instruction samples=0
  0x8c0b2e10  3328  sub R2,R3                                  ; mapped instruction samples=0
  0x8c0b2e12  4311  cmp/pz R3                                  ; mapped instruction samples=0
  0x8c0b2e14  8b01  bf 0x8C0B2E1A                              ; mapped instruction samples=0
block 0x8c0b2e1a: entries/replay=125; pooled raw block samples=0
  0x8c0b2e1a  53f3  mov.l @(0xC,R15),R3                        ; mapped instruction samples=0
  0x8c0b2e1c  8533  mov.w @(0x6,R3),R0                         ; mapped instruction samples=0
  0x8c0b2e1e  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0b2e20  1f01  mov.l R0,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c0b2e22  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=0
  0x8c0b2e24  9018  mov.w @(0x8C0B2E58[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2e26  023e  mov.l @(R0,R3),R2                          ; mapped instruction samples=0
  0x8c0b2e28  5326  mov.l @(0x18,R2),R3                        ; mapped instruction samples=0
  0x8c0b2e2a  51f1  mov.l @(0x4,R15),R1                        ; mapped instruction samples=0
  0x8c0b2e2c  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0b2e2e  1f11  mov.l R1,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c0b2e30  52f8  mov.l @(0x20,R15),R2                       ; mapped instruction samples=0
  0x8c0b2e32  3218  sub R1,R2                                  ; mapped instruction samples=0
  0x8c0b2e34  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2e36  8910  bt 0x8C0B2E5A                              ; mapped instruction samples=0
block 0x8c0b2e5a: entries/replay=125; pooled raw block samples=0
  0x8c0b2e5a  52f3  mov.l @(0xC,R15),R2                        ; mapped instruction samples=0
  0x8c0b2e5c  8525  mov.w @(0xA,R2),R0                         ; mapped instruction samples=0
  0x8c0b2e5e  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0b2e60  53f1  mov.l @(0x4,R15),R3                        ; mapped instruction samples=0
  0x8c0b2e62  330c  add R0,R3                                  ; mapped instruction samples=0
  0x8c0b2e64  1f31  mov.l R3,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c0b2e66  52f8  mov.l @(0x20,R15),R2                       ; mapped instruction samples=0
  0x8c0b2e68  3238  sub R3,R2                                  ; mapped instruction samples=0
  0x8c0b2e6a  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2e6c  8b10  bf 0x8C0B2E90                              ; mapped instruction samples=0
block 0x8c0b2e90: entries/replay=125; pooled raw block samples=0
  0x8c0b2e90  50f3  mov.l @(0xC,R15),R0                        ; mapped instruction samples=0
  0x8c0b2e92  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0b2e94  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0b2e96  8801  cmp/eq #0x1,R0                             ; mapped instruction samples=0
  0x8c0b2e98  8b2e  bf 0x8C0B2EF8                              ; mapped instruction samples=0
block 0x8c0b2e9a: entries/replay=125; pooled raw block samples=0
  0x8c0b2e9a  52f1  mov.l @(0x4,R15),R2                        ; mapped instruction samples=0
  0x8c0b2e9c  2f22  mov.l R2,@R15                              ; mapped instruction samples=0
  0x8c0b2e9e  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=0
  0x8c0b2ea0  9029  mov.w @(0x8C0B2EF6[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2ea2  023e  mov.l @(R0,R3),R2                          ; mapped instruction samples=0
  0x8c0b2ea4  5326  mov.l @(0x18,R2),R3                        ; mapped instruction samples=0
  0x8c0b2ea6  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0b2ea8  3138  sub R3,R1                                  ; mapped instruction samples=0
  0x8c0b2eaa  2f12  mov.l R1,@R15                              ; mapped instruction samples=0
  0x8c0b2eac  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0b2eae  9022  mov.w @(0x8C0B2EF6[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2eb0  032e  mov.l @(R0,R2),R3                          ; mapped instruction samples=0
  0x8c0b2eb2  5338  mov.l @(0x20,R3),R3                        ; mapped instruction samples=0
  0x8c0b2eb4  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0b2eb6  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0b2eb8  2f12  mov.l R1,@R15                              ; mapped instruction samples=0
  0x8c0b2eba  52fa  mov.l @(0x28,R15),R2                       ; mapped instruction samples=0
  0x8c0b2ebc  3218  sub R1,R2                                  ; mapped instruction samples=0
  0x8c0b2ebe  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2ec0  8b1a  bf 0x8C0B2EF8                              ; mapped instruction samples=0
block 0x8c0b2ef8: entries/replay=125; pooled raw block samples=0
  0x8c0b2ef8  50f3  mov.l @(0xC,R15),R0                        ; mapped instruction samples=0
  0x8c0b2efa  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0b2efc  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0b2efe  8802  cmp/eq #0x2,R0                             ; mapped instruction samples=0
  0x8c0b2f00  8b3a  bf 0x8C0B2F78                              ; mapped instruction samples=0
block 0x8c0b2f78: entries/replay=125; pooled raw block samples=0
  0x8c0b2f78  52f2  mov.l @(0x8,R15),R2                        ; mapped instruction samples=0
  0x8c0b2f7a  2f22  mov.l R2,@R15                              ; mapped instruction samples=0
  0x8c0b2f7c  53f4  mov.l @(0x10,R15),R3                       ; mapped instruction samples=0
  0x8c0b2f7e  901f  mov.w @(0x8C0B2FC0[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2f80  023e  mov.l @(R0,R3),R2                          ; mapped instruction samples=0
  0x8c0b2f82  5325  mov.l @(0x14,R2),R3                        ; mapped instruction samples=0
  0x8c0b2f84  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0b2f86  3138  sub R3,R1                                  ; mapped instruction samples=0
  0x8c0b2f88  2f12  mov.l R1,@R15                              ; mapped instruction samples=0
  0x8c0b2f8a  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0b2f8c  9018  mov.w @(0x8C0B2FC0[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2f8e  032e  mov.l @(R0,R2),R3                          ; mapped instruction samples=0
  0x8c0b2f90  5337  mov.l @(0x1C,R3),R3                        ; mapped instruction samples=0
  0x8c0b2f92  61f2  mov.l @R15,R1                              ; mapped instruction samples=0
  0x8c0b2f94  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0b2f96  2f12  mov.l R1,@R15                              ; mapped instruction samples=0
  0x8c0b2f98  52fb  mov.l @(0x2C,R15),R2                       ; mapped instruction samples=0
  0x8c0b2f9a  3218  sub R1,R2                                  ; mapped instruction samples=0
  0x8c0b2f9c  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2f9e  8b12  bf 0x8C0B2FC6                              ; mapped instruction samples=0
block 0x8c0b2fc6: entries/replay=125; pooled raw block samples=0
  0x8c0b2fc6  52f3  mov.l @(0xC,R15),R2                        ; mapped instruction samples=0
  0x8c0b2fc8  8524  mov.w @(0x8,R2),R0                         ; mapped instruction samples=0
  0x8c0b2fca  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0b2fcc  63f2  mov.l @R15,R3                              ; mapped instruction samples=0
  0x8c0b2fce  3308  sub R0,R3                                  ; mapped instruction samples=0
  0x8c0b2fd0  2f32  mov.l R3,@R15                              ; mapped instruction samples=0
  0x8c0b2fd2  52fb  mov.l @(0x2C,R15),R2                       ; mapped instruction samples=0
  0x8c0b2fd4  3238  sub R3,R2                                  ; mapped instruction samples=0
  0x8c0b2fd6  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0b2fd8  8919  bt 0x8C0B300E                              ; mapped instruction samples=0
block 0x8c0b2fda: entries/replay=125; pooled raw block samples=0
  0x8c0b2fda  52f4  mov.l @(0x10,R15),R2                       ; mapped instruction samples=0
  0x8c0b2fdc  9015  mov.w @(0x8C0B300A[PC]),R0                 ; mapped instruction samples=0
  0x8c0b2fde  9315  mov.w @(0x8C0B300C[PC]),R3                 ; mapped instruction samples=0
  0x8c0b2fe0  012d  mov.w @(R0,R2),R1                          ; mapped instruction samples=0
  0x8c0b2fe2  213b  or R3,R1                                   ; mapped instruction samples=0
  0x8c0b2fe4  0215  mov.w R1,@(R0,R2)                          ; mapped instruction samples=0
  0x8c0b2fe6  52f3  mov.l @(0xC,R15),R2                        ; mapped instruction samples=0
  0x8c0b2fe8  8524  mov.w @(0x8,R2),R0                         ; mapped instruction samples=0
  0x8c0b2fea  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0b2fec  52f2  mov.l @(0x8,R15),R2                        ; mapped instruction samples=0
  0x8c0b2fee  3208  sub R0,R2                                  ; mapped instruction samples=0
  0x8c0b2ff0  1f22  mov.l R2,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2ff2  51f2  mov.l @(0x8,R15),R1                        ; mapped instruction samples=0
  0x8c0b2ff4  71ff  add #-1,R1                                 ; mapped instruction samples=0
  0x8c0b2ff6  1f12  mov.l R1,@(0x8,R15)                        ; mapped instruction samples=0
  0x8c0b2ff8  1f19  mov.l R1,@(0x24,R15)                       ; mapped instruction samples=0
  0x8c0b2ffa  e015  mov #0x15,R0                               ; mapped instruction samples=0
  0x8c0b2ffc  00fc  mov.b @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0b2ffe  cb80  or #0x80,R0                                ; mapped instruction samples=0
  0x8c0b3000  62f3  mov R15,R2                                 ; mapped instruction samples=0
  0x8c0b3002  7215  add #21,R2                                 ; mapped instruction samples=0
  0x8c0b3004  2200  mov.b R0,@R2                               ; mapped instruction samples=0
  0x8c0b3006  a074  bra 0x8C0B30F2                             ; mapped instruction samples=0
  0x8c0b3008  0009  nop                                        ; mapped instruction samples=0
block 0x8c0b30f2: entries/replay=1,941,504; pooled raw block samples=22
  0x8c0b30f2  85fb  mov.w @(0x16,R15),R0                       ; mapped instruction samples=0
  0x8c0b30f4  7001  add #1,R0                                  ; mapped instruction samples=0
  0x8c0b30f6  81fb  mov.w R0,@(0x16,R15)                       ; mapped instruction samples=2
  0x8c0b30f8  53f6  mov.l @(0x18,R15),R3                       ; mapped instruction samples=3
  0x8c0b30fa  7308  add #8,R3                                  ; mapped instruction samples=0
  0x8c0b30fc  1f36  mov.l R3,@(0x18,R15)                       ; mapped instruction samples=4
  0x8c0b30fe  85fb  mov.w @(0x16,R15),R0                       ; mapped instruction samples=4
  0x8c0b3100  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0b3102  9305  mov.w @(0x8C0B3110[PC]),R3                 ; mapped instruction samples=0
  0x8c0b3104  3033  cmp/ge R3,R0                               ; mapped instruction samples=6
  0x8c0b3106  a004  bra 0x8C0B3112                             ; mapped instruction samples=0
  0x8c0b3108  0009  nop                                        ; mapped instruction samples=0
block 0x8c0b3112: entries/replay=1,941,978; pooled raw block samples=13
  0x8c0b3112  8901  bt 0x8C0B3118                              ; mapped instruction samples=0
```

### 5. 0x8c05e7c8 (natural)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 1.0, "jit_self/register_store": 12.0, "jit_self/unclassified": 39.0}`

Native leaf samples, inferred caller attribution: `{"unknown native leaf": 1.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 1222059, "allocator_stores_x_block_entries_upper_bound": 6110295}`

```text
block 0x8c05e7c8: entries/replay=1,222,059; pooled raw block samples=52
  0x8c05e7c8  9060  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e7ca  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=3
  0x8c05e7cc  4200  shll R2                                    ; mapped instruction samples=0
  0x8c05e7ce  935e  mov.w @(0x8C05E88E[PC]),R3                 ; mapped instruction samples=0
  0x8c05e7d0  33fc  add R15,R3                                 ; mapped instruction samples=0
  0x8c05e7d2  323c  add R3,R2                                  ; mapped instruction samples=0
  0x8c05e7d4  905a  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e7d6  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=7
  0x8c05e7d8  71ff  add #-1,R1                                 ; mapped instruction samples=0
  0x8c05e7da  4100  shll R1                                    ; mapped instruction samples=0
  0x8c05e7dc  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c05e7de  6111  mov.w @R1,R1                               ; mapped instruction samples=4
  0x8c05e7e0  6021  mov.w @R2,R0                               ; mapped instruction samples=2
  0x8c05e7e2  301c  add R1,R0                                  ; mapped instruction samples=0
  0x8c05e7e4  2201  mov.w R0,@R2                               ; mapped instruction samples=4
  0x8c05e7e6  9051  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e7e8  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=2
  0x8c05e7ea  4200  shll R2                                    ; mapped instruction samples=0
  0x8c05e7ec  61f3  mov R15,R1                                 ; mapped instruction samples=0
  0x8c05e7ee  321c  add R1,R2                                  ; mapped instruction samples=0
  0x8c05e7f0  904c  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e7f2  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=1
  0x8c05e7f4  70ff  add #-1,R0                                 ; mapped instruction samples=0
  0x8c05e7f6  4000  shll R0                                    ; mapped instruction samples=0
  0x8c05e7f8  001d  mov.w @(R0,R1),R0                          ; mapped instruction samples=4
  0x8c05e7fa  6321  mov.w @R2,R3                               ; mapped instruction samples=1
  0x8c05e7fc  330c  add R0,R3                                  ; mapped instruction samples=0
  0x8c05e7fe  2231  mov.w R3,@R2                               ; mapped instruction samples=3
  0x8c05e800  9044  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e802  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c05e804  7201  add #1,R2                                  ; mapped instruction samples=10
  0x8c05e806  9041  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e808  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c05e80a  9341  mov.w @(0x8C05E890[PC]),R3                 ; mapped instruction samples=1
  0x8c05e80c  903e  mov.w @(0x8C05E88C[PC]),R0                 ; mapped instruction samples=0
  0x8c05e80e  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=3
  0x8c05e810  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c05e812  8bd9  bf 0x8C05E7C8                              ; mapped instruction samples=0
```

### 6. 0x8c01255c (natural)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 20.0, "jit_self/register_load": 3.0, "jit_self/register_store": 5.0, "jit_self/unclassified": 14.0}`

Native leaf samples, inferred caller attribution: `{"WriteMemBlock_nommu_sq(unsigned int, SQBuffer const*)": 2.0, "mmu_TranslateSQW(unsigned int, unsigned int*)": 3.0, "setSqwHandler()": 6.0, "unknown native leaf": 9.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 4317630, "allocator_stores_x_block_entries_upper_bound": 4317630}`

```text
block 0x8c01254a: entries/replay=431,763; pooled raw block samples=11
  0x8c01254a  6426  mov.l @R2+,R4                              ; mapped instruction samples=1
  0x8c01254c  6526  mov.l @R2+,R5                              ; mapped instruction samples=0
  0x8c01254e  6626  mov.l @R2+,R6                              ; mapped instruction samples=1
  0x8c012550  6726  mov.l @R2+,R7                              ; mapped instruction samples=4
  0x8c012552  6826  mov.l @R2+,R8                              ; mapped instruction samples=1
  0x8c012554  6926  mov.l @R2+,R9                              ; mapped instruction samples=3
  0x8c012556  0283  pref @R2                                   ; mapped instruction samples=0
  0x8c012558  4110  dt R1                                      ; mapped instruction samples=1
  0x8c01255a  8911  bt 0x8C012580                              ; mapped instruction samples=0
block 0x8c01255c: entries/replay=431,763; pooled raw block samples=31
  0x8c01255c  2096  mov.l R9,@-R0                              ; mapped instruction samples=3
  0x8c01255e  2086  mov.l R8,@-R0                              ; mapped instruction samples=2
  0x8c012560  2076  mov.l R7,@-R0                              ; mapped instruction samples=1
  0x8c012562  2066  mov.l R6,@-R0                              ; mapped instruction samples=3
  0x8c012564  2056  mov.l R5,@-R0                              ; mapped instruction samples=3
  0x8c012566  2046  mov.l R4,@-R0                              ; mapped instruction samples=1
  0x8c012568  2036  mov.l R3,@-R0                              ; mapped instruction samples=4
  0x8c01256a  0083  pref @R0                                   ; mapped instruction samples=11
  0x8c01256c  afed  bra 0x8C01254A                             ; mapped instruction samples=0
  0x8c01256e  703c  add #60,R0                                 ; mapped instruction samples=0
```

### 7. 0x8c0d60e6 (natural)

Exclusive sample categories: `{"callee_via_jit_return/unclassified": 1.0, "jit_self/register_store": 6.0, "jit_self/unclassified": 35.0}`

Native leaf samples, inferred caller attribution: `{"unknown native leaf": 1.0}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 3719549, "allocator_stores_x_block_entries_upper_bound": 11291965}`

```text
block 0x8c0d60e6: entries/replay=764,160; pooled raw block samples=4
  0x8c0d60e6  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d60e8  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0d60ea  2118  tst R1,R1                                  ; mapped instruction samples=0
  0x8c0d60ec  8903  bt 0x8C0D60F6                              ; mapped instruction samples=0
block 0x8c0d60ee: entries/replay=740,280; pooled raw block samples=4
  0x8c0d60ee  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d60f0  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d60f2  881f  cmp/eq #0x1F,R0                            ; mapped instruction samples=0
  0x8c0d60f4  8b01  bf 0x8C0D60FA                              ; mapped instruction samples=0
block 0x8c0d60fa: entries/replay=716,400; pooled raw block samples=6
  0x8c0d60fa  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d60fc  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=2
  0x8c0d60fe  6231  mov.w @R3,R2                               ; mapped instruction samples=0
  0x8c0d6100  622d  extu.w R2,R2                               ; mapped instruction samples=0
  0x8c0d6102  9331  mov.w @(0x8C0D6168[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6104  2239  and R3,R2                                  ; mapped instruction samples=0
  0x8c0d6106  9130  mov.w @(0x8C0D616A[PC]),R1                 ; mapped instruction samples=0
  0x8c0d6108  3210  cmp/eq R1,R2                               ; mapped instruction samples=0
  0x8c0d610a  8907  bt 0x8C0D611C                              ; mapped instruction samples=0
block 0x8c0d610c: entries/replay=716,400; pooled raw block samples=7
  0x8c0d610c  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d610e  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=2
  0x8c0d6110  6201  mov.w @R0,R2                               ; mapped instruction samples=0
  0x8c0d6112  622d  extu.w R2,R2                               ; mapped instruction samples=0
  0x8c0d6114  2239  and R3,R2                                  ; mapped instruction samples=0
  0x8c0d6116  9029  mov.w @(0x8C0D616C[PC]),R0                 ; mapped instruction samples=1
  0x8c0d6118  3200  cmp/eq R0,R2                               ; mapped instruction samples=0
  0x8c0d611a  8b01  bf 0x8C0D6120                              ; mapped instruction samples=0
block 0x8c0d6120: entries/replay=45,275; pooled raw block samples=1
  0x8c0d6120  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6122  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0d6124  6321  mov.w @R2,R3                               ; mapped instruction samples=0
  0x8c0d6126  633d  extu.w R3,R3                               ; mapped instruction samples=0
  0x8c0d6128  d212  mov.l @(0x8C0D6174[PC]),R2                 ; mapped instruction samples=0
  0x8c0d612a  2329  and R2,R3                                  ; mapped instruction samples=0
  0x8c0d612c  911f  mov.w @(0x8C0D616E[PC]),R1                 ; mapped instruction samples=0
  0x8c0d612e  3310  cmp/eq R1,R3                               ; mapped instruction samples=0
  0x8c0d6130  8906  bt 0x8C0D6140                              ; mapped instruction samples=0
block 0x8c0d6132: entries/replay=45,275; pooled raw block samples=0
  0x8c0d6132  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6134  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d6136  6301  mov.w @R0,R3                               ; mapped instruction samples=0
  0x8c0d6138  633d  extu.w R3,R3                               ; mapped instruction samples=0
  0x8c0d613a  9015  mov.w @(0x8C0D6168[PC]),R0                 ; mapped instruction samples=0
  0x8c0d613c  2308  tst R0,R3                                  ; mapped instruction samples=0
  0x8c0d613e  8b23  bf 0x8C0D6188                              ; mapped instruction samples=0
block 0x8c0d6188: entries/replay=45,275; pooled raw block samples=4
  0x8c0d6188  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d618a  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d618c  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0d618e  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0d6190  4019  shlr8 R0                                   ; mapped instruction samples=0
  0x8c0d6192  c90f  and #0xF,R0                                ; mapped instruction samples=0
  0x8c0d6194  d134  mov.l @(0x8C0D6268[PC]),R1                 ; mapped instruction samples=0
  0x8c0d6196  021c  mov.b @(R0,R1),R2                          ; mapped instruction samples=0
  0x8c0d6198  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d619a  0f24  mov.b R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d619c  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d619e  03fc  mov.b @(R0,R15),R3                         ; mapped instruction samples=1
  0x8c0d61a0  633c  extu.b R3,R3                               ; mapped instruction samples=0
  0x8c0d61a2  925a  mov.w @(0x8C0D625A[PC]),R2                 ; mapped instruction samples=1
  0x8c0d61a4  3320  cmp/eq R2,R3                               ; mapped instruction samples=1
  0x8c0d61a6  8b01  bf 0x8C0D61AC                              ; mapped instruction samples=0
block 0x8c0d61ac: entries/replay=45,275; pooled raw block samples=1
  0x8c0d61ac  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d61ae  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d61b0  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0d61b2  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0d61b4  c90f  and #0xF,R0                                ; mapped instruction samples=0
  0x8c0d61b6  405a  lds R0,FPUL                                ; mapped instruction samples=0
  0x8c0d61b8  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0d61ba  c72c  mova @(0x8C0D626C[PC]),R0                  ; mapped instruction samples=0
  0x8c0d61bc  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0d61be  c72c  mova @(0x8C0D6270[PC]),R0                  ; mapped instruction samples=0
  0x8c0d61c0  f008  fmov.s @R0,FR0                             ; mapped instruction samples=1
  0x8c0d61c2  f23e  fmac FR0,FR3,FR2                           ; mapped instruction samples=0
  0x8c0d61c4  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0d61c6  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d61c8  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d61ca  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d61cc  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0d61ce  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0d61d0  c9f0  and #0xF0,R0                               ; mapped instruction samples=0
  0x8c0d61d2  e3fc  mov #0xFFFFFFFC,R3                         ; mapped instruction samples=0
  0x8c0d61d4  403c  shad R3,R0                                 ; mapped instruction samples=0
  0x8c0d61d6  405a  lds R0,FPUL                                ; mapped instruction samples=0
  0x8c0d61d8  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0d61da  f10c  fmov FR0,FR1                               ; mapped instruction samples=0
  0x8c0d61dc  c723  mova @(0x8C0D626C[PC]),R0                  ; mapped instruction samples=0
  0x8c0d61de  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0d61e0  f01c  fmov FR1,FR0                               ; mapped instruction samples=0
  0x8c0d61e2  f23e  fmac FR0,FR3,FR2                           ; mapped instruction samples=0
  0x8c0d61e4  e060  mov #0x60,R0                               ; mapped instruction samples=0
  0x8c0d61e6  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d61e8  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d61ea  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0d61ec  6001  mov.w @R0,R0                               ; mapped instruction samples=0
  0x8c0d61ee  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0d61f0  d320  mov.l @(0x8C0D6274[PC]),R3                 ; mapped instruction samples=0
  0x8c0d61f2  2039  and R3,R0                                  ; mapped instruction samples=0
  0x8c0d61f4  d120  mov.l @(0x8C0D6278[PC]),R1                 ; mapped instruction samples=0
  0x8c0d61f6  3010  cmp/eq R1,R0                               ; mapped instruction samples=0
  0x8c0d61f8  8915  bt 0x8C0D6226                              ; mapped instruction samples=0
block 0x8c0d61fa: entries/replay=40,499; pooled raw block samples=2
  0x8c0d61fa  912f  mov.w @(0x8C0D625C[PC]),R1                 ; mapped instruction samples=0
  0x8c0d61fc  3010  cmp/eq R1,R0                               ; mapped instruction samples=0
  0x8c0d61fe  8b01  bf 0x8C0D6204                              ; mapped instruction samples=0
block 0x8c0d6204: entries/replay=40,499; pooled raw block samples=0
  0x8c0d6204  912b  mov.w @(0x8C0D625E[PC]),R1                 ; mapped instruction samples=0
  0x8c0d6206  3010  cmp/eq R1,R0                               ; mapped instruction samples=0
  0x8c0d6208  8b01  bf 0x8C0D620E                              ; mapped instruction samples=0
block 0x8c0d620e: entries/replay=36,917; pooled raw block samples=0
  0x8c0d620e  9127  mov.w @(0x8C0D6260[PC]),R1                 ; mapped instruction samples=0
  0x8c0d6210  3010  cmp/eq R1,R0                               ; mapped instruction samples=0
  0x8c0d6212  8b01  bf 0x8C0D6218                              ; mapped instruction samples=0
block 0x8c0d6218: entries/replay=32,738; pooled raw block samples=0
  0x8c0d6218  9123  mov.w @(0x8C0D6262[PC]),R1                 ; mapped instruction samples=0
  0x8c0d621a  3010  cmp/eq R1,R0                               ; mapped instruction samples=0
  0x8c0d621c  8b01  bf 0x8C0D6222                              ; mapped instruction samples=0
block 0x8c0d6226: entries/replay=4,776; pooled raw block samples=0
  0x8c0d6226  d115  mov.l @(0x8C0D627C[PC]),R1                 ; mapped instruction samples=0
  0x8c0d6228  6211  mov.w @R1,R2                               ; mapped instruction samples=0
  0x8c0d622a  622d  extu.w R2,R2                               ; mapped instruction samples=0
  0x8c0d622c  901a  mov.w @(0x8C0D6264[PC]),R0                 ; mapped instruction samples=0
  0x8c0d622e  3203  cmp/ge R0,R2                               ; mapped instruction samples=0
  0x8c0d6230  8b01  bf 0x8C0D6236                              ; mapped instruction samples=0
block 0x8c0d63c4: entries/replay=4,776; pooled raw block samples=0
  0x8c0d63c4  d347  mov.l @(0x8C0D64E4[PC]),R3                 ; mapped instruction samples=0
  0x8c0d63c6  6232  mov.l @R3,R2                               ; mapped instruction samples=0
  0x8c0d63c8  9088  mov.w @(0x8C0D64DC[PC]),R0                 ; mapped instruction samples=0
  0x8c0d63ca  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d63cc  6132  mov.l @R3,R1                               ; mapped instruction samples=0
  0x8c0d63ce  9086  mov.w @(0x8C0D64DE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d63d0  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d63d2  a241  bra 0x8C0D6858                             ; mapped instruction samples=0
  0x8c0d63d4  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d64f8: entries/replay=3,582; pooled raw block samples=0
  0x8c0d64f8  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d64fa  02fc  mov.b @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0d64fc  2228  tst R2,R2                                  ; mapped instruction samples=0
  0x8c0d64fe  8b02  bf 0x8C0D6506                              ; mapped instruction samples=0
block 0x8c0d6500: entries/replay=3,582; pooled raw block samples=1
  0x8c0d6500  e207  mov #0x7,R2                                ; mapped instruction samples=0
  0x8c0d6502  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d6504  0f24  mov.b R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6506  d340  mov.l @(0x8C0D6608[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6508  d240  mov.l @(0x8C0D660C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d650a  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d650c  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d650e  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d6510  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d6512  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6514  6213  mov R1,R2                                  ; mapped instruction samples=0
  0x8c0d6516  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d6518  3128  sub R2,R1                                  ; mapped instruction samples=0
  0x8c0d651a  9070  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d651c  d23c  mov.l @(0x8C0D6610[PC]),R2                 ; mapped instruction samples=0
  0x8c0d651e  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d6520  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6522: entries/replay=3,582; pooled raw block samples=1
  0x8c0d6522  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d6524  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d6526  6803  mov R0,R8                                  ; mapped instruction samples=0
  0x8c0d6528  d23a  mov.l @(0x8C0D6614[PC]),R2                 ; mapped instruction samples=0
  0x8c0d652a  d038  mov.l @(0x8C0D660C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d652c  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d652e  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d6530  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d6532  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6534  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0d6536  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d6538  3138  sub R3,R1                                  ; mapped instruction samples=0
  0x8c0d653a  9060  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d653c  d334  mov.l @(0x8C0D6610[PC]),R3                 ; mapped instruction samples=0
  0x8c0d653e  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6540  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6542: entries/replay=3,582; pooled raw block samples=0
  0x8c0d6542  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d6544  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d6546  d334  mov.l @(0x8C0D6618[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6548  d030  mov.l @(0x8C0D660C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d654a  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d654c  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d654e  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d6550  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6552  6013  mov R1,R0                                  ; mapped instruction samples=0
  0x8c0d6554  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d6556  3108  sub R0,R1                                  ; mapped instruction samples=0
  0x8c0d6558  9051  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d655a  d32d  mov.l @(0x8C0D6610[PC]),R3                 ; mapped instruction samples=0
  0x8c0d655c  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d655e  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6560: entries/replay=3,582; pooled raw block samples=0
  0x8c0d6560  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d6562  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d6564  d329  mov.l @(0x8C0D660C[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6566  6130  mov.b @R3,R1                               ; mapped instruction samples=0
  0x8c0d6568  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d656a  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d656c  6013  mov R1,R0                                  ; mapped instruction samples=0
  0x8c0d656e  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d6570  3108  sub R0,R1                                  ; mapped instruction samples=0
  0x8c0d6572  9044  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6574  d326  mov.l @(0x8C0D6610[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6576  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6578  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d657a: entries/replay=3,582; pooled raw block samples=0
  0x8c0d657a  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d657c  9040  mov.w @(0x8C0D6600[PC]),R0                 ; mapped instruction samples=0
  0x8c0d657e  0f86  mov.l R8,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6580  d321  mov.l @(0x8C0D6608[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6582  d022  mov.l @(0x8C0D660C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6584  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d6586  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d6588  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d658a  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d658c  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d658e  6213  mov R1,R2                                  ; mapped instruction samples=0
  0x8c0d6590  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d6592  3128  sub R2,R1                                  ; mapped instruction samples=0
  0x8c0d6594  9033  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6596  d21e  mov.l @(0x8C0D6610[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6598  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d659a  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d659c: entries/replay=3,582; pooled raw block samples=0
  0x8c0d659c  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d659e  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d65a0  6803  mov R0,R8                                  ; mapped instruction samples=0
  0x8c0d65a2  d21c  mov.l @(0x8C0D6614[PC]),R2                 ; mapped instruction samples=0
  0x8c0d65a4  d019  mov.l @(0x8C0D660C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65a6  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d65a8  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d65aa  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d65ac  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d65ae  9028  mov.w @(0x8C0D6602[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65b0  210e  mulu.w R0,R1                               ; mapped instruction samples=0
  0x8c0d65b2  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d65b4  9023  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65b6  d316  mov.l @(0x8C0D6610[PC]),R3                 ; mapped instruction samples=0
  0x8c0d65b8  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d65ba  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d65bc: entries/replay=3,582; pooled raw block samples=0
  0x8c0d65bc  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d65be  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d65c0  d315  mov.l @(0x8C0D6618[PC]),R3                 ; mapped instruction samples=0
  0x8c0d65c2  d012  mov.l @(0x8C0D660C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65c4  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d65c6  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d65c8  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d65ca  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d65cc  901a  mov.w @(0x8C0D6604[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65ce  210e  mulu.w R0,R1                               ; mapped instruction samples=0
  0x8c0d65d0  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d65d2  9014  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65d4  d30e  mov.l @(0x8C0D6610[PC]),R3                 ; mapped instruction samples=0
  0x8c0d65d6  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d65d8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d65da: entries/replay=3,582; pooled raw block samples=0
  0x8c0d65da  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d65dc  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d65de  d30b  mov.l @(0x8C0D660C[PC]),R3                 ; mapped instruction samples=0
  0x8c0d65e0  6130  mov.b @R3,R1                               ; mapped instruction samples=0
  0x8c0d65e2  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d65e4  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d65e6  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d65e8  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d65ea  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d65ec  9007  mov.w @(0x8C0D65FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65ee  d208  mov.l @(0x8C0D6610[PC]),R2                 ; mapped instruction samples=0
  0x8c0d65f0  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d65f2  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d65f4: entries/replay=3,582; pooled raw block samples=0
  0x8c0d65f4  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d65f6  9006  mov.w @(0x8C0D6606[PC]),R0                 ; mapped instruction samples=0
  0x8c0d65f8  0f86  mov.l R8,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d65fa  a12d  bra 0x8C0D6858                             ; mapped instruction samples=0
  0x8c0d65fc  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d661c: entries/replay=4,179; pooled raw block samples=0
  0x8c0d661c  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d661e  01fc  mov.b @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0d6620  2118  tst R1,R1                                  ; mapped instruction samples=0
  0x8c0d6622  8b02  bf 0x8C0D662A                              ; mapped instruction samples=0
block 0x8c0d6624: entries/replay=4,179; pooled raw block samples=0
  0x8c0d6624  e207  mov #0x7,R2                                ; mapped instruction samples=0
  0x8c0d6626  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d6628  0f24  mov.b R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d662a  d33a  mov.l @(0x8C0D6714[PC]),R3                 ; mapped instruction samples=0
  0x8c0d662c  d23a  mov.l @(0x8C0D6718[PC]),R2                 ; mapped instruction samples=0
  0x8c0d662e  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6630  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d6632  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d6634  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d6636  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6638  6213  mov R1,R2                                  ; mapped instruction samples=0
  0x8c0d663a  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d663c  3128  sub R2,R1                                  ; mapped instruction samples=0
  0x8c0d663e  9062  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6640  d236  mov.l @(0x8C0D671C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6642  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d6644  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6646: entries/replay=4,179; pooled raw block samples=1
  0x8c0d6646  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d6648  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d664a  6803  mov R0,R8                                  ; mapped instruction samples=0
  0x8c0d664c  d234  mov.l @(0x8C0D6720[PC]),R2                 ; mapped instruction samples=0
  0x8c0d664e  d032  mov.l @(0x8C0D6718[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6650  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d6652  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d6654  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d6656  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6658  9056  mov.w @(0x8C0D6708[PC]),R0                 ; mapped instruction samples=0
  0x8c0d665a  210e  mulu.w R0,R1                               ; mapped instruction samples=0
  0x8c0d665c  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d665e  9052  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6660  d32e  mov.l @(0x8C0D671C[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6662  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6664  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6666: entries/replay=4,179; pooled raw block samples=2
  0x8c0d6666  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d6668  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d666a  d32e  mov.l @(0x8C0D6724[PC]),R3                 ; mapped instruction samples=0
  0x8c0d666c  d02a  mov.l @(0x8C0D6718[PC]),R0                 ; mapped instruction samples=0
  0x8c0d666e  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0d6670  2139  and R3,R1                                  ; mapped instruction samples=0
  0x8c0d6672  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d6674  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6676  9048  mov.w @(0x8C0D670A[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6678  210e  mulu.w R0,R1                               ; mapped instruction samples=1
  0x8c0d667a  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d667c  9043  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d667e  d327  mov.l @(0x8C0D671C[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6680  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6682  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6684: entries/replay=4,179; pooled raw block samples=0
  0x8c0d6684  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d6686  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d6688  d323  mov.l @(0x8C0D6718[PC]),R3                 ; mapped instruction samples=0
  0x8c0d668a  6130  mov.b @R3,R1                               ; mapped instruction samples=0
  0x8c0d668c  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d668e  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d6690  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6692  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6694  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6696  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6698  9035  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d669a  d220  mov.l @(0x8C0D671C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d669c  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d669e  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d66a0: entries/replay=4,179; pooled raw block samples=0
  0x8c0d66a0  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d66a2  9033  mov.w @(0x8C0D670C[PC]),R0                 ; mapped instruction samples=0
  0x8c0d66a4  0f86  mov.l R8,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d66a6  d21b  mov.l @(0x8C0D6714[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66a8  6132  mov.l @R3,R1                               ; mapped instruction samples=0
  0x8c0d66aa  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d66ac  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d66ae  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d66b0  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d66b2  6213  mov R1,R2                                  ; mapped instruction samples=0
  0x8c0d66b4  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0d66b6  3128  sub R2,R1                                  ; mapped instruction samples=0
  0x8c0d66b8  9025  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d66ba  d218  mov.l @(0x8C0D671C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66bc  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d66be  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d66c0: entries/replay=4,179; pooled raw block samples=0
  0x8c0d66c0  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d66c2  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d66c4  6803  mov R0,R8                                  ; mapped instruction samples=0
  0x8c0d66c6  d216  mov.l @(0x8C0D6720[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66c8  6132  mov.l @R3,R1                               ; mapped instruction samples=0
  0x8c0d66ca  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d66cc  4129  shlr16 R1                                  ; mapped instruction samples=0
  0x8c0d66ce  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d66d0  921d  mov.w @(0x8C0D670E[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66d2  212e  mulu.w R2,R1                               ; mapped instruction samples=0
  0x8c0d66d4  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d66d6  9016  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d66d8  d210  mov.l @(0x8C0D671C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66da  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d66dc  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d66de: entries/replay=4,179; pooled raw block samples=0
  0x8c0d66de  4028  shll16 R0                                  ; mapped instruction samples=0
  0x8c0d66e0  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d66e2  d210  mov.l @(0x8C0D6724[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66e4  6132  mov.l @R3,R1                               ; mapped instruction samples=0
  0x8c0d66e6  2129  and R2,R1                                  ; mapped instruction samples=0
  0x8c0d66e8  4119  shlr8 R1                                   ; mapped instruction samples=0
  0x8c0d66ea  611c  extu.b R1,R1                               ; mapped instruction samples=0
  0x8c0d66ec  e270  mov #0x70,R2                               ; mapped instruction samples=0
  0x8c0d66ee  212e  mulu.w R2,R1                               ; mapped instruction samples=0
  0x8c0d66f0  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0d66f2  9008  mov.w @(0x8C0D6706[PC]),R0                 ; mapped instruction samples=0
  0x8c0d66f4  d209  mov.l @(0x8C0D671C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d66f6  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c0d66f8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d66fa: entries/replay=4,179; pooled raw block samples=0
  0x8c0d66fa  4018  shll8 R0                                   ; mapped instruction samples=0
  0x8c0d66fc  280b  or R0,R8                                   ; mapped instruction samples=0
  0x8c0d66fe  9007  mov.w @(0x8C0D6710[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6700  0f86  mov.l R8,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6702  a0a9  bra 0x8C0D6858                             ; mapped instruction samples=0
  0x8c0d6704  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d682c: entries/replay=32,738; pooled raw block samples=0
  0x8c0d682c  d206  mov.l @(0x8C0D6848[PC]),R2                 ; mapped instruction samples=0
  0x8c0d682e  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6830  9006  mov.w @(0x8C0D6840[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6832  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6834  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6836  9004  mov.w @(0x8C0D6842[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6838  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d683a  a00d  bra 0x8C0D6858                             ; mapped instruction samples=0
  0x8c0d683c  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6858: entries/replay=45,275; pooled raw block samples=5
  0x8c0d6858  d22c  mov.l @(0x8C0D690C[PC]),R2                 ; mapped instruction samples=0
  0x8c0d685a  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c0d685c  e05b  mov #0x5B,R0                               ; mapped instruction samples=0
  0x8c0d685e  01fc  mov.b @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0d6860  d02b  mov.l @(0x8C0D6910[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6862  0314  mov.b R1,@(R0,R3)                          ; mapped instruction samples=0
  0x8c0d6864  9045  mov.w @(0x8C0D68F2[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6866  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0d6868  9044  mov.w @(0x8C0D68F4[PC]),R0                 ; mapped instruction samples=0
  0x8c0d686a  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=1
  0x8c0d686c  9043  mov.w @(0x8C0D68F6[PC]),R0                 ; mapped instruction samples=0
  0x8c0d686e  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0d6870  9042  mov.w @(0x8C0D68F8[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6872  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6874  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0d6876  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0d6878  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0d687a  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0d687c  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0d687e  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0d6880  e070  mov #0x70,R0                               ; mapped instruction samples=0
  0x8c0d6882  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d6884  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6886  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0d6888  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0d688a  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0d688c  73f0  add #-16,R3                                ; mapped instruction samples=0
  0x8c0d688e  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0d6890  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0d6892  e074  mov #0x74,R0                               ; mapped instruction samples=0
  0x8c0d6894  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d6896  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0d6898  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d689a  e07c  mov #0x7C,R0                               ; mapped instruction samples=0
  0x8c0d689c  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d689e  e060  mov #0x60,R0                               ; mapped instruction samples=0
  0x8c0d68a0  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d68a2  902a  mov.w @(0x8C0D68FA[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68a4  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=1
  0x8c0d68a6  e070  mov #0x70,R0                               ; mapped instruction samples=0
  0x8c0d68a8  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d68aa  9027  mov.w @(0x8C0D68FC[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68ac  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68ae  c719  mova @(0x8C0D6914[PC]),R0                  ; mapped instruction samples=0
  0x8c0d68b0  f308  fmov.s @R0,FR3                             ; mapped instruction samples=0
  0x8c0d68b2  e074  mov #0x74,R0                               ; mapped instruction samples=0
  0x8c0d68b4  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=1
  0x8c0d68b6  f230  fadd FR3,FR2                               ; mapped instruction samples=0
  0x8c0d68b8  9021  mov.w @(0x8C0D68FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68ba  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68bc  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0d68be  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0d68c0  901e  mov.w @(0x8C0D6900[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68c2  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68c4  c714  mova @(0x8C0D6918[PC]),R0                  ; mapped instruction samples=0
  0x8c0d68c6  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0d68c8  e060  mov #0x60,R0                               ; mapped instruction samples=0
  0x8c0d68ca  f1f6  fmov.s @(R0,R15),FR1                       ; mapped instruction samples=0
  0x8c0d68cc  f120  fadd FR2,FR1                               ; mapped instruction samples=0
  0x8c0d68ce  9018  mov.w @(0x8C0D6902[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68d0  ff17  fmov.s FR1,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68d2  e070  mov #0x70,R0                               ; mapped instruction samples=0
  0x8c0d68d4  f1f6  fmov.s @(R0,R15),FR1                       ; mapped instruction samples=0
  0x8c0d68d6  f130  fadd FR3,FR1                               ; mapped instruction samples=0
  0x8c0d68d8  9014  mov.w @(0x8C0D6904[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68da  ff17  fmov.s FR1,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68dc  e074  mov #0x74,R0                               ; mapped instruction samples=0
  0x8c0d68de  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=1
  0x8c0d68e0  9011  mov.w @(0x8C0D6906[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68e2  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d68e4  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0d68e6  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d68e8  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0d68ea  900d  mov.w @(0x8C0D6908[PC]),R0                 ; mapped instruction samples=0
  0x8c0d68ec  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=1
  0x8c0d68ee  a015  bra 0x8C0D691C                             ; mapped instruction samples=0
  0x8c0d68f0  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d691c: entries/replay=45,275; pooled raw block samples=1
  0x8c0d691c  e060  mov #0x60,R0                               ; mapped instruction samples=0
  0x8c0d691e  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d6920  9068  mov.w @(0x8C0D69F4[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6922  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d6924  9067  mov.w @(0x8C0D69F6[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6926  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d6928  9066  mov.w @(0x8C0D69F8[PC]),R0                 ; mapped instruction samples=0
  0x8c0d692a  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d692c  9065  mov.w @(0x8C0D69FA[PC]),R0                 ; mapped instruction samples=0
  0x8c0d692e  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0d6930  9064  mov.w @(0x8C0D69FC[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6932  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d6934  c733  mova @(0x8C0D6A04[PC]),R0                  ; mapped instruction samples=0
  0x8c0d6936  f308  fmov.s @R0,FR3                             ; mapped instruction samples=0
  0x8c0d6938  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0d693a  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0d693c  f230  fadd FR3,FR2                               ; mapped instruction samples=0
  0x8c0d693e  905e  mov.w @(0x8C0D69FE[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6940  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d6942  e060  mov #0x60,R0                               ; mapped instruction samples=0
  0x8c0d6944  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0d6946  f230  fadd FR3,FR2                               ; mapped instruction samples=0
  0x8c0d6948  905a  mov.w @(0x8C0D6A00[PC]),R0                 ; mapped instruction samples=0
  0x8c0d694a  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0d694c  f39d  fldi1 FR3                                  ; mapped instruction samples=1
  0x8c0d694e  d32e  mov.l @(0x8C0D6A08[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6950  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0d6952  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0d6954  8939  bt 0x8C0D69CA                              ; mapped instruction samples=0
block 0x8c0d69ca: entries/replay=45,275; pooled raw block samples=0
  0x8c0d69ca  f39d  fldi1 FR3                                  ; mapped instruction samples=0
  0x8c0d69cc  d310  mov.l @(0x8C0D6A10[PC]),R3                 ; mapped instruction samples=0
  0x8c0d69ce  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0d69d0  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0d69d2  894d  bt 0x8C0D6A70                              ; mapped instruction samples=0
block 0x8c0d6a70: entries/replay=45,275; pooled raw block samples=0
  0x8c0d6a70  d321  mov.l @(0x8C0D6AF8[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6a72  f338  fmov.s @R3,FR3                             ; mapped instruction samples=0
  0x8c0d6a74  f28d  fldi0 FR2                                  ; mapped instruction samples=0
  0x8c0d6a76  f324  fcmp/eq FR2,FR3                            ; mapped instruction samples=0
  0x8c0d6a78  8b01  bf 0x8C0D6A7E                              ; mapped instruction samples=0
block 0x8c0d6bf2: entries/replay=45,275; pooled raw block samples=0
  0x8c0d6bf2  d238  mov.l @(0x8C0D6CD4[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6bf4  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6bf6  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0d6bf8  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6bfa  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0d6bfc  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6bfe  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c00  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c02  d035  mov.l @(0x8C0D6CD8[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6c04  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0d6c06  62f3  mov R15,R2                                 ; mapped instruction samples=0
  0x8c0d6c08  7270  add #112,R2                                ; mapped instruction samples=0
  0x8c0d6c0a  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0d6c0c  d333  mov.l @(0x8C0D6CDC[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6c0e  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6c10  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6c12: entries/replay=45,275; pooled raw block samples=0
  0x8c0d6c12  d230  mov.l @(0x8C0D6CD4[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c14  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6c16  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0d6c18  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c1a  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0d6c1c  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c1e  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c20  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c22  d02f  mov.l @(0x8C0D6CE0[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6c24  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0d6c26  9250  mov.w @(0x8C0D6CCA[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c28  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0d6c2a  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0d6c2c  d32b  mov.l @(0x8C0D6CDC[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6c2e  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6c30  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6c32: entries/replay=45,275; pooled raw block samples=3
  0x8c0d6c32  d228  mov.l @(0x8C0D6CD4[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c34  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0d6c36  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0d6c38  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c3a  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0d6c3c  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c3e  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c40  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c42  d028  mov.l @(0x8C0D6CE4[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6c44  310c  add R0,R1                                  ; mapped instruction samples=1
  0x8c0d6c46  9241  mov.w @(0x8C0D6CCC[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c48  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0d6c4a  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0d6c4c  d323  mov.l @(0x8C0D6CDC[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6c4e  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6c50  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6c52: entries/replay=45,275; pooled raw block samples=2
  0x8c0d6c52  d220  mov.l @(0x8C0D6CD4[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c54  6122  mov.l @R2,R1                               ; mapped instruction samples=1
  0x8c0d6c56  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0d6c58  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c5a  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0d6c5c  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c5e  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0d6c60  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0d6c62  d021  mov.l @(0x8C0D6CE8[PC]),R0                 ; mapped instruction samples=0
  0x8c0d6c64  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0d6c66  922e  mov.w @(0x8C0D6CC6[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c68  32fc  add R15,R2                                 ; mapped instruction samples=1
  0x8c0d6c6a  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0d6c6c  d31b  mov.l @(0x8C0D6CDC[PC]),R3                 ; mapped instruction samples=0
  0x8c0d6c6e  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0d6c70  0009  nop                                        ; mapped instruction samples=0
block 0x8c0d6c72: entries/replay=45,275; pooled raw block samples=1
  0x8c0d6c72  d218  mov.l @(0x8C0D6CD4[PC]),R2                 ; mapped instruction samples=0
  0x8c0d6c74  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c0d6c76  7301  add #1,R3                                  ; mapped instruction samples=0
  0x8c0d6c78  2232  mov.l R3,@R2                               ; mapped instruction samples=0
  0x8c0d6c7a  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6c7c  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0d6c7e  7201  add #1,R2                                  ; mapped instruction samples=0
  0x8c0d6c80  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6c82  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6c84  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6c86  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=1
  0x8c0d6c88  7302  add #2,R3                                  ; mapped instruction samples=0
  0x8c0d6c8a  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6c8c  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6c8e  e220  mov #0x20,R2                               ; mapped instruction samples=0
  0x8c0d6c90  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6c92  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0d6c94  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0d6c96  8901  bt 0x8C0D6C9C                              ; mapped instruction samples=0
block 0x8c0d6c7a: entries/replay=718,885; pooled raw block samples=15
  0x8c0d6c7a  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6c7c  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0d6c7e  7201  add #1,R2                                  ; mapped instruction samples=0
  0x8c0d6c80  e068  mov #0x68,R0                               ; mapped instruction samples=0
  0x8c0d6c82  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0d6c84  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6c86  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=2
  0x8c0d6c88  7302  add #2,R3                                  ; mapped instruction samples=0
  0x8c0d6c8a  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0d6c8c  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=1
  0x8c0d6c8e  e220  mov #0x20,R2                               ; mapped instruction samples=0
  0x8c0d6c90  e068  mov #0x68,R0                               ; mapped instruction samples=1
  0x8c0d6c92  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=1
  0x8c0d6c94  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0d6c96  8901  bt 0x8C0D6C9C                              ; mapped instruction samples=0
```

### 8. 0x8c072dfc (natural)

Exclusive sample categories: `{"jit_self/register_load": 1.0, "jit_self/register_store": 4.0, "jit_self/unclassified": 34.0}`

Native leaf samples, inferred caller attribution: `{}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 794028, "allocator_stores_x_block_entries_upper_bound": 4896506}`

```text
block 0x8c072dfc: entries/replay=264,676; pooled raw block samples=12
  0x8c072dfc  52f1  mov.l @(0x4,R15),R2                        ; mapped instruction samples=3
  0x8c072dfe  4208  shll2 R2                                   ; mapped instruction samples=0
  0x8c072e00  d02b  mov.l @(0x8C072EB0[PC]),R0                 ; mapped instruction samples=0
  0x8c072e02  032e  mov.l @(R0,R2),R3                          ; mapped instruction samples=2
  0x8c072e04  d12b  mov.l @(0x8C072EB4[PC]),R1                 ; mapped instruction samples=0
  0x8c072e06  2132  mov.l R3,@R1                               ; mapped instruction samples=1
  0x8c072e08  d32a  mov.l @(0x8C072EB4[PC]),R3                 ; mapped instruction samples=0
  0x8c072e0a  6232  mov.l @R3,R2                               ; mapped instruction samples=0
  0x8c072e0c  512c  mov.l @(0x30,R2),R1                        ; mapped instruction samples=1
  0x8c072e0e  2f12  mov.l R1,@R15                              ; mapped instruction samples=0
  0x8c072e10  62f2  mov.l @R15,R2                              ; mapped instruction samples=1
  0x8c072e12  2228  tst R2,R2                                  ; mapped instruction samples=0
  0x8c072e14  8935  bt 0x8C072E82                              ; mapped instruction samples=0
block 0x8c072e16: entries/replay=264,676; pooled raw block samples=3
  0x8c072e16  62f2  mov.l @R15,R2                              ; mapped instruction samples=2
  0x8c072e18  8521  mov.w @(0x2,R2),R0                         ; mapped instruction samples=0
  0x8c072e1a  2008  tst R0,R0                                  ; mapped instruction samples=0
  0x8c072e1c  8931  bt 0x8C072E82                              ; mapped instruction samples=0
block 0x8c072e1e: entries/replay=132,338; pooled raw block samples=5
  0x8c072e1e  d226  mov.l @(0x8C072EB8[PC]),R2                 ; mapped instruction samples=0
  0x8c072e20  6122  mov.l @R2,R1                               ; mapped instruction samples=1
  0x8c072e22  2118  tst R1,R1                                  ; mapped instruction samples=0
  0x8c072e24  890d  bt 0x8C072E42                              ; mapped instruction samples=0
block 0x8c072e26: entries/replay=6,220; pooled raw block samples=1
  0x8c072e26  d024  mov.l @(0x8C072EB8[PC]),R0                 ; mapped instruction samples=0
  0x8c072e28  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c072e2a  d324  mov.l @(0x8C072EBC[PC]),R3                 ; mapped instruction samples=0
  0x8c072e2c  5115  mov.l @(0x14,R1),R1                        ; mapped instruction samples=0
  0x8c072e2e  6232  mov.l @R3,R2                               ; mapped instruction samples=0
  0x8c072e30  321c  add R1,R2                                  ; mapped instruction samples=0
  0x8c072e32  2322  mov.l R2,@R3                               ; mapped instruction samples=0
  0x8c072e34  d220  mov.l @(0x8C072EB8[PC]),R2                 ; mapped instruction samples=0
  0x8c072e36  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c072e38  d121  mov.l @(0x8C072EC0[PC]),R1                 ; mapped instruction samples=0
  0x8c072e3a  5336  mov.l @(0x18,R3),R3                        ; mapped instruction samples=0
  0x8c072e3c  6012  mov.l @R1,R0                               ; mapped instruction samples=0
  0x8c072e3e  303c  add R3,R0                                  ; mapped instruction samples=0
  0x8c072e40  2102  mov.l R0,@R1                               ; mapped instruction samples=0
  0x8c072e42  d11c  mov.l @(0x8C072EB4[PC]),R1                 ; mapped instruction samples=0
  0x8c072e44  6212  mov.l @R1,R2                               ; mapped instruction samples=0
  0x8c072e46  d31d  mov.l @(0x8C072EBC[PC]),R3                 ; mapped instruction samples=0
  0x8c072e48  5225  mov.l @(0x14,R2),R2                        ; mapped instruction samples=0
  0x8c072e4a  6032  mov.l @R3,R0                               ; mapped instruction samples=0
  0x8c072e4c  3028  sub R2,R0                                  ; mapped instruction samples=0
  0x8c072e4e  2302  mov.l R0,@R3                               ; mapped instruction samples=0
  0x8c072e50  d218  mov.l @(0x8C072EB4[PC]),R2                 ; mapped instruction samples=0
  0x8c072e52  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c072e54  d01a  mov.l @(0x8C072EC0[PC]),R0                 ; mapped instruction samples=0
  0x8c072e56  5336  mov.l @(0x18,R3),R3                        ; mapped instruction samples=0
  0x8c072e58  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c072e5a  3138  sub R3,R1                                  ; mapped instruction samples=0
  0x8c072e5c  2012  mov.l R1,@R0                               ; mapped instruction samples=0
  0x8c072e5e  d315  mov.l @(0x8C072EB4[PC]),R3                 ; mapped instruction samples=0
  0x8c072e60  6032  mov.l @R3,R0                               ; mapped instruction samples=0
  0x8c072e62  d115  mov.l @(0x8C072EB8[PC]),R1                 ; mapped instruction samples=0
  0x8c072e64  2102  mov.l R0,@R1                               ; mapped instruction samples=0
  0x8c072e66  d217  mov.l @(0x8C072EC4[PC]),R2                 ; mapped instruction samples=0
  0x8c072e68  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c072e6a  0009  nop                                        ; mapped instruction samples=0
block 0x8c072e42: entries/replay=126,118; pooled raw block samples=3
  0x8c072e42  d11c  mov.l @(0x8C072EB4[PC]),R1                 ; mapped instruction samples=0
  0x8c072e44  6212  mov.l @R1,R2                               ; mapped instruction samples=0
  0x8c072e46  d31d  mov.l @(0x8C072EBC[PC]),R3                 ; mapped instruction samples=0
  0x8c072e48  5225  mov.l @(0x14,R2),R2                        ; mapped instruction samples=0
  0x8c072e4a  6032  mov.l @R3,R0                               ; mapped instruction samples=0
  0x8c072e4c  3028  sub R2,R0                                  ; mapped instruction samples=0
  0x8c072e4e  2302  mov.l R0,@R3                               ; mapped instruction samples=0
  0x8c072e50  d218  mov.l @(0x8C072EB4[PC]),R2                 ; mapped instruction samples=0
  0x8c072e52  6322  mov.l @R2,R3                               ; mapped instruction samples=1
  0x8c072e54  d01a  mov.l @(0x8C072EC0[PC]),R0                 ; mapped instruction samples=0
  0x8c072e56  5336  mov.l @(0x18,R3),R3                        ; mapped instruction samples=0
  0x8c072e58  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c072e5a  3138  sub R3,R1                                  ; mapped instruction samples=0
  0x8c072e5c  2012  mov.l R1,@R0                               ; mapped instruction samples=1
  0x8c072e5e  d315  mov.l @(0x8C072EB4[PC]),R3                 ; mapped instruction samples=0
  0x8c072e60  6032  mov.l @R3,R0                               ; mapped instruction samples=0
  0x8c072e62  d115  mov.l @(0x8C072EB8[PC]),R1                 ; mapped instruction samples=0
  0x8c072e64  2102  mov.l R0,@R1                               ; mapped instruction samples=1
  0x8c072e66  d217  mov.l @(0x8C072EC4[PC]),R2                 ; mapped instruction samples=0
  0x8c072e68  420b  jsr @R2                                    ; mapped instruction samples=0
  0x8c072e6a  0009  nop                                        ; mapped instruction samples=0
block 0x8c072e6c: entries/replay=132,338; pooled raw block samples=8
  0x8c072e6c  d216  mov.l @(0x8C072EC8[PC]),R2                 ; mapped instruction samples=0
  0x8c072e6e  6321  mov.w @R2,R3                               ; mapped instruction samples=1
  0x8c072e70  d016  mov.l @(0x8C072ECC[PC]),R0                 ; mapped instruction samples=0
  0x8c072e72  6101  mov.w @R0,R1                               ; mapped instruction samples=0
  0x8c072e74  3310  cmp/eq R1,R3                               ; mapped instruction samples=0
  0x8c072e76  8b04  bf 0x8C072E82                              ; mapped instruction samples=0
block 0x8c072e82: entries/replay=264,676; pooled raw block samples=7
  0x8c072e82  52f1  mov.l @(0x4,R15),R2                        ; mapped instruction samples=0
  0x8c072e84  7201  add #1,R2                                  ; mapped instruction samples=2
  0x8c072e86  1f21  mov.l R2,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c072e88  e302  mov #0x2,R3                                ; mapped instruction samples=0
  0x8c072e8a  51f1  mov.l @(0x4,R15),R1                        ; mapped instruction samples=2
  0x8c072e8c  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c072e8e  8bb5  bf 0x8C072DFC                              ; mapped instruction samples=0
```

### 9. 0x8c0c9f84 (natural)

Exclusive sample categories: `{"jit_self/register_load": 2.0, "jit_self/register_store": 7.0, "jit_self/unclassified": 29.0}`

Native leaf samples, inferred caller attribution: `{}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 1107221, "allocator_stores_x_block_entries_upper_bound": 4775886}`

```text
block 0x8c0c9f84: entries/replay=35,820; pooled raw block samples=0
  0x8c0c9f84  9035  mov.w @(0x8C0C9FF2[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9f86  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0c9f88  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0c9f8a  88ff  cmp/eq #0xFFFFFFFF,R0                      ; mapped instruction samples=0
  0x8c0c9f8c  8907  bt 0x8C0C9F9E                              ; mapped instruction samples=0
block 0x8c0c9f8e: entries/replay=17,859; pooled raw block samples=0
  0x8c0c9f8e  88fc  cmp/eq #0xFFFFFFFC,R0                      ; mapped instruction samples=0
  0x8c0c9f90  8905  bt 0x8C0C9F9E                              ; mapped instruction samples=0
block 0x8c0c9f92: entries/replay=17,859; pooled raw block samples=0
  0x8c0c9f92  88fd  cmp/eq #0xFFFFFFFD,R0                      ; mapped instruction samples=0
  0x8c0c9f94  8903  bt 0x8C0C9F9E                              ; mapped instruction samples=0
block 0x8c0c9f96: entries/replay=347; pooled raw block samples=0
  0x8c0c9f96  88fe  cmp/eq #0xFFFFFFFE,R0                      ; mapped instruction samples=0
  0x8c0c9f98  8901  bt 0x8C0C9F9E                              ; mapped instruction samples=0
block 0x8c0c9f9e: entries/replay=35,473; pooled raw block samples=1
  0x8c0c9f9e  9029  mov.w @(0x8C0C9FF4[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fa0  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0c9fa2  4118  shll8 R1                                   ; mapped instruction samples=0
  0x8c0c9fa4  e070  mov #0x70,R0                               ; mapped instruction samples=0
  0x8c0c9fa6  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0c9fa8  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0c9faa  9224  mov.w @(0x8C0C9FF6[PC]),R2                 ; mapped instruction samples=1
  0x8c0c9fac  312c  add R2,R1                                  ; mapped instruction samples=0
  0x8c0c9fae  9023  mov.w @(0x8C0C9FF8[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fb0  d313  mov.l @(0x8C0CA000[PC]),R3                 ; mapped instruction samples=0
  0x8c0c9fb2  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0c9fb4  0009  nop                                        ; mapped instruction samples=0
block 0x8c0c9fb6: entries/replay=35,473; pooled raw block samples=0
  0x8c0c9fb6  405a  lds R0,FPUL                                ; mapped instruction samples=0
  0x8c0c9fb8  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0c9fba  c712  mova @(0x8C0CA004[PC]),R0                  ; mapped instruction samples=0
  0x8c0c9fbc  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0c9fbe  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0c9fc0  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0c9fc2  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0c9fc4  f235  fcmp/gt FR3,FR2                            ; mapped instruction samples=0
  0x8c0c9fc6  8b06  bf 0x8C0C9FD6                              ; mapped instruction samples=0
block 0x8c0c9fc8: entries/replay=25,710; pooled raw block samples=0
  0x8c0c9fc8  c70f  mova @(0x8C0CA008[PC]),R0                  ; mapped instruction samples=0
  0x8c0c9fca  f308  fmov.s @R0,FR3                             ; mapped instruction samples=0
  0x8c0c9fcc  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0c9fce  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0c9fd0  f230  fadd FR3,FR2                               ; mapped instruction samples=0
  0x8c0c9fd2  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0c9fd4  ff27  fmov.s FR2,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0c9fd6  9010  mov.w @(0x8C0C9FFA[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fd8  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0c9fda  4318  shll8 R3                                   ; mapped instruction samples=0
  0x8c0c9fdc  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0c9fde  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0c9fe0  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0c9fe2  9108  mov.w @(0x8C0C9FF6[PC]),R1                 ; mapped instruction samples=0
  0x8c0c9fe4  331c  add R1,R3                                  ; mapped instruction samples=0
  0x8c0c9fe6  9009  mov.w @(0x8C0C9FFC[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fe8  4311  cmp/pz R3                                  ; mapped instruction samples=0
  0x8c0c9fea  8b0f  bf 0x8C0CA00C                              ; mapped instruction samples=0
block 0x8c0c9fd6: entries/replay=9,763; pooled raw block samples=0
  0x8c0c9fd6  9010  mov.w @(0x8C0C9FFA[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fd8  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0c9fda  4318  shll8 R3                                   ; mapped instruction samples=0
  0x8c0c9fdc  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0c9fde  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0c9fe0  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0c9fe2  9108  mov.w @(0x8C0C9FF6[PC]),R1                 ; mapped instruction samples=0
  0x8c0c9fe4  331c  add R1,R3                                  ; mapped instruction samples=0
  0x8c0c9fe6  9009  mov.w @(0x8C0C9FFC[PC]),R0                 ; mapped instruction samples=0
  0x8c0c9fe8  4311  cmp/pz R3                                  ; mapped instruction samples=0
  0x8c0c9fea  8b0f  bf 0x8C0CA00C                              ; mapped instruction samples=0
block 0x8c0c9fec: entries/replay=35,473; pooled raw block samples=0
  0x8c0c9fec  2309  and R0,R3                                  ; mapped instruction samples=0
  0x8c0c9fee  a012  bra 0x8C0CA016                             ; mapped instruction samples=0
  0x8c0c9ff0  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca016: entries/replay=35,473; pooled raw block samples=0
  0x8c0ca016  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0ca018  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0ca01a  c731  mova @(0x8C0CA0E0[PC]),R0                  ; mapped instruction samples=0
  0x8c0ca01c  f208  fmov.s @R0,FR2                             ; mapped instruction samples=0
  0x8c0ca01e  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0ca020  e058  mov #0x58,R0                               ; mapped instruction samples=0
  0x8c0ca022  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca024  f235  fcmp/gt FR3,FR2                            ; mapped instruction samples=0
  0x8c0ca026  8b06  bf 0x8C0CA036                              ; mapped instruction samples=0
block 0x8c0ca036: entries/replay=35,473; pooled raw block samples=0
  0x8c0ca036  e300  mov #0x0,R3                                ; mapped instruction samples=0
  0x8c0ca038  e048  mov #0x48,R0                               ; mapped instruction samples=0
  0x8c0ca03a  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca03c  e200  mov #0x0,R2                                ; mapped instruction samples=0
  0x8c0ca03e  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca040  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca042  904b  mov.w @(0x8C0CA0DC[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca044  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca046  e002  mov #0x2,R0                                ; mapped instruction samples=0
  0x8c0ca048  3038  sub R3,R0                                  ; mapped instruction samples=0
  0x8c0ca04a  4008  shll2 R0                                   ; mapped instruction samples=0
  0x8c0ca04c  4008  shll2 R0                                   ; mapped instruction samples=0
  0x8c0ca04e  d126  mov.l @(0x8C0CA0E8[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca050  001d  mov.w @(R0,R1),R0                          ; mapped instruction samples=0
  0x8c0ca052  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca054  4019  shlr8 R0                                   ; mapped instruction samples=0
  0x8c0ca056  c820  tst #0x20,R0                               ; mapped instruction samples=0
  0x8c0ca058  8905  bt 0x8C0CA066                              ; mapped instruction samples=0
block 0x8c0ca066: entries/replay=35,473; pooled raw block samples=1
  0x8c0ca066  9039  mov.w @(0x8C0CA0DC[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca068  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca06a  e002  mov #0x2,R0                                ; mapped instruction samples=0
  0x8c0ca06c  3038  sub R3,R0                                  ; mapped instruction samples=0
  0x8c0ca06e  4008  shll2 R0                                   ; mapped instruction samples=0
  0x8c0ca070  4008  shll2 R0                                   ; mapped instruction samples=0
  0x8c0ca072  d11d  mov.l @(0x8C0CA0E8[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca074  001d  mov.w @(R0,R1),R0                          ; mapped instruction samples=0
  0x8c0ca076  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca078  4019  shlr8 R0                                   ; mapped instruction samples=0
  0x8c0ca07a  c840  tst #0x40,R0                               ; mapped instruction samples=0
  0x8c0ca07c  8905  bt 0x8C0CA08A                              ; mapped instruction samples=0
block 0x8c0ca08a: entries/replay=35,473; pooled raw block samples=0
  0x8c0ca08a  e070  mov #0x70,R0                               ; mapped instruction samples=0
  0x8c0ca08c  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca08e  633b  neg R3,R3                                  ; mapped instruction samples=0
  0x8c0ca090  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0ca092  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0ca094  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0ca096  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0ca098  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0ca09a  c714  mova @(0x8C0CA0EC[PC]),R0                  ; mapped instruction samples=0
  0x8c0ca09c  f108  fmov.s @R0,FR1                             ; mapped instruction samples=0
  0x8c0ca09e  f313  fdiv FR1,FR3                               ; mapped instruction samples=0
  0x8c0ca0a0  f33d  ftrc FR3, FPUL                             ; mapped instruction samples=0
  0x8c0ca0a2  035a  sts FPUL,R3                                ; mapped instruction samples=0
  0x8c0ca0a4  e050  mov #0x50,R0                               ; mapped instruction samples=0
  0x8c0ca0a6  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca0a8  61f3  mov R15,R1                                 ; mapped instruction samples=0
  0x8c0ca0aa  7148  add #72,R1                                 ; mapped instruction samples=0
  0x8c0ca0ac  6012  mov.l @R1,R0                               ; mapped instruction samples=0
  0x8c0ca0ae  c801  tst #0x1,R0                                ; mapped instruction samples=0
  0x8c0ca0b0  890a  bt 0x8C0CA0C8                              ; mapped instruction samples=0
block 0x8c0ca0c8: entries/replay=35,473; pooled raw block samples=1
  0x8c0ca0c8  d30a  mov.l @(0x8C0CA0F4[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca0ca  6031  mov.w @R3,R0                               ; mapped instruction samples=0
  0x8c0ca0cc  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca0ce  8817  cmp/eq #0x17,R0                            ; mapped instruction samples=0
  0x8c0ca0d0  8b1f  bf 0x8C0CA112                              ; mapped instruction samples=0
block 0x8c0ca112: entries/replay=35,473; pooled raw block samples=0
  0x8c0ca112  e078  mov #0x78,R0                               ; mapped instruction samples=0
  0x8c0ca114  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca116  e050  mov #0x50,R0                               ; mapped instruction samples=0
  0x8c0ca118  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca11a  3123  cmp/ge R2,R1                               ; mapped instruction samples=0
  0x8c0ca11c  8903  bt 0x8C0CA126                              ; mapped instruction samples=0
block 0x8c0ca11e: entries/replay=35,473; pooled raw block samples=0
  0x8c0ca11e  e050  mov #0x50,R0                               ; mapped instruction samples=0
  0x8c0ca120  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca122  4311  cmp/pz R3                                  ; mapped instruction samples=0
  0x8c0ca124  890e  bt 0x8C0CA144                              ; mapped instruction samples=0
block 0x8c0ca126: entries/replay=396; pooled raw block samples=0
  0x8c0ca126  e201  mov #0x1,R2                                ; mapped instruction samples=0
  0x8c0ca128  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca12a  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca12c  9009  mov.w @(0x8C0CA142[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca12e  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca130  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0ca132  88fc  cmp/eq #0xFFFFFFFC,R0                      ; mapped instruction samples=0
  0x8c0ca134  8906  bt 0x8C0CA144                              ; mapped instruction samples=0
block 0x8c0ca136: entries/replay=396; pooled raw block samples=0
  0x8c0ca136  9004  mov.w @(0x8C0CA142[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca138  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca13a  e3fd  mov #0xFFFFFFFD,R3                         ; mapped instruction samples=0
  0x8c0ca13c  2232  mov.l R3,@R2                               ; mapped instruction samples=0
  0x8c0ca13e  a5ff  bra 0x8C0CAD40                             ; mapped instruction samples=0
  0x8c0ca140  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca144: entries/replay=35,077; pooled raw block samples=1
  0x8c0ca144  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0ca146  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca148  611b  neg R1,R1                                  ; mapped instruction samples=0
  0x8c0ca14a  415a  lds R1,FPUL                                ; mapped instruction samples=0
  0x8c0ca14c  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0ca14e  e058  mov #0x58,R0                               ; mapped instruction samples=0
  0x8c0ca150  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0ca152  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0ca154  c713  mova @(0x8C0CA1A4[PC]),R0                  ; mapped instruction samples=0
  0x8c0ca156  f108  fmov.s @R0,FR1                             ; mapped instruction samples=0
  0x8c0ca158  f313  fdiv FR1,FR3                               ; mapped instruction samples=0
  0x8c0ca15a  f33d  ftrc FR3, FPUL                             ; mapped instruction samples=0
  0x8c0ca15c  035a  sts FPUL,R3                                ; mapped instruction samples=0
  0x8c0ca15e  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca160  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=1
  0x8c0ca162  61f3  mov R15,R1                                 ; mapped instruction samples=0
  0x8c0ca164  7148  add #72,R1                                 ; mapped instruction samples=0
  0x8c0ca166  6012  mov.l @R1,R0                               ; mapped instruction samples=0
  0x8c0ca168  c802  tst #0x2,R0                                ; mapped instruction samples=0
  0x8c0ca16a  8902  bt 0x8C0CA172                              ; mapped instruction samples=0
block 0x8c0ca172: entries/replay=35,077; pooled raw block samples=2
  0x8c0ca172  e074  mov #0x74,R0                               ; mapped instruction samples=0
  0x8c0ca174  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca176  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca178  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca17a  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c0ca17c  8903  bt 0x8C0CA186                              ; mapped instruction samples=0
block 0x8c0ca17e: entries/replay=26,221; pooled raw block samples=0
  0x8c0ca17e  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca180  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca182  4211  cmp/pz R2                                  ; mapped instruction samples=0
  0x8c0ca184  8910  bt 0x8C0CA1A8                              ; mapped instruction samples=0
block 0x8c0ca186: entries/replay=8,856; pooled raw block samples=0
  0x8c0ca186  e301  mov #0x1,R3                                ; mapped instruction samples=0
  0x8c0ca188  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca18a  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca18c  9009  mov.w @(0x8C0CA1A2[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca18e  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca190  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0ca192  88fc  cmp/eq #0xFFFFFFFC,R0                      ; mapped instruction samples=0
  0x8c0ca194  8908  bt 0x8C0CA1A8                              ; mapped instruction samples=0
block 0x8c0ca196: entries/replay=8,856; pooled raw block samples=0
  0x8c0ca196  9004  mov.w @(0x8C0CA1A2[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca198  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca19a  e3fd  mov #0xFFFFFFFD,R3                         ; mapped instruction samples=0
  0x8c0ca19c  2232  mov.l R3,@R2                               ; mapped instruction samples=0
  0x8c0ca19e  a5cf  bra 0x8C0CAD40                             ; mapped instruction samples=0
  0x8c0ca1a0  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca1a8: entries/replay=26,221; pooled raw block samples=0
  0x8c0ca1a8  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca1aa  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca1ac  2118  tst R1,R1                                  ; mapped instruction samples=0
  0x8c0ca1ae  8b41  bf 0x8C0CA234                              ; mapped instruction samples=0
block 0x8c0ca1b0: entries/replay=26,221; pooled raw block samples=7
  0x8c0ca1b0  e050  mov #0x50,R0                               ; mapped instruction samples=0
  0x8c0ca1b2  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=1
  0x8c0ca1b4  e074  mov #0x74,R0                               ; mapped instruction samples=0
  0x8c0ca1b6  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca1b8  0237  mul.l R3,R2                                ; mapped instruction samples=0
  0x8c0ca1ba  011a  sts MACL,R1                                ; mapped instruction samples=0
  0x8c0ca1bc  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca1be  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=1
  0x8c0ca1c0  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0ca1c2  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0ca1c4  e07c  mov #0x7C,R0                               ; mapped instruction samples=0
  0x8c0ca1c6  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=2
  0x8c0ca1c8  031e  mov.l @(R0,R1),R3                          ; mapped instruction samples=0
  0x8c0ca1ca  e044  mov #0x44,R0                               ; mapped instruction samples=0
  0x8c0ca1cc  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0ca1ce  6033  mov R3,R0                                  ; mapped instruction samples=0
  0x8c0ca1d0  88fe  cmp/eq #0xFFFFFFFE,R0                      ; mapped instruction samples=0
  0x8c0ca1d2  8b0a  bf 0x8C0CA1EA                              ; mapped instruction samples=0
block 0x8c0ca1d4: entries/replay=8,247; pooled raw block samples=0
  0x8c0ca1d4  9035  mov.w @(0x8C0CA242[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca1d6  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca1d8  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0ca1da  88fc  cmp/eq #0xFFFFFFFC,R0                      ; mapped instruction samples=0
  0x8c0ca1dc  8903  bt 0x8C0CA1E6                              ; mapped instruction samples=0
block 0x8c0ca1de: entries/replay=8,247; pooled raw block samples=0
  0x8c0ca1de  9030  mov.w @(0x8C0CA242[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca1e0  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca1e2  e1fd  mov #0xFFFFFFFD,R1                         ; mapped instruction samples=0
  0x8c0ca1e4  2012  mov.l R1,@R0                               ; mapped instruction samples=0
  0x8c0ca1e6  a00e  bra 0x8C0CA206                             ; mapped instruction samples=0
  0x8c0ca1e8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca1ea: entries/replay=17,974; pooled raw block samples=1
  0x8c0ca1ea  902b  mov.w @(0x8C0CA244[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca1ec  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca1ee  6102  mov.l @R0,R1                               ; mapped instruction samples=0
  0x8c0ca1f0  3130  cmp/eq R3,R1                               ; mapped instruction samples=0
  0x8c0ca1f2  8904  bt 0x8C0CA1FE                              ; mapped instruction samples=0
block 0x8c0ca1f4: entries/replay=1,684; pooled raw block samples=0
  0x8c0ca1f4  9025  mov.w @(0x8C0CA242[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca1f6  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca1f8  2132  mov.l R3,@R1                               ; mapped instruction samples=0
  0x8c0ca1fa  a004  bra 0x8C0CA206                             ; mapped instruction samples=0
  0x8c0ca1fc  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca1fe: entries/replay=16,290; pooled raw block samples=2
  0x8c0ca1fe  9020  mov.w @(0x8C0CA242[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca200  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0ca202  e0ff  mov #0xFFFFFFFF,R0                         ; mapped instruction samples=0
  0x8c0ca204  2102  mov.l R0,@R1                               ; mapped instruction samples=1
  0x8c0ca206  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca208  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca20a  4318  shll8 R3                                   ; mapped instruction samples=0
  0x8c0ca20c  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0ca20e  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0ca210  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0ca212  2f36  mov.l R3,@-R15                             ; mapped instruction samples=0
  0x8c0ca214  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca216  07fe  mov.l @(R0,R15),R7                         ; mapped instruction samples=0
  0x8c0ca218  4718  shll8 R7                                   ; mapped instruction samples=0
  0x8c0ca21a  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0ca21c  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca21e  373c  add R3,R7                                  ; mapped instruction samples=0
  0x8c0ca220  9011  mov.w @(0x8C0CA246[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca222  06fe  mov.l @(R0,R15),R6                         ; mapped instruction samples=0
  0x8c0ca224  9010  mov.w @(0x8C0CA248[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca226  05fe  mov.l @(R0,R15),R5                         ; mapped instruction samples=0
  0x8c0ca228  900c  mov.w @(0x8C0CA244[PC]),R0                 ; mapped instruction samples=1
  0x8c0ca22a  04fe  mov.l @(R0,R15),R4                         ; mapped instruction samples=0
  0x8c0ca22c  d107  mov.l @(0x8C0CA24C[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca22e  410b  jsr @R1                                    ; mapped instruction samples=0
  0x8c0ca230  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca206: entries/replay=9,931; pooled raw block samples=2
  0x8c0ca206  e04c  mov #0x4C,R0                               ; mapped instruction samples=0
  0x8c0ca208  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=1
  0x8c0ca20a  4318  shll8 R3                                   ; mapped instruction samples=0
  0x8c0ca20c  e064  mov #0x64,R0                               ; mapped instruction samples=0
  0x8c0ca20e  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=1
  0x8c0ca210  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0ca212  2f36  mov.l R3,@-R15                             ; mapped instruction samples=0
  0x8c0ca214  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0ca216  07fe  mov.l @(R0,R15),R7                         ; mapped instruction samples=0
  0x8c0ca218  4718  shll8 R7                                   ; mapped instruction samples=0
  0x8c0ca21a  e06c  mov #0x6C,R0                               ; mapped instruction samples=0
  0x8c0ca21c  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0ca21e  373c  add R3,R7                                  ; mapped instruction samples=0
  0x8c0ca220  9011  mov.w @(0x8C0CA246[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca222  06fe  mov.l @(R0,R15),R6                         ; mapped instruction samples=0
  0x8c0ca224  9010  mov.w @(0x8C0CA248[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca226  05fe  mov.l @(R0,R15),R5                         ; mapped instruction samples=0
  0x8c0ca228  900c  mov.w @(0x8C0CA244[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca22a  04fe  mov.l @(R0,R15),R4                         ; mapped instruction samples=0
  0x8c0ca22c  d107  mov.l @(0x8C0CA24C[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca22e  410b  jsr @R1                                    ; mapped instruction samples=0
  0x8c0ca230  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca232: entries/replay=26,221; pooled raw block samples=1
  0x8c0ca232  7f04  add #4,R15                                 ; mapped instruction samples=0
  0x8c0ca234  e378  mov #0x78,R3                               ; mapped instruction samples=0
  0x8c0ca236  d106  mov.l @(0x8C0CA250[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca238  6212  mov.l @R1,R2                               ; mapped instruction samples=1
  0x8c0ca23a  3233  cmp/ge R3,R2                               ; mapped instruction samples=0
  0x8c0ca23c  8b0a  bf 0x8C0CA254                              ; mapped instruction samples=0
block 0x8c0ca254: entries/replay=26,221; pooled raw block samples=0
  0x8c0ca254  9046  mov.w @(0x8C0CA2E4[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca256  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca258  6002  mov.l @R0,R0                               ; mapped instruction samples=0
  0x8c0ca25a  88fe  cmp/eq #0xFFFFFFFE,R0                      ; mapped instruction samples=0
  0x8c0ca25c  8904  bt 0x8C0CA268                              ; mapped instruction samples=0
block 0x8c0ca25e: entries/replay=26,221; pooled raw block samples=3
  0x8c0ca25e  9041  mov.w @(0x8C0CA2E4[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca260  00fe  mov.l @(R0,R15),R0                         ; mapped instruction samples=0
  0x8c0ca262  6002  mov.l @R0,R0                               ; mapped instruction samples=1
  0x8c0ca264  88fd  cmp/eq #0xFFFFFFFD,R0                      ; mapped instruction samples=0
  0x8c0ca266  8b01  bf 0x8C0CA26C                              ; mapped instruction samples=0
block 0x8c0ca26c: entries/replay=17,974; pooled raw block samples=4
  0x8c0ca26c  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0ca26e  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=1
  0x8c0ca270  9039  mov.w @(0x8C0CA2E6[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca272  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca274  9037  mov.w @(0x8C0CA2E6[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca276  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0ca278  9036  mov.w @(0x8C0CA2E8[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca27a  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca27c  d21e  mov.l @(0x8C0CA2F8[PC]),R2                 ; mapped instruction samples=0
  0x8c0ca27e  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c0ca280  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0ca282  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0ca284  e05c  mov #0x5C,R0                               ; mapped instruction samples=0
  0x8c0ca286  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=0
  0x8c0ca288  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0ca28a  902e  mov.w @(0x8C0CA2EA[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca28c  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca28e  902c  mov.w @(0x8C0CA2EA[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca290  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0ca292  902b  mov.w @(0x8C0CA2EC[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca294  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca296  e058  mov #0x58,R0                               ; mapped instruction samples=0
  0x8c0ca298  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0ca29a  9028  mov.w @(0x8C0CA2EE[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca29c  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca29e  d117  mov.l @(0x8C0CA2FC[PC]),R1                 ; mapped instruction samples=0
  0x8c0ca2a0  6312  mov.l @R1,R3                               ; mapped instruction samples=0
  0x8c0ca2a2  435a  lds R3,FPUL                                ; mapped instruction samples=0
  0x8c0ca2a4  f32d  float FPUL,FR3                             ; mapped instruction samples=0
  0x8c0ca2a6  e058  mov #0x58,R0                               ; mapped instruction samples=0
  0x8c0ca2a8  f2f6  fmov.s @(R0,R15),FR2                       ; mapped instruction samples=1
  0x8c0ca2aa  f320  fadd FR2,FR3                               ; mapped instruction samples=0
  0x8c0ca2ac  9020  mov.w @(0x8C0CA2F0[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca2ae  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca2b0  901d  mov.w @(0x8C0CA2EE[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca2b2  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0ca2b4  901d  mov.w @(0x8C0CA2F2[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca2b6  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca2b8  901a  mov.w @(0x8C0CA2F0[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca2ba  f3f6  fmov.s @(R0,R15),FR3                       ; mapped instruction samples=0
  0x8c0ca2bc  901a  mov.w @(0x8C0CA2F4[PC]),R0                 ; mapped instruction samples=0
  0x8c0ca2be  ff37  fmov.s FR3,@(R0,R15)                       ; mapped instruction samples=0
  0x8c0ca2c0  d30f  mov.l @(0x8C0CA300[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca2c2  6031  mov.w @R3,R0                               ; mapped instruction samples=0
  0x8c0ca2c4  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca2c6  880f  cmp/eq #0xF,R0                             ; mapped instruction samples=0
  0x8c0ca2c8  8903  bt 0x8C0CA2D2                              ; mapped instruction samples=0
block 0x8c0ca2ca: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca2ca  6031  mov.w @R3,R0                               ; mapped instruction samples=0
  0x8c0ca2cc  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca2ce  8810  cmp/eq #0x10,R0                            ; mapped instruction samples=0
  0x8c0ca2d0  8b61  bf 0x8C0CA396                              ; mapped instruction samples=0
block 0x8c0ca396: entries/replay=17,974; pooled raw block samples=1
  0x8c0ca396  d21d  mov.l @(0x8C0CA40C[PC]),R2                 ; mapped instruction samples=0
  0x8c0ca398  6021  mov.w @R2,R0                               ; mapped instruction samples=0
  0x8c0ca39a  600d  extu.w R0,R0                               ; mapped instruction samples=0
  0x8c0ca39c  880f  cmp/eq #0xF,R0                             ; mapped instruction samples=0
  0x8c0ca39e  8901  bt 0x8C0CA3A4                              ; mapped instruction samples=0
block 0x8c0ca66c: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca66c  f39d  fldi1 FR3                                  ; mapped instruction samples=0
  0x8c0ca66e  d345  mov.l @(0x8C0CA784[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca670  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0ca672  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0ca674  8939  bt 0x8C0CA6EA                              ; mapped instruction samples=0
block 0x8c0ca6ea: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca6ea  f39d  fldi1 FR3                                  ; mapped instruction samples=0
  0x8c0ca6ec  d327  mov.l @(0x8C0CA78C[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca6ee  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0ca6f0  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0ca6f2  8939  bt 0x8C0CA768                              ; mapped instruction samples=0
block 0x8c0ca768: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca768  d30a  mov.l @(0x8C0CA794[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca76a  f338  fmov.s @R3,FR3                             ; mapped instruction samples=0
  0x8c0ca76c  f28d  fldi0 FR2                                  ; mapped instruction samples=0
  0x8c0ca76e  a013  bra 0x8C0CA798                             ; mapped instruction samples=0
  0x8c0ca770  0009  nop                                        ; mapped instruction samples=0
block 0x8c0ca798: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca798  f324  fcmp/eq FR2,FR3                            ; mapped instruction samples=0
  0x8c0ca79a  8b01  bf 0x8C0CA7A0                              ; mapped instruction samples=0
block 0x8c0ca8f4: entries/replay=17,974; pooled raw block samples=0
  0x8c0ca8f4  f39d  fldi1 FR3                                  ; mapped instruction samples=0
  0x8c0ca8f6  d317  mov.l @(0x8C0CA954[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca8f8  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0ca8fa  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0ca8fc  894c  bt 0x8C0CA998                              ; mapped instruction samples=0
block 0x8c0ca998: entries/replay=17,974; pooled raw block samples=1
  0x8c0ca998  f39d  fldi1 FR3                                  ; mapped instruction samples=0
  0x8c0ca99a  d333  mov.l @(0x8C0CAA68[PC]),R3                 ; mapped instruction samples=0
  0x8c0ca99c  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0ca99e  f234  fcmp/eq FR3,FR2                            ; mapped instruction samples=0
  0x8c0ca9a0  8939  bt 0x8C0CAA16                              ; mapped instruction samples=0
block 0x8c0caa16: entries/replay=17,974; pooled raw block samples=0
  0x8c0caa16  d316  mov.l @(0x8C0CAA70[PC]),R3                 ; mapped instruction samples=0
  0x8c0caa18  f338  fmov.s @R3,FR3                             ; mapped instruction samples=0
  0x8c0caa1a  f28d  fldi0 FR2                                  ; mapped instruction samples=0
  0x8c0caa1c  f324  fcmp/eq FR2,FR3                            ; mapped instruction samples=0
  0x8c0caa1e  8b01  bf 0x8C0CAA24                              ; mapped instruction samples=0
block 0x8c0caba0: entries/replay=17,974; pooled raw block samples=0
  0x8c0caba0  e300  mov #0x0,R3                                ; mapped instruction samples=0
  0x8c0caba2  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0caba4  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0caba6  a060  bra 0x8C0CAC6A                             ; mapped instruction samples=0
  0x8c0caba8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cabaa: entries/replay=40,714; pooled raw block samples=1
  0x8c0cabaa  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cabac  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0cabae  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0cabb0  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cabb2  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cabb4  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cabb6  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cabb8  921c  mov.w @(0x8C0CABF4[PC]),R2                 ; mapped instruction samples=0
  0x8c0cabba  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0cabbc  312c  add R2,R1                                  ; mapped instruction samples=0
  0x8c0cabbe  f38d  fldi0 FR3                                  ; mapped instruction samples=0
  0x8c0cabc0  f218  fmov.s @R1,FR2                             ; mapped instruction samples=1
  0x8c0cabc2  f325  fcmp/gt FR2,FR3                            ; mapped instruction samples=0
  0x8c0cabc4  8b01  bf 0x8C0CABCA                              ; mapped instruction samples=0
block 0x8c0cabca: entries/replay=31,760; pooled raw block samples=1
  0x8c0cabca  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cabcc  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0cabce  6233  mov R3,R2                                  ; mapped instruction samples=0
  0x8c0cabd0  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cabd2  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0cabd4  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0cabd6  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cabd8  910c  mov.w @(0x8C0CABF4[PC]),R1                 ; mapped instruction samples=0
  0x8c0cabda  31fc  add R15,R1                                 ; mapped instruction samples=0
  0x8c0cabdc  331c  add R1,R3                                  ; mapped instruction samples=0
  0x8c0cabde  f38d  fldi0 FR3                                  ; mapped instruction samples=0
  0x8c0cabe0  e004  mov #0x4,R0                                ; mapped instruction samples=0
  0x8c0cabe2  f236  fmov.s @(R0,R3),FR2                        ; mapped instruction samples=0
  0x8c0cabe4  f325  fcmp/gt FR2,FR3                            ; mapped instruction samples=0
  0x8c0cabe6  8b0d  bf 0x8C0CAC04                              ; mapped instruction samples=0
block 0x8c0cac04: entries/replay=25,215; pooled raw block samples=1
  0x8c0cac04  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac06  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0cac08  6233  mov R3,R2                                  ; mapped instruction samples=0
  0x8c0cac0a  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cac0c  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0cac0e  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0cac10  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cac12  911f  mov.w @(0x8C0CAC54[PC]),R1                 ; mapped instruction samples=0
  0x8c0cac14  31fc  add R15,R1                                 ; mapped instruction samples=0
  0x8c0cac16  331c  add R1,R3                                  ; mapped instruction samples=1
  0x8c0cac18  c70f  mova @(0x8C0CAC58[PC]),R0                  ; mapped instruction samples=0
  0x8c0cac1a  f308  fmov.s @R0,FR3                             ; mapped instruction samples=0
  0x8c0cac1c  f238  fmov.s @R3,FR2                             ; mapped instruction samples=0
  0x8c0cac1e  f325  fcmp/gt FR2,FR3                            ; mapped instruction samples=0
  0x8c0cac20  8901  bt 0x8C0CAC26                              ; mapped instruction samples=0
block 0x8c0cac26: entries/replay=14,089; pooled raw block samples=1
  0x8c0cac26  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac28  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0cac2a  6233  mov R3,R2                                  ; mapped instruction samples=1
  0x8c0cac2c  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cac2e  332c  add R2,R3                                  ; mapped instruction samples=0
  0x8c0cac30  4308  shll2 R3                                   ; mapped instruction samples=0
  0x8c0cac32  4300  shll R3                                    ; mapped instruction samples=0
  0x8c0cac34  910e  mov.w @(0x8C0CAC54[PC]),R1                 ; mapped instruction samples=0
  0x8c0cac36  31fc  add R15,R1                                 ; mapped instruction samples=0
  0x8c0cac38  331c  add R1,R3                                  ; mapped instruction samples=0
  0x8c0cac3a  c708  mova @(0x8C0CAC5C[PC]),R0                  ; mapped instruction samples=0
  0x8c0cac3c  f308  fmov.s @R0,FR3                             ; mapped instruction samples=0
  0x8c0cac3e  e004  mov #0x4,R0                                ; mapped instruction samples=0
  0x8c0cac40  f236  fmov.s @(R0,R3),FR2                        ; mapped instruction samples=0
  0x8c0cac42  f325  fcmp/gt FR2,FR3                            ; mapped instruction samples=0
  0x8c0cac44  8901  bt 0x8C0CAC4A                              ; mapped instruction samples=0
block 0x8c0cac4a: entries/replay=14,089; pooled raw block samples=0
  0x8c0cac4a  e3ff  mov #0xFFFFFFFF,R3                         ; mapped instruction samples=0
  0x8c0cac4c  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac4e  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cac50  a010  bra 0x8C0CAC74                             ; mapped instruction samples=0
  0x8c0cac52  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cac60: entries/replay=26,625; pooled raw block samples=1
  0x8c0cac60  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac62  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0cac64  7101  add #1,R1                                  ; mapped instruction samples=0
  0x8c0cac66  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac68  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cac6a  e304  mov #0x4,R3                                ; mapped instruction samples=0
  0x8c0cac6c  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac6e  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0cac70  3233  cmp/ge R3,R2                               ; mapped instruction samples=0
  0x8c0cac72  8b9a  bf 0x8C0CABAA                              ; mapped instruction samples=0
block 0x8c0cac6a: entries/replay=17,974; pooled raw block samples=0
  0x8c0cac6a  e304  mov #0x4,R3                                ; mapped instruction samples=0
  0x8c0cac6c  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac6e  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0cac70  3233  cmp/ge R3,R2                               ; mapped instruction samples=0
  0x8c0cac72  8b9a  bf 0x8C0CABAA                              ; mapped instruction samples=0
block 0x8c0cac74: entries/replay=17,974; pooled raw block samples=0
  0x8c0cac74  e054  mov #0x54,R0                               ; mapped instruction samples=0
  0x8c0cac76  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0cac78  4311  cmp/pz R3                                  ; mapped instruction samples=0
  0x8c0cac7a  894a  bt 0x8C0CAD12                              ; mapped instruction samples=0
block 0x8c0cac7c: entries/replay=14,089; pooled raw block samples=1
  0x8c0cac7c  d229  mov.l @(0x8C0CAD24[PC]),R2                 ; mapped instruction samples=0
  0x8c0cac7e  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cac80  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cac82  d029  mov.l @(0x8C0CAD28[PC]),R0                 ; mapped instruction samples=0
  0x8c0cac84  9349  mov.w @(0x8C0CAD1A[PC]),R3                 ; mapped instruction samples=0
  0x8c0cac86  33fc  add R15,R3                                 ; mapped instruction samples=0
  0x8c0cac88  6332  mov.l @R3,R3                               ; mapped instruction samples=0
  0x8c0cac8a  0136  mov.l R3,@(R0,R1)                          ; mapped instruction samples=0
  0x8c0cac8c  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cac8e  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0cac90  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cac92  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cac94  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cac96  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cac98  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cac9a  d324  mov.l @(0x8C0CAD2C[PC]),R3                 ; mapped instruction samples=0
  0x8c0cac9c  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cac9e  923d  mov.w @(0x8C0CAD1C[PC]),R2                 ; mapped instruction samples=0
  0x8c0caca0  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0caca2  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0caca4  d322  mov.l @(0x8C0CAD30[PC]),R3                 ; mapped instruction samples=0
  0x8c0caca6  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0caca8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cacaa: entries/replay=14,089; pooled raw block samples=3
  0x8c0cacaa  d21e  mov.l @(0x8C0CAD24[PC]),R2                 ; mapped instruction samples=0
  0x8c0cacac  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cacae  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0cacb0  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacb2  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cacb4  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacb6  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacb8  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacba  d01e  mov.l @(0x8C0CAD34[PC]),R0                 ; mapped instruction samples=0
  0x8c0cacbc  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0cacbe  922e  mov.w @(0x8C0CAD1E[PC]),R2                 ; mapped instruction samples=0
  0x8c0cacc0  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0cacc2  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0cacc4  d31a  mov.l @(0x8C0CAD30[PC]),R3                 ; mapped instruction samples=0
  0x8c0cacc6  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0cacc8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cacca: entries/replay=14,089; pooled raw block samples=1
  0x8c0cacca  d216  mov.l @(0x8C0CAD24[PC]),R2                 ; mapped instruction samples=0
  0x8c0caccc  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cacce  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0cacd0  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacd2  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cacd4  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacd6  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacd8  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacda  d017  mov.l @(0x8C0CAD38[PC]),R0                 ; mapped instruction samples=0
  0x8c0cacdc  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0cacde  921f  mov.w @(0x8C0CAD20[PC]),R2                 ; mapped instruction samples=0
  0x8c0cace0  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0cace2  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0cace4  d312  mov.l @(0x8C0CAD30[PC]),R3                 ; mapped instruction samples=0
  0x8c0cace6  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0cace8  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cacea: entries/replay=14,089; pooled raw block samples=0
  0x8c0cacea  d20e  mov.l @(0x8C0CAD24[PC]),R2                 ; mapped instruction samples=0
  0x8c0cacec  6122  mov.l @R2,R1                               ; mapped instruction samples=0
  0x8c0cacee  6313  mov R1,R3                                  ; mapped instruction samples=0
  0x8c0cacf0  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacf2  313c  add R3,R1                                  ; mapped instruction samples=0
  0x8c0cacf4  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacf6  4108  shll2 R1                                   ; mapped instruction samples=0
  0x8c0cacf8  4100  shll R1                                    ; mapped instruction samples=0
  0x8c0cacfa  d010  mov.l @(0x8C0CAD3C[PC]),R0                 ; mapped instruction samples=0
  0x8c0cacfc  310c  add R0,R1                                  ; mapped instruction samples=0
  0x8c0cacfe  9210  mov.w @(0x8C0CAD22[PC]),R2                 ; mapped instruction samples=0
  0x8c0cad00  32fc  add R15,R2                                 ; mapped instruction samples=0
  0x8c0cad02  e018  mov #0x18,R0                               ; mapped instruction samples=0
  0x8c0cad04  d30a  mov.l @(0x8C0CAD30[PC]),R3                 ; mapped instruction samples=0
  0x8c0cad06  430b  jsr @R3                                    ; mapped instruction samples=0
  0x8c0cad08  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cad0a: entries/replay=14,089; pooled raw block samples=0
  0x8c0cad0a  d206  mov.l @(0x8C0CAD24[PC]),R2                 ; mapped instruction samples=0
  0x8c0cad0c  6322  mov.l @R2,R3                               ; mapped instruction samples=0
  0x8c0cad0e  7301  add #1,R3                                  ; mapped instruction samples=0
  0x8c0cad10  2232  mov.l R3,@R2                               ; mapped instruction samples=0
  0x8c0cad12  a015  bra 0x8C0CAD40                             ; mapped instruction samples=0
  0x8c0cad14  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cad16: entries/replay=347; pooled raw block samples=0
  0x8c0cad16  a013  bra 0x8C0CAD40                             ; mapped instruction samples=0
  0x8c0cad18  0009  nop                                        ; mapped instruction samples=0
block 0x8c0cad40: entries/replay=35,820; pooled raw block samples=4
  0x8c0cad40  9038  mov.w @(0x8C0CADB4[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad42  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0cad44  7201  add #1,R2                                  ; mapped instruction samples=0
  0x8c0cad46  9035  mov.w @(0x8C0CADB4[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad48  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cad4a  9034  mov.w @(0x8C0CADB6[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad4c  03fe  mov.l @(R0,R15),R3                         ; mapped instruction samples=0
  0x8c0cad4e  7304  add #4,R3                                  ; mapped instruction samples=0
  0x8c0cad50  9031  mov.w @(0x8C0CADB6[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad52  0f36  mov.l R3,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cad54  9030  mov.w @(0x8C0CADB8[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad56  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0cad58  7104  add #4,R1                                  ; mapped instruction samples=0
  0x8c0cad5a  902d  mov.w @(0x8C0CADB8[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad5c  0f16  mov.l R1,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cad5e  902c  mov.w @(0x8C0CADBA[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad60  02fe  mov.l @(R0,R15),R2                         ; mapped instruction samples=0
  0x8c0cad62  7201  add #1,R2                                  ; mapped instruction samples=1
  0x8c0cad64  9029  mov.w @(0x8C0CADBA[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad66  0f26  mov.l R2,@(R0,R15)                         ; mapped instruction samples=0
  0x8c0cad68  e305  mov #0x5,R3                                ; mapped instruction samples=1
  0x8c0cad6a  9026  mov.w @(0x8C0CADBA[PC]),R0                 ; mapped instruction samples=0
  0x8c0cad6c  01fe  mov.l @(R0,R15),R1                         ; mapped instruction samples=0
  0x8c0cad6e  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c0cad70  8901  bt 0x8C0CAD76                              ; mapped instruction samples=0
```

### 10. 0x8c06d33c (natural)

Exclusive sample categories: `{"jit_self/register_load": 1.0, "jit_self/register_store": 8.0, "jit_self/unclassified": 25.0}`

Native leaf samples, inferred caller attribution: `{}`

Per-replay fallback upper bounds: `{"interpreter_sites_x_block_entries_upper_bound": 0}`

Per-replay allocator-sync upper bounds: `{"allocator_loads_x_block_entries_upper_bound": 2421845, "allocator_stores_x_block_entries_upper_bound": 9687380}`

```text
block 0x8c06d33c: entries/replay=968,738; pooled raw block samples=19
  0x8c06d33c  50f1  mov.l @(0x4,R15),R0                        ; mapped instruction samples=8
  0x8c06d33e  8802  cmp/eq #0x2,R0                             ; mapped instruction samples=0
  0x8c06d340  8b0f  bf 0x8C06D362                              ; mapped instruction samples=0
block 0x8c06d362: entries/replay=968,738; pooled raw block samples=10
  0x8c06d362  9320  mov.w @(0x8C06D3A6[PC]),R3                 ; mapped instruction samples=0
  0x8c06d364  52f1  mov.l @(0x4,R15),R2                        ; mapped instruction samples=3
  0x8c06d366  0237  mul.l R3,R2                                ; mapped instruction samples=1
  0x8c06d368  031a  sts MACL,R3                                ; mapped instruction samples=0
  0x8c06d36a  d113  mov.l @(0x8C06D3B8[PC]),R1                 ; mapped instruction samples=1
  0x8c06d36c  331c  add R1,R3                                  ; mapped instruction samples=0
  0x8c06d36e  d00e  mov.l @(0x8C06D3A8[PC]),R0                 ; mapped instruction samples=0
  0x8c06d370  6202  mov.l @R0,R2                               ; mapped instruction samples=1
  0x8c06d372  3230  cmp/eq R3,R2                               ; mapped instruction samples=1
  0x8c06d374  8b26  bf 0x8C06D3C4                              ; mapped instruction samples=0
block 0x8c06d3c4: entries/replay=484,369; pooled raw block samples=5
  0x8c06d3c4  52f1  mov.l @(0x4,R15),R2                        ; mapped instruction samples=0
  0x8c06d3c6  7201  add #1,R2                                  ; mapped instruction samples=1
  0x8c06d3c8  1f21  mov.l R2,@(0x4,R15)                        ; mapped instruction samples=0
  0x8c06d3ca  e303  mov #0x3,R3                                ; mapped instruction samples=1
  0x8c06d3cc  51f1  mov.l @(0x4,R15),R1                        ; mapped instruction samples=0
  0x8c06d3ce  3133  cmp/ge R3,R1                               ; mapped instruction samples=0
  0x8c06d3d0  8bb4  bf 0x8C06D33C                              ; mapped instruction samples=0
```
