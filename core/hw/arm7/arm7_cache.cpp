#include "arm7_cache.h"

#include <cstring>

namespace aica::arm
{

constexpr u32 ICACHE_LINES = 4096;
constexpr u32 ICACHE_MASK = ICACHE_LINES - 1;

struct Arm7ICacheLine
{
	u32 tag = ~0u;
	u32 opcode = 0;
};

static Arm7ICacheLine s_lines[ICACHE_LINES];
static Arm7ICacheStats s_stats;

void icacheReset()
{
	for (auto& line : s_lines)
	{
		line.tag = ~0u;
		line.opcode = 0;
	}
	s_stats = {};
}

void icacheInvalidate(u32 addr, u32 size)
{
	if (size == 0)
		return;
	s_stats.invalidates++;
	const u32 start = addr & ~3u;
	const u32 end = (addr + size + 3u) & ~3u;
	for (u32 pc = start; pc < end; pc += 4)
	{
		Arm7ICacheLine& line = s_lines[(pc >> 2) & ICACHE_MASK];
		if (line.tag == pc)
			line.tag = ~0u;
	}
}

u32 icacheFetch(u32 pc, u32 opcode_from_memory)
{
	Arm7ICacheLine& line = s_lines[(pc >> 2) & ICACHE_MASK];
	if (line.tag == pc)
	{
		s_stats.hits++;
		return line.opcode;
	}
	s_stats.misses++;
	line.tag = pc;
	line.opcode = opcode_from_memory;
	return opcode_from_memory;
}

Arm7ICacheStats icacheStats()
{
	return s_stats;
}

} // namespace aica::arm
