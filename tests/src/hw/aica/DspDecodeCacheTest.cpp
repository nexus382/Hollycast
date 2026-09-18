#include "gtest/gtest.h"
#include "hw/aica/dsp.h"
#include <cstring>

TEST(DspDecodeCache, DecodeInstMatchesProgram)
{
	u32 mpro[128 * 4] = {};
	mpro[4] = (0x12 << 9) | 0x100 | (0x34 << 1); // TRA, TWT, TWA
	mpro[5] = 0x8000 | (2 << 13) | (0x10 << 7) | 0x40 | (3 << 1); // XSEL YSEL IRA IWT IWA

	aica::dsp::Instruction one{};
	aica::dsp::DecodeInst(mpro + 4, &one);

	aica::dsp::Instruction prog[128]{};
	aica::dsp::decodeProgram(prog, mpro);

	EXPECT_EQ(prog[1].TRA, one.TRA);
	EXPECT_EQ(prog[1].TWT, one.TWT);
	EXPECT_EQ(prog[1].TWA, one.TWA);
	EXPECT_EQ(prog[1].XSEL, one.XSEL);
	EXPECT_EQ(prog[1].YSEL, one.YSEL);
	EXPECT_EQ(prog[1].IRA, one.IRA);
	EXPECT_EQ(prog[1].IWT, one.IWT);
	EXPECT_EQ(prog[1].IWA, one.IWA);
	EXPECT_EQ(prog[0].TRA, 0);
	EXPECT_FALSE(prog[0].TWT);
}

TEST(DspDecodeCache, EmptyProgramStaysZero)
{
	u32 mpro[128 * 4] = {};
	aica::dsp::Instruction prog[128];
	memset(prog, 0xff, sizeof(prog));
	aica::dsp::decodeProgram(prog, mpro);
	EXPECT_EQ(prog[0].TRA, 0);
	EXPECT_FALSE(prog[127].EWT);
}
