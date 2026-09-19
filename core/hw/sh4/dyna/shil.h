#pragma once
#include "hw/sh4/sh4_if.h"

struct shil_opcode;
typedef void shil_chfp(shil_opcode* op);
extern shil_chfp* shil_chf[];

enum Sh4RegType
{
	//GPRs
	reg_r0,
	reg_r1,
	reg_r2,
	reg_r3,
	reg_r4,
	reg_r5,
	reg_r6,
	reg_r7,
	reg_r8,
	reg_r9,
	reg_r10,
	reg_r11,
	reg_r12,
	reg_r13,
	reg_r14,
	reg_r15,

	//FPU, bank 0
	reg_fr_0,
	reg_fr_1,
	reg_fr_2,
	reg_fr_3,
	reg_fr_4,
	reg_fr_5,
	reg_fr_6,
	reg_fr_7,
	reg_fr_8,
	reg_fr_9,
	reg_fr_10,
	reg_fr_11,
	reg_fr_12,
	reg_fr_13,
	reg_fr_14,
	reg_fr_15,

	//FPU, bank 1
	reg_xf_0,
	reg_xf_1,
	reg_xf_2,
	reg_xf_3,
	reg_xf_4,
	reg_xf_5,
	reg_xf_6,
	reg_xf_7,
	reg_xf_8,
	reg_xf_9,
	reg_xf_10,
	reg_xf_11,
	reg_xf_12,
	reg_xf_13,
	reg_xf_14,
	reg_xf_15,

	//GPR Interrupt bank
	reg_r0_Bank,
	reg_r1_Bank,
	reg_r2_Bank,
	reg_r3_Bank,
	reg_r4_Bank,
	reg_r5_Bank,
	reg_r6_Bank,
	reg_r7_Bank,

	//Misc regs
	reg_gbr,
	reg_ssr,
	reg_spc,
	reg_sgr,
	reg_dbr,
	reg_vbr,
	reg_mach,
	reg_macl,
	reg_pr,
	reg_fpul,
	reg_nextpc,
	reg_sr_status,     //Only the status bits
	reg_sr_T,          //Only T
	reg_old_fpscr,
	reg_fpscr,

	reg_pc_dyn,        //Write only, for dynarec only (dynamic block exit address)
	reg_temp,

	sh4_reg_count,

	/*
		These are virtual registers, used by the dynarec decoder
	*/
	regv_dr_0,
	regv_dr_2,
	regv_dr_4,
	regv_dr_6,
	regv_dr_8,
	regv_dr_10,
	regv_dr_12,
	regv_dr_14,

	regv_xd_0,
	regv_xd_2,
	regv_xd_4,
	regv_xd_6,
	regv_xd_8,
	regv_xd_10,
	regv_xd_12,
	regv_xd_14,

	regv_fv_0,
	regv_fv_4,
	regv_fv_8,
	regv_fv_12,

	regv_xmtrx,
	regv_fmtrx,

	reg_sq_buffer,

	NoReg=-1
};

// Byte offset of each register within Sh4Context. The SHIL VM resolves this for
// every operand of every op it runs, so it is a table built at compile time
// rather than a chain of range checks and a switch evaluated on each access.
constexpr u16 SH4_REG_NO_OFFSET = 0xFFFF;

struct Sh4RegOffsets
{
	u16 offsets[reg_sq_buffer + 1];
};

constexpr Sh4RegOffsets makeSh4RegOffsets()
{
	Sh4RegOffsets table{};
	for (u32 i = 0; i <= (u32)reg_sq_buffer; i++)
		table.offsets[i] = SH4_REG_NO_OFFSET;

	for (u32 i = 0; i < 16; i++)
	{
		table.offsets[reg_r0 + i] = (u16)(offsetof(Sh4Context, r[0]) + i * sizeof(u32));
		table.offsets[reg_fr_0 + i] = (u16)(offsetof(Sh4Context, fr[0]) + i * sizeof(float));
		table.offsets[reg_xf_0 + i] = (u16)(offsetof(Sh4Context, xf[0]) + i * sizeof(float));
	}
	for (u32 i = 0; i < 8; i++)
		table.offsets[reg_r0_Bank + i] = (u16)(offsetof(Sh4Context, r_bank[0]) + i * sizeof(u32));

	table.offsets[reg_gbr] = offsetof(Sh4Context, gbr);
	table.offsets[reg_vbr] = offsetof(Sh4Context, vbr);
	table.offsets[reg_ssr] = offsetof(Sh4Context, ssr);
	table.offsets[reg_spc] = offsetof(Sh4Context, spc);
	table.offsets[reg_sgr] = offsetof(Sh4Context, sgr);
	table.offsets[reg_dbr] = offsetof(Sh4Context, dbr);
	table.offsets[reg_mach] = offsetof(Sh4Context, mac.h);
	table.offsets[reg_macl] = offsetof(Sh4Context, mac.l);
	table.offsets[reg_pr] = offsetof(Sh4Context, pr);
	table.offsets[reg_fpul] = offsetof(Sh4Context, fpul);
	table.offsets[reg_nextpc] = offsetof(Sh4Context, pc);
	table.offsets[reg_sr_status] = offsetof(Sh4Context, sr.status);
	table.offsets[reg_sr_T] = offsetof(Sh4Context, sr.T);
	table.offsets[reg_old_fpscr] = offsetof(Sh4Context, old_fpscr.full);
	table.offsets[reg_fpscr] = offsetof(Sh4Context, fpscr.full);
	table.offsets[reg_pc_dyn] = offsetof(Sh4Context, jdyn);
	table.offsets[reg_temp] = offsetof(Sh4Context, temp_reg);
	table.offsets[reg_sq_buffer] = offsetof(Sh4Context, sq_buffer);

	return table;
}

inline constexpr Sh4RegOffsets sh4RegOffsets = makeSh4RegOffsets();

static_assert(sizeof(Sh4Context) < SH4_REG_NO_OFFSET, "Sh4Context offsets must fit in u16");

[[noreturn]] void dieInvalidReg(u32 reg);

inline u32 getRegOffset(Sh4RegType reg)
{
	const u32 offset = (u32)reg <= (u32)reg_sq_buffer ? sh4RegOffsets.offsets[reg] : SH4_REG_NO_OFFSET;
	if (offset == SH4_REG_NO_OFFSET)
		dieInvalidReg((u32)reg);
	return offset;
}

inline u32* GetRegPtr(Sh4Context& ctx, u32 reg)
{
	return (u32 *)((u8 *)&ctx + getRegOffset((Sh4RegType)reg));
}

enum shil_param_type
{
	FMT_NULL,
	FMT_IMM,
	FMT_I32,
	FMT_F32,
	FMT_F64,
	
	FMT_V4,
	FMT_V16,

	FMT_REG_BASE = FMT_I32,
	FMT_VECTOR_BASE = FMT_V4,

	FMT_MASK = 0xFFFF,
};

/*
	formats : 16u 16s 32u 32s, 32f, 64f
	param types: r32, r64
*/

#define SHIL_MODE 0
#include "shil_canonical.h"

struct shil_param
{
	shil_param()
	{
		type = FMT_NULL;
		_imm = 0xFFFFFFFF;
		memset(version, 0, sizeof(version));
	}

	shil_param(u32 imm)
	{
		this->type = FMT_IMM;
		_imm = imm;
		memset(version, 0, sizeof(version));
	}

	shil_param(Sh4RegType reg)
	{
		if (reg >= reg_fr_0 && reg <= reg_xf_15)
		{
			type = FMT_F32;
			_imm = reg;
		}
		else if (reg >= regv_dr_0 && reg <= regv_dr_14)
		{
			type = FMT_F64;
			_imm = (reg - regv_dr_0) * 2 + reg_fr_0;
		}
		else if (reg >= regv_xd_0 && reg <= regv_xd_14)
		{
			type = FMT_F64;
			_imm = (reg - regv_xd_0) * 2 + reg_xf_0;
		}
		else if (reg >= regv_fv_0 && reg <= regv_fv_12)
		{
			type = FMT_V4;
			_imm = (reg - regv_fv_0) * 4 + reg_fr_0;
		}
		else if (reg == regv_xmtrx)
		{
			type = FMT_V16;
			_imm = reg_xf_0;
		}
		else if (reg == regv_fmtrx)
		{
			type = FMT_V16;
			_imm = reg_fr_0;
		}
		else
		{
			type = FMT_I32;
			_reg = reg;
		}
		memset(version, 0, sizeof(version));
	}
	union
	{
		u32 _imm;
		Sh4RegType _reg;
	};
	u32 type;
	u16 version[16];

	bool is_null() const { return type==FMT_NULL; }
	bool is_imm() const { return type==FMT_IMM; }
	bool is_reg() const { return type>=FMT_REG_BASE; }

	bool is_r32i() const { return type==FMT_I32; }
	bool is_r32f() const { return type==FMT_F32; }
	u32 is_r32fv() const { return type >= FMT_VECTOR_BASE ? count() : 0; }
	bool is_r64f() const { return type==FMT_F64; }

	bool is_r32() const { return is_r32i() || is_r32f(); }

	bool is_imm_s8() const { return is_imm() && (int8_t)_imm == (int32_t)_imm; }

	u32* reg_ptr(Sh4Context& ctx) const { verify(is_reg()); return GetRegPtr(ctx, _reg); }
	u32  reg_offset() const { verify(is_reg()); return getRegOffset(_reg); }
	s32  reg_nofs() const { verify(is_reg()); return (int)getRegOffset(_reg) - sizeof(Sh4Context); }

	u32 imm_value() const { verify(is_imm()); return _imm; }

	u32 count() const { return  type == FMT_F64 ? 2 :
								type == FMT_V4 ? 4 :
								type == FMT_V16 ? 16 : 1; }	//count of hardware regs

	/*	
		Imms:
		is_imm
		
		regs:
		integer regs            : is_r32i,is_r32,count=1
		fpu regs, single view   : is_r32f,is_r32,count=1
		fpu regs, double view   : is_r64f,count=2
		fpu regs, quad view     : is_r32fv=4, count=4
		fpu regs, matrix view   : is_r32fv=16, count=16
	*/
};

struct shil_opcode
{
	shilop op;
	u32 size;	// memory access size

	shil_param rd, rd2;
	shil_param rs1, rs2, rs3;

	u16 host_offs;
	u16 guest_offs;
	bool delay_slot;

    std::string dissasm() const;
};

const char* shil_opcode_name(int op);

std::string name_reg(Sh4RegType reg);
