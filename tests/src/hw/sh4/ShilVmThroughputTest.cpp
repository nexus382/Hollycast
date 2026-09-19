#include "gtest/gtest.h"
#include "build.h"

#if FEAT_SHREC == DYNAREC_IR
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/dyna/shil_vm.h"

#include <chrono>
#include <cstdio>
#include <string>

class ShilVmThroughputTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		interp = Get_Sh4Interpreter();
		interp->Init();
	}

	void TearDown() override
	{
		interp->Term();
		delete interp;
	}

	void write16(u32 pc, u16 op)
	{
		addrspace::write16(pc, op);
	}

	void writeAluLoop()
	{
		write16(START_PC, 0x7001);      // add #1, r0
		write16(START_PC + 2, 0xAFFD);  // bra START
		write16(START_PC + 4, 0x0009);  // nop
	}

	void writeRamLoop()
	{
		write16(START_PC, 0x6042);      // mov.l @r4, r0
		write16(START_PC + 2, 0x7001);  // add #1, r0
		write16(START_PC + 4, 0x2402);  // mov.l r0, @r4
		write16(START_PC + 6, 0xAFFB);  // bra START
		write16(START_PC + 8, 0x0009);  // nop
	}

	void resetState(u32 r0)
	{
		addrspace::write32(DATA_ADDR, 0);
		ctx->pc = START_PC;
		ctx->r[0] = r0;
		ctx->r[4] = DATA_ADDR;
		ctx->cycle_counter = 1'000'000'000;
		ctx->CpuRunning = true;
	}

	template <typename Fn>
	double secondsFor(u64 iters, Fn&& fn)
	{
		for (int i = 0; i < WARMUP_ITERS; i++)
			fn();
		const auto t0 = std::chrono::steady_clock::now();
		for (u64 i = 0; i < iters; i++)
			fn();
		const auto t1 = std::chrono::steady_clock::now();
		return std::chrono::duration<double>(t1 - t0).count();
	}

	static void printRow(const char *name, u64 iters, int opsPerIter, double interpSec, double vmSec)
	{
		const double interpMops = (iters * opsPerIter / interpSec) / 1e6;
		const double vmMops = (iters * opsPerIter / vmSec) / 1e6;
		std::printf("  %-8s  interpreter %6.2f Mops/s (%.3fs)   SHIL VM %6.2f Mops/s (%.3fs)   %.2fx\n",
			name, interpMops, interpSec, vmMops, vmSec, vmMops / interpMops);
		fflush(stdout);
	}

	Sh4Context *ctx = nullptr;
	Sh4Executor *interp = nullptr;
	static constexpr u32 START_PC = 0xAC000000;
	static constexpr u32 DATA_ADDR = 0xAC001000;
	static constexpr int WARMUP_ITERS = 64;
	static constexpr u64 ITERS = 40'000'000;
};

TEST_F(ShilVmThroughputTest, AluLoopHostSpeed)
{
	writeAluLoop();

	resetState(0);
	const double interpSec = secondsFor(ITERS, [&] {
		interp->Step(); // add
		interp->Step(); // bra + delay nop
	});

	const u32 interpResult = ctx->r[0];
	EXPECT_EQ(interpResult, ITERS + WARMUP_ITERS);
	EXPECT_EQ(ctx->pc, START_PC);

	resetState(0);
	ASSERT_TRUE(shilVmStepBlock());
	// Compilation executes the block; restore the same starting state.
	resetState(0);
	const double vmSec = secondsFor(ITERS, [&] {
		shilVmStepBlock();
	});

	printRow("ALU", ITERS, 3, interpSec, vmSec);
	EXPECT_EQ(ctx->r[0], ITERS + WARMUP_ITERS);
	EXPECT_EQ(ctx->r[0], interpResult);
	EXPECT_EQ(ctx->pc, START_PC);
}

TEST_F(ShilVmThroughputTest, RamLoopHostSpeed)
{
	writeRamLoop();

	resetState(0);
	const double interpSec = secondsFor(ITERS, [&] {
		interp->Step(); // load
		interp->Step(); // add
		interp->Step(); // store
		interp->Step(); // bra + delay nop
	});

	const u32 interpResult = ctx->r[0];
	EXPECT_EQ(interpResult, ITERS + WARMUP_ITERS);
	EXPECT_EQ(addrspace::read32(DATA_ADDR), interpResult);
	EXPECT_EQ(ctx->pc, START_PC);

	resetState(0);
	ASSERT_TRUE(shilVmStepBlock());
	// Compilation executes the block; restore the same starting state.
	resetState(0);
	const double vmSec = secondsFor(ITERS, [&] {
		shilVmStepBlock();
	});

	printRow("RAM", ITERS, 5, interpSec, vmSec);
	EXPECT_EQ(addrspace::read32(DATA_ADDR), ITERS + WARMUP_ITERS);
	EXPECT_EQ(addrspace::read32(DATA_ADDR), interpResult);
	EXPECT_EQ(ctx->r[0], interpResult);
	EXPECT_EQ(ctx->pc, START_PC);
}
#endif
