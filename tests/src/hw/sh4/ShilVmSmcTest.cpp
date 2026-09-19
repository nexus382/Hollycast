#include "gtest/gtest.h"
#include "hw/sh4/dyna/shil_vm_smc.h"
#include "hw/sh4/dyna/shil_vm.h"
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

// Independent guard contract: compare every byte, including unaligned tails,
// without invalidating on unrelated writes or retaining an invalid snapshot.
TEST(ShilVmSmc, ExactSnapshotChecksEveryByte)
{
	std::vector<u8> ram(4096, 0x09);
	ShilVmCodeGuard guard;
	for (u32 size : { 1u, 2u, 6u, 15u, 16u, 17u, 63u, 256u })
	{
		guard.capture(ram.data(), 4096, 4095, 0x8C000003, size);
		ASSERT_TRUE(guard.matches(ram.data(), 4096));
		for (u32 i = 0; i < size; i++)
		{
			ram[3 + i] ^= 0x80;
			EXPECT_FALSE(guard.matches(ram.data(), 4096)) << size << ":" << i;
			ram[3 + i] ^= 0x80;
		}
		ram[2] ^= 1;
		ram[3 + size] ^= 1;
		EXPECT_TRUE(guard.matches(ram.data(), 4096));
	}
}

TEST(ShilVmSmc, ExactSnapshotRejectsInvalidAndReplacedStorage)
{
	u8 ram[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ShilVmCodeGuard guard;
	EXPECT_FALSE(guard.matches(ram, 8));
	guard.capture(ram, 8, 7, 0xAC000006, 2);
	EXPECT_TRUE(guard.matches(ram, 8));
	EXPECT_FALSE(guard.matches(nullptr, 8));
	EXPECT_FALSE(guard.matches(ram, 7));
	u8 replacement[8] = {};
	EXPECT_FALSE(guard.matches(replacement, 8));
	memcpy(replacement, ram, 8);
	EXPECT_TRUE(guard.matches(replacement, 8));
	for (u32 size : { 0u, 9u, 0xffffffffu })
	{
		guard.capture(ram, 8, 7, 0x8C000006, size);
		EXPECT_FALSE(guard.matches(ram, 8));
	}
	guard.capture(ram, 8, 7, 0x8C000000, 8);
	guard.capture(ram, 8, 7, 0xA0000000, 8);
	EXPECT_FALSE(guard.matches(ram, 8));
	guard.capture(nullptr, 8, 7, 0x8C000000, 8);
	EXPECT_FALSE(guard.matches(ram, 8));
}

TEST(ShilVmSmc, ExactSnapshotRejectsHashCollision)
{
	// Synthetic byte strings with identical FNV-1a hashes. They are not game data.
	u8 ram[8] = { 0x12, 0xd3, 0x71, 0x36, 0xc2, 0xe2, 0xf0, 0xc9 };
	const u8 collision[8] = { 0x9d, 0xd4, 0x49, 0xff, 0x44, 0x08, 0x1d, 0x85 };
	ASSERT_EQ(shilVmHashBytes(ram, 8), shilVmHashBytes(collision, 8));
	ShilVmCodeGuard guard;
	guard.capture(ram, 8, 7, 0x8C000000, 8);
	memcpy(ram, collision, 8);
	EXPECT_FALSE(guard.matches(ram, 8));
	guard.capture(ram, 8, 7, 0x8C000000, 8);
	EXPECT_TRUE(guard.matches(ram, 8));
}

TEST(ShilVmSmc, ExactSnapshotWrapsAtPhysicalRamMask)
{
	// The backing allocation can exceed the masked guest RAM window.
	u8 ram[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	ShilVmCodeGuard guard;
	for (u32 addr : { 0x8C000006u, 0xAC000006u })
	{
		guard.capture(ram, 16, 7, addr, 5);
		ASSERT_TRUE(guard.matches(ram, 16));
		for (u32 offset : { 6u, 7u, 0u, 1u, 2u })
		{
			ram[offset] ^= 0x80;
			EXPECT_FALSE(guard.matches(ram, 16)) << offset;
			ram[offset] ^= 0x80;
			EXPECT_TRUE(guard.matches(ram, 16));
		}
		ram[3] ^= 1;
		ram[8] ^= 1;
		EXPECT_TRUE(guard.matches(ram, 16));
		EXPECT_FALSE(guard.matches(ram, 7));
	}
	// Covering the masked window more than once still checks every physical byte.
	guard.capture(ram, 16, 7, 0x8C000006, 16);
	ASSERT_TRUE(guard.matches(ram, 16));
	for (u32 offset = 0; offset < 8; offset++)
	{
		ram[offset] ^= 1;
		EXPECT_FALSE(guard.matches(ram, 16));
		ram[offset] ^= 1;
	}
	// The physical RAM boundary itself is also a legal wrap, not a failed capture.
	guard.capture(ram, 8, 7, 0xAC000007, 3);
	ASSERT_TRUE(guard.matches(ram, 8));
	ram[0] ^= 1;
	EXPECT_FALSE(guard.matches(ram, 8));
	guard.capture(ram, 8, 7, 0xAC000007, 3);
	EXPECT_TRUE(guard.matches(ram, 8));
}
