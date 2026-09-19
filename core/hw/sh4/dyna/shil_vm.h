#pragma once

#include "types.h"
#include "shil_vm_mem.h"
#include <algorithm>
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
		// A physical RAM mask describes a power-of-two window. Guest code may
		// straddle its end even when the backing allocation is larger.
		if (ram == nullptr || !shilVmIsRam(addr) || size == 0 || size > ramSize
			|| ramMask >= ramSize || (ramMask & (ramMask + 1)) != 0)
			return;
		const u32 window = ramMask + 1;
		// If the guest spans a whole window, repeated physical bytes need only
		// one comparison. This also bounds the snapshot allocation by RAM size.
		size = std::min(size, window);
		firstSize = std::min(size, window - offset);
		bytes.reserve(size);
		bytes.assign(ram + offset, ram + offset + firstSize);
		bytes.insert(bytes.end(), ram, ram + size - firstSize);
	}

	bool matches(const u8 *ram, u32 ramSize) const
	{
		// For a split snapshot offset + firstSize is the full masked window,
		// which bounds both pieces. For a contiguous snapshot it is its end.
		return ram != nullptr && !bytes.empty() && offset + firstSize <= ramSize
			&& memcmp(ram + offset, bytes.data(), firstSize) == 0
			&& (firstSize == bytes.size()
				|| memcmp(ram, bytes.data() + firstSize, bytes.size() - firstSize) == 0);
	}

private:
	std::vector<u8> bytes;
	u32 offset = 0;
	u32 firstSize = 0;
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
