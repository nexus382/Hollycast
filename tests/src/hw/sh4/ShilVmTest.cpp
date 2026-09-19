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
#include "hw/sh4/dyna/aot_table.h"
#include "hw/sh4/dyna/aot_hot_dump.h"
#include "hw/sh4/dyna/shil_vm_smc.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <string>

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
		aotClearTableForTest();
		aotResetDispatchCounts();
		aotHotSetProfiling(false);
		aotHotReset();
	}

	void TearDown() override
	{
		aotClearTableForTest();
		aotResetDispatchCounts();
		aotHotSetProfiling(false);
		aotHotReset();
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

// A dynamic exit remembers its last target so that returns and register jumps
// stop falling through to a hash lookup. That cache is keyed on the PC, so a
// site that jumps to two different places must never be handed the target it
// saw last time.
TEST_F(ShilVmExecuteTest, DynamicJumpToNewTargetIsNotServedTheCachedOne)
{
	const u32 targetA = START_PC + 0x100;
	const u32 targetB = START_PC + 0x200;
	writeOp(START_PC, 0x412B);     // jmp @r1
	writeOp(START_PC + 2, 0x0009); // nop, delay slot
	writeLoop(targetA, 1);
	writeLoop(targetB, 5);

	// The VM only chains to blocks that already exist, so run both targets once
	// to get them compiled. Otherwise every exit below reports no successor and
	// the test passes for the wrong reason.
	for (u32 target : { targetA, targetB })
	{
		ctx->pc = target;
		ctx->cycle_counter = SH4_TIMESLICE;
		ASSERT_TRUE(shilVmStepBlock());
	}

	// The successor the VM chains to is what matters here, not ctx.pc: the PC is
	// recomputed on every exit and would look right even if the cache handed
	// back a block for a different address.
	ctx->r[1] = targetA;
	ctx->pc = START_PC;
	ctx->cycle_counter = SH4_TIMESLICE;
	EXPECT_EQ(shilVmStepToSuccessor(), targetA);

	// Same jump, different target: the cached entry has to be rejected.
	ctx->r[1] = targetB;
	ctx->pc = START_PC;
	ctx->cycle_counter = SH4_TIMESLICE;
	EXPECT_EQ(shilVmStepToSuccessor(), targetB);

	// And back, so that re-caching is exercised in both directions.
	ctx->r[1] = targetA;
	ctx->pc = START_PC;
	ctx->cycle_counter = SH4_TIMESLICE;
	EXPECT_EQ(shilVmStepToSuccessor(), targetA);
}

static int s_aotStubCalls = 0;
static int aotStubNeverReached(void *)
{
	s_aotStubCalls++;
	return 0;
}

TEST_F(ShilVmExecuteTest, AotLookupMissFallsBackToShilVm)
{
	aotClearTableForTest();
	aotResetDispatchCounts();
	s_aotStubCalls = 0;

	EXPECT_EQ(aotLookup(START_PC, ctx->fpscr.full), nullptr);
	EXPECT_EQ(aotClassify(START_PC, ctx->fpscr.full, 0), AotDispatch::Miss);

	ctx->pc = START_PC;
	ctx->r[0] = 1;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeLoop(START_PC, 1);
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 2u);
	EXPECT_EQ(s_aotStubCalls, 0);
	EXPECT_GE(g_aotMissCount, 1u);
	EXPECT_EQ(g_aotHitCount, 0u);
	EXPECT_EQ(g_aotSmcMismatchCount, 0u);
}

TEST_F(ShilVmExecuteTest, AotSmcMismatchFallsBackToShilVmWithoutNative)
{
	aotResetDispatchCounts();
	s_aotStubCalls = 0;

	ctx->pc = START_PC;
	ctx->r[0] = 0;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeLoop(START_PC, 1);

	// Table claims this PC, but the expected hash cannot match guest RAM.
	const AotTableEntry entry = {
		START_PC,
		ctx->fpscr.full,
		0xdeadbeefu,
		6u,
		&aotStubNeverReached,
	};
	aotSetTableForTest(&entry, 1);

	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_EQ(ctx->r[0], 1u);
	EXPECT_EQ(s_aotStubCalls, 0);
	EXPECT_EQ(g_aotHitCount, 0u);
	EXPECT_GE(g_aotSmcMismatchCount, 1u);

	aotClearTableForTest();
}

TEST_F(ShilVmExecuteTest, AotHotDumpWritesSchemaKeys)
{
	aotHotReset();
	aotHotSetProfiling(true);

	ctx->pc = START_PC;
	ctx->r[0] = 0;
	ctx->cycle_counter = SH4_TIMESLICE;
	writeLoop(START_PC, 1);
	ASSERT_TRUE(shilVmStepBlock());
	EXPECT_GE(aotHotTrackedCount(), 1u);

	// Grouping locales (fr_FR) used to turn sh4_hash into "2 833 011 226"
	// when streamed via iostream <<; dump must stay json.loads-clean.
	std::locale previous = std::locale::global(std::locale::classic());
	try
	{
		std::locale::global(std::locale("fr_FR.UTF-8"));
	}
	catch (const std::runtime_error &)
	{
		GTEST_SKIP() << "fr_FR.UTF-8 locale unavailable";
	}

	const std::string path = "/tmp/hollycast_aot_hot_dump_test.json";
	ASSERT_TRUE(aotHotDumpTopN(path, 8));
	std::locale::global(previous);

	FILE *f = fopen(path.c_str(), "rb");
	ASSERT_NE(f, nullptr);
	char buf[4096];
	const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	const std::string json(buf);
	EXPECT_NE(json.find("\"pc\""), std::string::npos);
	EXPECT_NE(json.find("\"sh4_hash\""), std::string::npos);
	EXPECT_NE(json.find("\"sh4_bytes\""), std::string::npos);
	EXPECT_NE(json.find("\"guest_cycles\""), std::string::npos);
	EXPECT_NE(json.find("\"BlockType\""), std::string::npos);
	EXPECT_NE(json.find("\"targets\""), std::string::npos);
	// Locale grouping inserts separators inside the number token.
	EXPECT_EQ(json.find("\"sh4_hash\": "), json.rfind("\"sh4_hash\": ")); // exactly one
	const size_t hashKey = json.find("\"sh4_hash\": ");
	ASSERT_NE(hashKey, std::string::npos);
	const size_t numStart = hashKey + strlen("\"sh4_hash\": ");
	const size_t numEnd = json.find_first_of(",\n", numStart);
	ASSERT_NE(numEnd, std::string::npos);
	const std::string hashTok = json.substr(numStart, numEnd - numStart);
	for (char c : hashTok)
		EXPECT_TRUE(c >= '0' && c <= '9') << "sh4_hash token not a plain JSON number: [" << hashTok << "]";

	const std::string py =
		"python3 -c \"import json,sys; json.load(open(sys.argv[1])); print('json.loads ok')\" "
		+ path;
	ASSERT_EQ(system(py.c_str()), 0) << "python json.loads rejected hot dump at " << path;

	aotHotSetProfiling(false);
	aotHotReset();
}
#endif
