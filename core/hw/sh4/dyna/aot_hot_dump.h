#pragma once

#include "types.h"

#if FEAT_SHREC == DYNAREC_IR

#include <cstddef>
#include <string>

struct RuntimeBlockInfo;

// Profile dump of hot SHIL blocks for offline AOT emit.
// Record shape matches AOT_APPSTORE_DESIGN.md FIRST_IMPLEMENTATION_SLICE:
//   {pc, sh4_hash, sh4_bytes, guest_cycles, BlockType, targets}

void aotHotReset();
void aotHotSetProfiling(bool enabled);
bool aotHotProfiling();
void aotHotNoteVisit(const RuntimeBlockInfo *block, u32 sh4_hash, const u8 *sh4_bytes, u32 sh4_len);
bool aotHotDumpTopN(const std::string &path, size_t n);
size_t aotHotTrackedCount();

#endif
