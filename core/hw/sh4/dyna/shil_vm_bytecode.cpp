#include "shil_vm_bytecode.h"

static void storeImm(ShilVmCell& cell, u32 value, int index)
{
	if (index == 0)
		cell.imm = value;
	else if (index == 1)
		cell.imm2 = value;
	else
		cell.imm3 = value;
}

static void packParam(ShilVmCell& cell, const shil_param& param, u16& reg, u8 immBit, int& immIndex)
{
	if (param.is_null())
	{
		reg = SHIL_VM_NO_REG;
		return;
	}
	if (param.is_imm())
	{
		cell.flags |= immBit;
		storeImm(cell, param._imm, immIndex++);
		reg = SHIL_VM_NO_REG;
		return;
	}
	reg = (u16)param._reg;
}

ShilVmCell shilVmEncodeOp(const shil_opcode& op)
{
	ShilVmCell cell;
	cell.op = (u16)op.op;
	cell.size = (u8)op.size;
	cell.guest_offs = op.guest_offs;
	if (op.delay_slot)
		cell.flags |= SHIL_VM_DELAY;

	int immIndex = 0;
	packParam(cell, op.rd, cell.rd, 0, immIndex);
	immIndex = 0;
	packParam(cell, op.rd2, cell.rd2, 0, immIndex);
	immIndex = 0;
	packParam(cell, op.rs1, cell.rs1, SHIL_VM_RS1_IMM, immIndex);
	packParam(cell, op.rs2, cell.rs2, SHIL_VM_RS2_IMM, immIndex);
	packParam(cell, op.rs3, cell.rs3, SHIL_VM_RS3_IMM, immIndex);

	if (op.op == shop_mov32 && cell.rs1Imm() && cell.rd != SHIL_VM_NO_REG)
		cell.op = vmop_mov_imm;
	else if (op.op == shop_add && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_add_imm;
	else if (op.op == shop_sub && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_sub_imm;
	else if (op.op == shop_and && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_and_imm;
	else if (op.op == shop_or && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_or_imm;
	else if (op.op == shop_xor && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_xor_imm;
	else if (op.op == shop_shl && cell.rs2Imm() && !cell.rs1Imm())
		cell.op = vmop_shl_imm;
	else if (op.op == shop_readm && cell.rd != SHIL_VM_NO_REG)
		cell.op = vmop_load;
	else if (op.op == shop_writem)
		cell.op = vmop_store;

	return cell;
}

static bool sameReg(const shil_param& a, const shil_param& b)
{
	return a.is_reg() && b.is_reg() && a._reg == b._reg;
}

static bool isAlu(shilop op)
{
	switch (op)
	{
	case shop_add:
	case shop_sub:
	case shop_and:
	case shop_or:
	case shop_xor:
	case shop_shl:
	case shop_shr:
	case shop_sar:
		return true;
	default:
		return false;
	}
}

static std::vector<shil_opcode> foldMovIntoAlu(const std::vector<shil_opcode>& ops)
{
	std::vector<shil_opcode> out;
	out.reserve(ops.size());
	for (size_t i = 0; i < ops.size(); i++)
	{
		if (i + 1 < ops.size()
			&& ops[i].op == shop_mov32
			&& ops[i].rd.is_r32i()
			&& isAlu(ops[i + 1].op)
			&& sameReg(ops[i].rd, ops[i + 1].rd)
			&& sameReg(ops[i].rd, ops[i + 1].rs1))
		{
			shil_opcode fused = ops[i + 1];
			fused.rs1 = ops[i].rs1;
			fused.guest_offs = ops[i].guest_offs;
			fused.delay_slot = ops[i].delay_slot || ops[i + 1].delay_slot;
			out.push_back(fused);
			i++;
			continue;
		}
		out.push_back(ops[i]);
	}
	return out;
}

ShilVmProgram shilVmEncode(const std::vector<shil_opcode>& ops)
{
	ShilVmProgram program;
	const std::vector<shil_opcode> folded = foldMovIntoAlu(ops);
	program.cells.reserve(folded.size());
	for (const shil_opcode& op : folded)
		program.cells.push_back(shilVmEncodeOp(op));
	return program;
}

static bool loadGpr(const ShilVmCell& cell, const u32 r[16], bool isImm, u16 reg, u32 immValue, u32 *out, std::string *err)
{
	if (isImm)
	{
		*out = immValue;
		return true;
	}
	if (reg == SHIL_VM_NO_REG)
	{
		if (err)
			*err = "missing register operand";
		return false;
	}
	if (reg > 15)
	{
		if (err)
			*err = "not a GPR";
		return false;
	}
	*out = r[reg];
	return true;
}

bool shilVmEvalGpr(const std::vector<ShilVmCell>& cells, u32 r[16], std::string *err)
{
	for (const ShilVmCell& cell : cells)
	{
		u32 a = 0, b = 0, result = 0;
		switch (cell.op)
		{
		case shop_mov32:
		case vmop_mov_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &result, err))
				return false;
			break;
		case shop_add:
		case vmop_add_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a + b;
			break;
		case shop_sub:
		case vmop_sub_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a - b;
			break;
		case shop_and:
		case vmop_and_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a & b;
			break;
		case shop_or:
		case vmop_or_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a | b;
			break;
		case shop_xor:
		case vmop_xor_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a ^ b;
			break;
		case shop_not:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err))
				return false;
			result = ~a;
			break;
		case shop_neg:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err))
				return false;
			result = -a;
			break;
		case shop_shl:
		case vmop_shl_imm:
			if (!loadGpr(cell, r, cell.rs1Imm(), cell.rs1, cell.src1(), &a, err)
				|| !loadGpr(cell, r, cell.rs2Imm(), cell.rs2, cell.src2(), &b, err))
				return false;
			result = a << (b & 31);
			break;
		default:
			if (err)
				*err = "unsupported SHIL VM GPR op";
			return false;
		}

		if (cell.rd > 15)
		{
			if (err)
				*err = "destination is not a GPR";
			return false;
		}
		r[cell.rd] = result;
	}
	return true;
}
