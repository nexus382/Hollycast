# Dump-free ARM64 backend comparison

`Sh4BackendThroughputTest.*` runs the same finite SH4 programs through the
interpreter for correctness and the configured recompiler mainloop for timing.
The JIT test build measures the real ARM64 JIT; the JIT-less build measures the
SHIL VM with an asserted empty AOT table. Only the JIT reference binary generates
native code at runtime. No product sources or build options are changed.

The existing `ShilVmThroughputTest` and its interpreter/VM equivalence assertions
are unchanged. The new fixture reuses its emulator setup, synthetic RAM and
instruction-writing approach, adding a guest loop counter so both mainloops
perform exactly the same amount of work.

## Independent completion criterion

Defined before implementation, independently of the repository's check scripts:

* Both ARM64 binaries execute exactly `Sh4BackendThroughputTest.*`, comprising
  `AluLoopHostSpeed` and `RamLoopHostSpeed`. Missing or skipped tests fail.
* Each executes 64 untimed warm-up iterations, then resets and executes exactly
  40,000,000 measured iterations. The full reset context is shared; r0/RAM start
  at zero, r1 is the iteration count, r4 points to RAM, and r5 to completion MMIO.
* ALU executes ADD/DT/BF; RAM executes MOV.L/ADD/MOV.L/DT/BF. The completion
  store and terminal branch follow the loop and are not counted as loop ops.
  FNV-1a hashes of the exact code bytes must be 4010934473 and 1377643567.
* Warm-up and measured runs assert exact r0, r1, RAM and final PC. All GPRs,
  SR and FPSCR match the interpreter. Both backends emit matching workload,
  count, hash and result fields, with positive, internally consistent Mops/s.

The original supplied JIT binary failed this gate before implementation:

```
python3 tests/src/hw/sh4/check_backend_bench.py <original-jit> <original-vm>
AssertionError: jit: expected 2 tests, got 0
```

The checker also rejects passing a JIT binary in the VM position. XML reports
are removed before execution, subprocesses have a 120-second timeout, and dump,
disc, state and gtest sharding/repetition environment variables are cleared.
Each run retains its complete log and XML report. JSON uses the classic locale,
including when earlier test suites changed the process locale.

## Build and run

Use the existing macOS ARM64 presets with matching configurations:

```sh
cmake --preset darwin-jit-tests
cmake --build build-jit-tests --config RelWithDebInfo
cmake --preset darwin-jitless-tests
cmake --build build-jitless-tests --config RelWithDebInfo
bash tests/bench-ab.sh --synthetic \
  build-jit-tests/RelWithDebInfo/Hollycast.app/Contents/MacOS/Hollycast \
  build-jitless-tests/RelWithDebInfo/Hollycast.app/Contents/MacOS/Hollycast \
  --rounds 6 --output /tmp/sh4-backend-results
```

Synthetic mode retains the existing machine-wide benchmark lock and quiet-host
check, runs the binaries directly, and needs no shared app bundle slot. Python
3.9 or newer is required. The checker can also be invoked directly for correctness
validation; the shell entry point provides benchmark locking. Ordering alternates
JIT/VM and VM/JIT; summaries compare each backend's best throughput per workload.

The large, identical cycle budget keeps peripheral scheduling out of the timed
loops. The final synthetic MMIO store stops the CPU and expires the timeslice;
the JIT exits normally at its dispatcher. The warm-up compiles and links the
executed paths before measurement. These results measure CPU loop throughput,
not game performance or frame rate.

## Verification on 2026-09-19

Base: `17b0b3b81`. The owner explicitly authorised finishing on this supplied
base without creating or rebasing onto a nonexistent local `main`.

Used the provisioned RelWithDebInfo ARM64 build objects with their compiler
response files and link lists. Recompiled the new fixture for both binaries,
current `rec_arm64.cpp` and its offline runtime helper for JIT, and current
`shil_vm.cpp` plus unchanged `ShilVmThroughputTest.cpp` for VM. This is a targeted
rebuild/relink, not a clean application build. The reproducible local recipe is
`/tmp/build-sh4-backend-bench.py`, adapted from the provisioned rebuild recipe:

```sh
python3 /tmp/build-sh4-backend-bench.py synthetic-jit /Users/thomasswift/Documents/HollyCast/build-jit-tests
python3 /tmp/build-sh4-backend-bench.py synthetic-vm /Users/thomasswift/Documents/HollyCast/build-jitless-tests
```

Both return exit 0. Existing VIXL deprecation warnings and macOS deployment-target
warnings for Homebrew libpng/libomp remain. Deployment on macOS 12 was not tested.

```
/tmp/vm-exit-cache/synthetic-vm/Hollycast --gtest_filter='ShilVm*:Sh4BackendThroughputTest.*'
[==========] 35 tests from 8 test suites ran.
[  PASSED  ] 35 tests.

/tmp/vm-exit-cache/synthetic-vm/Hollycast
[==========] 164 tests from 35 test suites ran.
[  PASSED  ] 164 tests.
```

Both full-suite throughput rows also parse as JSON after locale-changing tests.
The full reused JIT test binary passes 162/166 tests, skips the VM-only default
setting test, and fails `AicaArmTest.LogicOpsTest`,
`SDLControllerMappingTest.split_axis` and `.reverse_axis`. All three failures
reproduce in the original, unmodified provisioned JIT binary with their exact
filter; they are not introduced by the benchmark. Logs:
`/tmp/sh4-jit-full-suite.log`, `/tmp/sh4-baseline-failures.log`, and
`/tmp/sh4-vm-full-suite.log`. The shared benchmark filter passes in both builds.

`bash -n tests/bench-ab.sh`, `git diff --check`,
`python3 tests/src/hw/sh4/check_vm_exit_cache.py` and
`python3 core/hw/sh4/dyna/check_aot_infra.py` pass.

Final isolated interleaved run (three rounds, six binary invocations, twelve
passing benchmark test executions):

```sh
bash tests/bench-ab.sh --synthetic \
  /tmp/vm-exit-cache/synthetic-jit/Hollycast \
  /tmp/vm-exit-cache/synthetic-vm/Hollycast \
  --rounds 3 --output /tmp/sh4-synthetic-verified
```

```
ALU: JIT 4185.73 Mops/s  SHIL VM 370.95 Mops/s  VM/JIT 8.86%
RAM: JIT 4348.68 Mops/s  SHIL VM 388.11 Mops/s  VM/JIT 8.92%
PASS: identical Sh4BackendThroughputTest.*; 2 passing tests/build/round; matching ALU/RAM work and results; logs /private/tmp/sh4-synthetic-verified
```

These are best-of-three synthetic measurements on this host, not evidence of
75% of JIT performance. The harness establishes an honest, repeatable baseline.
