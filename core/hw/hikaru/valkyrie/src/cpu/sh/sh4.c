/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* 
 * Valkyrie
 * Copyright (C) 2011, 2012, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * IRQs
 * ====
 *
 * IRQ priorities are encoded in sh4.intc.irqs. Whenever an external IRQ
 * or on-chip IRQ should be raised or cleared, call sh4_set_irq_state.
 * It will update the sh4.intc.irqs table with the proper state.
 *
 * The interrupt priorities are either fixed (for external IRQs an
 * exceptions) or decided by the INTC settings; sh4_ireg_put will make
 * sure to update the sh4.intc.irqs priorities according to the INTC
 * configuration.
 */

/* TODO: MMU */
/* TODO: propagate get/put and instruction errors to the main loop */
/* TODO: implement exceptions; this is really needed only with an MMU */
/* TODO: handle FP exceptions and rounding mode */

#include "vk/core.h"
#include "engine_internal.h"
#include "vk/cpu.h"
#include "vk/state.h"

#include "sh4.h"
#include "sh4-ireg.h"

/* Helper Macros */

#define _RN	((inst >> 8) & 15)
#define _RM	((inst >> 4) & 15)
#define _UIMM8	((uint32_t)(uint8_t) inst)
#define _SIMM8	((int32_t)(int8_t) inst)
#define _SIMM12 ((int32_t)((inst & 0xFFF) | ((inst & 0x800) ? 0xFFFFF000 : 0)))

#define PC	ctx->regs.pc
#define PR	ctx->regs.pr
#define SR	ctx->regs.sr
#define MAC	ctx->regs.mac.full
#define MACH	ctx->regs.mac.field.hi
#define MACL	ctx->regs.mac.field.lo
#define GBR	ctx->regs.gbr
#define VBR	ctx->regs.vbr
#define SSR	ctx->regs.ssr
#define SPC	ctx->regs.spc
#define DBR	ctx->regs.dbr
#define SGR	ctx->regs.sgr

#define R(n_)	ctx->regs.r[n_]
#define R0	R(0)
#define RN	R(_RN)
#define RM	R(_RM)
#define SP	R(15)

#define RBANK(n_) ctx->regs.rbank[n_]

#define T	SR.bit.t
#define S	SR.bit.s
#define Q	SR.bit.q
#define M	SR.bit.m

#define FPSCR	ctx->regs.fpscr
#define FPUL	ctx->regs.fpul

#define FR(n_)	ctx->regs.f.f[n_]
#define DR(n_)	ctx->regs.f.d[(n_)/2]

#define XF(n_)	ctx->regs.x.f[n_]
#define XD(n_)	ctx->regs.x.d[(n_)/2]

#define FRN	FR(_RN)
#define FRM	FR(_RM)

#define DRN	DR(_RN)
#define DRM	DR(_RM)

#define XDN	XD(_RN)
#define XDM	XD(_RM)

#define IS_PRIVILEGED \
	(SR.bit.md == 1)

#define IS_FP_ENABLED \
	(SR.bit.fd == 0)

#define CHECK_PM \
	do { \
		VK_CPU_ASSERT (ctx, IS_PRIVILEGED); \
	} while (0);

#define CHECK_FP \
	do { \
		VK_CPU_ASSERT (ctx, IS_FP_ENABLED); \
	} while (0);

/* Taken from MAME */
#define SHRINK(addr_) \
	((((addr_) >> 8) & 0xFF00) | ((addr_) & 0xFF))

#define IREG_GET(size_, addr_) \
	vk_buffer_get (ctx->iregs, size_, SHRINK (addr_))

#define IREG_PUT(size_, addr_, val_) \
	vk_buffer_put (ctx->iregs, size_, SHRINK (addr_), val_)

/* Forward declarations */
static int	sh4_get (sh4_t *, unsigned, uint32_t, void *);
static int	sh4_put (sh4_t *, unsigned, uint32_t, uint64_t);
static void	sh4_update_irqs (sh4_t *ctx);
static int	sh4_set_irq_state (vk_cpu_t *cpu, unsigned num, vk_irq_state_t state);

/* Generic Helpers */

static void
swap_r_banks (sh4_t *ctx)
{
	unsigned i;
	for (i = 0; i < 8; i++) {
		uint32_t tmp = R(i);
		R(i) = RBANK(i);
		RBANK(i) = tmp;
	}
}

static void
swap_f_banks (sh4_t *ctx)
{
	unsigned i;
	for (i = 0; i < 16; i++) {
		uint32_t tmp = FR(i).u;
		FR(i).u = XF(i).u;
		XF(i).u = tmp;
	}
}

static unsigned
get_r_bank_num (sh4_sr_t sr)
{
	/* In user mode, bank 0 is always selected; in privileged mode, bank
	 * SR.bit.rb is selected; it follows that bank 1 is only selected if
	 * both MD and RB are set. */
	return sr.bit.md & sr.bit.rb;
}

static void
set_sr (sh4_t *ctx, uint32_t data)
{
	sh4_sr_t old = SR;

	SR.full = data & 0x700083F3;

	/* Swap R banks if required */
	if (get_r_bank_num (old) != get_r_bank_num (SR))
		swap_r_banks (ctx);

	/* If any IRQ-related bits changed, re-validate pending IRQs */
	if ((old.bit.i != SR.bit.i) ||
	    (old.bit.bl != SR.bit.bl))
		sh4_update_irqs (ctx);
}

static uint32_t
get_sr (sh4_t *ctx)
{
	return SR.full;
}

static void
set_fpscr (sh4_t *ctx, uint32_t val)
{
	sh4_fpscr_t old = FPSCR;

	FPSCR.full = val & 0x003FFFFF;
	/* Swap FR banks if required */
	if (old.bit.fr != FPSCR.bit.fr)
		swap_f_banks (ctx);

	/* SZ and PR can't be both set */
	if (FPSCR.bit.sz && FPSCR.bit.pr)
		VK_CPU_ABORT (ctx, "invalid FPSCR: SZ and PR both set");
}

static uint32_t
get_fpscr (sh4_t *ctx)
{
	return FPSCR.full;
}

/* Port A */

/* XXX Port A emulation is still very rough; we probably want to notify the
 * external handlers about the directions of all bits; although they already
 * probably know what to do with them. */

static void
set_porta (sh4_t *ctx, uint16_t data)
{
	uint32_t pctra;
	uint16_t pdtra;
	unsigned i;

	VK_ASSERT (ctx->porta.put);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to output */
		if (direction != 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	if (ctx->porta.put (ctx, pdtra))
		VK_ASSERT (0);
}

static uint16_t
get_porta (sh4_t *ctx)
{
	uint32_t pctra;
	uint16_t pdtra, data;
	unsigned i;

	VK_ASSERT (ctx->porta.get);

	pdtra = IREG_GET (2, BSC_PDTRA);
	pctra = IREG_GET (4, BSC_PCTRA);

	if (ctx->porta.get (ctx, &data))
		VK_ASSERT (0);
	for (i = 0; i < 16; i++) {
		unsigned direction = (pctra >> (i * 2)) & 1;
		/* only override those bits that are set to input */
		if (direction == 0) {
			pdtra &= ~(1 << i);
			pdtra |= data & (1 << i);
		}
	}
	return pdtra;
}

/* Interrupt Controller */

static void
set_irq_priority (sh4_t *ctx, unsigned num, unsigned priority)
{
	/* An IRQ priority can't be lowered while an IRQ is firing */
	VK_ASSERT ((ctx->intc.irqs[num].state != VK_IRQ_STATE_RAISED) ||
	           (ctx->intc.irqs[num].priority <= priority));
	ctx->intc.irqs[num].priority = priority;
}

static void
sh4_intc_update_priorities (sh4_t *ctx)
{
	uint16_t ipra = IREG_GET (2, INTC_IPRA);
	uint16_t iprb = IREG_GET (2, INTC_IPRB);
	uint16_t iprc = IREG_GET (2, INTC_IPRC);

	/* See Table 19.5, "Interrupt Exception Sources and Priority Order" */
	set_irq_priority (ctx, SH4_IESOURCE_UDI, iprc & 15);
	set_irq_priority (ctx, SH4_IESOURCE_GPIOI, (iprc >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE0, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE1, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE2, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMTE3, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_DMAE, (iprc >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI0, (ipra >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI1, (ipra >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TUNI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TICPI2, (ipra >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ATI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_PRI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_CUI, ipra & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TEI, (iprb >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ERIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_BRIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_TXIF, (iprc >> 4) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ITI, (iprb >> 12) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_RCMI, (iprb >> 8) & 15);
	set_irq_priority (ctx, SH4_IESOURCE_ROVI, (iprb >> 8) & 15);

	/* Update the pending flag for the new priorities */
	sh4_update_irqs (ctx);
}

/* DMA Controller */

/* XXX DTR mode */
/* XXX validate RS settings against SAR and DAR */
/* XXX raise a DMA AE if any access error occurs. */
/* XXX synchronization with the TMU if required */
/* XXX most of this stuff can be set at write time */

static const uint32_t ts_incr[8] = { 8, 1, 2, 4, 32, 0, 0, 0 };

#define DMAC_DO_TRANSFER(size_) \
	do { \
		for (; cycles > 0 && tcr > 0; cycles--, tcr--) { \
			uint64_t tmp; \
			if (sm == 2) \
				sar -= size_; \
			if (dm == 2) \
				dar -= size_; \
			if ((size_) == 32) { \
				sh4_get (ctx, 8, sar, &tmp); \
				sh4_put (ctx, 8, dar, tmp); \
				sh4_get (ctx, 8, sar+8, &tmp); \
				sh4_put (ctx, 8, dar+8, tmp); \
				sh4_get (ctx, 8, sar+16, &tmp); \
				sh4_put (ctx, 8, dar+16, tmp); \
				sh4_get (ctx, 8, sar+24, &tmp); \
				sh4_put (ctx, 8, dar+24, tmp); \
			} else { \
				sh4_get (ctx, size_, sar, &tmp); \
				sh4_put (ctx, size_, dar, tmp); \
			} \
			if (sm == 1) \
				sar += size_; \
			if (dm == 1) \
				dar += size_; \
		} \
	} while (0)

static void
sh4_dmac_run_channel (sh4_t *ctx, unsigned ch, int cycles)
{
	uint32_t offs = ch * 0x10;
	uint32_t sar  = IREG_GET (4, DMAC_SAR0 + offs);
	uint32_t dar  = IREG_GET (4, DMAC_DAR0 + offs);
	uint32_t tcr  = IREG_GET (4, DMAC_TCR0 + offs);
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);

	uint32_t ts = (chcr >> 4) & 7;
	uint32_t sm = (chcr >> 12) & 3;
	uint32_t dm = (chcr >> 14) & 3;

	VK_CPU_LOG (ctx, "DMAC ch%u: %08X->%08X x %X [%uB, sm=%u, dm=%u]",
	            ch, sar, dar, tcr, ts_incr[ts], sm, dm);

	switch (ts) {
	case 0: /* 8 bytes */
		DMAC_DO_TRANSFER (8);
		break;
	case 1: /* 1 byte */
		DMAC_DO_TRANSFER (1);
		break;
	case 2: /* 2 bytes */
		DMAC_DO_TRANSFER (2);
		break;
	case 3: /* 4 bytes */
		DMAC_DO_TRANSFER (4);
		break;
	case 4: /* 32 bytes */
		DMAC_DO_TRANSFER (32);
		break;
	default:
		VK_ASSERT (!"invalid DMAC transfer size");
		break;
	}

	IREG_PUT (4, DMAC_SAR0 + offs, sar);
	IREG_PUT (4, DMAC_DAR0 + offs, dar);
	IREG_PUT (4, DMAC_TCR0 + offs, tcr);

	if (tcr == 0) {
		chcr |= 2; /* TE */
		if (chcr & 4) { /* IE */
			unsigned num;
			num = (ch == 0) ? SH4_IESOURCE_DMTE0 :
			      (ch == 1) ? SH4_IESOURCE_DMTE1 :
			      (ch == 2) ? SH4_IESOURCE_DMTE2 :
			      SH4_IESOURCE_DMTE3;
			sh4_set_irq_state ((vk_cpu_t *) ctx, num,
			                   VK_IRQ_STATE_RAISED);
		}
		ctx->dmac.is_running[ch] = false;
		IREG_PUT (4, DMAC_CHCR0 + offs, chcr);
	}
}

static void
sh4_dmac_run (sh4_t *ctx, int cycles)
{
	/* TODO: priorities (DMAOR.PR). Are they really that important? */
	bool was_dma_active = ctx->dma_active;
	ctx->dma_active = true;

	if (ctx->dmac.is_running[0])
		sh4_dmac_run_channel (ctx, 0, cycles);
	if (ctx->dmac.is_running[1])
		sh4_dmac_run_channel (ctx, 1, cycles);
	if (ctx->dmac.is_running[2])
		sh4_dmac_run_channel (ctx, 2, cycles);
	if (ctx->dmac.is_running[3])
		sh4_dmac_run_channel (ctx, 3, cycles);
	ctx->dma_active = was_dma_active;
}

static void
sh4_dmac_update_channel_state (sh4_t *ctx, unsigned ch, uint32_t request_type)
{
	uint32_t offs = ch * 0x10;
	uint32_t dmaor = IREG_GET (4, DMAC_DMAOR);
	uint32_t chcr = IREG_GET (4, DMAC_CHCR0 + offs);

	VK_ASSERT (ch < 4);
	ctx->dmac.is_running[ch] = false;

	/* Check that both DME and DE are set */
	if (dmaor & chcr & 1) {
		uint32_t sar = IREG_GET (4, DMAC_SAR0 + offs);
		uint32_t dar = IREG_GET (4, DMAC_DAR0 + offs);
		uint32_t ts = (chcr >> 4) & 7;
		uint32_t rs = (chcr >> 8) & 15;
		uint32_t sm = (chcr >> 12) & 3;
		uint32_t dm = (chcr >> 14) & 3;

		VK_ASSERT (ts < 5);
		VK_ASSERT (sm != 3);
		VK_ASSERT (dm != 3);
		VK_ASSERT (rs != 1 && rs != 7 && rs != 15);
//		VK_ASSERT (AREA (dar) != 7);

		/* Check the addresses and update AE if needed; bail out and
		 * send an Address Error exception. We only do it here,
		 * because tick_channel () can't alter the addresses as to
		 * raise an AE if they are correct here (by induction.) */
		if ((sar | dar) & (ts_incr[ts] - 1)) {
			VK_CPU_LOG (ctx, "DMAC: raising DMA address error");
			sh4_set_irq_state ((vk_cpu_t *) ctx,
			                   SH4_IESOURCE_DMAE,
			                   VK_IRQ_STATE_RAISED);
			return;
		}

		/* Check if NMIF, AE or TE have been set */
		if ((dmaor & 6) || (chcr & 2))
			return;

		/* All checks passed; this DMA channel may now run */
		if ((rs >> 2) == request_type) {
			VK_CPU_LOG (ctx, "DMAC: enabling channel %u", ch);
			ctx->dmac.is_running[ch] = true;
		}
	}
}

static void
sh4_dmac_update_state (sh4_t *ctx, uint32_t request_type)
{
	sh4_dmac_update_channel_state (ctx, 0, request_type);
	sh4_dmac_update_channel_state (ctx, 1, request_type);
	sh4_dmac_update_channel_state (ctx, 2, request_type);
	sh4_dmac_update_channel_state (ctx, 3, request_type);
}

/* Timer Unit */

/* Note: for performance reasons, TCNT{0,1,2} are handled differently than
 * the other on-chip registers, and are defined as uint32_t directly. */

#define TMU_TCOR(n_)	(TMU_TCOR0 + (n_) * 12)
#define TMU_TCNT(n_)	(TMU_TCNT0 + (n_) * 12)
#define TMU_TCR(n_)	(TMU_TCR0 + (n_) * 12)

static void
sh4_tmu_run_channel (sh4_t *ctx, unsigned ch, int cycles)
{
	uint32_t counter = ctx->tmu.counter[ch];

	for (; cycles > 0; cycles--, counter--) {
		/* Check for underflow */
		if (counter == 0) {
			uint16_t tcr;

			/* Set UNF */
			tcr = IREG_GET (2, TMU_TCR (ch));
			IREG_PUT (2, TMU_TCR (ch), tcr | 0x100);

			/* Reload the timer */
			counter = IREG_GET (4, TMU_TCOR (ch));

			/* Raise an IRQ if UNIE is set */
			if (tcr & 0x20) {
				static const unsigned nums[3] = {
					SH4_IESOURCE_TUNI0,
					SH4_IESOURCE_TUNI1,
					SH4_IESOURCE_TUNI2,
				};

				VK_CPU_LOG (ctx, "TMU: rising ch%u IRQ", ch);

				sh4_set_irq_state ((vk_cpu_t *) ctx, nums[ch],
				                   VK_IRQ_STATE_RAISED);
			}
		}
	}

	ctx->tmu.counter[ch] = counter;
}

static void
sh4_tmu_run (sh4_t *ctx, int cycles)
{
	if (ctx->tmu.is_running[0])
		sh4_tmu_run_channel (ctx, 0, cycles);
	if (ctx->tmu.is_running[1])
		sh4_tmu_run_channel (ctx, 1, cycles);
	if (ctx->tmu.is_running[2])
		sh4_tmu_run_channel (ctx, 2, cycles);
}

static void
sh4_tmu_update_freq (sh4_t *ctx)
{
	/* TODO */
}

static void
sh4_tmu_update_state (sh4_t *ctx)
{
	uint8_t tstr = IREG_GET (1, TMU_TSTR);

	ctx->tmu.is_running[0] = tstr & 1;
	ctx->tmu.is_running[1] = (tstr >> 1) & 1;
	ctx->tmu.is_running[2] = (tstr >> 2) & 1;

	VK_CPU_LOG (ctx, "TMU: settings states: %u, %u, %u",
	            ctx->tmu.is_running[0],
	            ctx->tmu.is_running[1],
	            ctx->tmu.is_running[2]);
}

/* On-chip Modules
 * See Table A.1, "Address List" */

static int
sh4_ireg_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	VK_CPU_LOG (ctx, "IREG R%u %08X", size * 8, addr);

	set_ptr (val, size, IREG_GET (size, addr));

	switch (addr & 0xFFFFFF) {
	case BSC_RFCR:
	case BSC_PDTRA:
	case CPG_WTCSR:
		VK_ASSERT (size == 2);
		set_ptr (val, size, get_porta (ctx));
		break;
	case CCN_CCR:
	case CCN_INTEVT:
	case BSC_PCTRA:
		VK_ASSERT (size == 4);
		break;
	/* INTC */
	case INTC_IPRA:
	case INTC_IPRB:
	case INTC_IPRC:
		VK_ASSERT (size == 2);
		break;
	/* DMAC */
	case DMAC_SAR0 ... DMAC_DMAOR:
		VK_ASSERT (size == 4);
		break;
	/* TMU */
	case TMU_TSTR:
		VK_ASSERT (size == 1);
		break;
	case TMU_TCNT0:
		VK_ASSERT (size == 4);
		set_ptr (val, size, ctx->tmu.counter[0]);
		break;
	/* Invalid/Unhandled */
	default:
		return -1;
	}
	return 0;
}

/* TODO: mask writes to read-only bits */

/* Writes to SAR0,DAR0,TCR0,CHCR0 are masked when DMAOR.DDT is set */
#define DMAC_MASK_ON_DDT \
	{ \
		uint32_t dmaor = IREG_GET (4, DMAC_DMAOR); \
		if ((dmaor & 0x8000) && (ch == 0)) \
			return 0; \
	}

static int
sh4_ireg_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	VK_CPU_LOG (ctx, "IREG W%u %08X = %" PRIX64 "", size * 8, addr, val);

	switch (addr & 0xFFFFFF) {
	case 0x900000 ... 0x90FFFF: /* BSC_SDRM2 */
	case 0x940000 ... 0x94FFFF: /* BSC_SDRM3 */
	case CPG_STBCR:
		VK_ASSERT (size == 1);
		break;
	case BSC_BCR2:
	case BSC_PCR:
	case BSC_RTCSR:
	case BSC_RTCNT:
	case BSC_RTCOR:
	case BSC_RFCR:
	case CPG_WTCSR:
		VK_ASSERT (size == 2);
		break;
	case BSC_PDTRA:
		VK_ASSERT (size == 2);
		set_porta (ctx, val);
		break;
	case CCN_MMUCR:
	case CCN_CCR:
	case CCN_QACR0:
	case CCN_QACR1:
	case BSC_BCR1:
	case BSC_WCR1:
	case BSC_WCR2:
	case BSC_WCR3:
	case BSC_MCR:
	case BSC_PCTRA:
		VK_ASSERT (size == 4);
		break;
	/* UBC */
	case UBC_BBRA:
	case UBC_BBRB:
		VK_ASSERT (size == 2);
		break;
	/* INTC */
	case INTC_ICR:
		{
			uint16_t old = IREG_GET (2, addr);
			VK_ASSERT (size == 2);
			VK_ASSERT (!(val & ~0xC380));
			/* ICR.NMIL is read only */
			IREG_PUT (2, addr, (old & 0x8000) | (val & 0x7FFF));
			sh4_update_irqs (ctx);
		}
		return 0;
	case INTC_IPRA:
		VK_ASSERT (size == 2);
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	case INTC_IPRB:
		VK_ASSERT (size == 2);
		VK_ASSERT (!(val & 0xF));
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	case INTC_IPRC:
		VK_ASSERT (size == 2);
		IREG_PUT (2, addr, val);
		sh4_intc_update_priorities (ctx);
		return 0;
	/* DMAC */
	case DMAC_SAR0:
	case DMAC_SAR1:
	case DMAC_SAR2:
	case DMAC_SAR3:
	case DMAC_DAR0:
	case DMAC_DAR1:
	case DMAC_DAR2:
	case DMAC_DAR3:
		{
			unsigned ch = (addr >> 4) & 3;
			DMAC_MASK_ON_DDT;
			VK_ASSERT (size == 4);
			VK_ASSERT (!(ctx->dmac.is_running[ch]));
		}
		break;
	case DMAC_TCR0:
	case DMAC_TCR1:
	case DMAC_TCR2:
	case DMAC_TCR3:
		{
			unsigned ch = (addr >> 4) & 3;
			DMAC_MASK_ON_DDT;
			VK_ASSERT (size == 4);
			/* violated by SGNASCAR @0C071EA8:
			VK_ASSERT (!(val & 0xFF000000));
			*/
			VK_ASSERT (!(ctx->dmac.is_running[ch]));
		}
		break;
	case DMAC_CHCR0:
	case DMAC_CHCR1:
	case DMAC_CHCR2:
	case DMAC_CHCR3:
		{
			unsigned ch = (addr >> 4) & 3;
			uint32_t old = IREG_GET (size, addr);
			DMAC_MASK_ON_DDT;
			VK_ASSERT (size == 4);
			VK_ASSERT (!(val & 0x00F00008));
			VK_ASSERT ((ch < 2) || !(val & 0x00050000));
			/* Make sure that TE doesn't get set */
			IREG_PUT (size, addr, (val & ~2) | (old & val & 2));
			sh4_dmac_update_channel_state (ctx, ch, 1);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		}
		return 0;
	case DMAC_DMAOR:
		{
			uint32_t old = IREG_GET (size, addr);
			uint32_t nmil = IREG_GET (2, INTC_ICR) & 0x8000 ? 1 : 0;
			VK_ASSERT (size == 4);
			VK_ASSERT (!(val & 0xFFFF7CF8));
			/* DDT is unsupported */
			//VK_ASSERT (!(val & 0x8000)); /* XXX used in Hikaru... */
			/* Make sure that AE and NMIF don't get set; also
			 * make sure that NMIF is never cleared if an NMI
			 * is still raised. */
			IREG_PUT (size, addr, (val & ~6) | (old & val & 6) | (nmil << 1));
			sh4_dmac_update_state (ctx, 1);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		}
		return 0;
	/* TMU */
	case TMU_TOCR:
		VK_ASSERT (size == 1);
		VK_ASSERT (!(val & 0xFE));
		IREG_PUT (size, addr, val);
		sh4_tmu_update_freq (ctx);
		return 0;
	case TMU_TSTR:
		VK_ASSERT (size == 1);
		VK_ASSERT (!(val & 0xF8));
		IREG_PUT (size, addr, val);
		sh4_tmu_update_state (ctx);
		return 0;
	case TMU_TCR0:
	case TMU_TCR1:
		{
			uint16_t old = IREG_GET (size, addr);
			VK_ASSERT (size == 2);
			VK_ASSERT (!(val & 0xFEC0));
			/* Make sure not to set UNF */
			IREG_PUT (size, addr, (val & 0x00FF) | (old & val & 0x0100));
			sh4_tmu_update_freq (ctx);
		}
		return 0;
	case TMU_TCR2:
		{
			uint16_t old = IREG_GET (size, addr);
			VK_ASSERT (size == 2);
		 	VK_ASSERT (!(val & 0xFC00));
			/* XXX input capture is unsupported */
			VK_ASSERT (!(val & 0x0080));
			/* Make sure not to set ICPF, UNF */
			IREG_PUT (size, addr, (val & 0x00FF) | (old & val & 0x0300));
			sh4_tmu_update_freq (ctx);
		}
		return 0;
	case TMU_TCOR0:
	case TMU_TCOR1:
	case TMU_TCOR2:
		VK_ASSERT (size == 4);
		break;
	case TMU_TCNT0:
		VK_ASSERT (size == 4);
		ctx->tmu.counter[0] = val;
		return 0;
	case TMU_TCNT1:
		VK_ASSERT (size == 4);
		ctx->tmu.counter[1] = val;
		return 0;
	case TMU_TCNT2:
		VK_ASSERT (size == 4);
		ctx->tmu.counter[2] = val;
		return 0;
	/* Invalid/Unhandled */
	default:
		return -1;
	}
	IREG_PUT (size, addr, val);
	return 0;
}

/* Store Queues */

static uint32_t
get_sq_addr (sh4_t *ctx, uint32_t addr)
{
	uint32_t sq_num = (addr >> 5) & 1;
	uint32_t sq_base = (sq_num == 0) ?
	                   vk_buffer_get (ctx->iregs, 4, SHRINK (CCN_QACR0)) :
	                   vk_buffer_get (ctx->iregs, 4, SHRINK (CCN_QACR1));
	return ((sq_base & 0x1C) << 24) | (addr & 0x03FFFFE0);
}

static int
sh4_sq_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	uint32_t sq_addr = get_sq_addr (ctx, addr);
	return sh4_get (ctx, size, sq_addr | (addr & 0x1F), val);
}

static int
sh4_sq_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	uint32_t sq_addr = get_sq_addr (ctx, addr) | (addr & 0x1F);
	return sh4_put (ctx, size, sq_addr | (addr & 0x1F), val);
}

/* Bus Access */

#define IS_STORE_QUEUE(addr_) \
	(addr >= 0xE0000000 && addr <= 0xE3FFFFFF)

#define IS_ON_CHIP(addr_) \
	(((addr >> 24) == 0x1F) || \
	 ((addr >> 24) == 0xFF))

#define ADDR_MASK 0x1FFFFFFF

#define AREA(addr_) \
	(((addr_) >> 26) & 7)

static int
sh4_fetch (sh4_t *ctx, uint32_t addr, uint16_t *inst)
{
	int ret;

	ret = vk_mmap_fetch16 (ctx->base.mmap, addr & ADDR_MASK, inst);
	if (ret)
		VK_CPU_ABORT (ctx, "unhandled fetch @%08X", addr);
	return 0;
}

void
sh4_charge_sound_ram_read(vk_cpu_t *cpu, unsigned cpu_clock_mhz)
{
	if (!cpu) return;
	sh4_t *ctx = (sh4_t *)cpu;
	if (!ctx->cpu_active || !ctx->data_read_active || ctx->dma_active) return;
	/* Estimated compatibility latency, not a measured 315-6154 value:
	 * 320 ns per CPU read, converted to CPU cycles with fractional carry. */
	ctx->sound_wait_this_slice = true;
	uint32_t scaled = 320u * cpu_clock_mhz + ctx->sound_wait_fraction;
	ctx->sound_wait_fraction = scaled % 1000u;
	cpu->remaining -= (int)(scaled / 1000u);
}

static int
sh4_get (sh4_t *ctx, unsigned size, uint32_t addr, void *val)
{
	int ret;
	bool was_read_active = ctx->data_read_active;
	ctx->data_read_active = true;

	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_get (ctx, size, addr, val);
	else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_get (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF8000000) {
		VK_CPU_LOG (ctx, "ONCHIP R%d @%08X", 8*size, addr);
		set_ptr (val, size, 0);
		ctx->data_read_active = was_read_active;
		return 0;
	} else
		ret = vk_mmap_get_data_fast (ctx->base.mmap, size, addr & ADDR_MASK, val);
	ctx->data_read_active = was_read_active;
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret) {
		uint64_t count = vk_host_diagnostic (ctx->base.base.mach->host, 1, addr);
		if (count) VK_CPU_ERROR (ctx, "unhandled R%d @%08X (diagnostic count %llu)", 8*size, addr, (unsigned long long) count);
	}
	return ret;
}

static int
sh4_put (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t val)
{
	int ret;

	if (IS_ON_CHIP (addr))
		ret = sh4_ireg_put (ctx, size, addr, val);
	else if (addr >= 0xF0000000 && addr < 0xF8000000) {
		VK_CPU_ABORT (ctx, "ONCHIP W%d @%08X = %" PRIX64 "", 8*size, addr, val);
		return 0;
	} else if (IS_STORE_QUEUE (addr))
		ret = sh4_sq_put (ctx, size, addr, val);
	else
		ret = vk_mmap_put_data_fast (ctx->base.mmap, size, addr & ADDR_MASK, val);
	/* TODO propagate to the caller to allow for memory exceptions */
	if (ret) {
		uint64_t count = vk_host_diagnostic (ctx->base.base.mach->host, 2, addr);
		if (count) VK_CPU_ERROR (ctx, "unhandled W%d @%08X = %" PRIX64 " (diagnostic count %llu)", 8*size, addr, val, (unsigned long long) count);
	}
	return ret;
}

#ifdef HIKARU_VENDOR_TESTING
static bool read_hit_enabled = true;
void hikaru_vendor_test_read_hit (bool enabled) { read_hit_enabled = enabled; }
#endif

/* Reuse only an already validated plain-LE read span. This keeps the common
 * fixed-width RAM load in its instruction handler; every miss still follows
 * sh4_get, including endian conversion, overlays, logging and device effects.
 * High special spaces and the physical on-chip alias never use this shortcut. */
static inline
#if defined(__OPTIMIZE__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((always_inline))
#endif
bool
sh4_read_cached (sh4_t *ctx, unsigned size, uint32_t addr, void *out)
{
#ifdef HIKARU_VENDOR_TESTING
	if (!read_hit_enabled) return false;
#endif
	if (addr >= 0xE0000000 || (addr >> 24) == 0x1F) return false;
	unsigned index;
	switch (size) {
	case 1: index = 0; break;
	case 2: index = 1; break;
	case 4: index = 2; break;
	case 8: index = 3; break;
	default: return false;
	}
	const vk_mmap_read_span_t *span = &ctx->base.mmap->data_spans[index];
	uint32_t physical = addr & ADDR_MASK;
	if (!span->data || physical < span->first ||
	    (uint64_t)physical + size - 1 > span->last) return false;
	memcpy (out, span->data + physical - span->first, size);
	return true;
}

static inline uint8_t
R8 (sh4_t *ctx, uint32_t addr)
{
	uint8_t tmp;
	if (!sh4_read_cached (ctx, 1, addr, &tmp))
		sh4_get (ctx, 1, addr, &tmp);
	return tmp;
}

static inline uint16_t
R16 (sh4_t *ctx, uint32_t addr)
{
	uint16_t tmp;
	if (!sh4_read_cached (ctx, 2, addr, &tmp))
		sh4_get (ctx, 2, addr, &tmp);
	return tmp;
}

static inline uint32_t
R32 (sh4_t *ctx, uint32_t addr)
{
	uint32_t tmp;
	if (!sh4_read_cached (ctx, 4, addr, &tmp))
		sh4_get (ctx, 4, addr, &tmp);
	return tmp;
}

static inline uint64_t
R64 (sh4_t *ctx, uint32_t addr)
{
	uint64_t tmp;
	if (!sh4_read_cached (ctx, 8, addr, &tmp))
		sh4_get (ctx, 8, addr, &tmp);
	return tmp;
}

#ifdef HIKARU_VENDOR_TESTING
bool hikaru_vendor_test_cached_read (sh4_t *ctx, unsigned size, uint32_t addr, void *out)
{ return sh4_read_cached (ctx, size, addr, out); }
uint64_t hikaru_vendor_test_read_value (sh4_t *ctx, unsigned size, uint32_t addr)
{
	switch (size) {
	case 1: return R8 (ctx, addr);
	case 2: return R16 (ctx, addr);
	case 4: return R32 (ctx, addr);
	case 8: return R64 (ctx, addr);
	default: return 0;
	}
}
#endif

#ifdef HIKARU_VENDOR_TESTING
static bool write_hit_enabled = true;
void hikaru_vendor_test_write_hit (bool enabled) { write_hit_enabled = enabled; }
#endif

/* As with reads, inline only existing validated plain-LE spans. Special
 * spaces and misses keep sh4_put's mapping, byte conversion and side effects. */
static inline
#if defined(__OPTIMIZE__) && (defined(__GNUC__) || defined(__clang__))
__attribute__((always_inline))
#endif
bool
sh4_write_cached (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t value)
{
#ifdef HIKARU_VENDOR_TESTING
	if (!write_hit_enabled) return false;
#endif
	if (addr >= 0xE0000000 || (addr >> 24) == 0x1F) return false;
	unsigned index;
	switch (size) {
	case 1: index = 0; break;
	case 2: index = 1; break;
	case 4: index = 2; break;
	case 8: index = 3; break;
	default: return false;
	}
	const vk_mmap_write_span_t *span = &ctx->base.mmap->write_spans[index];
	uint32_t physical = addr & ADDR_MASK;
	if (!span->data || physical < span->first ||
	    (uint64_t)physical + size - 1 > span->last) return false;
	uint8_t *destination = span->data + physical - span->first;
	switch (size) {
	case 1: *destination = (uint8_t)value; break;
	case 2: { uint16_t v = (uint16_t)value; memcpy(destination, &v, sizeof(v)); break; }
	case 4: { uint32_t v = (uint32_t)value; memcpy(destination, &v, sizeof(v)); break; }
	case 8: memcpy(destination, &value, sizeof(value)); break;
	}
	return true;
}

#ifdef HIKARU_VENDOR_TESTING
bool hikaru_vendor_test_cached_write (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t value)
{ return sh4_write_cached (ctx, size, addr, value); }
int hikaru_vendor_test_write_value (sh4_t *ctx, unsigned size, uint32_t addr, uint64_t value)
{
	if (sh4_write_cached (ctx, size, addr, value)) return 0;
	return sh4_put (ctx, size, addr, value);
}
#endif

static inline void
W8 (sh4_t *ctx, uint32_t addr, uint8_t val)
{
	if (!sh4_write_cached (ctx, 1, addr, val))
		sh4_put (ctx, 1, addr, val);
}

static inline void
W16 (sh4_t *ctx, uint32_t addr, uint16_t val)
{
	if (!sh4_write_cached (ctx, 2, addr, val))
		sh4_put (ctx, 2, addr, val);
}

static inline void
W32 (sh4_t *ctx, uint32_t addr, uint32_t val)
{
	if (!sh4_write_cached (ctx, 4, addr, val))
		sh4_put (ctx, 4, addr, val);
}

static inline void
W64 (sh4_t *ctx, uint32_t addr, uint64_t val)
{
	if (!sh4_write_cached (ctx, 8, addr, val))
		sh4_put (ctx, 8, addr, val);
}

/* Interrupt Controller */

/**
 * Updates the intc.pending flag depending on whether an IRQ is pending or
 * not.
 */
static void
sh4_update_irqs (sh4_t *ctx)
{
	unsigned priority, i;
	int maxi;
	uint16_t icr = IREG_GET (2, INTC_ICR);

	/* Default state: no IRQ pending */
	ctx->intc.pending = false;
	ctx->intc.index = -1;

	/* Handle NMI first. NMI is always accepted when the CPU is in SLEEP
	 * or STANDBY state, and when ICR.NMIB is set. It is blocked by BL
	 * only if ICR.NMIB is clear. */
	if (ctx->intc.irqs[SH4_IESOURCE_NMI].state == VK_IRQ_STATE_RAISED) {
		ctx->intc.pending = (SR.bit.bl == 0) || ((icr & 0x200) != 0);
		if (ctx->intc.pending) {
			ctx->intc.index = SH4_IESOURCE_NMI;
			return;
		}
	}

	/* TODO: ICR.MIE */

	/* All interrupts are blocked when SR.BL is set */
	if (SR.bit.bl)
		return;

	/* Find the highest priority raised IRQ; note that interrupt source
	 * numbers are sorted from highest to lowest priority. */
	priority = 0;
	for (i = 0; i < SH4_NUM_IESOURCES; i++) {
		/* TODO: handle ties like the hardware does */
		if (ctx->intc.irqs[i].state == VK_IRQ_STATE_RAISED &&
		    ctx->intc.irqs[i].priority > priority) {
			priority = ctx->intc.irqs[i].priority;
			maxi = i;
		}
	}
	/* Set the pending flag */
	if (priority > 0) {
		ctx->intc.pending = true;
		ctx->intc.index = maxi;
	}
}

static int
sh4_set_irq_state (vk_cpu_t *cpu, unsigned num, vk_irq_state_t state)
{
	sh4_t *ctx = (sh4_t *) cpu;
	if (num >= SH4_NUM_IESOURCES)
		return -1;

	ctx->intc.irqs[num].state = state;

	/* Handle NMI */
	if (num == SH4_IESOURCE_NMI) {
		if (state == VK_IRQ_STATE_RAISED) {
			/* Set ICR.NMIL and DMAOR.NMIF */
			IREG_PUT (2, INTC_ICR, IREG_GET (2, INTC_ICR) | 0x8000);
			IREG_PUT (4, DMAC_DMAOR, IREG_GET (4, DMAC_DMAOR) | 2);
			/* Notify the DMAC that an NMI occurred */
			sh4_dmac_update_state (ctx, 0);
			sh4_dmac_run (ctx, 0x7FFFFFFF);
		} else {
			/* Clear ICR.NMIL; DMAOR.NMIF must be cleared
			 * manually by software. */
			IREG_PUT (2, INTC_ICR, IREG_GET (2, INTC_ICR) & 0x7FFF);
		}
	}

	/* Update the pending flag */
	sh4_update_irqs (ctx);
	return 0;
}

static void
sh4_process_irqs (vk_cpu_t *cpu)
{
	sh4_t *ctx = (sh4_t *) cpu;
	int index;

	/* Check if there's something to do */
	if (!ctx->intc.pending)
		return;

	index = ctx->intc.index;
	if (ctx->intc.irqs[index].priority > SR.bit.i) {
		sh4_sr_t tmp;

		/* Standard interrupt context switch */
		SPC = PC;
		SSR = SR;
		SGR = R(15);

		PC = VBR + ctx->intc.irqs[index].offset;

		VK_CPU_LOG (ctx, "IRQ taken: SR.i=%X PRI=%X VBR=%08X offs=%X code=%X; jumping at %08X",
		            SR.bit.i, ctx->intc.irqs[index].priority,
		            VBR, ctx->intc.irqs[index].offset,
		            ctx->intc.irqs[index].code, PC);

		tmp.full = SR.full;
		tmp.bit.bl = 1;
		tmp.bit.md = 1;
		tmp.bit.rb = 1;
		set_sr (ctx, tmp.full);

		IREG_PUT (4, CCN_INTEVT, ctx->intc.irqs[index].code);

		/* Clear the interrupt source; TODO this is not correct, the
		 * source should be cleared externally! */
		ctx->intc.irqs[index].state = VK_IRQ_STATE_CLEAR;

		/* Update the pending flag */
		sh4_update_irqs (ctx);
	}
}

/* Instructions */

static void delay_slot (sh4_t *ctx, uint32_t pc);

typedef void (* itype) (sh4_t *ctx, uint16_t inst);

/* This name-only list is generated from the descriptors. Adding a descriptor
 * without regenerating it fails compilation at SH4_HANDLER_##name_ below. */
enum sh4_handler_id {
#define SH4_DISPATCH_HANDLER(name_) SH4_HANDLER_##name_,
#include "sh-dispatch-list.h"
#undef SH4_DISPATCH_HANDLER
};
static uint16_t insn_ids[65536];

/* Keep Debug stepping useful. Direct calls in the switch can inline the same
 * instruction bodies in optimized builds, without altering FP contraction. */
#if defined(__OPTIMIZE__) && defined(NDEBUG) && (defined(__GNUC__) || defined(__clang__))
#define SH4_HANDLER_INLINE inline __attribute__((always_inline))
#else
#define SH4_HANDLER_INLINE
#endif

#define I(name_) \
	static SH4_HANDLER_INLINE void sh4_interp_##name_ (sh4_t *ctx, uint16_t inst)

#define IDEF(mask_, match_, name_) \
	{ \
		mask_, \
		match_, \
		sh4_interp_##name_, \
		SH4_HANDLER_##name_, \
	}

#define IS_SH4

/* The SH4 and devices run serially within each machine slice, and TMU state
 * advances at the end of sh4_run. Collapse only complete iterations of this
 * side-effect-free RAM polling loop; no external writer can run mid-slice. */
#ifdef HIKARU_VENDOR_TESTING
static bool poll_skip_enabled = true;
static uint64_t poll_skipped;
void hikaru_vendor_test_poll_skip(bool enabled) { poll_skip_enabled = enabled; poll_skipped = 0; }
uint64_t hikaru_vendor_test_poll_skipped(void) { return poll_skipped; }
#endif
/* Collapse only complete no-carry iterations of this RAM wait loop. A carry
 * causes a port-A read/write and must execute normally. The current BT has
 * not yet been charged, so leave one step in the current slice for it. */
static void
sh4_skip_slave_wait (sh4_t *ctx, uint16_t branch)
{
#ifdef HIKARU_VENDOR_TESTING
	if (!poll_skip_enabled) return;
#endif
	static const uint16_t code[] = {
		0xc400, 0xe202, 0x4218, 0x372e, 0xe180, 0x8f06, 0x4128, 0x7130,
		0x6211, 0xe702, 0x227a, 0x2121, 0xe700, 0x2008, 0xe000, 0x89ef
	};
	if (branch != 0x89ef || ctx->base.patch || ctx->in_slot ||
	    ctx->base.remaining < 11 || ctx->intc.pending || PC < 30 || R(0) != 0 || !T) return;
	uint32_t start = (PC - 30) & ADDR_MASK;
	vk_mmap_read_span_t insns, data;
	if (vk_mmap_get_read_span (ctx->base.mmap, 2, start, &insns) ||
	    (uint64_t)start + sizeof(code) - 1 > insns.last ||
	    memcmp (insns.data + start - insns.first, code, sizeof(code))) return;
	uint32_t addr = GBR, physical = addr & ADDR_MASK;
	if (physical < 0x0c000000 || physical > 0x0dffffff ||
	    IS_ON_CHIP (addr) || IS_STORE_QUEUE (addr) ||
	    vk_mmap_get_read_span (ctx->base.mmap, 1, physical, &data) ||
	    data.data[physical - data.first] != 0) return;
	uint32_t n = (uint32_t)(ctx->base.remaining - 1) / 10;
	uint32_t before_carry = (UINT32_MAX - R(7)) / 513u;
	if (n > before_carry) n = before_carry;
	if (!n) return;
	R(7) += n * 513u;
	R(0) = 0;
	R(1) = 0xff800000u;
	R(2) = 512;
	ctx->base.remaining -= n * 10;
#ifdef HIKARU_VENDOR_TESTING
	poll_skipped += (uint64_t)n * 10;
#endif
}

static void
sh4_skip_poll_loop (sh4_t *ctx, uint16_t branch)
{
	sh4_skip_slave_wait (ctx, branch);
#ifdef HIKARU_VENDOR_TESTING
	if (!poll_skip_enabled) return;
#endif
	if (branch != 0x89fc || ctx->base.patch || ctx->in_slot ||
	    ctx->base.remaining < 4 || ctx->intc.pending || PC < 4) return;
	uint32_t code = (PC - 4) & ADDR_MASK;
	vk_mmap_read_span_t instructions, data;
	if (vk_mmap_get_read_span (ctx->base.mmap, 2, code, &instructions) ||
	    (uint64_t)code + 5 > instructions.last) return;
	uint16_t op[3];
	memcpy (op, instructions.data + (code - instructions.first), sizeof op);
	if ((op[0] & 0xff00) != 0xc400 || op[1] != 0x4015 || op[2] != 0x89fc) return;
	uint32_t addr = GBR + (op[0] & 0xff);
	uint32_t physical = addr & ADDR_MASK;
	if (physical < 0x0c000000 || physical > 0x0dffffff || IS_ON_CHIP (addr) || IS_STORE_QUEUE (addr)) return;
	if (vk_mmap_get_read_span (ctx->base.mmap, 1, physical, &data)) return;
	int32_t current = (int8_t)data.data[physical - data.first];
	if (current <= 0 || R(0) != (uint32_t)current) return;
	int skip = ((ctx->base.remaining - 1) / 3) * 3;
	ctx->base.remaining -= skip;
#ifdef HIKARU_VENDOR_TESTING
	poll_skipped += (uint64_t)skip;
#endif
}

#include "sh-insns-interp.h"
#include "sh-insns-desc.h"

#undef I
#undef IDEF
#undef IS_SH4

#define CHECK_COLLISION \
	do { \
		if (insns[inst] != sh4_interp_invalid) { \
			VK_LOG ("inst=%04X insns[inst]=%p", inst, insns[inst]); \
			VK_ABORT ("insns table collision"); \
		} \
	} while (0);

static void
setup_insns_handlers_from_table (const idesctype *desc,
                                 unsigned size)
{
	unsigned i, j;
	uint16_t inst;

	for (i = 0; i < size; i++) {
		switch (desc[i].mask) {
		case 0xF000:
			for (j = 0; j < 4096; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xF00F:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | (j << 4);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xFF00:
			for (j = 0; j < 256; j++) {
				inst = desc[i].match | j;
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xF08F:
			for (j = 0; j < 128; j++) {
				inst = desc[i].match |
				       ((j & 7) << 4) |
				       ((j >> 3) << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xF0FF:
			for (j = 0; j < 16; j++) {
				inst = desc[i].match | (j << 8);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xF1FF:
			for (j = 0; j < 8; j++) {
				inst = desc[i].match | (j << 9);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xF3FF:
			for (j = 0; j < 4; j++) {
				inst = desc[i].match | (j << 10);
				CHECK_COLLISION;
				insns[inst] = desc[i].handler;
				insn_ids[inst] = desc[i].handler_id;
			}
			break;
		case 0xFFFF:
			inst = desc[i].match;
			CHECK_COLLISION;
			insns[inst] = desc[i].handler;
			insn_ids[inst] = desc[i].handler_id;
			break;
		default:
			VK_ABORT ("unhandled mask %04X", desc[i].mask);
		}
	}
}

#undef CHECK_COLLISION

static void
setup_insns_handlers (void)
{
	unsigned i;

	for (i = 0; i < 65536; i++) {
		insns[i] = sh4_interp_invalid;
		insn_ids[i] = SH4_HANDLER_invalid;
	}

	setup_insns_handlers_from_table (insns_desc_sh2, NUMELEM (insns_desc_sh2));
	setup_insns_handlers_from_table (insns_desc_sh4, NUMELEM (insns_desc_sh4));
}

/* Execution */

static void
sh4_step (sh4_t *ctx, uint32_t pc)
{
	uint16_t inst;

	sh4_fetch (ctx, pc, &inst);

	inst = vk_cpu_patch ((vk_cpu_t *) ctx, pc & 0x1FFFFFFF, inst);

	insns[inst] (ctx, inst);

	ctx->base.remaining --;
}

/* Delay slots continue through sh4_step's original indirect handler path. */
static SH4_HANDLER_INLINE void
sh4_step_switch (sh4_t *ctx, uint32_t pc)
{
	uint16_t inst;
	sh4_fetch (ctx, pc, &inst);
	inst = vk_cpu_patch ((vk_cpu_t *) ctx, pc & 0x1FFFFFFF, inst);
	switch (insn_ids[inst]) {
#define SH4_DISPATCH_HANDLER(name_) \
	case SH4_HANDLER_##name_: sh4_interp_##name_ (ctx, inst); break;
#include "sh-dispatch-list.h"
#undef SH4_DISPATCH_HANDLER
	default: sh4_interp_invalid (ctx, inst); break;
	}
	ctx->base.remaining --;
}

#ifdef HIKARU_VENDOR_TESTING
/* Test selection happens between run calls on the serial machine scheduler. */
static bool switch_dispatch_enabled = true;
void hikaru_vendor_test_switch_dispatch (bool enabled) { switch_dispatch_enabled = enabled; }
int hikaru_vendor_test_verify_dispatch (void)
{
	for (unsigned inst = 0; inst < 65536; ++inst) {
		switch (insn_ids[inst]) {
#define SH4_DISPATCH_HANDLER(name_) \
		case SH4_HANDLER_##name_: if (insns[inst] != sh4_interp_##name_) return -1; break;
#include "sh-dispatch-list.h"
#undef SH4_DISPATCH_HANDLER
		default: return -1;
		}
	}
	return 0;
}
#endif

static void
delay_slot (sh4_t *ctx, uint32_t pc)
{
	ctx->in_slot = true;
	sh4_step (ctx, pc);
	ctx->in_slot = false;
}

static int
sh4_run (vk_cpu_t *cpu, int cycles)
{
	sh4_t *ctx = (sh4_t *) cpu;

	int debt = ctx->sound_cycle_debt;
	ctx->sound_cycle_debt = 0;
	ctx->sound_wait_this_slice = false;
	cpu->remaining = cycles - debt;
	ctx->cpu_active = true;
#ifdef HIKARU_VENDOR_TESTING
	if (!switch_dispatch_enabled) {
		while (cpu->remaining > 0 && cpu->state == VK_CPU_STATE_RUN) {
			sh4_process_irqs (cpu);
			sh4_step (ctx, PC);
			PC += 2;
		}
	} else
#endif
	{
		while (cpu->remaining > 0 && cpu->state == VK_CPU_STATE_RUN) {
			sh4_process_irqs (cpu);
			sh4_step_switch (ctx, PC);
			PC += 2;
		}
	}
	ctx->cpu_active = false;
	if (cpu->remaining < 0 && (debt || ctx->sound_wait_this_slice))
		ctx->sound_cycle_debt = -cpu->remaining;
	/* Timers advance with the supplied wall-time budget, including wait. */
	/* XXX BSC, SCI */
	sh4_tmu_run (ctx, cycles);
	//sh4_dmac_run (ctx, cycles);
	return -cpu->remaining;
}

static void
sh4_set_state (vk_cpu_t *cpu, vk_cpu_state_t state)
{
	/* TODO: standby, deep sleep, etc. */
	cpu->state = state;
}

/* See Table 19.5, "Interrupt Exception Handling Sources and
 * Priority Orders".
 *
 * Note that the indices of this array are ordered from highest priority
 * to lowest. */

static const sh4_irq_state_t default_irq_state[SH4_NUM_IESOURCES] = {
	[SH4_IESOURCE_NMI] = { VK_IRQ_STATE_CLEAR, 16, 0x600, 0x1C0 },
	/* IRQs */
	[SH4_IESOURCE_IRQ0] = { VK_IRQ_STATE_CLEAR, 15, 0x600, 0x200 },
	[SH4_IESOURCE_IRQ1] = { VK_IRQ_STATE_CLEAR, 14, 0x600, 0x220 },
	[SH4_IESOURCE_IRQ2] = { VK_IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	[SH4_IESOURCE_IRQ3] = { VK_IRQ_STATE_CLEAR, 12, 0x600, 0x260 },
	[SH4_IESOURCE_IRQ4] = { VK_IRQ_STATE_CLEAR, 11, 0x600, 0x280 },
	[SH4_IESOURCE_IRQ5] = { VK_IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	[SH4_IESOURCE_IRQ6] = { VK_IRQ_STATE_CLEAR,  9, 0x600, 0x2C0 },
	[SH4_IESOURCE_IRQ7] = { VK_IRQ_STATE_CLEAR,  8, 0x600, 0x2E0 },
	[SH4_IESOURCE_IRQ8] = { VK_IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	[SH4_IESOURCE_IRQ9] = { VK_IRQ_STATE_CLEAR,  6, 0x600, 0x320 },
	[SH4_IESOURCE_IRQ10] = { VK_IRQ_STATE_CLEAR,  5, 0x600, 0x360 },
	[SH4_IESOURCE_IRQ11] = { VK_IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	[SH4_IESOURCE_IRQ12] = { VK_IRQ_STATE_CLEAR,  3, 0x600, 0x380 },
	[SH4_IESOURCE_IRQ13] = { VK_IRQ_STATE_CLEAR,  2, 0x600, 0x3A0 },
	[SH4_IESOURCE_IRQ14] = { VK_IRQ_STATE_CLEAR,  1, 0x600, 0x3C0 },
	/* IRLs */
	[SH4_IESOURCE_IRL0] = { VK_IRQ_STATE_CLEAR, 13, 0x600, 0x240 },
	[SH4_IESOURCE_IRL1] = { VK_IRQ_STATE_CLEAR, 10, 0x600, 0x2A0 },
	[SH4_IESOURCE_IRL2] = { VK_IRQ_STATE_CLEAR,  7, 0x600, 0x300 },
	[SH4_IESOURCE_IRL3] = { VK_IRQ_STATE_CLEAR,  4, 0x600, 0x360 },
	/* UDI */
	[SH4_IESOURCE_UDI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x600 },
	/* GPIO */
	[SH4_IESOURCE_GPIOI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x620 },
	/* DMAC */
	[SH4_IESOURCE_DMTE0] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x640 },
	[SH4_IESOURCE_DMTE1] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x660 },
	[SH4_IESOURCE_DMTE2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x680 },
	[SH4_IESOURCE_DMTE3] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x6A0 },
	[SH4_IESOURCE_DMAE] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x6C0 },
	/* TMU */
	[SH4_IESOURCE_TUNI0] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x400 },
	[SH4_IESOURCE_TUNI1] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x420 },
	[SH4_IESOURCE_TUNI2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x440 },
	[SH4_IESOURCE_TICPI2] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x460 },
	/* RTC */
	[SH4_IESOURCE_ATI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x480 },
	[SH4_IESOURCE_PRI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4A0 },
	[SH4_IESOURCE_CUI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4C0 },
	/* SCI1 */
	[SH4_IESOURCE_ERI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x4E0 },
	[SH4_IESOURCE_RXI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x500 },
	[SH4_IESOURCE_TXI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x520 },
	[SH4_IESOURCE_TEI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x540 },
	/* SCIF */
	[SH4_IESOURCE_ERIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x700 },
	[SH4_IESOURCE_RXIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x720 },
	[SH4_IESOURCE_BRIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x740 },
	[SH4_IESOURCE_TXIF] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x760 },
	/* WDT */
	[SH4_IESOURCE_ITI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x560 },
	/* REF */
	[SH4_IESOURCE_RCMI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x580 },
	[SH4_IESOURCE_ROVI] = { VK_IRQ_STATE_CLEAR, -1, 0x600, 0x5A0 },
};

static void
sh4_reset (vk_device_t *dev, vk_reset_type_t type)
{
	vk_cpu_t *cpu = (vk_cpu_t *) dev;
	sh4_t *ctx = (sh4_t *) cpu;
	uint32_t tmp;

	cpu->state = (ctx->config.master) ?
	              VK_CPU_STATE_RUN :
	              VK_CPU_STATE_STOP;

	ctx->in_slot = false;
	ctx->cpu_active = ctx->data_read_active = ctx->dma_active = ctx->sound_wait_this_slice = false;
	ctx->sound_wait_fraction = 0;
	ctx->sound_cycle_debt = 0;

	memset ((void *) &ctx->regs, 0, sizeof (ctx->regs));

	PC = 0xA0000000;

	SR.full = 0;
	SR.bit.i = 0xF;
	SR.bit.bl = 1;
	SR.bit.rb = 1;
	SR.bit.md = 1;

	FPSCR.full = 0;
	FPSCR.bit.rm = 1;
	FPSCR.bit.dn = 1;

	/* See Table A.1, "Address List" */
	vk_buffer_clear (ctx->iregs);

	tmp = (ctx->config.master ? 0x40000000 : 0) |
	      (ctx->config.little_endian ? 0x80000000 : 0);

	IREG_PUT (4, CCN_EXPEVT, (type == VK_RESET_TYPE_HARD) ? 0 : 0x20);
	IREG_PUT (4, BSC_BCR1, tmp);
	IREG_PUT (2, BSC_BCR2, 0x3FFC);
	IREG_PUT (4, BSC_WCR1, 0x77777777);
	IREG_PUT (4, BSC_WCR2, 0xFFFEEFFF);
	IREG_PUT (4, BSC_WCR3, 0x07777777);
	IREG_PUT (4, TMU_TCOR0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT0, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT1, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCOR2, 0xFFFFFFFF);
	IREG_PUT (4, TMU_TCNT2, 0xFFFFFFFF);
	IREG_PUT (1, SCI_SCBRR1, 0xFF);
	IREG_PUT (1, SCI_SCTDR1, 0xFF);
	IREG_PUT (1, SCI_SCSSR1, 0x84);
	IREG_PUT (1, SCIF_SCBRR2, 0xFF);
	IREG_PUT (2, SCIF_SCFSR2, 0x0060);
	IREG_PUT (2, UDI_SDIR, 0xFFFF);

	ctx->intc.pending = false;
	ctx->intc.index = -1;
	VK_ASSERT (sizeof (ctx->intc.irqs) == sizeof (default_irq_state));
	memcpy (ctx->intc.irqs, default_irq_state, sizeof (default_irq_state));

	memset ((void *) &ctx->dmac, 0, sizeof (ctx->dmac));

	memset ((void *) &ctx->tmu, 0, sizeof (ctx->tmu));
	ctx->tmu.counter[0] = 0xFFFFFFFF;
	ctx->tmu.counter[1] = 0xFFFFFFFF;
	ctx->tmu.counter[2] = 0xFFFFFFFF;
}

#define SAVE(thing_) \
		vk_state_put (state, (void *) &(thing_), sizeof (thing_))

#define LOAD(thing_) \
		vk_state_get (state, (void *) &(thing_), sizeof (thing_))

static int
sh4_load_state (vk_device_t *dev, vk_state_t *state)
{
	sh4_t *ctx = (sh4_t *) dev;
	int ret = 0;

	LOAD (ctx->in_slot);
	LOAD (ctx->regs);
	LOAD (ctx->intc);
	LOAD (ctx->dmac);
	LOAD (ctx->tmu);
	LOAD (ctx->config);
	ret |= LOAD (ctx->sound_wait_fraction);
	ret |= LOAD (ctx->sound_cycle_debt);
	ctx->cpu_active = ctx->data_read_active = ctx->dma_active = ctx->sound_wait_this_slice = false;

	return ret;
}

static int
sh4_save_state (vk_device_t *dev, vk_state_t *state)
{
	sh4_t *ctx = (sh4_t *) dev;
	int ret = 0;

	SAVE (ctx->in_slot);
	SAVE (ctx->regs);
	SAVE (ctx->intc);
	SAVE (ctx->dmac);
	SAVE (ctx->tmu);
	SAVE (ctx->config);
	ret |= SAVE (ctx->sound_wait_fraction);
	ret |= SAVE (ctx->sound_cycle_debt);

	return ret;
}

#undef SAVE
#undef LOAD

static const char *
sh4_get_debug_string (vk_cpu_t *cpu)
{
	sh4_t *ctx = (sh4_t *) cpu;
	static char str[256];
	sprintf (str, "%c @%08X @%08X %08X",
	         ctx->config.master ? 'M' : 'S', PC, PR, SR.full);
	return str;
}

void
sh4_set_porta_handlers (vk_cpu_t *cpu,
                        int (* get)(sh4_t *ctx, uint16_t *val),
                        int (* put)(sh4_t *ctx, uint16_t val))
{
	sh4_t *ctx = (sh4_t *) cpu;

	ctx->porta.get = get;
	ctx->porta.put = put;
}

vk_cpu_t *
sh4_new (vk_machine_t *mach, vk_mmap_t *mmap, bool master, bool le)
{
	vk_device_t *dev;
	vk_cpu_t *cpu;
	sh4_t *ctx;

	VK_CPU_ALLOC (ctx, mach, mmap);
	cpu = (vk_cpu_t *) ctx;
	dev = (vk_device_t *) ctx;
	if (!ctx)
		goto fail;

	dev->reset		= sh4_reset;
	dev->destroy		= NULL;
	dev->load_state		= sh4_load_state;
	dev->save_state		= sh4_save_state;

	cpu->set_state		= sh4_set_state;
	cpu->run		= sh4_run;
	cpu->set_irq_state	= sh4_set_irq_state;
	cpu->get_debug_string	= sh4_get_debug_string;

	ctx->config.master = master;
	ctx->config.little_endian = le;

	ctx->iregs = vk_buffer_le32_new (0x10000, 0);
	if (!ctx->iregs)
		goto fail;

	vk_machine_register_buffer (mach, ctx->iregs);

	setup_insns_handlers ();

	return (vk_cpu_t *) ctx;
fail:
	vk_device_destroy (&dev);
	return NULL;
}
