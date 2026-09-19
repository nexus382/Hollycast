#pragma once

#include "types.h"

#include "decoder.h"

struct ShilVmChain
{
	void *next = nullptr;
	void *branch = nullptr;
	u32 nextPc = NullAddress;
	u32 branchPc = NullAddress;
};

enum class ShilVmExitKind
{
	StaticJump,
	StaticCall,
	Cond0,
	Cond1,
	Dynamic,
	StaticIntr,
	DynamicIntr,
};

inline ShilVmExitKind shilVmExitKind(BlockEndType type)
{
	switch (type)
	{
	case BET_StaticJump:
		return ShilVmExitKind::StaticJump;
	case BET_StaticCall:
		return ShilVmExitKind::StaticCall;
	case BET_Cond_0:
		return ShilVmExitKind::Cond0;
	case BET_Cond_1:
		return ShilVmExitKind::Cond1;
	case BET_StaticIntr:
		return ShilVmExitKind::StaticIntr;
	case BET_DynamicIntr:
		return ShilVmExitKind::DynamicIntr;
	default:
		return ShilVmExitKind::Dynamic;
	}
}

// Returns the cached successor if its stored PC matches the PC we are about to run.
// Otherwise nullptr — caller must look up / compile.
inline void *shilVmSelectSuccessor(ShilVmExitKind kind, bool condTaken, u32 actualPc, const ShilVmChain& chain)
{
	switch (kind)
	{
	case ShilVmExitKind::StaticJump:
	case ShilVmExitKind::StaticCall:
		if (chain.branch != nullptr && chain.branchPc == actualPc)
			return chain.branch;
		return nullptr;
	case ShilVmExitKind::Cond0:
	case ShilVmExitKind::Cond1:
		if (condTaken)
		{
			if (chain.branch != nullptr && chain.branchPc == actualPc)
				return chain.branch;
		}
		else if (chain.next != nullptr && chain.nextPc == actualPc)
		{
			return chain.next;
		}
		return nullptr;
	case ShilVmExitKind::StaticIntr:
		if (chain.next != nullptr && chain.nextPc == actualPc)
			return chain.next;
		return nullptr;
	default:
		if (chain.branch != nullptr && chain.branchPc == actualPc)
			return chain.branch;
		return nullptr;
	}
}

inline u32 shilVmExitPc(ShilVmExitKind kind, bool condTaken, u32 nextPc, u32 branchPc, u32 dynamicPc)
{
	switch (kind)
	{
	case ShilVmExitKind::StaticJump:
	case ShilVmExitKind::StaticCall:
		return branchPc;
	case ShilVmExitKind::Cond0:
	case ShilVmExitKind::Cond1:
		return condTaken ? branchPc : nextPc;
	case ShilVmExitKind::StaticIntr:
		return nextPc;
	default:
		return dynamicPc;
	}
}
