# Synthetic backend audit, 2026-09-19

The 75% target is **not met**. Two isolated batches of nine interleaved pairs
measure median VM/JIT throughput of 11.65–15.31% for ALU and 9.02–9.04% for RAM.
ALU JIT timing is unstable, so neither batch establishes a precise ALU ratio.
Even the highest individual paired ratio, 20.28%, remains far below 75%.

## Independent done criterion

Chosen before running the audit: at least nine real samples per backend with
identical finite ALU/RAM guest work and correct results, positive elapsed times,
median and noise calculations checked against an independent arithmetic oracle,
and an explicit `ratio >= 0.75` gate. Completion means an evidenced audit and
an optimisation selection; it does not mean that the performance target passes.

`check_backend_audit.py` checks known times `[1,2,9]` and `[2,8,10]`: ratio of
median times must be 0.25, median paired ratio 0.5, JIT MAD 50%, and range 400%.
It independently reads the real log rows, checks code hashes/results/counts,
and recalculates the reported ratio and target gate. The existing
`check_backend_bench.py` remains the exemplar for execution, XML test validation,
log retention and strict workload equivalence; its summary now exposes medians,
MAD, range, paired ratios and raw times instead of selecting best runs.

## Execution and provenance

Base `369d29c48`; no VM or JIT product code changed. The local `main` ref is
absent, as recorded in the preceding integration report; this audit retains
that report's supplied-base workflow rather than inventing a replacement ref.
No repository AGENTS.md or CLAUDE.md was present.

Used the integrated ARM64 RelWithDebInfo binaries described in
`SYNTHETIC_BACKEND_BENCH.md`, built by targeted recompile/relink with provisioned
objects, not a clean application build. SHA-256:

* JIT: `5443e296c7cb680172b000200634f0296d32d02dbe0129a89d84dcdc5031b320`
* VM: `e4aa208b8c16b84162fdd2760c1a811680fe56f7f95eed8fac7cef3519cf30a7`

Host: macOS 26.6, ARM64. No disc, state, dump or title corpus inputs. The VM
fixture asserts an empty AOT table; only the reference JIT emits native code.
Both use the same 40,000,000 iterations, 64 warm-up iterations and guest code.
Each process also checks the result against the interpreter outside timing.

```sh
bash tests/bench-ab.sh --synthetic \
  /tmp/vm-exit-cache/synthetic-jit/Hollycast \
  /tmp/vm-exit-cache/synthetic-vm/Hollycast \
  --rounds 9 --output /tmp/sh4-audit-interleaved
# Repeat with --output /tmp/sh4-audit-confirm
python3 tests/src/hw/sh4/check_backend_audit.py /tmp/sh4-audit-interleaved
python3 tests/src/hw/sh4/check_backend_audit.py /tmp/sh4-audit-confirm
```

Both checker commands print:

```text
PASS: median/MAD oracle; >=9 real samples/backend; identical ALU/RAM results; ratios and 0.75 gate
```

The shell harness serialises benchmark runs. These two accepted batches ran
without our profiler or full-suite checks overlapping. A separate intermediate
batch in `/tmp/sh4-audit-repeat` overlapped suite checks and is excluded.
Full logs and XML remain in the two accepted output directories; all raw timing
samples and calculated statistics are committed in `SYNTHETIC_BACKEND_AUDIT.json`.

## Medians and noise

MAD is median absolute deviation divided by median; range is (max−min)/median.
Times are ns per complete guest loop iteration, so the differing instruction
counts do not disguise the incremental RAM cost. Throughput ratio is JIT median
seconds / VM median seconds; paired ratios additionally expose scheduling drift.

| Batch / loop | JIT ns | VM ns | JIT MAD / range | VM MAD / range | VM/JIT | Paired range |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Initial ALU | 1.278 | 8.344 | 12.96% / 71.11% | 2.21% / 15.86% | 15.31% | 8.05–20.28% |
| Initial RAM | 1.182 | 13.098 | 0.59% / 3.05% | 1.04% / 4.33% | 9.02% | 8.86–9.21% |
| Confirmation ALU | 0.982 | 8.428 | 22.07% / 55.50% | 0.64% / 7.06% | 11.65% | 8.67–14.65% |
| Confirmation RAM | 1.210 | 13.392 | 1.55% / 14.26% | 1.07% / 3.86% | 9.04% | 8.60–10.14% |

VM RAM adds 4.75–4.96 ns/iteration over ALU. This includes memory helpers,
extra cell dispatch and a longer SMC guard, not a pure memory latency estimate.
About 63% of the RAM loop's VM time is already present in the ALU workload.

## Measured attribution and selected optimisation

Collected separate 1 ms macOS `sample` profiles of three invocations per workload:
launch the VM with `--gtest_filter=Sh4BackendThroughputTest.AluLoopHostSpeed`
(or `RamLoopHostSpeed`), then `sample <pid> 3 1 -mayDie -file <report>`.
Raw profiles: `/tmp/sh4-audit-profile/{Alu,Ram}-{0,1,2}.sample`. All six test
invocations passed. The first ALU profile captured no stacks and is excluded.
Only VM frames are counted: interpreter reference work and process setup are
excluded. Counts below use exclusive top-of-stack totals to avoid double counting.

| Region | ALU samples (2 profiles) | RAM samples (3 profiles) |
| --- | ---: | ---: |
| runBlock, exclusive | 308 / 555 (55.5%) | 611 / 1367 (44.7%) |
| executeCell, exclusive | 226 / 555 (40.7%) | 728 / 1367 (53.3%) |
| mainloop, exclusive | 21 / 555 (3.8%) | 28 / 1367 (2.0%) |
| SMC guard at shil_vm.cpp:987, subset of runBlock | 142 / 555 (25.6%) | 336 / 1367 (24.6%) |
| Opcode switch at shil_vm.cpp:593, subset of executeCell | 107 / 555 (19.3%) | 268 / 1367 (19.6%) |

These are statistical instruction-location samples, not instrumented cycle
counts. Optimised line attribution can encompass adjacent checks. The recurring
SMC location corresponds to `shilVmGuestMatches`, which recomputes serial FNV-1a
across every guest byte at every block entry. It is the largest identified
individual hotspot in both workloads, followed by opcode dispatch. Together
these explain substantial general VM overhead without relying on title traces.

**Selected next optimisation:** cache an exact copy of guest bytes when compiling
an ordinary VM block and compare them at entry using bounded fixed-size loads
for short spans and a standard byte comparison for longer spans. Replace the
serial per-byte hash in both normal execution and idle probing. Keep range
validation, SMC invalidation/recompile behaviour and instruction visibility intact;
retain AOT identity hashes for their separate purpose. This applies to all VM
blocks regardless of title or opcode pattern and emits no runtime native code.
It should be validated against same-page data writes, every code-byte mutation,
short/unaligned spans and RAM boundaries before implementation is accepted.

This is a selection, not an implemented or measured speedup. Eliminating all of
the sampled guard cost would bound its standalone speedup near 1.33–1.34x;
real comparison overhead makes the gain smaller. It cannot close a roughly
8.3x RAM speedup requirement to reach 75%. Cell dispatch and block entry/exit
remain subsequent targets; no per-title superblock or corpus is justified here.

## Regression checks and limitations

Both accepted batches pass all 72 benchmark test executions in total. The full
VM suite passes 164/164 tests (`/tmp/sh4-audit-vm-full.log`). The reused JIT suite
passes 162, skips one VM-only test and repeats the three pre-existing failures
recorded by the integration report: `AicaArmTest.LogicOpsTest`,
`SDLControllerMappingTest.split_axis` and `.reverse_axis`
(`/tmp/sh4-audit-jit-full.log`). No full-JIT-suite green claim is made.

`bash -n tests/bench-ab.sh tests/bench-lib.sh`, `git diff --check`,
`python3 tests/src/hw/sh4/check_vm_exit_cache.py` and
`python3 core/hw/sh4/dyna/check_aot_infra.py` pass. The latter checks existing
infrastructure only; its title-derived artifact is not a benchmark input.

The owner explicitly accepted those unrelated pre-existing JIT failures as
non-blocking for this audit. Before committing, both nine-pair audit checkers
passed again, as did the exit-cache check and shell/diff checks. A fresh focused
benchmark invocation with `--rounds 1 --output /tmp/sh4-audit-final-check`
passed both tests in both builds, including the strict XML and workload/result
checks. This single pair is a correctness recheck, not an additional noise
estimate or a replacement for the two accepted nine-pair timing batches.
