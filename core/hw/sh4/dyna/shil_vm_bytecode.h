#pragma once

#include "shil.h"

#include <string>
#include <vector>

constexpr u16 SHIL_VM_NO_REG = 0xFFFF;

constexpr u8 SHIL_VM_RS1_IMM = 1 << 0;
constexpr u8 SHIL_VM_RS2_IMM = 1 << 1;
constexpr u8 SHIL_VM_RS3_IMM = 1 << 2;
constexpr u8 SHIL_VM_DELAY = 1 << 3;

// Extra ops after shop_max. Kept in a u16 so the handler table can index them.
enum ShilVmExtraOp : u16
{
	vmop_mov_imm = (u16)shop_max,
	vmop_add_imm,
	vmop_sub_imm,
	vmop_and_imm,
	vmop_or_imm,
	vmop_xor_imm,
	vmop_shl_imm,
	vmop_load,
	vmop_store,
	vmop_end
};

constexpr u16 SHIL_VM_HANDLER_COUNT = vmop_end;

static_assert(shop_max < 128, "handler table assumes SHIL opcodes fit in a compact table");
static_assert(vmop_end <= 256, "SHIL VM op ids must fit in u8-sized maps");

struct ShilVmCell
{
	u16 op = 0;
	u8 size = 0;
	u8 flags = 0;
	u16 rd = SHIL_VM_NO_REG;
	u16 rd2 = SHIL_VM_NO_REG;
	u16 rs1 = SHIL_VM_NO_REG;
	u16 rs2 = SHIL_VM_NO_REG;
	u16 rs3 = SHIL_VM_NO_REG;
	u32 imm = 0;
	u32 imm2 = 0;
	u32 imm3 = 0;
	u16 guest_offs = 0;

	bool rs1Imm() const { return (flags & SHIL_VM_RS1_IMM) != 0; }
	bool rs2Imm() const { return (flags & SHIL_VM_RS2_IMM) != 0; }
	bool rs3Imm() const { return (flags & SHIL_VM_RS3_IMM) != 0; }
	bool delaySlot() const { return (flags & SHIL_VM_DELAY) != 0; }

	u32 src1() const { return imm; }
	u32 src2() const { return rs1Imm() ? imm2 : imm; }
	u32 src3() const
	{
		const int idx = (int)rs1Imm() + (int)rs2Imm();
		if (idx == 0)
			return imm;
		if (idx == 1)
			return imm2;
		return imm3;
	}
};

struct ShilVmProgram
{
	std::vector<ShilVmCell> cells;
};

ShilVmCell shilVmEncodeOp(const shil_opcode& op);
ShilVmProgram shilVmEncode(const std::vector<shil_opcode>& ops);

// GPR-only interpreter for focused tests (r0-r15). Returns false if an op is not GPR-only.
bool shilVmEvalGpr(const std::vector<ShilVmCell>& cells, u32 r[16], std::string *err = nullptr);
