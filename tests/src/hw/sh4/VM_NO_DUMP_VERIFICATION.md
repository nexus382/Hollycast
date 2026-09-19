# VM exit-cache verification, 2026-09-19

Verified optimisation `2aa26ef01f9f834248ce10c60dbe3ac92895f76f` against
parent `07bfe2819`. The optional AOT revision `fa0b4093f` is outside this
verification. No local `main` branch is needed.

## Independent completion criterion

The explicit revision must classify exits at compilation, reuse that classification
in execution and idle probing, default to an empty AOT table, and encode live
guest SHIL on a table miss. Its VM must not require a dump input or introduce
executable-memory APIs. Runtime fallback tests and the deterministic benchmark
must pass. A JIT percentage may be stated only with a matching measured workload.

The independent source gate is `check_vm_no_dump.py`, following the nearby
standalone Python-check convention. It reads the named commit through `git show`,
not the current working tree. It is a structural regression check, not an
exhaustive call-graph or platform-policy audit.

```
python3 tests/src/hw/sh4/check_vm_no_dump.py 07bfe2819
AssertionError: exit classification must occur once
# Expected exit 1: negative control.

python3 tests/src/hw/sh4/check_vm_no_dump.py 2aa26ef01
PASS: 2aa26ef01: cached exits; empty-table VM fallback; no dump input or executable-memory API in VM/table
```

On the clean optimisation checkout, these existing gates also passed:

```
python3 tests/src/hw/sh4/check_vm_exit_cache.py
PASS: exit kind classified once at compile time; dispatch and idle probe reuse it
python3 core/hw/sh4/dyna/check_aot_infra.py
PASS: AOT infra present (table=aot_table.h, dump=aot_hot_dump.h, artifact=crazy_taxi_hot_blocks.json, tests=ShilVmTest.cpp, json.loads=ok)
git diff --check
# Exit 0, no output.
```

## Build and runtime checks

Used the provisioned `/tmp/build-vm-exit-xcode.py` relink recipe and
`/Users/thomasswift/Documents/HollyCast/build-jitless-tests` RelWithDebInfo objects.
From the clean optimisation checkout:

```
python3 /tmp/build-vm-exit-xcode.py duke-verify
# Exit 0: rebuilt shil_vm, ShilVmChainTest and ShilVmThroughputTest; linked Hollycast.
env -u HOLLYCAST_AOT_HOT_DUMP /tmp/vm-exit-cache/duke-verify/Hollycast '--gtest_filter=ShilVm*'
[==========] 33 tests from 7 test suites ran. (2050 ms total)
[  PASSED  ] 33 tests.
```

Ran from the temporary executable directory without a dump argument. In
particular, `AotLookupMissFallsBackToShilVm` and
`AotSmcMismatchFallsBackToShilVmWithoutNative` passed. The separate dump-writer
test creates its own output; that output is not a VM input. Source inspection
finds the dump environment variable only in the opt-in game benchmark, and no
dump JSON requirement in the application CMake entry point. The existing
infrastructure checker does require the checked-in JSON schema fixture; this is
a development check, not a runtime requirement.

VM source SHA-256 matched the explicit optimisation revision:
`98ea19ad391499e1fa906d0e075013e32ebf916535db93f545a66d45308f0726`.
The throughput test source matched deterministic-test commit `069e2b1a1`:
`e3d481b950680e851c9637298a39124542a232572dc490ceefe0a8888938a0f6`.
That test resets equal initial states and asserts exactly 40,000,064 increments
and the final PC for both execution paths.

This was a successful targeted rebuild/relink, not a clean full-emulator build.
Other objects were reused from the provisioned build. The linker warned that
Homebrew libpng and libomp target macOS 26 while the executable targets macOS 12;
execution passed on this host, but deployment on macOS 12 was not verified.

## Deterministic ALU/RAM measurements

Built the parent VM with the same recipe (`duke-baseline`) from `07bfe2819`.
Ran `--gtest_filter=ShilVmThroughputTest.*` five times per executable, alternating
baseline/optimised ordering, with the dump environment variable unset. All 20
individual test executions passed. Medians of reported Mops/s:

| Workload | Parent VM | Optimised VM | Interpreter (optimised runs) | VM / parent | VM / interpreter |
| --- | ---: | ---: | ---: | ---: | ---: |
| ALU | 378.42 | 404.84 | 311.84 | 1.0698 | 1.2982 |
| RAM | 388.28 | 400.92 | 229.76 | 1.0326 | 1.7450 |

Raw fresh output is in `/tmp/vm-exit-cache/duke-verify/{baseline,verify}-[0-4].log`
and `measurements.json`; full-suite output is `/tmp/duke-verify-suite.log`.
These short synthetic measurements are subject to host scheduling noise and do
not establish a game frame-rate improvement.

The provisioned JIT executable was queried and run with the same filter:

```
/Users/thomasswift/Documents/HollyCast/build-jit-tests/RelWithDebInfo/Hollycast.app/Contents/MacOS/Hollycast '--gtest_filter=ShilVmThroughputTest.*'
[==========] Running 0 tests from 0 test suites.
[  PASSED  ] 0 tests.
```

The throughput fixture is compiled only for `FEAT_SHREC == DYNAREC_IR`. Thus a
matching JIT counterpart is absent: **VM/JIT ratio unavailable; the 25%-of-JIT
target is unverified**, not demonstrated by the interpreter ratios above.
