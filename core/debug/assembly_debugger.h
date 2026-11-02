// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "types.h"
#include <atomic>
#include <set>
#include <string_view>

namespace assembly_debugger {

extern std::atomic<bool> enabled;

inline bool active() { return enabled.load(std::memory_order_relaxed); }

// Session and breakpoint changes belong to the UI thread, with the CPU stopped.
bool available();
bool open();
void close();
void resume();
void step();
void reset();
bool breakPending();
void acknowledgeBreak();
bool setBreakpoint(u32 address, bool set);
const std::set<u32>& breakpoints();
bool parseAddress(std::string_view text, u32& address);
bool readInstruction(u32 address, u16& opcode);

// Called before executing a top-level instruction. Taken delayed branches and
// their slots form one stepping unit; a slot breakpoint stops before the branch.
bool checkBreakpoint(u32 pc, u16 opcode, bool t);

}
