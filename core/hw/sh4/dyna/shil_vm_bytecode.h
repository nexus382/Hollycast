#pragma once

#include "shil.h"

#include <string>
#include <type_traits>
#include <vector>

// A cell addresses a register as a signed word slot counted from
// Sh4Context::r[0], resolved once when the cell is encoded. Reaching an operand
// is then a single indexed load, instead of a range check plus a lookup in
// sh4RegOffsets on every operand of every op the VM runs. Slots run negative
// for the registers that sit before r[0] in the context.
constexpr s16 SHIL_VM_NO_REG = 0x7FFF;

// Byte the slot count starts from. A negative slot names a byte before r[0] but
// still inside the context, so the VM reaches it by walking the bytes of the
// whole Sh4Context object from here, as GetRegPtr() does, rather than by
// indexing r[16] outside of its own bounds.
constexpr s32 SHIL_VM_SLOT_BASE = (s32)offsetof(Sh4Context, r[0]);

constexpr s32 shilVmSlotFromOffset(u16 offset)
{
	return ((s32)offset - SHIL_VM_SLOT_BASE) / (s32)sizeof(u32);
}

inline s16 shilVmRegSlot(Sh4RegType reg)
{
	return (s16)shilVmSlotFromOffset((u16)getRegOffset(reg));
}

// A slot has to land on exactly the byte GetRegPtr() would have returned, for
// every register the SHIL can name: it must round trip back to the register's
// offset, fit in the cell, never collide with the no-operand sentinel, and land
// on a word that lies inside the context. That last one is what makes the VM's
// byte arithmetic from the context base defined rather than merely working.
constexpr bool shilVmSlotsAddressEveryRegister()
{
	for (u32 reg = 0; reg <= (u32)reg_sq_buffer; reg++)
	{
		const u16 offset = sh4RegOffsets.offsets[reg];
		if (offset == SH4_REG_NO_OFFSET)
			continue;
		const s32 slot = shilVmSlotFromOffset(offset);
		if (slot != (s32)(s16)slot || slot == SHIL_VM_NO_REG)
			return false;
		const s32 addressed = SHIL_VM_SLOT_BASE + slot * (s32)sizeof(u32);
		if (addressed != (s32)offset)
			return false;
		if (addressed < 0 || addressed + (s32)sizeof(u32) > (s32)sizeof(Sh4Context))
			return false;
	}
	return true;
}

// offsetof is only required to work on a standard-layout type, and the VM reads
// a register by adding a byte offset to the address of the context itself.
static_assert(std::is_standard_layout<Sh4Context>::value, "SHIL VM slots address Sh4Context by byte offset from its own base");
static_assert(shilVmSlotsAddressEveryRegister(), "every SH4 register must be addressable as a signed word slot from r[0]");
// shilVmEvalGpr reads a slot as a register number, so this mapping is load bearing.
static_assert(shilVmSlotFromOffset(sh4RegOffsets.offsets[reg_r0]) == 0, "r0 must be slot 0");
static_assert(shilVmSlotFromOffset(sh4RegOffsets.offsets[reg_r15]) == 15, "r15 must be slot 15");

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
	s16 rd = SHIL_VM_NO_REG;
	s16 rd2 = SHIL_VM_NO_REG;
	s16 rs1 = SHIL_VM_NO_REG;
	s16 rs2 = SHIL_VM_NO_REG;
	s16 rs3 = SHIL_VM_NO_REG;
	u16 guest_offs = 0;
	u32 imm = 0;
	u32 imm2 = 0;
	u32 imm3 = 0;

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
