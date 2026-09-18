#pragma once

#include "types.h"

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
