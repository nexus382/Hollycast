//
// Audio Overload SDK
//
// Copyright (c) 2007-2009 R. Belmont and Richard Bannister, and others.
// All rights reserved.
//

#include "build.h"

#if FEAT_DSPREC != DYNAREC_JIT

#include "dsp.h"
#include "aica.h"
#include "aica_if.h"

namespace aica
{

namespace dsp
{

void runStep()
{
	if (state.stopped)
		return;

	s32 ACC = 0;		//26 bit
	s32 SHIFTED = 0;	//24 bit
	s32 X = 0;			//24 bit
	s32 Y = 0;			//13 bit
	s32 B = 0;			//26 bit
	s32 INPUTS = 0;		//24 bit
	s32 MEMVAL[4] = {0};
	s32 FRC_REG = 0;	//13 bit
	s32 Y_REG = 0;		//24 bit
	u32 ADRS_REG = 0;	//13 bit

	const Instruction *prog = decodedProgram();

	for (int step = 0; step < 128; ++step)
	{
		const u32 *IPtr = DSPData->MPRO + step * 4;

		if (IPtr[0] == 0 && IPtr[1] == 0 && IPtr[2] == 0 && IPtr[3] == 0)
		{
			X = state.TEMP[state.MDEC_CT & 0x7F];
			Y = FRC_REG;
			ACC = (((s64)X * (s64)Y) >> 12) + X;
			continue;
		}

		const Instruction& ins = prog[step];
		const u32 COEF = step;

		if (ins.IRA <= 0x1f)
			INPUTS = state.MEMS[ins.IRA];
		else if (ins.IRA <= 0x2F)
			INPUTS = state.MIXS[ins.IRA - 0x20] << 4;
		else if (ins.IRA <= 0x31)
			INPUTS = DSPData->EXTS[ins.IRA - 0x30] << 8;
		else
			INPUTS = 0;

		if (ins.IWT)
			state.MEMS[ins.IWA] = MEMVAL[step & 3];

		if (!ins.ZERO)
		{
			if (ins.BSEL)
				B = ACC;
			else
				B = state.TEMP[(ins.TRA + state.MDEC_CT) & 0x7F];
			if (ins.NEGB)
				B = -B;
		}
		else
		{
			B = 0;
		}

		if (ins.XSEL)
			X = INPUTS;
		else
			X = state.TEMP[(ins.TRA + state.MDEC_CT) & 0x7F];

		if (ins.YSEL == 0)
			Y = FRC_REG;
		else if (ins.YSEL == 1)
			Y = ((s32)(s16)DSPData->COEF[COEF]) >> 3;
		else if (ins.YSEL == 2)
			Y = Y_REG >> 11;
		else
			Y = (Y_REG >> 4) & 0x0FFF;

		if (ins.YRL)
			Y_REG = INPUTS;

		if (ins.SHIFT == 0 || ins.SHIFT == 3)
			SHIFTED = ACC;
		else
			SHIFTED = ACC << 1;

		if (ins.SHIFT < 2)
			SHIFTED = std::min(std::max(SHIFTED, -0x00800000), 0x007FFFFF);

		ACC = (((s64)X * (s64)Y) >> 12) + B;

		if (ins.TWT)
			state.TEMP[(ins.TWA + state.MDEC_CT) & 0x7F] = SHIFTED;

		if (ins.FRCL)
		{
			if (ins.SHIFT == 3)
				FRC_REG = SHIFTED & 0x0FFF;
			else
				FRC_REG = SHIFTED >> 11;
		}

		if (step & 1)
		{
			if (ins.MRD || ins.MWT)
			{
				u32 ADDR = DSPData->MADRS[ins.MASA];
				if (ins.ADREB)
					ADDR += ADRS_REG & 0x0FFF;
				if (ins.NXADR)
					ADDR++;
				if (!ins.TABLE)
				{
					ADDR += state.MDEC_CT;
					ADDR &= state.RBL;
				}
				else
					ADDR &= 0xFFFF;

				ADDR <<= 1;
				ADDR += state.RBP;
				if (ins.MRD)
					MEMVAL[(step + 2) & 3] = UNPACK(*(u16 *)&aica_ram[ADDR & ARAM_MASK]);
				if (ins.MWT)
					*(u16 *)&aica_ram[ADDR & ARAM_MASK] = PACK(SHIFTED);
			}
		}

		if (ins.ADRL)
		{
			if (ins.SHIFT == 3)
				ADRS_REG = SHIFTED >> 12;
			else
				ADRS_REG = INPUTS >> 16;
		}

		if (ins.EWT)
			DSPData->EFREG[ins.EWA] = SHIFTED >> 8;
	}
	--state.MDEC_CT;
	if (state.MDEC_CT == 0)
		state.MDEC_CT = state.RBL + 1;
}

} // namespace dsp
} // namespace aica
#endif
