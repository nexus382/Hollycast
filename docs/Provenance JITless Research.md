# Provenance as a JITless reference

Research-only review, 19 September 2026. **Provenance has genuine interpreter
paths, but the reviewed evidence does not establish a transferable way to reach
75% of JIT performance.** The most useful additional experiment for HollyCast
is a statically compiled, tail-call-threaded SHIL executor with carefully bounded
specialisation. No emulator implementation changed in this slice.

## Scope and pinned sources

Reviewed Provenance `6bf61f298c2af20da8ebe535eee9eb20f73e0626`, its pinned
Dolphin/iCube `d09f0ac62a9d51d1a747cd70f0d080401996dc36`, pinned PPSSPP
`4c10f663ef2a40172ef51da6993d13d56a902290`, and the separately discovered
Flycast `JoeMatt/CachedIR` branch at
`4e8a7d035681bfef8be83a4b072bb5e244cc732b`.
The [submodule manifest](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/.gitmodules)
identifies upstream repositories; Git tree entries supplied the pinned commits.
A branch's source capability, frontend selection, and the contents of a distributed
binary are different evidence levels. This review does not treat them interchangeably.

Independent completion criterion, defined before writing this report: a report
with six named sections, at least ten immutable source references, at least four
confirmed execution-path rows, explicit constraints, numerical acceptance, and
measurement limitations. The initial `/tmp/check-provenance-research.py` run
failed with `AssertionError: research report missing`. Its durable equivalent is:

```sh
python3 docs/check_provenance_research.py
python3 docs/check_provenance_research.py --fetch
```

The optional network check additionally retrieves every unique pinned source link
and checks selected source signatures. These checks establish reference integrity
and report completeness, not benchmark correctness or emulator compatibility.
The local report/checker pattern follows
`tests/src/hw/sh4/SYNTHETIC_BACKEND_BENCH.md` and `check_backend_bench.py`.

## Core findings

| Core / path | Evidence status | What actually runs |
|---|---|---|
| Dolphin/iCube cached interpreter | Confirmed source path and Provenance selection | Precompiled callbacks with decoded operands; cached blocks and optional record/block chaining. |
| Dolphin/iCube IR interpreter | Confirmed source path and frontend option | Non-executable IR records; computed-goto execution on Clang/GCC, switch fallback. |
| PPSSPP interpreter and IR_JIT | Confirmed source paths and native frontend options | Ordinary interpreter, or cached IR executed by a C++ switch. Despite the older `IR_JIT` name, this path does not emit native CPU instructions. |
| Mednafen PS1 | Confirmed source execution path | `PS_CPU::RunReal` dispatches MIPS operations with computed goto; no native recompilation in this executor. |
| Mupen64Plus | Confirmed frontend configuration; pinned backend not retrieved | Pure interpreter (0), cached interpreter (1, default), dynarec (2) are distinct choices. Backend performance unverified here. |
| Flycast native wrapper | Confirmed iOS JIT requirement | The native wrapper reports `requiredOrCrash` on iOS/tvOS; it must not be mistaken for the separate JITless core. |
| Flycast JITless libretro | Confirmed packaging declaration; exact binary provenance unverified | Separate custom library declared for App Store builds. Public experimental cached-IR source exists, but linkage to the distributed binary is unproven. |

**Flycast: the critical distinction.** Provenance's
[core manifest](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/CoresRetro/RetroArch/scripts/cores.yml#L224)
separates ordinary `flycast` from `flycast-jitless`, marking the latter
`local: true` and App Store enabled. The
[asset script](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/CoresRetro/RetroArch/scripts/local-dylibs.sh#L1)
fetches custom builds from private release assets. The
[native wrapper](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/Flycast/PVFlycastCore/Core/PVFlycastEmuCore.swift#L22)
requires JIT on iOS/tvOS. Thus neither that wrapper nor the pinned ordinary
Flycast submodule identifies the JITless binary's implementation.

The public fork's
[build selection](https://github.com/Provenance-Emu/flycast-flyinghead/blob/4e8a7d035681bfef8be83a4b072bb5e244cc732b/core/build.h#L47)
selects `DYNAREC_JITLESS` with `ENABLE_SH4_JITLESS` and disables ARM/DSP
recompilers. Its
[driver](https://github.com/Provenance-Emu/flycast-flyinghead/blob/4e8a7d035681bfef8be83a4b072bb5e244cc732b/core/hw/sh4/dyna_jitless/driver_jitless.cpp#L459)
stores a block pointer in the nominal code buffer; execution reads `oplist`
and switches over SHIL operations. This is runtime translation to data interpreted
by precompiled code, not runtime code generation. It reuses SH4 decode/analysis.
The branch has multiple experimental executor paths; source inspection here is
not a successful build, compatibility result, or proof of which ships.

**Dolphin is the strongest concrete reference.** The
[Provenance bridge](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/Dolphin/PVDolphinCore/Core/PVDolphinCore.mm#L439)
falls back to CachedInterpreter when JIT is unavailable, also selecting the
software vertex loader. CPU fallback alone would miss that secondary code
generator. Its JIT capability probe itself attempts to generate/execute code;
that probe is unsuitable under HollyCast's stricter no-runtime-generation constraint.

The [emitter](https://github.com/Provenance-Emu/iCube/blob/d09f0ac62a9d51d1a747cd70f0d080401996dc36/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.cpp#L11)
serialises callback pointers and operands, and patches data for chaining.
[CodeBlock's non-executable specialisation](https://github.com/Provenance-Emu/iCube/blob/d09f0ac62a9d51d1a747cd70f0d080401996dc36/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreterEmitter.h#L168)
explicitly uses `Common::CodeBlock<CachedInterpreterEmitter, false>`.
The [executor](https://github.com/Provenance-Emu/iCube/blob/d09f0ac62a9d51d1a747cd70f0d080401996dc36/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreter.cpp#L128)
uses Clang `musttail` for record chains, cached dynamic successors with generation
checks, direct specialised operation dispatch, and fast memory handlers. These
are game-independent mechanisms; the PowerPC handlers themselves cannot be
copied into an SH4 core. The
[IR executor](https://github.com/Provenance-Emu/iCube/blob/d09f0ac62a9d51d1a747cd70f0d080401996dc36/Source/Core/Core/PowerPC/CachedInterpreter/CachedInterpreterIR.cpp#L6)
uses per-handler computed-goto dispatch, optimisation passes, and guarded linking.

Its own [performance handoff](https://github.com/Provenance-Emu/iCube/blob/d09f0ac62a9d51d1a747cd70f0d080401996dc36/docs/superpowers/plans/2026-09-17-jitless-perf-plan.md#L1)
reports the IR engine approximately 1.7 times slower than the shipping cached
interpreter, and dynamic-link gains of 5% and 8% in two game scenes. Those are
maintainer-reported measurements against another interpreter configuration,
not against JIT and not independently reproduced here. It distinguishes a
finite linked-list search from an idle loop, warns that adaptive clocks and heat
mislead FPS comparisons, and identifies IR persistence as a startup benefit
when translation is not hot. Do not add these separate percentages together.

**PPSSPP:** the
[native options](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/PPSSPP/PVPPSSPPCore/Core/CoreOptions.swift#L117)
default to ordinary interpretation and expose value 2 as IR JIT.
[MIPS backend selection](https://github.com/hrydgard/ppsspp/blob/4c10f663ef2a40172ef51da6993d13d56a902290/Core/MIPS/MIPS.cpp#L214)
constructs `IRJit` for that value.
[IRJit](https://github.com/hrydgard/ppsspp/blob/4c10f663ef2a40172ef51da6993d13d56a902290/Core/MIPS/IR/IRJit.cpp#L108)
caches `IRInst` vectors and calls `IRInterpret`;
[IRInterpret](https://github.com/hrydgard/ppsspp/blob/4c10f663ef2a40172ef51da6993d13d56a902290/Core/MIPS/IR/IRInterpreter.cpp#L101)
executes a switch. Useful precedent for IR optimisation, not evidence that
HollyCast needs an additional IR layer. Its replacement-function path must be
reviewed separately; do not import title/library signatures as a shortcut.

**Mupen64Plus and Mednafen:**
[Mupen options](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/Mupen64Plus/Sources/PVMupenBridge/MupenOptions.swift#L85)
and [configuration](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/Mupen64Plus/Sources/PVMupenBridge/PVMupenBridge+Mupen.m#L173)
select cached interpretation independently of dynarec. Retrieving that pinned
backend returned 404/422, so no source-level optimisation claim is made for it.
[Mednafen's PS1 executor](https://github.com/Provenance-Emu/Provenance/blob/6bf61f298c2af20da8ebe535eee9eb20f73e0626/Cores/Mednafen/Sources/mednafen/mednafen/src/psx/cpu.cpp#L600)
is a concrete computed-goto interpreter example. Success emulating other CPUs
or older consoles supplies no SH4/JIT throughput ratio.

## HollyCast comparison

Reviewed local base `369d29c48` and its `core/hw/sh4/dyna/shil_vm.cpp`,
`shil_vm_bytecode.h`, and synthetic benchmark report. Existing infrastructure
already includes SH4-to-SHIL decode/SSA, cached compact cells, common/rare
operation separation, memory fast paths, static/dynamic successor caching,
SMC handling, and guarded idle skipping. Its `executeCells` loop calls
`executeCell`, whose hot path is a switch; this is a concrete comparison point
for Dolphin's handler chains. Comments describing a handler table should not
substitute for inspecting this actual dispatch.

The existing AOT table is a separate facility. A title-specific hot-block corpus,
even compiled offline, violates this task's constraints. Keep its table empty
for this investigation's performance acceptance. Generic, build-time-compiled
handlers and opcode-pattern superinstructions are eligible if selected only
from semantic structure and runtime data, never a title ID or known game PC.

## Recommendations

1. **First bounded experiment: threaded SHIL handlers.** Reuse SHIL decode,
   cell operands, canonical operations, memory helpers and block manager.
   Compare the existing switch with a fixed-signature Clang `musttail` handler
   chain. Store only data and pointers to precompiled functions. Verify ARM64
   assembly actually keeps separate tail branches and avoids stack growth;
   compiler merging can erase the advantage. Preserve per-cell exception PCs,
   delay-slot adjustment, interrupt budgets and invalidation boundaries. Retain
   the existing executor as a correctness oracle during the experiment.
2. **Then specialise and fuse semantic patterns.** Profile dispatch and operand
   traffic on independently generated SH4 workloads. Consider bounded
   load/ALU/store or compare/branch handlers and operand-form specialisation
   with temporaries kept live within each handler. Extend the existing encoder
   and superop model. Do not cross faulting memory operations, delay slots,
   FP-mode changes or observable scheduling boundaries without equivalent
   state/exception reconstruction. Measure text size and instruction-cache costs.
3. **Reuse existing successor caching before extending it.** Dolphin's generation
   guards and bounded chaining are useful comparison cases. HollyCast already
   caches dynamic exits, so a straight port is unlikely to reproduce the reported
   gains. Only consider multiple-entry caches or longer chains after measuring
   miss rates, while testing SMC, cache eviction and target alternation.
4. **Separate peripheral savings from CPU improvements.** Fixed SIMD helpers and
   sound/video optimisations can improve complete-emulator speed, but cannot
   improve an SH4-only throughput result. Never count reduced guest clocks,
   adaptive underclocking, frame skipping, unsafe FP flags or skipped finite
   computation as equivalent-work speedups. Persistent decoded IR primarily
   reduces startup when compile/decode is already outside the timed interval.

These are prioritised experiments, not a forecast of achieving 75%. No reviewed
reference supplies a demonstrated 8-fold SH4 interpreter improvement.

## Performance acceptance

Existing identical-work synthetic results are ALU **8.86%** and RAM **8.92%**
of ARM64 JIT (see `tests/src/hw/sh4/SYNTHETIC_BACKEND_BENCH.md`). Reaching 75%
from those baselines requires approximately **8.47x** and **8.41x** speedups.
The earlier approximately 35% whole-game/headless result is a different metric;
it would require about 2.14x. Do not merge these baselines.

For a fixed workload, acceptance means `throughput_vm / throughput_jit >= 0.75`,
equivalently `time_vm / time_jit <= 1.333333...`. A 75%-of-JIT speed target permits
33.33% more elapsed time, not 25%. For a model with fraction `p` of JIT-reference
work accelerated to JIT speed and the remainder at ratio `r`, the ratio is
`1 / (p + (1-p)/r)`. At `r=0.0886`, even this optimistic model needs about
96.76% work coverage. Small specialised islands cannot by themselves close it.

Use the existing interleaved `tests/bench-ab.sh --synthetic` checker with the
same guest bytes, iteration counts, cycle model, compiler configuration and
hardware; empty AOT table; no disc/state/dump inputs. Preserve all correctness
assertions and logs. Add deterministic, title-independent workloads covering
branches, calls, memory aliasing, MMU faults, FP modes, delay slots and SMC to
avoid optimising only two tiny loops. Report per-workload ratios and a declared
aggregate; keep end-to-end emulation measurements separate. An iOS device run
is needed before extrapolating macOS ARM64 results to the product.

## Limitations

Performance of these references on HollyCast was **not measured**. This slice
inspected source and configuration; it did not build or run Provenance or its
cores, inspect private assets, establish App Store policy compliance, or prove
whole-program absence of executable-memory operations. In particular, Flycast's
shipped JITless binary cannot be tied to the public experimental branch from the
reviewed records. Ask its maintainers for exact source revision/build flags and
an equivalent-work JIT comparison before treating its performance as evidence.

No local `main` ref exists in this supplied checkout, so no rebase onto `main`
was possible; this documentation-only change stays on the supplied base. No
runtime source changed, so application builds and emulator suites were not
rerun for this research slice. The independent report gate, source retrieval,
and `git diff --check` are the relevant checks.
