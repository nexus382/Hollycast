#pragma once

#include "types.h"

#if FEAT_SHREC == DYNAREC_IR

// Runs the cached block at ctx.pc (compiling if needed). Returns false if decode failed.
bool shilVmStepBlock();

u32 shilVmCompileCount();
void shilVmResetCompileCount();

#endif
