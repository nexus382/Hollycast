// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "sound.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica_mem.h"
#include "hw/arm7/arm7.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace hikaru
{
namespace
{
constexpr size_t SoundRamSize = 8 * 1024 * 1024;
constexpr u64 Sh4Clock = 200000000;
constexpr u64 SampleRate = 44100;

// AICA state is process-global. The recursive lock permits callbacks on the
// frame-owning thread while preventing another Sound instance from observing
// or replacing an active board context.
std::recursive_mutex soundMutex;
aica::BoardState* frameScratch = nullptr;
aica::BoardState* selectedBoard = nullptr;
RamRegion previousRam;
u8* previousRegisters = nullptr;
aica::sgc::ChannelState* previousChannels = nullptr;
bool previousHost = false;

// Restore every Dreamcast singleton, including its RAM view, before returning
// to the caller. Only the attached board's snapshot and RAM may be changed.
class ScopedBoardHost
{
public:
	explicit ScopedBoardHost(aica::BoardState& scratch) : host(scratch), local(&scratch != frameScratch) {
		if (local) { ram = aica::aica_ram; registers = aica::aica_reg; channels = aica::sgc::activeChannelState(); host.save(); oldHost = aica::setBoardHost(true); }
	}
	~ScopedBoardHost() { if (local) { host.restoreRegisters(registers, channels); aica::aica_ram = ram; aica::setBoardHost(oldHost); } }
	void select(aica::BoardState& state, u8* data) {
		if (local || selectedBoard != &state) {
			aica::aica_ram.setRegion(data, SoundRamSize);
			state.selectBoard();
			if (!local) selectedBoard = &state;
		}
	}
private:
	aica::BoardState& host;
	RamRegion ram;
	u8* registers = nullptr;
	aica::sgc::ChannelState* channels = nullptr;
	bool oldHost;
	bool local;
};
}

Sound::FrameScope::FrameScope(Sound& sound) : sound(sound), lock(soundMutex), active(sound.beginFrame()) {}
Sound::FrameScope::~FrameScope() { if (active) sound.endFrame(); }

bool Sound::beginFrame()
{
	if (frameScratch) return false;
	hostScratch.save();
	previousRam = aica::aica_ram;
	previousRegisters = aica::aica_reg;
	previousChannels = aica::sgc::activeChannelState();
	previousHost = aica::setBoardHost(true);
	frameScratch = &hostScratch;
	selectedBoard = nullptr;
	return true;
}

void Sound::endFrame()
{
	if (frameScratch != &hostScratch) return;
	hostScratch.restoreRegisters(previousRegisters, previousChannels);
	aica::aica_ram = previousRam;
	aica::setBoardHost(previousHost);
	frameScratch = selectedBoard = nullptr;
}

int Sound::attach(unsigned board, u8* ram, size_t size)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (board >= 2 || !ram || size != SoundRamSize) return -1;
	boards[board].ram = ram;
	if (frameScratch == &hostScratch) selectedBoard = nullptr;
	return 0;
}

int Sound::reset()
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (!boards[0].ram || !boards[1].ram) return -1;
	ScopedBoardHost host(hostScratch);
	if (frameScratch == &hostScratch) selectedBoard = nullptr;
	for (auto& board : boards) {
		aica::aica_ram.setRegion(board.ram, SoundRamSize);
		board.state.initializeBoard();
	}
	sampleFraction = 0;
	resetAsserted = true;
	return 0;
}

int Sound::setReset(bool asserted)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (asserted && !resetAsserted) {
		if (reset()) return -1;
	}
	else if (!asserted && resetAsserted) {
		ScopedBoardHost host(hostScratch);
		for (auto& board : boards) {
			if (!board.ram) return -1;
			host.select(board.state, board.ram);
			aica::arm::enableInterpreter((aica::aica_reg[0x2c00] & 1) == 0);
			board.state.save();
		}
	}
	resetAsserted = asserted;
	return 0;
}

int Sound::read(unsigned board, u32 offset, unsigned size, u64& value)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (board >= 2 || !boards[board].ram || (size != 1 && size != 2 && size != 4) || offset >= 0x8000) return -1;
	ScopedBoardHost host(hostScratch);
	auto& b = boards[board];
	host.select(b.state, b.ram);
	if (size == 1)
		value = aica::readAicaReg<u8>(offset);
	else
		value = aica::readAicaReg<u16>(offset);
	b.state.save(); // reads can consume MIDI or update channel status
	return 0;
}

int Sound::write(unsigned board, u32 offset, unsigned size, u64 value)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (board >= 2 || !boards[board].ram || (size != 1 && size != 2 && size != 4) || offset >= 0x8000) return -1;
	ScopedBoardHost host(hostScratch);
	auto& b = boards[board];
	host.select(b.state, b.ram);
	if (size == 1)
		aica::writeAicaReg<u8>(offset, static_cast<u8>(value));
	else
		aica::writeAicaReg<u16>(offset, static_cast<u16>(value));
	b.state.save();
	return 0;
}

int Sound::advance(u32 cycles)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (!boards[0].ram || !boards[1].ram) return -1;
	sampleFraction += u64(cycles) * SampleRate;
	u32 samples = static_cast<u32>(sampleFraction / Sh4Clock);
	sampleFraction %= Sh4Clock;
	if (!samples) return 0;
	ScopedBoardHost host(hostScratch);
	// Both boards run independently until control returns to the SH4. Process
	// a small block per board, preserving each board's sample/ARM interleave,
	// then emit the mixed frames in order. This avoids copying the complete
	// register/channel state for every sample when a slice contains several.
	while (samples) {
		constexpr u32 BatchSize = 16;
		const u32 count = std::min(samples, BatchSize);
		std::array<int, BatchSize> left{}, right{};
		std::array<unsigned, BatchSize> saturated{};
		if (!resetAsserted) {
			for (auto& b : boards) {
				host.select(b.state, b.ram);
				for (u32 i = 0; i < count; ++i) {
					aica::arm::runInterpreterCycles(ARM_CYCLES_PER_SAMPLE);
					s16 l = 0, r = 0;
					aica::boardTimeStep(l, r);
					saturated[i] += l == -32768 || l == 32767 || r == -32768 || r == 32767;
					left[i] += l; right[i] += r;
				}
				b.state.save();
			}
		}
		for (u32 i = 0; i < count; ++i) {
			++mixedSampleCount;
			boardSaturationCount += saturated[i];
			mixedClipCount += left[i] < -32768 || left[i] > 32767 || right[i] < -32768 || right[i] > 32767;
			if (sink) sink(sinkOpaque, static_cast<s16>(std::clamp(left[i], -32768, 32767)), static_cast<s16>(std::clamp(right[i], -32768, 32767)));
		}
		samples -= count;
	}
	return 0;
}

int Sound::irq(unsigned board)
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (board >= 2 || !boards[board].ram) return -1;
	if (resetAsserted) return 0;
	return boards[board].state.interruptPending() ? 1 : 0;
}

bool Sound::diagnostics(unsigned board, BoardDiagnostics& output) const
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	if (board >= 2 || !boards[board].ram) return false;
	const auto& b = boards[board];
	output = {};
	output.armPc = b.state.armProgramCounter();
	output.armEnabled = b.state.armEnabled();
	output.irq = !resetAsserted && b.state.interruptPending();
	output.keys = b.state.activeKeyCount();
	std::memcpy(&output.status, b.ram + 0x5c, sizeof(u32));
	std::memcpy(&output.bankReady, b.ram + 0xb0, sizeof(u32));
	std::memcpy(&output.bankRequest, b.ram + 0xb4, sizeof(u32));
	std::memcpy(output.bankDescriptors, b.ram + 0x60, sizeof(output.bankDescriptors));
	for (u32 i = 0x400; i < 0x500; i += 4) {
		u32 value;
		std::memcpy(&value, b.ram + i, sizeof(value));
		output.queuedCommands += value != 0;
	}
	for (size_t i = 0x100000; i < SoundRamSize; ++i)
		output.bankBytes += b.ram[i] != 0;
	return true;
}

u64 Sound::mixedSamples() const
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	return mixedSampleCount;
}

u64 Sound::mixedClippedSamples() const
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	return mixedClipCount;
}

u64 Sound::boardSaturatedSamples() const
{
	std::lock_guard<std::recursive_mutex> lock(soundMutex);
	return boardSaturationCount;
}

HikaruEngineSound Sound::callbacks()
{
	return {this,
		[](void* p, unsigned b, u8* ram, size_t n) -> int { try { return static_cast<Sound*>(p)->attach(b, ram, n); } catch (...) { return -1; } },
		[](void* p) { std::lock_guard<std::recursive_mutex> lock(soundMutex); auto* s = static_cast<Sound*>(p); if (frameScratch == &s->hostScratch) selectedBoard = nullptr; for (auto& b : s->boards) b.ram = nullptr; },
		[](void* p) -> int { try { return static_cast<Sound*>(p)->reset(); } catch (...) { return -1; } },
		[](void* p, int v) -> int { try { return static_cast<Sound*>(p)->setReset(v != 0); } catch (...) { return -1; } },
		[](void* p, unsigned b, u32 a, unsigned n, u64* v) -> int { try { return v ? static_cast<Sound*>(p)->read(b, a, n, *v) : -1; } catch (...) { return -1; } },
		[](void* p, unsigned b, u32 a, unsigned n, u64 v) -> int { try { return static_cast<Sound*>(p)->write(b, a, n, v); } catch (...) { return -1; } },
		[](void* p, u32 n) -> int { try { return static_cast<Sound*>(p)->advance(n); } catch (...) { return -1; } },
		[](void* p, unsigned b) -> int { try { return static_cast<Sound*>(p)->irq(b); } catch (...) { return -1; } }};
}

} // namespace hikaru
