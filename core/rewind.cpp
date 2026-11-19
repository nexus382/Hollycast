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
#include "hw/naomi/card_reader.h"
#include "hw/naomi/hopper.h"
#include "hw/naomi/netdimm.h"
#include "input/maplelink.h"
#include "network/ice.h"
#include "network/net_handshake.h"
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
bool buttonInputAllowed = false;
std::unordered_map<const void*, unsigned> heldSources;
std::atomic<bool> held{false};
std::atomic<bool> stepRequested{false};
std::atomic<bool> invalidated{false};
bool reversing = false;
bool failed = false;
bool configuredEnabled = false;
int interval = 0, duration = 0, compression = -1;

void clearRequestsLocked()
{
	heldSources.clear();
	held.store(false, std::memory_order_relaxed);
	stepRequested.store(false, std::memory_order_relaxed);
}

// Leave room for the emulator, renderer and compression workspace on devices
// with a small address space or an OS-enforced application memory budget.
#if defined(__ANDROID__) || defined(TARGET_IPHONE) || defined(__SWITCH__) || defined(TARGET_UWP)
constexpr std::size_t SafetyMemoryLimit = 256_MB;
#else
constexpr std::size_t SafetyMemoryLimit = sizeof(void*) == 4 ? 256_MB : 2048_MB;
#endif

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

// The worker borrows no machine state. It owns the serialized buffer until
// compression finishes, then returns the workspace for the next capture.
struct CompressionResult
{
	std::vector<u8> compressed;
	std::vector<u8> workspace;
};
struct PendingCompression
{
	std::future<CompressionResult> result;
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
		if (pending.token != pendingToken)
			return;
		CompressionResult result = pending.result.get();
		scratch = std::move(result.workspace);
		if (!history.pushCompressed(std::move(result.compressed), pending.rawSize, pending.timestamp))
			throw std::runtime_error("The rewind memory limit cannot hold this game's state.");
		const u64 historyTicks = u64(duration) * SH4_MAIN_CLOCK;
		history.discardOlderThan(pending.timestamp > historyTicks ? pending.timestamp - historyTicks : 0);
	}
	catch (const std::exception& error)
	{
		fail(error);
	}
}

void syncHistory()
{
	// Multiple guest writes before the next frame share one history boundary.
	if (invalidated.load(std::memory_order_relaxed) && invalidated.exchange(false, std::memory_order_relaxed))
		clearHistory();
	harvestCompression();
}

// Measure only the first or growing snapshot. Subsequent frames serialize
// directly into the workspace returned by the compression worker.
std::size_t captureSnapshot()
{
	if (!scratch.empty())
	{
		try
		{
			Serializer ser(scratch.data(), scratch.size());
			dc_serialize(ser);
			return ser.size();
		}
		catch (const Serializer::Exception&) {} // A device's serialized state can grow.
	}
	Serializer measure;
	dc_serialize(measure);
	if (measure.size() > RewindBuffer::MaxStateSize)
		throw std::runtime_error("This game's state exceeds the rewind workspace limit.");
	scratch.reserve(measure.size());
	scratch.resize(measure.size());
	Serializer ser(scratch.data(), scratch.size());
	dc_serialize(ser);
	return ser.size();
}

const char* unsupportedReason(bool checkThreadMode)
{
	if (settings.platform.isHikaru() || settings.platform.isSystemSP())
		return Tnop("Rewind is not available for this arcade platform.");
	if (settings.raHardcoreMode)
		return Tnop("Rewind is unavailable in RetroAchievements hardcore mode.");
	const bool threaded = checkThreadMode && config::ThreadedRendering;
	const char* restartReason = Tnop("Restart the game to use Rewind with shared emulation and rendering.");
	// The guest thread can change the network connection and reconnect Maple devices.
	if (emu.running() && config::ThreadedRendering)
		return restartReason;
	// Battle Cable uses a serial pipe and never sets the modem's online flag.
	// The active handshake still owns that pipe if the user changes the setting.
	if (settings.network.online || config::GGPOEnable || config::BattleCableEnable
			|| NetworkHandshake::instance != nullptr || ice::getState() != ice::Offline || settings.naomi.multiboard
			|| settings.naomi.slave || settings.naomi.drivingSimSlave)
		return Tnop("Rewind is unavailable during network or linked-cabinet play.");
	if (threaded)
		return restartReason;
	if (config::SerialConsole)
		return Tnop("Rewind is unavailable with host serial output enabled.");
	// Fish Life's touchscreen and Alien Front's modem keep serial queues outside savestates.
	if ((settings.platform.isConsole() && settings.content.gameId == "HDR-0094")
			|| (settings.platform.isNaomi() && settings.content.gameId == "ALIEN FRONT"))
		return Tnop("Rewind is unavailable for this game's serial device.");
	const char* cardReason = Tnop("Rewind is unavailable with an arcade card reader or medal hopper.");
	if (settings.platform.isArcade())
	{
		// NetDIMM state omits writable DIMM memory and drops host sockets on load.
		if (dynamic_cast<NetDimm*>(CurrentCartridge) != nullptr)
			return Tnop("Rewind is unavailable for NetDIMM cartridges.");
		if (card_reader::serialReaderActive() || hopper::active())
			return cardReason;
	}
#ifdef HAVE_PHONE_VMU
	if (phone_vmu::status().running)
		return Tnop("Stop phone VMU sharing before using Rewind.");
#endif
	for (const auto& port : MapleDevices)
		for (const auto& device : port)
		{
			if (dynamic_cast<MapleLinkDevice*>(device.get()) != nullptr)
				return Tnop("Rewind is unavailable with passthrough DreamLink devices.");
			if (device && device->get_device_type() == MDT_RFIDReaderWriter)
				return cardReason;
		}
	return nullptr;
}
}

void setButtonInputAllowed(bool allowed)
{
	const std::lock_guard<std::mutex> lock(requestMutex);
	buttonInputAllowed = allowed;
	if (!allowed)
		clearRequestsLocked();
}

void setRequested(const void* source, bool pressed)
{
	if (source == nullptr)
		return;
	const std::lock_guard<std::mutex> lock(requestMutex);
	if (pressed)
	{
		if (!buttonInputAllowed)
			return;
		++heldSources[source];
	}
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
	clearRequestsLocked();
}

bool requested()
{
	return held.load(std::memory_order_relaxed);
}

bool requested(const void* source)
{
	const std::lock_guard<std::mutex> lock(requestMutex);
	return heldSources.count(source) != 0;
}

const char* unavailableReason()
{
	// Callers translate the returned pointer, so mark every literal for extraction.
	if (!config::RewindEnable)
		return Tnop("Enable Rewind in Advanced settings before starting a game.");
	if (settings.content.path.empty())
		return Tnop("Start a game to record rewind history.");
	if (const char* reason = unsupportedReason(true))
		return reason;
	if (failed)
		return Tnop("Rewind stopped after an error. Restart the game or change its rewind settings.");
	return nullptr;
}

bool requiresSharedThread()
{
	return config::RewindEnable && !settings.content.path.empty() && unsupportedReason(false) == nullptr;
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
	invalidated.store(true, std::memory_order_relaxed);
}

void poll(bool loading)
{
	syncHistory();
	if (loading)
	{
		// The loader owns the per-game options until the new game is ready.
		failed = false;
		return;
	}
	if (!config::RewindEnable)
	{
		if (configuredEnabled)
		{
			clearHistory();
			configuredEnabled = false;
			failed = false;
		}
		if (requested() || stepRequested.load(std::memory_order_relaxed))
			releaseRequests();
		return;
	}
	const int newInterval = std::clamp(config::RewindInterval.get(), 100, 1000);
	const int newDuration = std::clamp(config::RewindDuration.get(), 5, 120);
	const int newCompression = compressionLevel();
	if (!configuredEnabled || interval != newInterval || duration != newDuration || compression != newCompression)
	{
		clearHistory();
		failed = false;
		configuredEnabled = true;
		interval = newInterval;
		duration = newDuration;
		compression = newCompression;
		history.configure(SafetyMemoryLimit,
				(std::size_t(duration) * 1000 + interval - 1) / interval + 1, compression);
	}
}

FrameAction beforeFrame()
{
	syncHistory();
	const bool singleStep = stepRequested.exchange(false, std::memory_order_relaxed);
	if (!requested() && !singleStep)
	{
		if (reversing)
		{
			ClearAudioOutput();
			lastCapture = sh4_sched_now64();
			reversing = false;
		}
		return FrameAction::Run;
	}
	const char* reason = unavailableReason();
	if (reason != nullptr)
	{
		os_notify(i18n::T(reason), 3000);
		clearHistory();
		releaseRequests();
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
		}
		if (!history.pop(scratch, sh4_sched_now64()))
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

void afterFrame()
{
	// Guest storage and networking can change during the frame we just ran.
	syncHistory();
	if (unavailableReason() != nullptr)
	{
		if (history.size() != 0 || !scratch.empty() || (pendingCompression && pendingCompression->token == pendingToken))
			clearHistory();
		if (requested())
			releaseRequests();
		return;
	}
	const u64 now = sh4_sched_now64();
	const u64 ticks = u64(interval) * (SH4_MAIN_CLOCK / 1000);
	if (lastCapture != 0 && now >= lastCapture && now - lastCapture < ticks)
		return;
	if (pendingCompression)
		return;	// compression is behind; skip this capture instead of queueing
	try
	{
		const std::size_t stateSize = captureSnapshot();
		if (!compressWorker)
			compressWorker = std::make_unique<WorkerThread>("RewindCompress");
		const int level = compression;
		const unsigned token = pendingToken;
		pendingCompression = PendingCompression{
			compressWorker->runFuture([workspace = std::move(scratch), stateSize, level]() mutable {
				auto compressed = RewindBuffer::compress(workspace.data(), stateSize, level);
				return CompressionResult{std::move(compressed), std::move(workspace)};
			}), stateSize, now, token};
		lastCapture = now;
	}
	catch (const std::exception& error)
	{
		fail(error);
	}
}
}
