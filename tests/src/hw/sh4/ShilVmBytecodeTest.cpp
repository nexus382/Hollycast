#include "gtest/gtest.h"
#include "hw/sh4/dyna/shil_vm_bytecode.h"

static shil_opcode makeMov(Sh4RegType rd, Sh4RegType rs)
{
	shil_opcode op{};
	op.op = shop_mov32;
	op.rd = shil_param(rd);
	op.rs1 = shil_param(rs);
	return op;
}

static shil_opcode makeMovImm(Sh4RegType rd, u32 imm)
{
	shil_opcode op{};
	op.op = shop_mov32;
	op.rd = shil_param(rd);
	op.rs1 = shil_param(imm);
	return op;
}

static shil_opcode makeAluImm(shilop kind, Sh4RegType rd, Sh4RegType rs1, u32 imm)
{
	shil_opcode op{};
	op.op = kind;
	op.rd = shil_param(rd);
	op.rs1 = shil_param(rs1);
	op.rs2 = shil_param(imm);
	return op;
}

static shil_opcode makeAlu(shilop kind, Sh4RegType rd, Sh4RegType rs1, Sh4RegType rs2)
{
	shil_opcode op{};
	op.op = kind;
	op.rd = shil_param(rd);
	op.rs1 = shil_param(rs1);
	op.rs2 = shil_param(rs2);
	return op;
}

TEST(ShilVmBytecode, EncodesMovImmSuperop)
{
	const ShilVmCell cell = shilVmEncodeOp(makeMovImm(reg_r0, 0x11));
	EXPECT_EQ(cell.op, vmop_mov_imm);
	EXPECT_TRUE(cell.rs1Imm());
	EXPECT_EQ(cell.src1(), 0x11u);
	EXPECT_EQ(cell.rd, (u16)reg_r0);
}

TEST(ShilVmBytecode, EncodesAddImmSuperop)
{
	const ShilVmCell cell = shilVmEncodeOp(makeAluImm(shop_add, reg_r1, reg_r1, 4));
	EXPECT_EQ(cell.op, vmop_add_imm);
	EXPECT_EQ(cell.rd, (u16)reg_r1);
	EXPECT_EQ(cell.rs1, (u16)reg_r1);
	EXPECT_TRUE(cell.rs2Imm());
	EXPECT_EQ(cell.src2(), 4u);
}

TEST(ShilVmBytecode, FoldsMovIntoAdd)
{
	std::vector<shil_opcode> ops;
	ops.push_back(makeMov(reg_r2, reg_r0));
	ops.push_back(makeAluImm(shop_add, reg_r2, reg_r2, 8));
	const ShilVmProgram program = shilVmEncode(ops);
	ASSERT_EQ(program.cells.size(), 1u);
	EXPECT_EQ(program.cells[0].op, vmop_add_imm);
	EXPECT_EQ(program.cells[0].rd, (u16)reg_r2);
	EXPECT_EQ(program.cells[0].rs1, (u16)reg_r0);
	EXPECT_EQ(program.cells[0].src2(), 8u);
}

// `mov Rm,Rn; add Rn,Rn` decodes to mov32 rN,rM followed by add rN,rN,rN.
// Folding the mov away there would leave rs2 reading rN from before the mov,
// so the pair has to survive as two cells. SSA happens to rewrite a+a into a
// shift before the encoder sees it, which is why this needs its own test
// rather than being caught by the boot benchmark.
TEST(ShilVmBytecode, KeepsMovWhenAluReadsTheMovedRegisterAgain)
{
	std::vector<shil_opcode> ops;
	ops.push_back(makeMov(reg_r1, reg_r2));
	ops.push_back(makeAlu(shop_add, reg_r1, reg_r1, reg_r1));
	const ShilVmProgram program = shilVmEncode(ops);
	ASSERT_EQ(program.cells.size(), 2u);

	u32 r[16] = {};
	r[1] = 0x1000;
	r[2] = 7;
	std::string err;
	ASSERT_TRUE(shilVmEvalGpr(program.cells, r, &err)) << err;
	// r1 becomes 7, so the add must yield 14 - not 7 + the stale 0x1000.
	EXPECT_EQ(r[1], 14u);
}

TEST(ShilVmBytecode, EvalGprAddAndShift)
{
	std::vector<shil_opcode> ops;
	ops.push_back(makeMovImm(reg_r0, 5));
	ops.push_back(makeAluImm(shop_add, reg_r0, reg_r0, 7));
	ops.push_back(makeAluImm(shop_shl, reg_r1, reg_r0, 1));
	const ShilVmProgram program = shilVmEncode(ops);
	u32 r[16] = {};
	std::string err;
	ASSERT_TRUE(shilVmEvalGpr(program.cells, r, &err)) << err;
	EXPECT_EQ(r[0], 12u);
	EXPECT_EQ(r[1], 24u);
}

TEST(ShilVmBytecode, EvalGprAndOrXor)
{
	std::vector<shil_opcode> ops;
	ops.push_back(makeMovImm(reg_r0, 0xff00));
	ops.push_back(makeAluImm(shop_and, reg_r0, reg_r0, 0x0ff0));
	ops.push_back(makeAlu(shop_or, reg_r1, reg_r0, reg_r0));
	ops.push_back(makeAluImm(shop_xor, reg_r1, reg_r1, 0x00f0));
	const ShilVmProgram program = shilVmEncode(ops);
	u32 r[16] = {};
	ASSERT_TRUE(shilVmEvalGpr(program.cells, r));
	EXPECT_EQ(r[0], 0x0f00u);
	EXPECT_EQ(r[1], 0x0ff0u);
}

TEST(ShilVmBytecode, ThreadedDispatchRunsCellStream)
{
	std::vector<ShilVmCell> cells;
	cells.push_back(shilVmEncodeOp(makeMovImm(reg_r4, 1)));
	cells.push_back(shilVmEncodeOp(makeAluImm(shop_add, reg_r4, reg_r4, 1)));
	cells.push_back(shilVmEncodeOp(makeAluImm(shop_add, reg_r4, reg_r4, 1)));
	u32 r[16] = {};
	ASSERT_TRUE(shilVmEvalGpr(cells, r));
	EXPECT_EQ(r[4], 3u);
}
