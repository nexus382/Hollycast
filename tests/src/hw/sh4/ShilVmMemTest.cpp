#include "gtest/gtest.h"
#include "hw/sh4/dyna/shil_vm_mem.h"
#include <cstring>

TEST(ShilVmMem, DetectsSystemRam)
{
	EXPECT_TRUE(shilVmIsRam(0x8C000000));
	EXPECT_TRUE(shilVmIsRam(0x0C000000));
	EXPECT_FALSE(shilVmIsRam(0xA0000000));
	EXPECT_FALSE(shilVmIsRam(0xE0000000));
}

TEST(ShilVmMem, ReadWriteThroughMask)
{
	u8 ram[16] = {};
	const u32 mask = 15;
	ASSERT_TRUE(shilVmRamWrite(ram, 16, mask, 0x8C000004, 4, 0x11223344));
	u32 value = 0;
	ASSERT_TRUE(shilVmRamRead(ram, 16, mask, 0x8C000004, 4, &value));
	EXPECT_EQ(value, 0x11223344u);
}

TEST(ShilVmMem, SignExtendsSmallReads)
{
	u8 ram[8] = {};
	ram[0] = 0xfe;
	u32 value = 0;
	ASSERT_TRUE(shilVmRamRead(ram, 8, 7, 0x8C000000, 1, &value));
	EXPECT_EQ(value, 0xfffffffeu);
	u16 half = 0x8001;
	memcpy(ram + 2, &half, 2);
	ASSERT_TRUE(shilVmRamRead(ram, 8, 7, 0x8C000002, 2, &value));
	EXPECT_EQ(value, 0xffff8001u);
}

TEST(ShilVmMem, RejectsMmioAndOob)
{
	u8 ram[8] = {};
	u32 value = 0;
	EXPECT_FALSE(shilVmRamRead(ram, 8, 7, 0xA05F8000, 4, &value));
	EXPECT_FALSE(shilVmRamWrite(ram, 8, 7, 0x8C000006, 4, 1));
}

TEST(ShilVmMem, QuadwordRoundTrip)
{
	u8 ram[16] = {};
	ASSERT_TRUE(shilVmRamWrite64(ram, 16, 15, 0x8C000000, 0x0102030405060708ull));
	u64 value = 0;
	ASSERT_TRUE(shilVmRamRead64(ram, 16, 15, 0x8C000000, &value));
	EXPECT_EQ(value, 0x0102030405060708ull);
}
