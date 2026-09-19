#include "gtest/gtest.h"
#include "hw/sh4/dyna/shil_vm_chain.h"

TEST(ShilVmChain, StaticJumpUsesBranchWhenPcMatches)
{
	ShilVmChain chain;
	chain.branch = (void *)0x100;
	chain.branchPc = 0x8c000100;
	void *got = shilVmSelectSuccessor(ShilVmExitKind::StaticJump, false, 0x8c000100, chain);
	EXPECT_EQ(got, chain.branch);
}

TEST(ShilVmChain, StaticJumpMissesWhenPcDiffers)
{
	ShilVmChain chain;
	chain.branch = (void *)0x100;
	chain.branchPc = 0x8c000100;
	EXPECT_EQ(shilVmSelectSuccessor(ShilVmExitKind::StaticJump, false, 0x8c000200, chain), nullptr);
}

TEST(ShilVmChain, CondTakenUsesBranch)
{
	ShilVmChain chain;
	chain.branch = (void *)1;
	chain.next = (void *)2;
	chain.branchPc = 0x20;
	chain.nextPc = 0x30;
	EXPECT_EQ(shilVmSelectSuccessor(ShilVmExitKind::Cond1, true, 0x20, chain), chain.branch);
	EXPECT_EQ(shilVmSelectSuccessor(ShilVmExitKind::Cond1, false, 0x30, chain), chain.next);
}

TEST(ShilVmChain, ExitPcMatchesKind)
{
	EXPECT_EQ(shilVmExitPc(ShilVmExitKind::StaticJump, false, 0x10, 0x20, 0x30), 0x20u);
	EXPECT_EQ(shilVmExitPc(ShilVmExitKind::Cond0, true, 0x10, 0x20, 0x30), 0x20u);
	EXPECT_EQ(shilVmExitPc(ShilVmExitKind::Cond0, false, 0x10, 0x20, 0x30), 0x10u);
	EXPECT_EQ(shilVmExitPc(ShilVmExitKind::Dynamic, false, 0x10, 0x20, 0x99), 0x99u);
	EXPECT_EQ(shilVmExitPc(ShilVmExitKind::StaticIntr, false, 0x10, 0x20, 0x30), 0x10u);
}

TEST(ShilVmChain, MapsBlockEndTypes)
{
	EXPECT_EQ(shilVmExitKind(BET_StaticJump), ShilVmExitKind::StaticJump);
	EXPECT_EQ(shilVmExitKind(BET_StaticCall), ShilVmExitKind::StaticCall);
	EXPECT_EQ(shilVmExitKind(BET_StaticIntr), ShilVmExitKind::StaticIntr);
	EXPECT_EQ(shilVmExitKind(BET_Cond_0), ShilVmExitKind::Cond0);
	EXPECT_EQ(shilVmExitKind(BET_Cond_1), ShilVmExitKind::Cond1);
	EXPECT_EQ(shilVmExitKind(BET_DynamicJump), ShilVmExitKind::Dynamic);
	EXPECT_EQ(shilVmExitKind(BET_DynamicCall), ShilVmExitKind::Dynamic);
	EXPECT_EQ(shilVmExitKind(BET_DynamicRet), ShilVmExitKind::Dynamic);
	EXPECT_EQ(shilVmExitKind(BET_DynamicIntr), ShilVmExitKind::DynamicIntr);
}
