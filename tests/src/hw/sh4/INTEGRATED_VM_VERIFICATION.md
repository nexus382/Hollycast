# Independent integrated ARM64 verification

Verified revision: `21ea4129f1e2a27b024baaf8b2dc4ff7b89d22db`, incorporating
`ac01ea130` (exact guest-byte guards) and `21ea4129f` (RAM-mask boundary fix).
The 75% performance target is **not met**. Verification is complete; performance
attainment is a separate, explicitly failing gate.

## Criterion and integration provenance

Chosen before execution: nine interleaved real ARM64 runs per backend; identical
synthetic guest hashes and final results; positive elapsed times; independently
recalculated median-time VM/JIT ratios and an explicit >=0.75 check per workload;
no runtime native code generation or executable-memory API in the SHIL VM path.
The independent checker reconstructs hashes from guest words and reads raw logs
and XML without importing the repository benchmark checker. Existing
`check_backend_bench.py` is reused for execution and log retention.

The initial checkout was stale at `50c8d5e43` and the local `main` ref was absent.
Verification initially stopped rather than benchmarking that revision. The owner
then supplied the integrated cherry-picks, explicitly waived the `main` requirement,
and confirmed `feat/jitless-shil-vm` at `21ea4129f`. This checkout was advanced with
`git merge --ff-only 21ea4129f`; HEAD and clean status were verified afterwards.
Git emitted a sandbox warning about `packed-refs.lock`, but HEAD and worktree
successfully advanced to the intended revision. No remote refs were used.

## Build and execution

Host: native macOS ARM64. Rebuilt current fixture, SMC tests, VM tests and backend
sources with provisioned RelWithDebInfo compiler response files and objects.
This is a targeted recompile/relink, not a clean application build. The unchanged
build helper is `/tmp/build-sh4-code-guard.py`, described in
`VM_CODE_GUARD_VERIFICATION.md`. Commands (both exit 0):

```sh
python3 /tmp/build-sh4-code-guard.py doctor-vm /Users/thomasswift/Documents/HollyCast/build-jitless-tests
python3 /tmp/build-sh4-code-guard.py doctor-jit /Users/thomasswift/Documents/HollyCast/build-jit-tests
bash tests/bench-ab.sh --synthetic /tmp/vm-exit-cache/doctor-jit/Hollycast /tmp/vm-exit-cache/doctor-vm/Hollycast --rounds 9 --output /tmp/sh4-doctor-integrated
python3 tests/src/hw/sh4/check_integrated_vm.py /tmp/sh4-doctor-integrated
python3 tests/src/hw/sh4/check_backend_audit.py /tmp/sh4-doctor-integrated
```

The first sandboxed benchmark attempt exited 126 because its host-load check
could not run `ps`. The authorised unsandboxed rerun succeeded, retaining the
machine-wide benchmark lock. No profiler or full-suite test overlapped timing.
All 36 benchmark test executions passed (two workloads, two backends, nine pairs).
No disc, dump, savestate or title corpus was supplied. The fixture requires an
empty AOT table, uses 64 warm-up and 40,000,000 measured iterations, and compares
all GPRs, SR and FPSCR to the interpreter outside timing. Hashes are 4010934473
(ALU) and 1377643567 (RAM); r0=40,000,000, r1=0, RAM=0/40,000,000,
PC=0xAC000008/0xAC00000C respectively in every backend/run.

Independent checker output:

```text
ALU: VM/JIT=0.106279172521 (10.627917%), target_met=False
RAM: VM/JIT=0.098053977856 (9.805398%), target_met=False
PASS: nine raw-log/XML pairs; independent guest hashes, final results and median ratios
```

Adding `--require-target` exits 1 with
`FAIL: at least one workload is below 75% of JIT`, as required. The ordinary
checker exits 0 for an internally consistent audit even when performance fails.
Raw times and statistics are retained in `INTEGRATED_VM_VERIFICATION.json`;
logs and XML remain in `/tmp/sh4-doctor-integrated`.

| Workload | JIT median ns/iteration | VM median ns/iteration | VM/JIT | JIT MAD | VM MAD |
| --- | ---: | ---: | ---: | ---: | ---: |
| ALU | 0.901653 | 8.483818 | 10.627917% | 4.71% | 1.05% |
| RAM | 1.196767 | 12.205182 | 9.805398% | 0.48% | 1.74% |

JIT ALU range is 49.89%; its precise ratio is noisy. Even the best paired ALU
ratio is only 14.62%, and RAM's best pair is 10.08%. Neither approaches 75%.

Binary SHA-256 (`shasum -a 256 /tmp/vm-exit-cache/doctor-{jit,vm}/Hollycast`):

* JIT: `5d99450dd29fd1f3a3fecfa261d079cb34f884660f5f0fcfc7af601d1935f340`
* VM: `73a93f97e61e4e1f2de4378d754e929c1d613d71ade6d8305264192f75208800`

## Regression and structural checks

```sh
(cd /tmp && /tmp/vm-exit-cache/doctor-vm/Hollycast)
# 169 tests from 35 suites; 169 passed; exit 0
(cd /tmp && /tmp/vm-exit-cache/doctor-jit/Hollycast)
# 170 tests; 166 passed; one skipped; three pre-existing failures; exit 1
python3 tests/src/hw/sh4/check_vm_no_dump.py 21ea4129f
python3 tests/src/hw/sh4/check_vm_exit_cache.py
python3 core/hw/sh4/dyna/check_aot_infra.py
bash -n tests/bench-ab.sh tests/bench-lib.sh
git diff --check
```

All structural checks pass. The infrastructure checker inspects an existing
artifact; that artifact was not a benchmark input. JIT failures remain
`AicaArmTest.LogicOpsTest`, `SDLControllerMappingTest.split_axis` and
`SDLControllerMappingTest.reverse_axis`, already reproduced on the unchanged
baseline and accepted as non-blocking by the owner. No full-JIT-suite pass claim.
Full logs: `/tmp/sh4-doctor-{vm,jit}-full.log`.

Additional inspection stripped comments from all `shil_vm*` sources and
`aot_table.h` and found no executable-memory/code-generation calls. `nm -u` on
rebuilt `shil_vm.o` and the provisioned JIT-less `driver.o` found no mmap,
mprotect, JIT write-protection, executable-buffer allocator or VIXL imports.
The VM link object list contains no SH4 `rec_arm64.o` or ARM64 assembler.
`driver.cpp` places executable buffer setup/release under DYNAREC_JIT;
CMake excludes the SH4 ARM64 JIT/VIXL sources under JITLESS. VM compilation
encodes data cells, and both normal execution and idle probing call the exact
byte guard. This confirms the inspected SH4 VM path; ordinary data-memory
protection elsewhere in the emulator is not native code generation.

## Remaining universal bottleneck

General cell interpretation and repeated block entry/exit remain the bottleneck,
including opcode dispatch, operand/memory operations, guard comparisons and
successor bookkeeping. The exact-byte optimisation removes serial guest hashing
but does not remove these costs. RAM adds about 3.72 ns per iteration over ALU.

A separate post-benchmark statistical profile confirms substantial time in
`executeCell` and `runBlock`, including the dispatch switch and guard. Command:

```sh
/tmp/vm-exit-cache/doctor-vm/Hollycast --gtest_filter=Sh4BackendThroughputTest.RamLoopHostSpeed > /tmp/sh4-doctor-profile-test.log 2>&1 & bench_pid=$!
sample "$bench_pid" 3 1 -mayDie -file /tmp/sh4-doctor-ram.sample
wait "$bench_pid"
```

The profile contains 98 samples at the opcode dispatch location, 217 samples
under the cell execution call and 55 additional executeCell samples, versus
426 total VM-path samples. These are statistical stacks, not instrumented cycle
counts; interpreter reference work is excluded. No title-specific inference or
claim that this optimisation reaches the target is made.
