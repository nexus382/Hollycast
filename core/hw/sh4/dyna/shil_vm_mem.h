#pragma once

#include "types.h"

#include <cstring>

// Area 3 system RAM, excluding P4. Same predicate as IsOnRam() without pulling sh4_mem.h.
inline bool shilVmIsRam(u32 addr)
{
	return ((addr >> 26) & 7) == 3 && ((addr >> 29) & 7) != 7;
}

inline u32 shilVmRamOffset(u32 addr, u32 ram_mask)
{
	return addr & ram_mask;
}

inline bool shilVmRamInRange(u32 addr, u32 size, u32 ram_size, u32 ram_mask)
{
	if (!shilVmIsRam(addr) || size == 0)
		return false;
	const u32 off = shilVmRamOffset(addr, ram_mask);
	return off + size <= ram_size;
}

inline bool shilVmRamRead(const u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u32 size, u32 *out)
{
	if (!shilVmRamInRange(addr, size, ram_size, ram_mask) || out == nullptr)
		return false;
	const u8 *ptr = ram + shilVmRamOffset(addr, ram_mask);
	switch (size)
	{
	case 1:
		*out = (u32)(s32)(s8)*ptr;
		return true;
	case 2:
	{
		u16 value;
		memcpy(&value, ptr, 2);
		*out = (u32)(s32)(s16)value;
		return true;
	}
	case 4:
	{
		u32 value;
		memcpy(&value, ptr, 4);
		*out = value;
		return true;
	}
	default:
		return false;
	}
}

inline bool shilVmRamWrite(u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u32 size, u32 value)
{
	if (!shilVmRamInRange(addr, size, ram_size, ram_mask))
		return false;
	memcpy(ram + shilVmRamOffset(addr, ram_mask), &value, size);
	return true;
}

inline bool shilVmRamRead64(const u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u64 *out)
{
	if (!shilVmRamInRange(addr, 8, ram_size, ram_mask) || out == nullptr)
		return false;
	memcpy(out, ram + shilVmRamOffset(addr, ram_mask), 8);
	return true;
}

inline bool shilVmRamWrite64(u8 *ram, u32 ram_size, u32 ram_mask, u32 addr, u64 value)
{
	if (!shilVmRamInRange(addr, 8, ram_size, ram_mask))
		return false;
	memcpy(ram + shilVmRamOffset(addr, ram_mask), &value, 8);
	return true;
}
