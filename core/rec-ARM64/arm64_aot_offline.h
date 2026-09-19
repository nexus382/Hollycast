#pragma once

// RELOCATABLE OFFLINE emit mode for App-Store AOT (build-time native .text).
// Not a second IR. Not the runtime DYNAREC_JIT path (that stays RWX CodeCache + Relink).
//
// ABI matches core/hw/sh4/dyna/aot_table.h AotNativeEntry:
//   int (*)(void *sh4ctx)  — 0 = ran (next PC written); nonzero = SMC fail → dispatcher/VM
//
// Emitted blocks: PIC/relocatable, table-based mem helpers, SMC prologue, dispatcher
// return (ret) — never rdv_LinkBlock / Relink patching.
//
// The checked-in emit input is aot_offline/bench_fixture.json only.
// Gameplay fps is measured by tests/ GameBootBench, not by a title dump here.

#include "types.h"

#include <cstddef>
#include <string>

struct RuntimeBlockInfo;

extern "C" int aot_offline_smc_check(void *sh4ctx, u32 guestAddr, u32 smcSize, u32 expectedHash);
extern "C" u8 aot_offline_read8(u32 addr);
extern "C" u16 aot_offline_read16(u32 addr);
extern "C" u32 aot_offline_read32(u32 addr);
extern "C" u64 aot_offline_read64(u32 addr);
extern "C" void aot_offline_write8(u32 addr, u32 data);
extern "C" void aot_offline_write16(u32 addr, u32 data);
extern "C" void aot_offline_write32(u32 addr, u32 data);
extern "C" void aot_offline_write64(u32 addr, u64 data);

#ifdef AOT_OFFLINE_EMITTER_ONLY
struct Arm64AotOfflineBlockDesc
{
	u32 guestPc = 0;
	u32 sh4Hash = 0;
	u32 smcAddr = 0;
	u32 smcSize = 0;
	u32 guestCycles = 0;
	u32 blockType = 0;
	u32 branchTarget = 0xffffffffu;
	u32 nextTarget = 0xffffffffu;
};

// Host-only: write one relocatable block as assemblable .S
// (entry symbol aot_sh4_<pc>_<hash>). Returns false if the guest bytes
// cannot be emitted or the path cannot be written.
bool arm64AotEmitRelocatableOffline(const Arm64AotOfflineBlockDesc &desc,
	const u8 *sh4Bytes, u32 sh4Len, const std::string &asmPath);

// Build-time only: emit one validated synthetic fixture, independent of its path.
// Returns the .S path, or empty on invalid input or an I/O failure.
std::string arm64AotEmitBenchFixture(const std::string &fixtureJsonPath, const std::string &asmDir);

#endif // AOT_OFFLINE_EMITTER_ONLY
