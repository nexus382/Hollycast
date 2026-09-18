#include "gtest/gtest.h"
#include "build.h"
#include "hw/sh4/dyna/shil_vm_bytecode.h"
#include "cfg/option.h"

TEST(ShilVmTiming, SuperopsSitAfterShopMax)
{
	EXPECT_GE((int)vmop_end, (int)shop_max);
	EXPECT_EQ((int)vmop_mov_imm, (int)shop_max);
}

TEST(ShilVmTiming, AutoSkipDefaultsOn)
{
#if defined(TARGET_SHIL_VM)
	EXPECT_GE(config::AutoSkipFrame.get(), 1);
#else
	GTEST_SKIP() << "AutoSkipFrame default 1 is a TARGET_SHIL_VM setting";
#endif
}

#if FEAT_SHREC == DYNAREC_IR
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/dyna/shil_vm.h"
#include "hw/sh4/dyna/blockmanager.h"

class ShilVmExecuteTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		shilVmResetCompileCount();
	}

	void writeOp(u32 pc, u16 op)
	{
		addrspace::write16(pc, op);
	}

	void writeLoop(u32 pc, u16 addImm)
	{
		writeOp(pc, (u16)(0x7000 | (addImm & 0xff))); // add #imm, r0
		writeOp(pc + 2, 0xAFFE);                       // bra self
		writeOp(pc + 4, 0x0009);                       // nop
	}

	Sh4Context *ctx = nullptr;
	static constexpr u32 START_PC = 0xAC000000;
};

TEST_F(ShilVmExecuteTest, RunsCachedAddWithoutUnderclock)
{
	ctx->pc = START_PC;
	ctx->r[0] = 1;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeOp(START_PC, 0x7001);      // add #1, r0
	writeOp(START_PC + 2, 0xAFFE);  // bra self
	writeOp(START_PC + 4, 0x0009);  // nop delay
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 2u);
}

// Successors are compiled when they are first reached, never speculatively:
// decoding a block the guest has not executed yet can raise an SH4 exception
// for code that may not even be written to RAM at that point.
TEST_F(ShilVmExecuteTest, CompilesStaticSuccessorOnlyWhenReached)
{
	const u32 next = START_PC + 8;
	ctx->pc = START_PC;
	ctx->r[0] = 0;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeOp(START_PC, 0x7001);      // add #1, r0
	writeOp(START_PC + 2, 0xA001);  // bra next (disp +1)
	writeOp(START_PC + 4, 0x0009);
	writeLoop(next, 1);

	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 1u);
	EXPECT_EQ(ctx->pc, next);
	EXPECT_EQ(bm_GetIrBlock(next), nullptr);
	const u32 afterFirst = shilVmCompileCount();
	EXPECT_EQ(afterFirst, 1u);

	ctx->cycle_counter = SH4_TIMESLICE;
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 2u);
	EXPECT_NE(bm_GetIrBlock(next), nullptr);
	const u32 afterSecond = shilVmCompileCount();
	EXPECT_EQ(afterSecond, 2u);

	// The successor is now cached, so entering it again must not recompile.
	ctx->pc = next;
	ctx->cycle_counter = SH4_TIMESLICE;
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 3u);
	EXPECT_EQ(shilVmCompileCount(), afterSecond);
}

TEST_F(ShilVmExecuteTest, SamePageDataWriteDoesNotRecompile)
{
	ctx->pc = START_PC;
	ctx->r[0] = 0;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeLoop(START_PC, 1);
	ASSERT_TRUE(shilVmStepBlock());
	const u32 compiled = shilVmCompileCount();
	EXPECT_EQ(ctx->r[0], 1u);

	addrspace::write32(START_PC + 0x800, 0xDEADBEEF);
	ctx->pc = START_PC;
	ctx->cycle_counter = SH4_TIMESLICE;
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 2u);
	EXPECT_EQ(shilVmCompileCount(), compiled);
}

TEST_F(ShilVmExecuteTest, OpcodePatchRecompilesOnlyThatBlock)
{
	const u32 other = START_PC + 0x1000;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeLoop(START_PC, 1);
	writeLoop(other, 1);

	ctx->pc = START_PC;
	ctx->r[0] = 0;
	ASSERT_TRUE(shilVmStepBlock());
	ctx->pc = other;
	ASSERT_TRUE(shilVmStepBlock());
	const u32 compiled = shilVmCompileCount();
	EXPECT_EQ(ctx->r[0], 2u);

	writeOp(START_PC, 0x7005); // add #5, r0
	ctx->pc = START_PC;
	ctx->cycle_counter = SH4_TIMESLICE;
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 7u);
	EXPECT_EQ(shilVmCompileCount(), compiled + 1);

	ctx->pc = other;
	ctx->cycle_counter = SH4_TIMESLICE;
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 8u);
	EXPECT_EQ(shilVmCompileCount(), compiled + 1);
}
#endif
