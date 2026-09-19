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
Bounds checks use subtraction so invalid sizes cannot wrap. No guest pointer is
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
