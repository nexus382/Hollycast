#pragma once

#include "types.h"
#include "shil_vm_mem.h"
#include <vector>

// Own the decoded guest bytes: no cached RAM pointer survives a RAM remap.
// Only compilation allocates/copies; execution compares the exact byte range.
class ShilVmCodeGuard
{
public:
	void capture(const u8 *ram, u32 ramSize, u32 ramMask, u32 addr, u32 size)
	{
		bytes.clear();
		offset = shilVmRamOffset(addr, ramMask);
		if (ram == nullptr || !shilVmIsRam(addr) || size == 0
			|| offset > ramSize || size > ramSize - offset)
			return;
		bytes.assign(ram + offset, ram + offset + size);
	}

	bool matches(const u8 *ram, u32 ramSize) const
	{
		return ram != nullptr && !bytes.empty() && offset <= ramSize
			&& bytes.size() <= ramSize - offset
			&& memcmp(ram + offset, bytes.data(), bytes.size()) == 0;
	}

private:
	std::vector<u8> bytes;
	u32 offset = 0;
};

#if FEAT_SHREC == DYNAREC_IR

// Runs the cached block at ctx.pc (compiling if needed). Returns false if decode failed.
bool shilVmStepBlock();

// As above, but reports the successor the VM chained to, which is the block the
// main loop would run next. shilVmStepBlock throws that away, so it cannot see
// a successor cache handing back the wrong block. Returns NullAddress if the VM
// chained to nothing.
u32 shilVmStepToSuccessor();

u32 shilVmCompileCount();
void shilVmResetCompileCount();

#endif
