// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rewind.h"
#include "util/rewind_buffer.h"
#include "emulator.h"
#include "serialize.h"
#include "cfg/option.h"
#include "audio/audiostream.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/maple/maple_if.h"
#include "input/maplelink.h"
#include "network/phone_vmu.h"
#include "oslib/i18n.h"
#include "oslib/oslib.h"
#include "util/worker_thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rewind_state
{
namespace
{
RewindBuffer history;
std::vector<u8> scratch;
std::mutex requestMutex;
std::unordered_map<const void*, unsigned> heldSources;
std::atomic<bool> held{false};
std::atomic<bool> stepRequested{false};
std::atomic<unsigned> generation{0};
unsigned observedGeneration = 0;
bool reversing = false;
bool failed = false;
bool rejected = false;
bool configuredEnabled = false;
int interval = 0, duration = 0, compression = -1;

// Duration is the user-facing promise, so retained history is sized from it
// (duration / interval states), not from a second user memory budget. This
// fixed ceiling only protects the device from runaway allocation; compression
// is how users trade capture CPU for lower memory use.
constexpr std::size_t SafetyMemoryLimit = std::size_t{1} << 31;

int compressionLevel()
{
	switch (std::clamp(config::RewindCompression.get(), 0, 2))
	{
	case 2: return 9;	// Maximum
	case 1: return 6;	// Balanced
	default: return 1;	// Fast
	}
}
u64 lastCapture = 0;
std::chrono::steady_clock::time_point nextRestore;

// Compression is the only capture work that may leave the render thread: it
// is a pure function of an owned byte copy. Serialization itself must stay
// here, where the machine is quiescent between frames. One job at a time;
// a snapshot that cannot start compressing promptly is skipped rather than
// allowed to queue latency into gameplay.
struct PendingCompression
{
	std::future<std::vector<u8>> result;
	std::size_t rawSize = 0;
	u64 timestamp = 0;
	unsigned token = 0;
};
std::unique_ptr<WorkerThread> compressWorker;
std::optional<PendingCompression> pendingCompression;
unsigned pendingToken = 0;

void clearHistory()
{
	++pendingToken;	// any in-flight compression result is now stale
	history.clear();
	std::vector<u8>().swap(scratch);
	lastCapture = 0;
	nextRestore = {};
	reversing = false;
}

void fail(const std::exception& error)
{
	clearHistory();
	failed = true;
	releaseRequests();
	WARN_LOG(SAVESTATE, "Rewind stopped: %s", error.what());
	os_notify(i18n::T("Rewind stopped"), 5000, error.what());
}

// Render-thread only: collect a finished compression, if any. Results whose
// token no longer matches belong to an abandoned timeline or configuration.
void harvestCompression()
{
	if (!pendingCompression
			|| pendingCompression->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	PendingCompression pending = std::move(*pendingCompression);
	pendingCompression.reset();
	try
	{
		std::vector<u8> compressed = pending.result.get();
		if (pending.token != pendingToken)
			return;
		if (!history.pushCompressed(std::move(compressed), pending.rawSize, pending.timestamp))
			throw std::runtime_error("The rewind memory limit cannot hold this game's state.");
		const u64 historyTicks = u64(duration) * SH4_MAIN_CLOCK;
		history.discardOlderThan(pending.timestamp > historyTicks ? pending.timestamp - historyTicks : 0);
	}
	catch (const std::exception& error)
	{
		fail(error);
	}
}

// Serialize once into the retained scratch capacity. The size pass remains
// only as the growth fallback for a first or growing state, so steady-state
// captures pay one traversal and one copy instead of two traversals.
std::size_t captureSnapshot()
{
	constexpr std::size_t MaxStateSize = 128 * 1024 * 1024;
	try
	{
		Serializer ser(scratch.data(), scratch.capacity());
		dc_serialize(ser);
		if (ser.size() > MaxStateSize)
			throw std::runtime_error("This game's state exceeds the rewind workspace limit.");
		return ser.size();
	}
	catch (const SerializeBase::Exception&)
	{
		Serializer measure;
		dc_serialize(measure);
		if (measure.size() > MaxStateSize)
			throw std::runtime_error("This game's state exceeds the rewind workspace limit.");
		scratch.reserve(measure.size());
		Serializer ser(scratch.data(), scratch.capacity());
		dc_serialize(ser);
		return ser.size();
	}
}
}

void setRequested(const void* source, bool pressed)
{
	if (source == nullptr)
		return;
	const std::lock_guard<std::mutex> lock(requestMutex);
	if (pressed)
		++heldSources[source];
	else
	{
		const auto it = heldSources.find(source);
		if (it != heldSources.end() && --it->second == 0)
			heldSources.erase(it);
	}
	held.store(!heldSources.empty(), std::memory_order_relaxed);
}

void releaseSource(const void* source)
{
	const std::lock_guard<std::mutex> lock(requestMutex);
	heldSources.erase(source);
	held.store(!heldSources.empty(), std::memory_order_relaxed);
}

void releaseRequests()
{
	const std::lock_guard<std::mutex> lock(requestMutex);
	heldSources.clear();
	held.store(false, std::memory_order_relaxed);
	stepRequested.store(false, std::memory_order_relaxed);
}

bool requested()
{
	return held.load(std::memory_order_relaxed);
}

const char* unavailableReason()
{
	// Callers translate the returned pointer, so mark every literal for extraction.
#ifdef LIBRETRO
	return Tnop("Use your frontend's rewind controls with the libretro core.");
#else
	if (!config::RewindEnable)
		return Tnop("Enable Rewind in Advanced settings before starting a game.");
	if (settings.content.path.empty())
		return Tnop("Start a game to record rewind history.");
	if (settings.platform.isHikaru() || settings.platform.isSystemSP())
		return Tnop("Rewind is not available for this arcade platform.");
	if (settings.raHardcoreMode)
		return Tnop("Rewind is unavailable in RetroAchievements hardcore mode.");
	if (settings.network.online || config::GGPOEnable || settings.naomi.multiboard
			|| settings.naomi.slave || settings.naomi.drivingSimSlave)
		return Tnop("Rewind is unavailable during network or linked-cabinet play.");
	if (config::ThreadedRendering)
		return Tnop("Restart the game to use Rewind with shared emulation and rendering.");
#ifdef HAVE_PHONE_VMU
	if (phone_vmu::status().running)
		return Tnop("Stop phone VMU sharing before using Rewind.");
#endif
	for (const auto& port : MapleDevices)
		for (const auto& device : port)
			if (std::dynamic_pointer_cast<MapleLinkDevice>(device))
				return Tnop("Rewind is unavailable with passthrough DreamLink devices.");
	if (failed)
		return Tnop("Rewind stopped after an error. Restart the game or change its rewind settings.");
	return nullptr;
#endif
}

bool canStep()
{
	return unavailableReason() == nullptr && history.size() != 0;
}

void requestStep()
{
	if (canStep())
		stepRequested.store(true, std::memory_order_relaxed);
}

std::size_t snapshotCount() { return history.size(); }
std::size_t memoryUsed() { return history.memoryUsed(); }

void invalidate()
{
	// Device writes can originate on the CPU thread when rewind is disabled.
	generation.fetch_add(1, std::memory_order_relaxed);
}

void poll()
{
	const int newInterval = std::clamp(config::RewindInterval.get(), 100, 1000);
	const int newDuration = std::clamp(config::RewindDuration.get(), 5, 120);
	const int newCompression = compressionLevel();
	const unsigned currentGeneration = generation.load(std::memory_order_relaxed);
	if (configuredEnabled != config::RewindEnable || interval != newInterval
			|| duration != newDuration || compression != newCompression || observedGeneration != currentGeneration)
	{
		clearHistory();
		failed = false;
		configuredEnabled = config::RewindEnable;
		interval = newInterval;
		duration = newDuration;
		compression = newCompression;
		observedGeneration = currentGeneration;
		history.configure(SafetyMemoryLimit,
				(std::size_t(duration) * 1000 + interval - 1) / interval + 1, compression);
	}
	if (!configuredEnabled)
	{
		if (history.size() != 0 || !scratch.empty())
			clearHistory();
		releaseRequests();
	}
}

FrameAction beforeFrame()
{
	poll();
	harvestCompression();
	const bool singleStep = stepRequested.exchange(false, std::memory_order_relaxed);
	if (!requested() && !singleStep)
	{
		if (reversing)
		{
			ClearAudioOutput();
			lastCapture = sh4_sched_now64();
			reversing = false;
		}
		rejected = false;
		return FrameAction::Run;
	}
	const char* reason = unavailableReason();
	if (reason != nullptr)
	{
		if (!rejected)
			os_notify(i18n::T(reason), 3000);
		rejected = true;
		clearHistory();
		return FrameAction::Run;
	}
	const auto now = std::chrono::steady_clock::now();
	if (history.size() == 0 || (reversing && !singleStep && now < nextRestore))
	{
		// The main loop still polls host input; never advance at the oldest snapshot.
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
		return FrameAction::Hold;
	}
	bool applying = false;
	try
	{
		if (!reversing)
		{
			ClearAudioOutput();
			// A snapshot still compressing belongs to the future being abandoned.
			++pendingToken;
			// Skip a snapshot of the frame already on screen, when an older one exists.
			if (lastCapture == sh4_sched_now64() && history.size() > 1)
				history.pop(scratch);
		}
		if (!history.pop(scratch))
			return FrameAction::Hold;
		Deserializer deser(scratch.data(), scratch.size(), false, true);
		applying = true;
		emu.loadstate(deser);
		reversing = true;
		// Short intervals play back in real time; sparse history moves back faster.
		nextRestore = now + std::chrono::milliseconds(std::min(interval, 250));
		return FrameAction::Restore;
	}
	catch (const std::exception& error)
	{
		fail(error);
		// A partially deserialized machine must not execute another frame.
		if (applying)
		{
			dc_skipAutoSaveOnNextUnload();
			throw FlycastException(i18n::Ts("Failed to restore rewind state"));
		}
		return FrameAction::Run;
	}
}

void afterFrame(bool restored)
{
	if (restored || !config::RewindEnable)
		return;
	if (unavailableReason() != nullptr)
	{
		if (history.size() != 0)
			clearHistory();
		return;
	}
	poll();
	harvestCompression();
	const u64 now = sh4_sched_now64();
	const u64 ticks = u64(interval) * (SH4_MAIN_CLOCK / 1000);
	if (lastCapture != 0 && now >= lastCapture && now - lastCapture < ticks)
		return;
	if (pendingCompression)
		return;	// compression is behind; skip this capture instead of queueing
	try
	{
		const std::size_t stateSize = captureSnapshot();
		std::vector<u8> snapshot(scratch.data(), scratch.data() + stateSize);
		if (!compressWorker)
			compressWorker = std::make_unique<WorkerThread>("RewindCompress");
		const int level = compression;
		const unsigned token = pendingToken;
		pendingCompression = PendingCompression{
			compressWorker->runFuture([snapshot = std::move(snapshot), level]() {
				return RewindBuffer::compress(snapshot.data(), snapshot.size(), level);
			}), stateSize, now, token};
		lastCapture = now;
	}
	catch (const std::exception& error)
	{
		fail(error);
	}
}
}
