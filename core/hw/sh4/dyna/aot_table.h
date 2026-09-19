#pragma once

#include "types.h"

#if FEAT_SHREC == DYNAREC_IR

#include <cstddef>

// Readonly AOT lookup: guest PC + fpscr cfg bits → optional native entry + SMC hash.
// Empty/stub table is the App Store default; miss falls back to the SHIL VM.
// Native entries are ordinary .text function pointers linked at build time — never
// allocated or written at runtime.

using AotNativeEntry = int (*)(void *sh4ctx); // 0 = ran; nonzero = SMC fail inside prologue

struct AotTableEntry
{
	u32 guestPc = 0;
	u32 cfgBits = 0;          // fpscr.full key (same grain as RuntimeBlockInfo::fpu_cfg)
	u32 expectedSmcHash = 0;  // FNV-1a over guest SH4 bytes (shilVmHashBytes)
	u32 smcSize = 0;          // bytes covered by expectedSmcHash (0 = no SMC check)
	AotNativeEntry nativeEntry = nullptr;
};

enum class AotDispatch : u8
{
	Miss = 0,         // not in table → SHIL VM
	Hit = 1,          // table hit and live hash matches → call nativeEntry
	SmcMismatch = 2,  // table hit but guest bytes disagree → SHIL VM, never re-emit
};

// Shipped table (may be empty). Entries must be sorted by (guestPc, cfgBits).
inline const AotTableEntry *g_aotTable = nullptr;
inline size_t g_aotTableCount = 0;

inline void aotSetTableForTest(const AotTableEntry *entries, size_t count)
{
	g_aotTable = entries;
	g_aotTableCount = count;
}

inline void aotClearTableForTest()
{
	g_aotTable = nullptr;
	g_aotTableCount = 0;
}

inline const AotTableEntry *aotLookup(u32 guestPc, u32 cfgBits)
{
	const AotTableEntry *tab = g_aotTable;
	const size_t n = g_aotTableCount;
	if (tab == nullptr || n == 0)
		return nullptr;

	size_t lo = 0;
	size_t hi = n;
	while (lo < hi)
	{
		const size_t mid = lo + (hi - lo) / 2;
		const AotTableEntry &e = tab[mid];
		if (e.guestPc < guestPc || (e.guestPc == guestPc && e.cfgBits < cfgBits))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo < n && tab[lo].guestPc == guestPc && tab[lo].cfgBits == cfgBits)
		return &tab[lo];
	return nullptr;
}

// Classify a candidate AOT binding given the live guest hash (caller hashed RAM/ROM).
inline AotDispatch aotClassify(u32 guestPc, u32 cfgBits, u32 liveSmcHash)
{
	const AotTableEntry *e = aotLookup(guestPc, cfgBits);
	if (e == nullptr || e->nativeEntry == nullptr)
		return AotDispatch::Miss;
	if (e->smcSize != 0 && e->expectedSmcHash != liveSmcHash)
		return AotDispatch::SmcMismatch;
	return AotDispatch::Hit;
}

inline AotNativeEntry aotNativeOnHit(u32 guestPc, u32 cfgBits, u32 liveSmcHash)
{
	if (aotClassify(guestPc, cfgBits, liveSmcHash) != AotDispatch::Hit)
		return nullptr;
	return aotLookup(guestPc, cfgBits)->nativeEntry;
}

// Counters for tests / profiling (compile-path classification only).
inline u32 g_aotMissCount = 0;
inline u32 g_aotHitCount = 0;
inline u32 g_aotSmcMismatchCount = 0;

inline void aotResetDispatchCounts()
{
	g_aotMissCount = 0;
	g_aotHitCount = 0;
	g_aotSmcMismatchCount = 0;
}

inline void aotNoteDispatch(AotDispatch d)
{
	switch (d)
	{
	case AotDispatch::Miss: g_aotMissCount++; break;
	case AotDispatch::Hit: g_aotHitCount++; break;
	case AotDispatch::SmcMismatch: g_aotSmcMismatchCount++; break;
	}
}

#endif // FEAT_SHREC == DYNAREC_IR
