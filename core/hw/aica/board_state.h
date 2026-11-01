// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include "aica.h"
#include "dsp.h"
#include "sgc_if.h"
#include "hw/arm7/arm7.h"

namespace aica
{

// Snapshot of the singleton execution state while the AICA core is being
// adapted to multiple boards. Wave RAM is deliberately excluded: the caller
// selects a separately owned RAM view before executing the interpreter.
// All switching and execution must occur on the emulation thread. A newly
// constructed snapshot must be saved before it is loaded.
class BoardState
{
public:
	BoardState();
	~BoardState();
	BoardState(const BoardState&) = delete;
	BoardState& operator=(const BoardState&) = delete;
	// Initializes the active interpreter state, then saves it in this snapshot.
	// The caller must first preserve any currently active board or Dreamcast state.
	void initialize();
#ifdef USE_HIKARU
	void initializeBoard();
#endif
	void save();
	void load() const;
#ifdef USE_HIKARU
	void restoreRegisters(u8* bank, sgc::ChannelState* channelBank) const;
	void selectBoard();
#endif
	bool interruptPending() const;
	u32 armProgramCounter() const { return arm.registers[arm::R15_ARM_NEXT].I; }
	bool armEnabled() const { return arm.enabled; }
	u32 activeKeyCount() const;

private:
	alignas(4) u8 registers[0x8000]{};
	AicaTimer timers[3]{};
	dsp::DSPState dsp{};
	arm::ExecutionState arm{};
	std::deque<u8> midi;
	sgc::ChannelState* channels;
};

} // namespace aica
