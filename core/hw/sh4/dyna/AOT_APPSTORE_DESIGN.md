# App Store–legal path from ~35% JIT to ≥75% JIT

Investigation design for closing the remaining Crazy Taxi headless-bench gap
without runtime code generation. Companion gate:
`python3 core/hw/sh4/dyna/check_aot_appstore_design.py`.

## DONE_CRITERION

This investigation is done when the checker above exits 0. That is independent
of CMake, gtest, and any repo CI script. Implementation work is out of scope
for this slice; the first buildable cut is named under
`FIRST_IMPLEMENTATION_SLICE`.

## CONSTRAINT

Apple App Store policy (and the iOS/iPadOS kernel) forbid mapping pages as
writable+executable and forbid `MAP_JIT` / `PROT_EXEC` for third-party apps
that emit machine code at runtime. HollyCast already selects that world with
`TARGET_SHIL_VM` → `FEAT_SHREC == DYNAREC_IR` (`core/build.h`): decode once,
cache SHIL bytecode, never write host opcodes at run time.

**LEGALITY: build-time AOT native text is App-Store-legal; runtime emit is not**

What is legal: ordinary `.text` (or `__TEXT,__text`) that the linker placed in
the binary at our build, generated offline by tools we control. The CPU
executes those bytes the same way it executes any compiled C++ function. What
is illegal on device: allocating RWX/JIT pages and writing new opcodes while
the game runs.

## GAP_BUDGET

Owner bar: “within 25% of JIT” means ≥75% of JIT speed, not 25% of it.

Measured Crazy Taxi headless (idle-skip already in):

| Mode | fps | share of JIT |
|------|-----|--------------|
| JITLESS (SHIL VM + idle-skip) | ~491 | ~35% |
| JIT | ~1398 | 100% |

**BASELINE_JITLESS_FPS: 491**

**BASELINE_JIT_FPS: 1398**

**BASELINE_RATIO: 0.35**

**TARGET_RATIO: 0.75**

Target fps ≈ `0.75 × 1398 ≈ 1049`. Required multiplicative gain from today’s
jitless number:

**REQUIRED_SPEEDUP: 2.14**

Idle-skip already removed most “waiting” from the measurement. The remaining
~2.1× is almost entirely SH4 execute cost: handler-table dispatch, operand
loads, and the lack of host register allocation / block-local scheduling that
`core/rec-ARM64` and `core/rec-x64` already produce under `DYNAREC_JIT`.

A native path that covers the hot set should buy that factor: JIT itself is
already ~2.8× this jitless build on the same bench. Profile-guided AOT of the
hot SHIL blocks does not need to match JIT on cold code; it needs enough hot
coverage that the weighted average clears 75%. Rough coverage math:

- If AOT blocks run at ~1.0× JIT and the VM keeps ~0.35× JIT, then with hot
  fraction `c` of host time spent in AOT blocks:
  `0.35 + c × (1.0 − 0.35) ≥ 0.75` → `c ≥ 0.615`.
- So roughly ≥60% of SH4 host time in AOT-covered blocks is enough if those
  blocks are near-JIT speed. First slice must measure coverage, not assume it.

## WHAT_TO_AOT

Candidates:

1. **SH4 opcode handlers as C functions** (call-threaded interpreter). Close to
   what the SHIL VM already is (compact cells + handler table). Will not buy
   ~2.1× after idle-skip; register traffic and call overhead remain.
2. **Hot-path templates** (match SHIL sequences → fixed native stubs). Helps
   microkernels; poor coverage of Crazy Taxi’s real block mix; still needs a
   glue interpreter; hard to keep correct under SSA folds.
3. **Whole SHIL blocks** (output of `dec_DecodeBlock` → `AnalyseBlock`): one
   native function per guest PC range, same grain as today’s JIT and SHIL VM
   blocks.

**AOT_UNIT: SHIL_block**

AOT the same `RuntimeBlockInfo` grain the dynarec already uses: guest PC,
`oplist` after SSA, `guest_cycles`, exit kind, SMC extent. That reuses the
existing frontend and lines up 1:1 with `bm` / `irJumpTable` lookup.

## WHEN

Options:

| Option | Closes ~2.1× gap? | App Store legal? |
|--------|-------------------|------------------|
| Offline at our CI/build for profiled titles → native blobs linked into the app | Yes, if coverage is high | Yes |
| Host-side ahead-of-play compiler writing a cache of **non-executable** IR that a fixed interpreter runs | No (this is today’s SHIL VM) | Yes |
| Runtime dynarec into RWX / `MAP_JIT` on device | Yes | No |

**EMIT_TIME: offline_at_our_build**

**SHIP_FORM: native_code_blobs_linked_into_app**

**FALLBACK: SHIL_VM_for_miss_and_SMC**

Emit on a developer/CI host (macOS/Linux with `FEAT_SHREC == DYNAREC_JIT`),
not on the player’s device. Ship the resulting machine code as normal object
files / assembly included in the iOS (and optionally macOS jitless) binary.
Anything not in the AOT table keeps using the existing SHIL VM.

## HOW_PIPELINE

Existing on-device / JIT path today:

```
SH4 bytes → decoder (dec_DecodeBlock) → AnalyseBlock (SSA)
         → [JIT] Sh4Dynarec::compile → RWX CodeCache
         → [IR ] shilVmEncode → ShilVmCell[] → handler table
```

App-Store AOT path (build machine):

```
1. PROFILE (JIT or IR build, Crazy Taxi gameplay savestate bench)
     run with a block-hit counter; dump per hot PC:
       { vaddr, phys, sh4_bytes, sh4_hash, fpu_cfg, oplist/SHIL,
         guest_cycles, BlockType, BranchBlock, NextBlock }

2. OFFLINE EMIT (host tool, FEAT_SHREC=JIT backends, no RWX needed for ship)
     for each dumped block:
       rebuild RuntimeBlockInfo from the dump (or re-decode from sh4_bytes
       and verify hash + oplist match)
       call the existing arm64 / x64 Sh4Dynarec::compile against a
       relocatable buffer (same rec-ARM64 / rec-x64 code that JIT uses)
       serialise to .S / .o with:
         - entry symbol aot_sh4_<vaddr>_<hash>
         - embedded metadata: guest hash, size, exit targets as relocs
         - prologue SMC compare (same idea as CheckBlock / shilVmGuestMatches)
         - epilogue that returns to a fixed AOT dispatcher (no runtime link stubs
           that patch RX memory)

3. LINK into the App Store binary
     AOT objects → __TEXT
     readonly table: sorted (vaddr, hash) → function pointer + meta

4. RUN (TARGET_SHIL_VM on device)
     on miss in irJumpTable / before shilVm compile:
       look up AOT table by PC
       if hash matches guest RAM/ROM bytes → call native entry
       else → existing SHIL VM compile/execute
```

Reuse, do not fork: `decoder.cpp`, `AnalyseBlock` / `ssa.cpp`,
`core/rec-ARM64/rec_arm64.cpp`, `core/rec-x64/rec_x64.cpp`, and the
`Sh4Dynarec` interface in `ngen.h`. The new work is (a) a dump format + host
tool, (b) a relocatable “emit to object” mode that avoids runtime
`rdv_LinkBlock` patching, (c) a readonly AOT table + dispatcher hooked beside
the SHIL VM main loop.

Block linking: JIT rewrites jump sites at runtime (`rdv_LinkBlock`). AOT cannot
patch `__TEXT` on device. First cut: every AOT block exit returns to a fixed
dispatcher that looks up the next PC (AOT hit or VM). Later: only link exits
whose targets are also AOT-resident, using compile-time-known relative
branches encoded offline.

Memory fast path: keep the SHIL VM / JIT split between RAM direct access and
handler calls; AOT emit should prefer the same `rdv_readMemImmediate` /
canonical helpers the JIT already uses, without exception-based rewrite that
needs W^X.

## SMC_INVALIDATION

Guest self-modifying code must stay correct with **zero** runtime emit.

**SMC_STRATEGY: hash_check_then_fallback_to_SHIL_VM_no_reemit**

Mechanics (mirror what `shil_vm_smc.h` and ARM64 `CheckBlock` already do):

1. Every AOT entry stores `smcAddr`, `smcSize`, and `smcHash` (FNV-1a over the
   guest bytes, same as `shilVmHashBytes`).
2. Native prologue re-hashes (or compares words against immediates baked at
   emit time, as JIT `CheckBlock` does) before executing the body.
3. On mismatch: return a failure code to the dispatcher; **discard that AOT
   binding for the session** (or until the guest bytes again match a shipped
   hash); compile/run via SHIL VM. Never write new native opcodes.
4. Page writes that invalidate JIT blocks today (`bm_RamWriteAccess`) also
   clear AOT “armed” bits for PCs on that page so the next entry re-checks
   the hash instead of trusting a stale arming flag.
5. Optional later: ship N hash variants for known SMC sites; still no emit.

Read-only boot ROM / protected pages can skip the hash for those AOT entries
the same way `RuntimeBlockInfo::read_only` skips checks today.

## RECOMMENDATION

**CHOSEN_APPROACH: offline_profile_guided_SHIL_block_AOT_to_native_text**

Ship profile-guided, build-time native translations of hot SHIL blocks for the
titles we care about (Crazy Taxi first for the bench), linked as ordinary
binary text, with SHIL VM fallback for cold code and SMC. Do not invest further
in interpreter micro-opts as the primary path to 75%; do not pursue on-device
caches of non-executable IR as a performance strategy (already done).

## FIRST_IMPLEMENTATION_SLICE

**FIRST_SLICE: aot_table_lookup_plus_offline_arm64_emit_of_top_hot_blocks**

Concrete first cut (implementation slice after this investigation):

1. **Dump**: under `DYNAREC_IR` or JIT, during
   `GameBootBench.DISABLED_GameplayFramePace` for Crazy Taxi, record the top
   N blocks by visit count as
   `{pc, sh4_hash, sh4_bytes, guest_cycles, BlockType, targets}` into a
   checked-in or CI artifact under the dynarec tree scope agreed next.
2. **Emit**: host-only tool that feeds those blocks through `rec-ARM64` into
   one `.S` (or `.o`) with SMC prologues and dispatcher returns; no
   `rdv_LinkBlock` patches.
3. **Dispatch**: in the SHIL VM main loop / `compile` path, consult a readonly
   AOT table before bytecode compile; on hash hit call the native entry.
4. **Measure**: same Crazy Taxi headless bench — report fps, AOT hit rate, and
   whether `fps / JIT_fps ≥ 0.75`. If coverage `< ~60%` of host SH4 time,
   expand the dump set before tuning the emitter.

Done criterion for that future slice (not this one): Crazy Taxi gameplay
headless fps ≥ 75% of a same-machine JIT run, with Instruments/`vmmap`
confirming no RWX/`MAP_JIT` in the App Store build.

## REJECTED_ALTERNATIVES

- **Further SHIL VM tuning alone** — owner already rejected; idle-skip landed
  at ~35%; another ~2.1× from dispatch tweaks is unrealistic.
- **Ahead-of-play non-executable IR cache** — already the SHIL VM model; does
  not close the gap.
- **On-device dynarec / MAP_JIT** — App Store illegal for this product shape.
- **AOT of individual SH4 opcodes only** — insufficient speedup; wrong grain
  versus existing `AnalyseBlock` + host regalloc.
- **Template-only JIT without whole-block AOT** — weak coverage; still needs a
  slow fallback for the long tail that dominates until coverage is high.
