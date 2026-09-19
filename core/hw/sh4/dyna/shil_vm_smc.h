#pragma once

#include "types.h"
#include "shil_vm_mem.h"

// FNV-1a over the guest SH4 bytes of one block. Page-version SMC was too coarse:
// data writes on the same 4 KB page invalidated unrelated code and flushed the cache.
constexpr u32 SHIL_VM_SMC_FNV = 2166136261u;

inline u32 shilVmHashBytes(const u8 *p, u32 n)
{
	u32 h = SHIL_VM_SMC_FNV;
	if (p == nullptr || n == 0)
		return h;
	for (u32 i = 0; i < n; i++)
	{
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

inline u32 shilVmHashRam(const u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u32 size)
{
	if (!shilVmRamInRange(addr, size, ram_size, ram_mask) || ram == nullptr)
		return 0;
	return shilVmHashBytes(ram + shilVmRamOffset(addr, ram_mask), size);
}

inline bool shilVmGuestMatches(const u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u32 size, u32 hash)
{
	return shilVmHashRam(ram, ram_size, ram_mask, addr, size) == hash;
}
