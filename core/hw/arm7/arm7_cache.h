#pragma once

#include "types.h"

namespace aica::arm
{

struct Arm7ICacheStats
{
	u32 hits = 0;
	u32 misses = 0;
	u32 invalidates = 0;
};

void icacheReset();
void icacheInvalidate(u32 addr, u32 size);
u32 icacheFetch(u32 pc, u32 opcode_from_memory);
Arm7ICacheStats icacheStats();

// SH4 / G2 DMA writes AICA wave RAM without going through ARM writeMem.
inline void icacheOnSh4Write(u32 sh4_addr, u32 size)
{
	if (size == 0)
		return;
	const u32 p = sh4_addr & 0x3FFFFFFF;
	const u32 a0 = p & 0x01FFFFFF;
	u32 arm_addr;
	if (a0 >= 0x00800000 && a0 < 0x01000000)
		arm_addr = a0 & ARAM_MASK;
	else if (p >= 0x20000000 && p < 0x20800000)
		arm_addr = (p - 0x20000000) & ARAM_MASK;
	else
		return;
	icacheInvalidate(arm_addr, size);
}

} // namespace aica::arm
