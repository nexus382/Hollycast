/*
	Cached SHIL virtual machine.

	SH4 → SHIL → compact cells (with superops), executed via a handler table.
	No PROT_EXEC / MAP_JIT. Block chaining, RAM fast path, page-version SMC.
*/

#include "types.h"

#if FEAT_SHREC == DYNAREC_IR

#include "ngen.h"
#include "blockmanager.h"
#include "decoder.h"
#include "shil.h"
#include "shil_vm.h"
#include "shil_vm_bytecode.h"
#include "shil_vm_chain.h"
#include "shil_vm_mem.h"
#include "shil_vm_smc.h"
#include <algorithm>

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/modules/mmu.h"
#include "debug/gdb_server.h"

#include <cstring>
#include <vector>

#define SHIL_MODE 1
#include "shil_canonical.h"

struct ShilRuntimeBlock : RuntimeBlockInfo
{
	ShilVmProgram program;
	u32 smcHash = 0;
	u32 smcAddr = 0;
	u32 smcSize = 0;
	bool smcEnabled = false;
	// PC that pBranchBlock currently corresponds to. For a static exit this is
	// just BranchBlock, but a dynamic exit has no BranchBlock to match on, so
	// this is where the last observed target is remembered.
	u32 dynPc = NullAddress;
};

static u8 *s_ram = nullptr;
static u32 s_ramSize = 0;
static u32 s_ramMask = 0;
static u32 s_compileCount = 0;

// The slot is the operand's word offset from r[0], resolved when the cell was
// encoded, so this is one indexed load with nothing to look up or test.
static inline u32 *regPtr(Sh4Context& ctx, s16 slot)
{
	return ctx.r + slot;
}

static inline u32 loadI(Sh4Context& ctx, bool isImm, s16 slot, u32 imm)
{
	if (isImm)
		return imm;
	return *regPtr(ctx, slot);
}

static inline u32 load1(Sh4Context& ctx, const ShilVmCell& cell)
{
	return loadI(ctx, cell.rs1Imm(), cell.rs1, cell.src1());
}

static inline u32 load2(Sh4Context& ctx, const ShilVmCell& cell)
{
	return loadI(ctx, cell.rs2Imm(), cell.rs2, cell.src2());
}

static inline u32 load3(Sh4Context& ctx, const ShilVmCell& cell)
{
	return loadI(ctx, cell.rs3Imm(), cell.rs3, cell.src3());
}

static inline f32 loadF(Sh4Context& ctx, bool isImm, s16 slot, u32 imm)
{
	u32 bits = loadI(ctx, isImm, slot, imm);
	f32 value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}

static inline void storeI(Sh4Context& ctx, s16 rd, u32 value)
{
	*regPtr(ctx, rd) = value;
}

static inline void storeF(Sh4Context& ctx, s16 rd, f32 value)
{
	memcpy(regPtr(ctx, rd), &value, sizeof(value));
}

static inline void storeU64(Sh4Context& ctx, s16 rd, s16 rd2, u64 value)
{
	storeI(ctx, rd, (u32)value);
	if (rd2 != SHIL_VM_NO_REG)
		storeI(ctx, rd2, (u32)(value >> 32));
}

static inline u32 effectiveAddr(Sh4Context& ctx, const ShilVmCell& cell)
{
	u32 addr = load1(ctx, cell);
	if (cell.rs3 != SHIL_VM_NO_REG || cell.rs3Imm())
		addr += load3(ctx, cell);
	return addr;
}

static u32 readMem(Sh4Context& ctx, u32 addr, u32 size, u32 pc)
{
	u32 value;
	if (!mmu_enabled() && shilVmRamRead(s_ram, s_ramSize, s_ramMask, addr, size, &value))
		return value;

	ctx.pc = pc;
	switch (size)
	{
	case 1:
		return (u32)(s32)(s8)ReadMem8(addr);
	case 2:
		return (u32)(s32)(s16)ReadMem16(addr);
	case 4:
		return ReadMem32(addr);
	default:
		die("Invalid read size");
		return 0;
	}
}

static void writeMem(Sh4Context& ctx, u32 addr, u32 size, u32 value, u32 pc)
{
	if (!mmu_enabled() && shilVmRamWrite(s_ram, s_ramSize, s_ramMask, addr, size, value))
		return;

	ctx.pc = pc;

	switch (size)
	{
	case 1:
		WriteMem8(addr, (u8)value);
		break;
	case 2:
		WriteMem16(addr, (u16)value);
		break;
	case 4:
		WriteMem32(addr, value);
		break;
	default:
		die("Invalid write size");
		break;
	}
}

// Everything outside the hot set in executeCell. Kept out of line so that it
// does not share instruction cache with the ops that actually run.
__attribute__((noinline))
static void executeCellRare(Sh4Context& ctx, RuntimeBlockInfo *block, const ShilVmCell& op)
{
	switch (op.op)
	{
	case shop_mov64:
	{
		u32 *src = regPtr(ctx, op.rs1);
		u32 *dst = regPtr(ctx, op.rd);
		dst[0] = src[0];
		dst[1] = src[1];
		break;
	}

	case shop_ifb:
	{
		if (load1(ctx, op))
			ctx.pc = load2(ctx, op);
		const u32 opcode = load3(ctx, op);
		if (ctx.sr.FD == 1 && OpDesc[opcode]->IsFloatingPoint())
			throw SH4ThrownException(block->vaddr + op.guest_offs, Sh4Ex_FpuDisabled);
		OpDesc[opcode]->oph(&ctx, opcode);
		break;
	}

	case shop_sync_sr:
		shil_opcl_sync_sr::f1::impl();
		break;
	case shop_sync_fpscr:
		shil_opcl_sync_fpscr::f1::impl(&ctx);
		break;

	case shop_and:
	case vmop_and_imm:
		storeI(ctx, op.rd, shil_opcl_and::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_or:
	case vmop_or_imm:
		storeI(ctx, op.rd, shil_opcl_or::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_xor:
	case vmop_xor_imm:
		storeI(ctx, op.rd, shil_opcl_xor::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_not:
		storeI(ctx, op.rd, shil_opcl_not::f1::impl(load1(ctx, op)));
		break;
	case shop_neg:
		storeI(ctx, op.rd, shil_opcl_neg::f1::impl(load1(ctx, op)));
		break;
	case shop_shl:
	case vmop_shl_imm:
		storeI(ctx, op.rd, shil_opcl_shl::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_shr:
		storeI(ctx, op.rd, shil_opcl_shr::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_sar:
		storeI(ctx, op.rd, shil_opcl_sar::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_adc:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_adc::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op)));
		break;
	case shop_sbc:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_sbc::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op)));
		break;
	case shop_negc:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_negc::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_ror:
		storeI(ctx, op.rd, shil_opcl_ror::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_rocl:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_rocl::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_rocr:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_rocr::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_swaplb:
		storeI(ctx, op.rd, shil_opcl_swaplb::f1::impl(load1(ctx, op)));
		break;
	case shop_shld:
		storeI(ctx, op.rd, shil_opcl_shld::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_shad:
		storeI(ctx, op.rd, shil_opcl_shad::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_ext_s8:
		storeI(ctx, op.rd, shil_opcl_ext_s8::f1::impl(load1(ctx, op)));
		break;
	case shop_ext_s16:
		storeI(ctx, op.rd, shil_opcl_ext_s16::f1::impl(load1(ctx, op)));
		break;
	case shop_mul_u16:
		storeI(ctx, op.rd, shil_opcl_mul_u16::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_mul_s16:
		storeI(ctx, op.rd, shil_opcl_mul_s16::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_mul_i32:
		storeI(ctx, op.rd, shil_opcl_mul_i32::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_mul_u64:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_mul_u64::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_mul_s64:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_mul_s64::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_div32u:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_div32u::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op)));
		break;
	case shop_div32s:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_div32s::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op)));
		break;
	case shop_div32p2:
		storeI(ctx, op.rd, shil_opcl_div32p2::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op)));
		break;
	case shop_div1:
		storeU64(ctx, op.rd, op.rd2, shil_opcl_div1::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op), &ctx));
		break;
	case shop_debug_3:
		shil_opcl_debug_3::f1::impl(load1(ctx, op), load2(ctx, op), load3(ctx, op));
		break;
	case shop_debug_1:
		shil_opcl_debug_1::f1::impl(load1(ctx, op));
		break;
	case shop_cvt_f2i_t:
		storeI(ctx, op.rd, shil_opcl_cvt_f2i_t::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1())));
		break;
	case shop_cvt_i2f_n:
		storeF(ctx, op.rd, shil_opcl_cvt_i2f_n::f1::impl(load1(ctx, op)));
		break;
	case shop_cvt_i2f_z:
		storeF(ctx, op.rd, shil_opcl_cvt_i2f_z::f1::impl(load1(ctx, op)));
		break;
	case shop_pref:
		shil_opcl_pref::f1::impl(load1(ctx, op), &ctx);
		break;
	case shop_setgt:
		storeI(ctx, op.rd, shil_opcl_setgt::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_setpeq:
		storeI(ctx, op.rd, shil_opcl_setpeq::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_fadd:
		storeF(ctx, op.rd, shil_opcl_fadd::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_fsub:
		storeF(ctx, op.rd, shil_opcl_fsub::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_fmul:
		storeF(ctx, op.rd, shil_opcl_fmul::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_fdiv:
		storeF(ctx, op.rd, shil_opcl_fdiv::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_fabs:
		storeF(ctx, op.rd, shil_opcl_fabs::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1())));
		break;
	case shop_fneg:
		storeF(ctx, op.rd, shil_opcl_fneg::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1())));
		break;
	case shop_fsqrt:
		storeF(ctx, op.rd, shil_opcl_fsqrt::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1())));
		break;
	case shop_fipr:
		storeF(ctx, op.rd, shil_opcl_fipr::f1::impl((const float *)regPtr(ctx, op.rs1), (const float *)regPtr(ctx, op.rs2)));
		break;
	case shop_ftrv:
		shil_opcl_ftrv::f1::impl((float *)regPtr(ctx, op.rd), (const float *)regPtr(ctx, op.rs1), (const float *)regPtr(ctx, op.rs2));
		break;
	case shop_fmac:
		storeF(ctx, op.rd, shil_opcl_fmac::f1::impl(
			loadF(ctx, op.rs1Imm(), op.rs1, op.src1()),
			loadF(ctx, op.rs2Imm(), op.rs2, op.src2()),
			loadF(ctx, op.rs3Imm(), op.rs3, op.src3())));
		break;
	case shop_fsrra:
		storeF(ctx, op.rd, shil_opcl_fsrra::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1())));
		break;
	case shop_fsca:
		shil_opcl_fsca::fsca_impl::impl((float *)regPtr(ctx, op.rd), load1(ctx, op));
		break;
	case shop_fseteq:
		storeI(ctx, op.rd, shil_opcl_fseteq::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_fsetgt:
		storeI(ctx, op.rd, shil_opcl_fsetgt::f1::impl(loadF(ctx, op.rs1Imm(), op.rs1, op.src1()), loadF(ctx, op.rs2Imm(), op.rs2, op.src2())));
		break;
	case shop_frswap:
	{
		u32 *const bank0 = regPtr(ctx, op.rd);
		u32 *const bank1 = regPtr(ctx, op.rs1);
		for (int i = 0; i < 16; i++)
		{
			const u32 tmp = bank0[i];
			bank0[i] = bank1[i];
			bank1[i] = tmp;
		}
		break;
	}
	case shop_xtrct:
		storeI(ctx, op.rd, shil_opcl_xtrct::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_illegal:
		shil_opcl_illegal::f1::impl(load1(ctx, op), load2(ctx, op));
		break;
	default:
		ERROR_LOG(DYNAREC, "SHIL VM unhandled op %d", op.op);
		die("Unhandled SHIL opcode");
		break;
	}
}

// Thirteen opcodes account for over 95% of everything the VM executes on a
// real game, and the dispatch loop is instruction-fetch bound - forcing extra
// code into it measurably slows it down. So only those live here, and the
// remaining ~90 sit behind a call that is taken a few percent of the time.
static void executeCell(Sh4Context& ctx, RuntimeBlockInfo *block, const ShilVmCell& op)
{
	switch (op.op)
	{
	case shop_readm:
	case vmop_load:
	{
		const u32 addr = effectiveAddr(ctx, op);
		const u32 pc = block->vaddr + op.guest_offs + 2;
		if (op.size == 8)
		{
			u64 value;
			if (mmu_enabled() || !shilVmRamRead64(s_ram, s_ramSize, s_ramMask, addr, &value))
			{
				ctx.pc = pc;
				value = ReadMem64(addr);
			}
			u32 *dst = regPtr(ctx, op.rd);
			dst[0] = (u32)value;
			dst[1] = (u32)(value >> 32);
		}
		else
		{
			storeI(ctx, op.rd, readMem(ctx, addr, op.size, pc));
		}
		break;
	}

	case shop_writem:
	case vmop_store:
	{
		const u32 addr = effectiveAddr(ctx, op);
		const u32 pc = block->vaddr + op.guest_offs + 2;
		if (op.size == 8)
		{
			u32 *src = regPtr(ctx, op.rs2);
			u64 value = (u64)src[0] | ((u64)src[1] << 32);
			if (mmu_enabled() || !shilVmRamWrite64(s_ram, s_ramSize, s_ramMask, addr, value))
			{
				ctx.pc = pc;
				WriteMem64(addr, value);
			}
		}
		else
		{
			writeMem(ctx, addr, op.size, load2(ctx, op), pc);
		}
		break;
	}

	case shop_jdyn:
	case shop_jcond:
	{
		u32 value = load1(ctx, op);
		if (op.rs2Imm() || op.rs2 != SHIL_VM_NO_REG)
			value += load2(ctx, op);
		storeI(ctx, op.rd, value);
		break;
	}

	case shop_mov32:
	case vmop_mov_imm:
		storeI(ctx, op.rd, load1(ctx, op));
		break;
	case shop_add:
	case vmop_add_imm:
		storeI(ctx, op.rd, shil_opcl_add::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_sub:
	case vmop_sub_imm:
		storeI(ctx, op.rd, shil_opcl_sub::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_test:
		storeI(ctx, op.rd, shil_opcl_test::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_seteq:
		storeI(ctx, op.rd, shil_opcl_seteq::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_setge:
		storeI(ctx, op.rd, shil_opcl_setge::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_setae:
		storeI(ctx, op.rd, shil_opcl_setae::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;
	case shop_setab:
		storeI(ctx, op.rd, shil_opcl_setab::f1::impl(load1(ctx, op), load2(ctx, op)));
		break;

	default:
		executeCellRare(ctx, block, op);
		break;
	}
}

static void endBlock(Sh4Context& ctx, RuntimeBlockInfo *block, ShilVmExitKind kind, bool *condTaken)
{
	bool taken = false;
	u32 dynPc = ctx.jdyn;

	switch (kind)
	{
	case ShilVmExitKind::Cond0:
		taken = (block->has_jcond ? ctx.jdyn : ctx.sr.T) == 0;
		break;
	case ShilVmExitKind::Cond1:
		taken = (block->has_jcond ? ctx.jdyn : ctx.sr.T) == 1;
		break;
	default:
		break;
	}

	ctx.pc = shilVmExitPc(kind, taken, block->NextBlock, block->BranchBlock, dynPc);
	if (kind == ShilVmExitKind::StaticIntr || kind == ShilVmExitKind::DynamicIntr)
		UpdateINTC();
	if (condTaken)
		*condTaken = taken;
}

static RuntimeBlockInfo *lookupBlock(u32 pc)
{
	if (RuntimeBlockInfo *block = bm_GetIrBlock(pc))
		return block;
	RuntimeBlockInfoPtr found = bm_GetBlock(pc);
	return found.get();
}

static bool isDynamicExit(ShilVmExitKind kind)
{
	return kind == ShilVmExitKind::Dynamic || kind == ShilVmExitKind::DynamicIntr;
}

// AddRef does not deduplicate. A static successor is only ever linked once, but
// a dynamic one is relinked whenever the target changes, so the reference list
// would grow without bound on a polymorphic site.
static void addRefOnce(RuntimeBlockInfo *from, RuntimeBlockInfo *to)
{
	RuntimeBlockInfoPtr fromPtr = bm_GetBlock((void *)from->code);
	RuntimeBlockInfoPtr toPtr = bm_GetBlock((void *)to->code);
	if (!fromPtr || !toPtr)
		return;
	if (std::find(toPtr->pre_refs.begin(), toPtr->pre_refs.end(), fromPtr) == toPtr->pre_refs.end())
		toPtr->AddRef(fromPtr);
}

// A dynamic exit - a return, or a jump through a register - has BranchBlock set
// to NullAddress, so the static chain could never match it and every one of
// them fell through to a hash lookup. That was 39% of all block exits.
//
// Remember the last target instead. Returns and indirect call sites are
// overwhelmingly monomorphic in practice, so the next exit almost always goes
// to the same place. pBranchBlock is unused for this exit kind, and storing the
// target there means Discard() already clears it when the target is thrown
// away: a stale dynPc is then harmless because the pointer it pairs with is
// null, and the chain check requires both.
static void cacheDynamicSuccessor(ShilRuntimeBlock *from, RuntimeBlockInfo *to, u32 pc)
{
	from->pBranchBlock = to;
	from->dynPc = pc;
	addRefOnce(from, to);
}

static void linkSuccessor(RuntimeBlockInfo *from, RuntimeBlockInfo *to)
{
	if (from == nullptr || to == nullptr)
		return;
	bool linked = false;
	if (from->BranchBlock == to->vaddr && from->pBranchBlock == nullptr)
	{
		from->pBranchBlock = to;
		linked = true;
	}
	if (from->NextBlock == to->vaddr && from->pNextBlock == nullptr)
	{
		from->pNextBlock = to;
		linked = true;
	}
	if (linked)
		addRefOnce(from, to);
}

class ShilVm : public Sh4Dynarec
{
public:
	ShilVm()
	{
		sh4Dynarec = this;
	}

	void init(Sh4Context& ctx, Sh4CodeBuffer&) override
	{
		INFO_LOG(DYNAREC, "Initializing the SH4 cached IR virtual machine (compact cells, no native codegen)");
		sh4ctx = &ctx;
		s_ram = &mem_b[0];
		s_ramSize = RAM_SIZE;
		s_ramMask = RAM_MASK;
	}

	void compile(RuntimeBlockInfo *block, bool smc_checks, bool) override
	{
		s_compileCount++;
		block->code = (DynarecCodeEntryPtr)block;
		block->host_code_size = sizeof(void *);
		block->host_opcodes = (u32)block->oplist.size();

		auto *irBlock = static_cast<ShilRuntimeBlock *>(block);
		irBlock->program = shilVmEncode(block->oplist);
		block->host_opcodes = (u32)irBlock->program.cells.size();

		irBlock->dynPc = block->BranchBlock;
		irBlock->smcEnabled = smc_checks && block->sh4_code_size != 0 && shilVmIsRam(block->addr);
		if (irBlock->smcEnabled)
		{
			irBlock->smcAddr = block->addr;
			irBlock->smcSize = block->sh4_code_size;
			irBlock->smcHash = shilVmHashRam(s_ram, s_ramSize, s_ramMask, irBlock->smcAddr, irBlock->smcSize);
		}
	}

	void reset() override
	{
	}

	void mainloop(void *) override
	{
		Sh4Context& ctx = *sh4ctx;

		try {
			RuntimeBlockInfo *block = nullptr;
			while (ctx.CpuRunning)
			{
				try {
					if (block == nullptr || block->vaddr != ctx.pc)
					{
						RuntimeBlockInfo *found = lookupBlock(ctx.pc);
						if (found == nullptr)
						{
							rdv_FailedToFindBlock(ctx.pc);
							found = lookupBlock(ctx.pc);
							if (found == nullptr)
								continue;
						}
						block = found;
					}
					block = runBlock(block, ctx);
				}
				catch (SH4ThrownException& ex) {
					Do_Exception(ex.epc, ex.expEvn);
					ctx.cycle_counter -= 5;
					block = nullptr;
				}
			}
		}
		catch (const debugger::Stop&) {
		}
	}

	void handleException(host_context_t&) override {}

	bool rewrite(host_context_t&, void *) override
	{
		return false;
	}

	RuntimeBlockInfo *allocateBlock() override
	{
		return new ShilRuntimeBlock();
	}

	void canonStart(const shil_opcode *) override {}
	void canonParam(const shil_opcode *, const shil_param *, CanonicalParamType) override {}
	void canonCall(const shil_opcode *, void *) override
	{
		die("SHIL VM does not emit canonical native calls");
	}
	void canonFinish(const shil_opcode *) override {}

	RuntimeBlockInfo *runBlock(RuntimeBlockInfo *block, Sh4Context& ctx)
	{
		if (ctx.cycle_counter <= 0)
		{
			ctx.cycle_counter += SH4_TIMESLICE;
			if (!ctx.CpuRunning)
				return nullptr;
			if (UpdateSystem_INTC())
				return nullptr;
		}

		if (mmu_enabled() && ctx.pc != block->vaddr)
		{
			rdv_BlockCheckFail(block->addr);
			return nullptr;
		}

		auto *irBlock = static_cast<ShilRuntimeBlock *>(block);
		if (irBlock->smcEnabled && !shilVmGuestMatches(s_ram, s_ramSize, s_ramMask, irBlock->smcAddr, irBlock->smcSize, irBlock->smcHash))
		{
			rdv_BlockCheckFail(block->addr);
			RuntimeBlockInfo *fresh = lookupBlock(ctx.pc);
			if (fresh == nullptr || fresh == block)
				return nullptr;
			return runBlock(fresh, ctx);
		}

		if (block->has_fpu_op && ctx.sr.FD)
		{
			Do_Exception(block->vaddr, Sh4Ex_FpuDisabled);
			return nullptr;
		}

		// Full SH4 timing — no interpreter CPU_RATIO underclock.
		ctx.cycle_counter -= (int)block->guest_cycles;

		for (const ShilVmCell& cell : irBlock->program.cells)
		{
			try {
				executeCell(ctx, block, cell);
			}
			catch (SH4ThrownException& ex) {
				if (cell.delaySlot())
					AdjustDelaySlotException(ex);
				throw;
			}
		}

		// Derived once and handed to both users: endBlock and the successor
		// select each switched on BlockType, and with blocks averaging four
		// cells that duplication is not lost in the noise.
		const ShilVmExitKind kind = shilVmExitKind(block->BlockType);
		bool taken = false;
		endBlock(ctx, block, kind, &taken);

		ShilVmChain chain;
		chain.next = block->pNextBlock;
		chain.branch = block->pBranchBlock;
		chain.nextPc = block->NextBlock;
		// dynPc is BranchBlock for a static exit and the last observed target
		// for a dynamic one, so the chain check needs no extra branch here.
		chain.branchPc = irBlock->dynPc;
		void *cached = shilVmSelectSuccessor(kind, taken, ctx.pc, chain);
		if (cached != nullptr)
			return (RuntimeBlockInfo *)cached;

		RuntimeBlockInfo *found = lookupBlock(ctx.pc);
		if (found == nullptr)
			return nullptr;
		if (isDynamicExit(kind))
			cacheDynamicSuccessor(irBlock, found, ctx.pc);
		else
			linkSuccessor(block, found);
		return found;
	}

	Sh4Context *sh4ctx = nullptr;
};

static ShilVm shilVm;

bool shilVmStepBlock()
{
	Sh4Context& ctx = *shilVm.sh4ctx;
	RuntimeBlockInfo *found = lookupBlock(ctx.pc);
	if (found == nullptr)
	{
		rdv_FailedToFindBlock(ctx.pc);
		found = lookupBlock(ctx.pc);
		if (found == nullptr)
			return false;
	}
	shilVm.runBlock(found, ctx);
	return true;
}

u32 shilVmStepToSuccessor()
{
	Sh4Context& ctx = *shilVm.sh4ctx;
	RuntimeBlockInfo *found = lookupBlock(ctx.pc);
	if (found == nullptr)
	{
		rdv_FailedToFindBlock(ctx.pc);
		found = lookupBlock(ctx.pc);
		if (found == nullptr)
			return NullAddress;
	}
	RuntimeBlockInfo *next = shilVm.runBlock(found, ctx);
	return next != nullptr ? next->vaddr : NullAddress;
}

u32 shilVmCompileCount()
{
	return s_compileCount;
}

void shilVmResetCompileCount()
{
	s_compileCount = 0;
}

#endif	// FEAT_SHREC == DYNAREC_IR
