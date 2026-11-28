/*
	Copyright 2021 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "sh4_ops.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"

#include <cfenv>
#if defined(__MINGW32__) && HOST_CPU == CPU_X64
#include <xmmintrin.h>
#endif

class Sh4InterpreterTest : public Sh4OpTest {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		sh4 = Get_Sh4Interpreter();
		sh4->Init();
	}
	void PrepareOp(u16 op, u16 op2 = 0, u16 op3 = 0) override
	{
		ctx->pc = START_PC;
		addrspace::write16(ctx->pc, op);
		if (op2 != 0)
			addrspace::write16(ctx->pc + 2, op2);
		if (op3 != 0)
			addrspace::write16(ctx->pc + 4, op3);
	}
	void RunOp(int numOp = 1) override
	{
		ctx->pc = START_PC;
		for (int i = 0; i < numOp; i++)
			sh4->Step();
	}
};

TEST_F(Sh4InterpreterTest, MovRmRnTest)
{
	Sh4OpTest::MovRmRnTest();
}
TEST_F(Sh4InterpreterTest, MovImmRnTest)
{
	Sh4OpTest::MovImmRnTest();
}
TEST_F(Sh4InterpreterTest, MovMiscTest)
{
	Sh4OpTest::MovMiscTest();
}
TEST_F(Sh4InterpreterTest, LoadTest)
{
	Sh4OpTest::LoadTest();
}
TEST_F(Sh4InterpreterTest, LoadTest2)
{
	Sh4OpTest::LoadTest2();
}
TEST_F(Sh4InterpreterTest, StoreTest)
{
	Sh4OpTest::StoreTest();
}
TEST_F(Sh4InterpreterTest, StoreTest2)
{
	Sh4OpTest::StoreTest2();
}
TEST_F(Sh4InterpreterTest, ArithmeticTest)
{
	Sh4OpTest::ArithmeticTest();
}
TEST_F(Sh4InterpreterTest, MulDivTest)
{
	Sh4OpTest::MulDivTest();
}
TEST_F(Sh4InterpreterTest, CmpTest)
{
	Sh4OpTest::CmpTest();
}
TEST_F(Sh4InterpreterTest, StatusRegTest)
{
	Sh4OpTest::StatusRegTest();
}
TEST_F(Sh4InterpreterTest, FloatingPointTest)
{
	Sh4OpTest::FloatingPointTest();
}
TEST_F(Sh4InterpreterTest, FmacRoundsTowardZero)
{
	std::fenv_t savedEnv;
	ASSERT_EQ(std::fegetenv(&savedEnv), 0);
#if defined(__MINGW32__) && HOST_CPU == CPU_X64
	const unsigned savedMxcsr = _mm_getcsr();
#endif
	const Sh4Context savedContext = *ctx;
	const u16 savedOpcode = addrspace::read16(START_PC);

	ctx->fpscr.PR = 0;
	ctx->fpscr.RM = 1;
	ctx->fr_hex(0) = 0x3f19999a;
	ctx->fr_hex(3) = 0x41880000;
	ctx->fr_hex(2) = 0x43fde65b;
	PrepareOp(0xf23e); // Toy Racer: FMAC FR0, FR3, FR2.
	RunOp();
	const u32 capturedResult = ctx->fr_hex(2);

	// Round-to-nearest would give 0x43a7cccd for these operands.
	ctx->fr_hex(3) = 0x3f800000;
	ctx->fr_hex(2) = 0x43a78000;
	RunOp();
	const u32 roundingResult = ctx->fr_hex(2);

	addrspace::write16(START_PC, savedOpcode);
	*ctx = savedContext;
	ctx->restoreHostRoundingMode();
	const int restoreStatus = std::fesetenv(&savedEnv);
#if defined(__MINGW32__) && HOST_CPU == CPU_X64
	_mm_setcsr(savedMxcsr);
#endif
	ASSERT_EQ(restoreStatus, 0);
	ASSERT_EQ(capturedResult, 0x44017ffau);
	ASSERT_EQ(roundingResult, 0x43a7ccccu);
}
TEST_F(Sh4InterpreterTest, DoubleFloatingPointTest)
{
	Sh4OpTest::DoubleFloatingPointTest();
}
