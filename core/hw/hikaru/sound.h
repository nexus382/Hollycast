// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include "valkyrie/engine.h"
#include "hw/aica/board_state.h"
#include <mutex>

namespace hikaru
{

// Serialized interpreter-backed sound boards. The engine owns wave RAM.
class Sound
{
public:
	using SampleSink = void (*)(void*, s16 left, s16 right);
	struct BoardDiagnostics {
		u32 armPc = 0, keys = 0, status = 0, bankReady = 0, bankRequest = 0;
		u32 queuedCommands = 0, bankBytes = 0;
		u32 bankDescriptors[8]{};
		bool armEnabled = false, irq = false;
	};
	Sound(SampleSink sink, void* opaque) : sink(sink), sinkOpaque(opaque) {}
	// Holds the shared AICA context for one serialized engine frame. Construct
	// and destroy on the same thread, immediately around run_frame; it must end
	// before frontend UI, teardown, or Dreamcast AICA work. Nested scopes fail
	// acquisition without disturbing the outer scope.
	class FrameScope {
	public:
		explicit FrameScope(Sound& sound);
		~FrameScope();
		FrameScope(const FrameScope&) = delete;
		FrameScope& operator=(const FrameScope&) = delete;
		FrameScope(FrameScope&&) = delete;
		FrameScope& operator=(FrameScope&&) = delete;
		bool acquired() const { return active; }
	private:
		Sound& sound;
		std::unique_lock<std::recursive_mutex> lock;
		bool active;
	};
	HikaruEngineSound callbacks();
	bool diagnostics(unsigned board, BoardDiagnostics& output) const;
	u64 mixedSamples() const;
	u64 mixedClippedSamples() const;
	u64 boardSaturatedSamples() const;

private:
	struct Board {
		aica::BoardState state;
		u8* ram = nullptr;
	};
	Board boards[2];
	// Callbacks are serialized; reuse the host snapshot instead of allocating
	// and zeroing a new channel/register snapshot for every sound slice.
	aica::BoardState hostScratch;
	SampleSink sink;
	void* sinkOpaque;
	u64 sampleFraction = 0;
	u64 mixedSampleCount = 0, mixedClipCount = 0, boardSaturationCount = 0;
	bool resetAsserted = true;
	bool beginFrame();
	void endFrame();

	int attach(unsigned board, u8* ram, size_t size);
	int reset();
	int setReset(bool asserted);
	int read(unsigned board, u32 offset, unsigned size, u64& value);
	int write(unsigned board, u32 offset, unsigned size, u64 value);
	int advance(u32 cycles);
	int irq(unsigned board);
};

} // namespace hikaru
