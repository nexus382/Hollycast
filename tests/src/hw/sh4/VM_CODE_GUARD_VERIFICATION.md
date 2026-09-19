# Exact VM code guard

## Independent completion criterion

Before changing the guard, define executable checks that an owned snapshot:

- accepts unchanged bytes and unrelated same-page writes;
- rejects mutation of every byte for lengths 1, 2, 6, 15, 16, 17, 63 and 256,
  including unaligned starts and tails;
- rejects empty, null, non-RAM, out-of-range and overflowing captures, shortened
  RAM, changed replacement storage and stale snapshots after failed capture;
- accepts a new snapshot after recompilation, and compares exact bytes even
  when two synthetic byte strings have identical FNV-1a hashes.

The `ShilVmSmc.ExactSnapshot*` tests implement these checks. The initial build
failed with `error: unknown type name 'ShilVmCodeGuard'` (exit 1), before adding
its implementation. Existing execution tests additionally require same-page
writes to avoid recompilation and opcode patches to recompile only their block.
The shared backend benchmark must retain exact guest hashes, GPRs, SR, FPSCR,
RAM and final PC relative to the interpreter and JIT.

## Implementation boundary

`shil_vm_mem.h` supplies the existing RAM classification and offset primitives.
`ShilVmCodeGuard` owns a copy of each checked block's guest bytes. Both normal
execution and idle probing compare live RAM against that snapshot using memcmp.
Capture validates the masked RAM window and splits at its physical boundary;
comparison checks both portions within the live allocation. No guest pointer is
retained. The cost is one allocation and one guest-byte copy per compilation,
plus guest-code-sized storage for each checked cached block.

The AOT lookup, AOT hash guard, profiling hashes, readonly/SMC-check enablement,
bytecode execution and invalidation/recompile paths are unchanged. The default
AOT table remains empty. This change adds no executable memory, code generation,
title-specific path, dump input or shipping corpus.

## Verification, 2026-09-19

Base `369d29c48`. No local `main` ref exists; retained the supplied base under
the owner exception already recorded in `SYNTHETIC_BACKEND_BENCH.md`.

Used the provisioned ARM64 RelWithDebInfo objects, response files and link list
as in the existing backend verification, rebuilding `shil_vm.cpp`,
`ShilVmSmcTest.cpp`, `ShilVmTest.cpp`, `ShilVmThroughputTest.cpp` and
`Sh4BackendThroughputTest.cpp`. This is a targeted rebuild/relink, not a clean
application build. Local recipe and output:

```
python3 /tmp/build-sh4-code-guard.py exact-guard /Users/thomasswift/Documents/HollyCast/build-jitless-tests
# exit 0; existing Homebrew deployment-target linker warnings
cd /tmp
/tmp/vm-exit-cache/exact-guard/Hollycast
# [==========] 167 tests from 35 test suites ran.
# [  PASSED  ] 167 tests. (Two pre-existing disabled tests remain disabled.)
```

Full log: `/tmp/sh4-guard-full.log`. Existing exit-cache and AOT infrastructure
Python gates and `git diff --check` pass. The no-dump gate is run on the committed
revision. No JIT implementation changes are included.

```
bash tests/bench-ab.sh --synthetic /tmp/vm-exit-cache/synthetic-jit/Hollycast /tmp/vm-exit-cache/exact-guard/Hollycast --rounds 3 --output /tmp/sh4-guard-after
# ALU: JIT 2561.01 Mops/s  SHIL VM 339.11 Mops/s  VM/JIT 13.24%
# RAM: JIT 4073.83 Mops/s  SHIL VM 392.53 Mops/s  VM/JIT 9.64%
# PASS: identical Sh4BackendThroughputTest.*; 2 passing tests/build/round;
# matching ALU/RAM work and results
```

A separate five-pair alternating before/after run rebuilds the original VM
source from the base revision using the same flags and objects. It holds the
machine benchmark lock and validates every invocation through the shared
workload checker. Local command: `python3 /tmp/compare-sh4-guards.py` (exit 0).
Logs and XML are in `/tmp/sh4-guard-paired/{before,after}`.

```
ALU: before median 344.11, after 340.42 Mops/s; change -1.07%
RAM: before median 365.42, after 392.57 Mops/s; change 7.43%
```

These noisy synthetic timings show a RAM-workload improvement, not attainment
of the parent 75% target or any claim about game performance.

## RAM-mask boundary correction (fnd_mu8qoaea_hy8wkl)

The original guard incorrectly rejected valid writable code that crossed the
physical RAM window, leading to repeated recompilation. The independent added
criterion is that a wrapped snapshot matches unchanged RAM, detects mutations
on both sides of the mask, ignores bytes outside the window and can be
recaptured. An emulator regression must execute the wrapped block, reuse it,
then recompile exactly once when its wrapped delay-slot instruction changes.

Before the fix, `ShilVmSmc.ExactSnapshotWrapsAtPhysicalRamMask` failed (exit 1):
unchanged bytes at the physical boundary did not match, and a larger backing
allocation caused reads past the masked window instead of at offset zero.

Capture now splits at `ramMask + 1`, with at most two exact comparisons on
execution. A block spanning the entire window needs only one copy of each
physical byte. The contiguous path still performs one comparison. Tests cover
cached/uncached guest aliases, physical wrap, a mask smaller than the backing
allocation, every changed byte on both sides, whole-window coverage, smaller
replacement RAM and recapture. The execution test explicitly marks the page
writable through `bm_RamWriteAccess` and asserts it is not readonly, ensuring
it tests the VM guard rather than OS page protection.

```
python3 /tmp/build-sh4-code-guard.py wrap-fixed /Users/thomasswift/Documents/HollyCast/build-jitless-tests
# exit 0 (same targeted rebuild/relink limitations as above)
cd /tmp
/tmp/vm-exit-cache/wrap-fixed/Hollycast
# [==========] 169 tests from 35 test suites ran.
# [  PASSED  ] 169 tests.
```

This reruns all 167 original enabled tests plus the two new regressions; the two
pre-existing disabled tests remain disabled. Log: `/tmp/sh4-wrap-full.log`.
The exact-byte guard, empty AOT table and no-dump/no-code-generation boundary
remain intact.

Five alternating pairs against the original hash guard were rerun with
`python3 /tmp/compare-sh4-wrap.py` (exit 0), holding the benchmark lock and
validating every result via the shared workload checker:

```
ALU: before median 350.85, after 356.81 Mops/s; change 1.70%
RAM: before median 369.93, after 414.83 Mops/s; change 12.14%
```

Logs/XML: `/tmp/sh4-wrap-paired/{before,after}`. These remain synthetic, noisy
measurements, not game-performance or 75%-of-JIT claims. An additional shared
JIT/VM comparison passes:

```
python3 tests/src/hw/sh4/check_backend_bench.py /tmp/vm-exit-cache/synthetic-jit/Hollycast /tmp/vm-exit-cache/wrap-fixed/Hollycast --output /tmp/sh4-wrap-jit-comparison
# PASS: identical Sh4BackendThroughputTest.*; 2 passing tests/build/round;
# matching ALU/RAM work and results
```

`check_vm_exit_cache.py`, `check_aot_infra.py` and `git diff --check` pass; the
committed revision is also checked with `check_vm_no_dump.py HEAD`.
