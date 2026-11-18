// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "board_state.h"
#include "aica_mem.h"
#ifdef USE_HIKARU
#include "hw/hikaru/state_io.h"
#include <stdexcept>
#endif

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

void BoardState::serializeHikaruState(hikaru::StateWriter& writer) const
{
	writer.bytes(registers, sizeof(registers));
	// AicaTimer contains a register pointer; only its numeric phase is saved.
	for (const AicaTimer& timer : timers)
	{
		writer.value(timer.c_step);
		writer.value(timer.m_step);
	}
	writer.value(dsp.TEMP);
	writer.value(dsp.MEMS);
	writer.value(dsp.MIXS);
	writer.value(dsp.RBP);
	writer.value(dsp.RBL);
	writer.value(dsp.MDEC_CT);
	writer.value(dsp.SHIFTED);
	writer.value(dsp.B);
	writer.value(dsp.MEMVAL);
	writer.value(dsp.FRC_REG);
	writer.value(dsp.Y_REG);
	writer.value(dsp.ADRS_REG);

	for (const arm::reg_pair& reg : arm.registers)
		writer.value(reg.I);
	writer.value(arm.clockTicks);
	writer.value(arm.mode);
	writer.value(arm.irqEnable);
	writer.value(arm.fiqEnable);
	writer.value(arm.enabled);
	writer.value(arm.aicaInterrupt);
	writer.value(arm.aicaInterruptLevel);
	writer.value(arm.interruptOutput);
	writer.value(arm.interruptLevel);
	writer.value(arm.interruptMode);

	if (midi.size() > 4096)
		throw std::runtime_error("Hikaru MIDI queue is too large");
	const u32 midiSize = static_cast<u32>(midi.size());
	writer.value(midiSize);
	for (u8 byte : midi)
		writer.value(byte);
	sgc::serializeHikaruState(*channels, writer);
}

void BoardState::deserializeHikaruState(hikaru::StateReader& reader)
{
	u8 loadedRegisters[sizeof(registers)];
	reader.bytes(loadedRegisters, sizeof(loadedRegisters));

	// Rebuild each timer's pointer and board index from its register bank.
	s32 timerSteps[3]{};
	u32 timerRates[3]{};
	for (size_t i = 0; i < std::size(timerSteps); ++i)
	{
		reader.value(timerSteps[i]);
		reader.value(timerRates[i]);
		hikaru::StateReader::require(timerSteps[i] > 0 && timerRates[i] > 0 && timerRates[i] <= 128
			&& (timerRates[i] & (timerRates[i] - 1)) == 0 && u32(timerSteps[i]) <= timerRates[i],
			"Invalid Hikaru AICA timer state");
	}

	dsp::DSPState loadedDsp{};
	reader.value(loadedDsp.TEMP);
	reader.value(loadedDsp.MEMS);
	reader.value(loadedDsp.MIXS);
	reader.value(loadedDsp.RBP);
	reader.value(loadedDsp.RBL);
	reader.value(loadedDsp.MDEC_CT);
	reader.value(loadedDsp.SHIFTED);
	reader.value(loadedDsp.B);
	reader.value(loadedDsp.MEMVAL);
	reader.value(loadedDsp.FRC_REG);
	reader.value(loadedDsp.Y_REG);
	reader.value(loadedDsp.ADRS_REG);
	const bool validRingLength = loadedDsp.RBL == 0x1fff || loadedDsp.RBL == 0x3fff
		|| loadedDsp.RBL == 0x7fff || loadedDsp.RBL == 0xffff;
	hikaru::StateReader::require(validRingLength && loadedDsp.RBP <= 0x7ff800
		&& (loadedDsp.RBP & 0x7ff) == 0 && loadedDsp.MDEC_CT > 0
		&& loadedDsp.MDEC_CT <= 0x10000, "Invalid Hikaru DSP ring state");
	loadedDsp.dirty = true; // Rebuild the decoded DSP-program status on the next interpreter step.

	arm::ExecutionState loadedArm{};
	for (arm::reg_pair& reg : loadedArm.registers)
		reader.value(reg.I);
	reader.value(loadedArm.clockTicks);
	reader.value(loadedArm.mode);
	reader.value(loadedArm.irqEnable);
	reader.value(loadedArm.fiqEnable);
	reader.value(loadedArm.enabled);
	reader.value(loadedArm.aicaInterrupt);
	reader.value(loadedArm.aicaInterruptLevel);
	reader.value(loadedArm.interruptOutput);
	reader.value(loadedArm.interruptLevel);
	reader.value(loadedArm.interruptMode);
	const bool validArmMode = loadedArm.mode == 0x10 || loadedArm.mode == 0x1f
		|| loadedArm.mode == 0x11 || loadedArm.mode == 0x12 || loadedArm.mode == 0x13
		|| loadedArm.mode == 0x17 || loadedArm.mode == 0x1b;
	hikaru::StateReader::require(validArmMode || (loadedArm.mode == 0 && !loadedArm.enabled),
		"Invalid Hikaru ARM mode");

	u32 midiSize = 0;
	reader.value(midiSize);
	hikaru::StateReader::require(midiSize <= 4096, "Hikaru MIDI queue is too large");
	std::deque<u8> loadedMidi;
	for (u32 i = 0; i < midiSize; ++i)
	{
		u8 byte = 0;
		reader.value(byte);
		loadedMidi.push_back(byte);
	}

	sgc::deserializeHikaruState(*channels, reader);

	std::copy(std::begin(loadedRegisters), std::end(loadedRegisters), std::begin(registers));
	for (size_t i = 0; i < std::size(timers); ++i)
	{
		timers[i].Init(registers, static_cast<u32>(i));
		timers[i].c_step = timerSteps[i];
		timers[i].m_step = timerRates[i];
	}
	dsp = loadedDsp;
	arm = loadedArm;
	midi.swap(loadedMidi);
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
