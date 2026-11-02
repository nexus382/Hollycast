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
	// Register-level view of one keyed channel, for board diagnostics.
	struct ChannelInfo {
		u32 channel = 0, sa = 0, pcms = 0, lsa = 0, lea = 0, tl = 0, disdl = 0, dipan = 0, isel = 0, imxl = 0;
	};
	u32 activeChannels(ChannelInfo* out, u32 max) const;
	u32 masterVolume() const { return registers[0x2800] & 0xf; }
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
