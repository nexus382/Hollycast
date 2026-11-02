// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "board_state.h"
#include "aica_mem.h"

namespace aica
{

BoardState::BoardState() : channels(sgc::createChannelState()) {}
BoardState::~BoardState() { sgc::destroyChannelState(channels); }

#ifdef USE_HIKARU
void BoardState::initializeBoard()
{
 selectRegisters(registers);
 sgc::selectChannelState(channels);
 initialize();
}

#endif

void BoardState::initialize()
{
	std::fill(std::begin(aica_reg), std::end(aica_reg), 0);
	aica_reg[0x2c00] = 1; // ARM held in reset until the host releases it.
	for (unsigned i = 0; i < 3; ++i)
		aica::timers[i].Init(aica_reg, i);
	dsp::state = {};
	dsp::state.RBL = 0x8000 - 1;
	dsp::state.MDEC_CT = 1;
	dsp::state.dirty = true;
	midiSendBuffer.clear();
	sgc::resetBoardChannels();
	arm::resetInterpreter();
	save();
}

void BoardState::save()
{
#ifdef USE_HIKARU
	if (&aica_reg != &registers)
#endif
		std::copy(std::begin(aica_reg), std::end(aica_reg), std::begin(registers));
	std::copy(std::begin(aica::timers), std::end(aica::timers), std::begin(timers));
	dsp = dsp::state;
	arm::saveExecutionState(arm);
	midi = midiSendBuffer;
	sgc::saveChannelState(*channels);
}

void BoardState::load() const
{
	std::copy(std::begin(registers), std::end(registers), std::begin(aica_reg));
	std::copy(std::begin(timers), std::end(timers), std::begin(aica::timers));
#ifdef USE_HIKARU
	for (auto& timer : aica::timers) timer.rebind(aica_reg);
#endif
	dsp::state = dsp;
	arm::loadExecutionState(arm);
	midiSendBuffer = midi;
	sgc::loadChannelState(*channels);
}

#ifdef USE_HIKARU
void BoardState::selectBoard()
{
	selectRegisters(registers);
	sgc::selectChannelState(channels);
	std::copy(std::begin(timers), std::end(timers), std::begin(aica::timers));
	for (auto& timer : aica::timers) timer.rebind(aica_reg);
	dsp::state = dsp;
	arm::loadExecutionState(arm);
	midiSendBuffer = midi;
	sgc::loadChannelState(*channels);
}

void BoardState::restoreRegisters(u8* bank, sgc::ChannelState* channelBank) const
{
 std::copy(std::begin(registers), std::end(registers), bank);
 selectRegisters(bank);
 sgc::selectChannelState(channelBank);
 std::copy(std::begin(timers), std::end(timers), std::begin(aica::timers));
 for (auto& timer : aica::timers) timer.rebind(aica_reg);
 dsp::state = dsp; arm::loadExecutionState(arm); midiSendBuffer = midi; sgc::loadChannelState(*channels);
}

#endif

bool BoardState::interruptPending() const
{
	u32 enabled, pending;
	std::memcpy(&enabled, registers + MCIEB_addr, sizeof(enabled));
	std::memcpy(&pending, registers + MCIPD_addr, sizeof(pending));
	return (enabled & pending) != 0;
}

u32 BoardState::activeChannels(ChannelInfo* out, u32 max) const
{
	const auto word = [this](u32 slot, u32 index) {
		const u32 at = slot + index * 4;
		return u32(registers[at]) | (u32(registers[at + 1]) << 8);
	};
	u32 count = 0;
	for (u32 channel = 0; channel < 64 && count < max; ++channel) {
		const u32 slot = channel * 0x80;
		const u32 first = word(slot, 0);
		if (!(first & 0x4000)) continue; // KYONB
		auto& info = out[count++];
		info.channel = channel;
		info.sa = ((first & 0x7f) << 16) | word(slot, 1);
		info.pcms = (first >> 7) & 3;
		info.lsa = word(slot, 2);
		info.lea = word(slot, 3);
		const u32 mix = word(slot, 8);
		info.isel = mix & 0xf;
		info.imxl = (mix >> 4) & 0xf;
		const u32 send = word(slot, 9);
		info.dipan = send & 0x1f;
		info.disdl = (send >> 8) & 0xf;
		info.tl = (word(slot, 10) >> 8) & 0xff;
	}
	return count;
}

u32 BoardState::activeKeyCount() const
{
	u32 count = 0;
	for (u32 channel = 0; channel < 64; ++channel)
		count += (registers[channel * 0x80 + 1] & 0x40) != 0;
	return count;
}

} // namespace aica
