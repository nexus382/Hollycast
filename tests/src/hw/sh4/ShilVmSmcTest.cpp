#include "gtest/gtest.h"
#include "hw/sh4/dyna/shil_vm_smc.h"
#include <cstring>
#include <vector>

TEST(ShilVmSmc, HashChangesWhenBytesChange)
{
	u8 ram[64] = {};
	ram[0] = 0x01;
	ram[1] = 0x70; // add #1, r0 little-endian-ish payload
	const u32 h0 = shilVmHashRam(ram, 64, 63, 0x8C000000, 6);
	ram[0] = 0x02;
	const u32 h1 = shilVmHashRam(ram, 64, 63, 0x8C000000, 6);
	EXPECT_NE(h0, h1);
	EXPECT_FALSE(shilVmGuestMatches(ram, 64, 63, 0x8C000000, 6, h0));
	EXPECT_TRUE(shilVmGuestMatches(ram, 64, 63, 0x8C000000, 6, h1));
}

TEST(ShilVmSmc, SamePageDataDoesNotChangeBlockHash)
{
	std::vector<u8> ram(4096, 0);
	ram[0] = 0x01;
	ram[2] = 0xfe;
	ram[4] = 0x09;
	const u32 h0 = shilVmHashRam(ram.data(), 4096, 4095, 0x8C000000, 6);
	ram[0x800] = 0xff;
	EXPECT_TRUE(shilVmGuestMatches(ram.data(), 4096, 4095, 0x8C000000, 6, h0));
}

TEST(ShilVmSmc, IsolatedRanges)
{
	std::vector<u8> ram(8192, 0);
	const u32 a = shilVmHashRam(ram.data(), 8192, 8191, 0x8C000000, 8);
	const u32 b = shilVmHashRam(ram.data(), 8192, 8191, 0x8C001000, 8);
	EXPECT_EQ(a, b);
	ram[0x1000] = 1;
	EXPECT_TRUE(shilVmGuestMatches(ram.data(), 8192, 8191, 0x8C000000, 8, a));
	EXPECT_FALSE(shilVmGuestMatches(ram.data(), 8192, 8191, 0x8C001000, 8, b));
}

TEST(ShilVmSmc, EmptyOrOobIsStableZero)
{
	u8 ram[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	EXPECT_EQ(shilVmHashRam(ram, 8, 7, 0xA0000000, 4), 0u);
	EXPECT_EQ(shilVmHashBytes(nullptr, 0), SHIL_VM_SMC_FNV);
}
