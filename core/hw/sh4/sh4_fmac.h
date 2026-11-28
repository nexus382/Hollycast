// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "sh4_core.h"

inline f32 sh4Fmac(f32 lhs, f32 rhs, f32 addend, [[maybe_unused]] u32 roundingMode)
{
#if defined(__MINGW32__) && HOST_CPU == CPU_X64
	if (roundingMode == 1)
	{
		// MinGW's fmaf can round one float too far toward zero. A binary32
		// product fits exactly in binary64, and both the sum and narrowing
		// use the same toward-zero mode already set by guest FPSCR.
		return fixNaN(static_cast<f32>(static_cast<f64>(lhs) * rhs + addend));
	}
#endif
	return fixNaN(std::fma(lhs, rhs, addend));
}
