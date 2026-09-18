#include "gtest/gtest.h"
#include "hw/arm7/arm7_cache.h"

TEST(Arm7ICache, MissThenHit)
{
	aica::arm::icacheReset();
	EXPECT_EQ(aica::arm::icacheFetch(0x1000, 0xe3a00001), 0xe3a00001u);
	EXPECT_EQ(aica::arm::icacheFetch(0x1000, 0xdeadbeef), 0xe3a00001u);
	const auto stats = aica::arm::icacheStats();
	EXPECT_EQ(stats.misses, 1u);
	EXPECT_EQ(stats.hits, 1u);
}

TEST(Arm7ICache, InvalidateDropsLine)
{
	aica::arm::icacheReset();
	aica::arm::icacheFetch(0x2000, 0xe1a00000);
	aica::arm::icacheInvalidate(0x2000, 4);
	EXPECT_EQ(aica::arm::icacheFetch(0x2000, 0xe2800001), 0xe2800001u);
	const auto stats = aica::arm::icacheStats();
	EXPECT_EQ(stats.invalidates, 1u);
	EXPECT_EQ(stats.misses, 2u);
}

TEST(Arm7ICache, InvalidateRangeCoversNeighbors)
{
	aica::arm::icacheReset();
	aica::arm::icacheFetch(0x3000, 1);
	aica::arm::icacheFetch(0x3004, 2);
	aica::arm::icacheInvalidate(0x3000, 8);
	EXPECT_EQ(aica::arm::icacheFetch(0x3000, 3), 3u);
	EXPECT_EQ(aica::arm::icacheFetch(0x3004, 4), 4u);
}

TEST(Arm7ICache, Sh4WriteDropsCachedOpcode)
{
	settings.platform.aram_size = 2_MB;
	settings.platform.aram_mask = settings.platform.aram_size - 1;
	aica::arm::icacheReset();
	aica::arm::icacheFetch(0x1000, 0x11111111);
	aica::arm::icacheOnSh4Write(0x00801000, 4);
	EXPECT_EQ(aica::arm::icacheFetch(0x1000, 0x22222222), 0x22222222u);
}

TEST(Arm7ICache, Sh4DmaWriteDropsCachedRange)
{
	settings.platform.aram_size = 2_MB;
	settings.platform.aram_mask = settings.platform.aram_size - 1;
	aica::arm::icacheReset();
	aica::arm::icacheFetch(0x2000, 1);
	aica::arm::icacheFetch(0x2004, 2);
	aica::arm::icacheOnSh4Write(0xA0802000, 8);
	EXPECT_EQ(aica::arm::icacheFetch(0x2000, 3), 3u);
	EXPECT_EQ(aica::arm::icacheFetch(0x2004, 4), 4u);
}

TEST(Arm7ICache, ResetDropsCachedOpcode)
{
	aica::arm::icacheReset();
	aica::arm::icacheFetch(0x4000, 0xaaaa);
	aica::arm::icacheReset();
	EXPECT_EQ(aica::arm::icacheFetch(0x4000, 0xbbbb), 0xbbbbu);
}
