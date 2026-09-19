#include "gtest/gtest.h"
#include "build.h"

#if HOST_CPU == CPU_ARM64 && (FEAT_SHREC == DYNAREC_IR || FEAT_SHREC == DYNAREC_JIT)
#include "emulator.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_interpreter.h"
#if FEAT_SHREC == DYNAREC_IR
#include "hw/sh4/dyna/aot_table.h"
#endif
#if FEAT_SHREC == DYNAREC_JIT
#include "oslib/oslib.h"
#endif

#include <chrono>
#include <cstdio>
#include <memory>
#include <iomanip>
#include <locale>
#include <sstream>

// A finite guest loop runs through the real backend mainloop in both builds.
// The only MMIO is a completion write after the loop. It stops the CPU and
// expires its timeslice so the JIT returns through its normal dispatcher.
// No disc, BIOS, savestate, AOT table or title-derived code is used.
class Sh4BackendThroughputTest : public ::testing::Test {
protected:
	static constexpr u32 START_PC = 0xAC000000;
	static constexpr u32 DATA_ADDR = 0xAC001000;
	static constexpr u32 STOP_ADDR = 0xB0000000;
	static constexpr u32 ITERS = 40'000'000;
	static constexpr u32 WARMUP = 64;

	void SetUp() override
	{
		ASSERT_TRUE(addrspace::reserve());
#if FEAT_SHREC == DYNAREC_JIT
		os_InstallFaultHandler();
#endif
		emu.init();
#if FEAT_SHREC == DYNAREC_IR
		ASSERT_EQ(g_aotTableCount, 0u) << "benchmark requires the pure SHIL VM";
#endif
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		interp.reset(Get_Sh4Interpreter());
		interp->Init();
		interp->Reset(true);
		initial = *ctx;
		// mem_map_default resets handler registration, so register after it.
		const auto handler = addrspace::registerHandler(nullptr, nullptr, nullptr,
			nullptr, nullptr, complete);
		addrspace::mapHandler(handler, STOP_ADDR >> 24, STOP_ADDR >> 24);
	}

	void TearDown() override
	{
		if (ctx != nullptr)
			ctx->CpuRunning = false;
		if (interp)
			interp->Term();
		mem_map_default();
	}

	static void DYNACALL complete(u32 address, u32 value)
	{
		EXPECT_EQ(address, STOP_ADDR);
		EXPECT_EQ(value, Sh4cntx.r[0]);
		Sh4cntx.CpuRunning = false;
		Sh4cntx.cycle_counter = 0;
	}

	void reset(u32 iterations)
	{
		*ctx = initial;
		addrspace::write32(DATA_ADDR, 0);
		ctx->r[1] = iterations;
		ctx->r[4] = DATA_ADDR;
		ctx->r[5] = STOP_ADDR;
		ctx->pc = START_PC;
		// Keep peripheral scheduling out of this CPU-only measurement. Both
		// backends consume fewer than this many cycles for the fixed workload.
		ctx->cycle_counter = 1'000'000'000;
		ctx->CpuRunning = true;
	}

	void checkResult(u32 iterations, bool ram, u32 endPc)
	{
		ASSERT_FALSE(ctx->CpuRunning);
		ASSERT_EQ(ctx->r[0], iterations);
		ASSERT_EQ(ctx->r[1], 0u);
		ASSERT_EQ(ctx->r[4], DATA_ADDR);
		ASSERT_EQ(ctx->r[5], STOP_ADDR);
		ASSERT_EQ(addrspace::read32(DATA_ADDR), ram ? iterations : 0u);
		ASSERT_EQ(ctx->pc, endPc);
	}

	void benchmark(bool ram)
	{
		// Identical words and initial state in JIT and VM builds. DT/BF count
		// guest iterations rather than wall time or backend-specific cycles.
		const u16 aluCode[] = {0x7001, 0x4110, 0x8BFC, 0x2502, 0xAFFE, 0x0009};
		const u16 ramCode[] = {0x6042, 0x7001, 0x2402, 0x4110, 0x8BFA, 0x2502, 0xAFFE, 0x0009};
		const u16 *code = ram ? ramCode : aluCode;
		const size_t words = ram ? std::size(ramCode) : std::size(aluCode);
		u32 hash = 2166136261u;
		for (size_t i = 0; i < words; i++)
		{
			addrspace::write16(START_PC + 2 * i, code[i]);
			hash = (hash ^ (code[i] & 0xff)) * 16777619u;
			hash = (hash ^ (code[i] >> 8)) * 16777619u;
		}
		const u32 endPc = START_PC + (ram ? 12 : 8);
		const u32 ops = ram ? 5 : 3;
		auto reference = [&](u32 iterations) {
			reset(iterations);
			for (u64 i = 0; i < u64(iterations) * ops; i++)
				interp->Step();
			interp->Step(); // completion write leaves pc at the terminal branch
		};
		auto backend = [&](u32 iterations) {
			reset(iterations);
			Sh4Recompiler::Instance->Run();
		};

		reference(WARMUP);
		ASSERT_NO_FATAL_FAILURE(checkResult(WARMUP, ram, endPc));
		backend(WARMUP); // compile and link every executed path before timing
		ASSERT_NO_FATAL_FAILURE(checkResult(WARMUP, ram, endPc));
		reference(ITERS);
		ASSERT_NO_FATAL_FAILURE(checkResult(ITERS, ram, endPc));
		const Sh4Context expected = *ctx;
		const auto start = std::chrono::steady_clock::now();
		backend(ITERS);
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		ASSERT_NO_FATAL_FAILURE(checkResult(ITERS, ram, endPc));
		for (size_t i = 0; i < std::size(ctx->r); i++)
			ASSERT_EQ(ctx->r[i], expected.r[i]) << "r" << i;
		ASSERT_EQ(ctx->sr.getFull(), expected.sr.getFull());
		ASSERT_EQ(ctx->fpscr.full, expected.fpscr.full);
		ASSERT_GT(seconds, 0);
#if FEAT_SHREC == DYNAREC_JIT
		const char *name = "jit";
#else
		const char *name = "shil_vm";
#endif
		// Other suites change the process locale; JSON always needs decimal dots.
		std::ostringstream row;
		row.imbue(std::locale::classic());
		row << std::fixed << std::setprecision(12)
			<< "SH4_BENCH {\"backend\":\"" << name << "\",\"workload\":\"" << (ram ? "RAM" : "ALU")
			<< "\",\"iterations\":" << ITERS << ",\"warmup\":" << WARMUP
			<< ",\"ops_per_iteration\":" << ops << ",\"code_hash\":" << hash
			<< ",\"r0\":" << ctx->r[0] << ",\"r1\":" << ctx->r[1]
			<< ",\"ram\":" << addrspace::read32(DATA_ADDR) << ",\"pc\":" << ctx->pc
			<< ",\"seconds\":" << seconds << ",\"mops\":" << double(ITERS) * ops / seconds / 1e6 << "}";
		std::puts(row.str().c_str());
	}

	Sh4Context *ctx = nullptr;
	Sh4Context initial{};
	std::unique_ptr<Sh4Executor> interp;
};

TEST_F(Sh4BackendThroughputTest, AluLoopHostSpeed)
{
	benchmark(false);
}

TEST_F(Sh4BackendThroughputTest, RamLoopHostSpeed)
{
	benchmark(true);
}
#endif
