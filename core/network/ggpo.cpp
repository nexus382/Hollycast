/*
	Copyright 2021 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// Portions Copyright 2026 The Hollycast Authors
#include "ggpo.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/flashrom/nvmem.h"
#include "session_state.h"
#include <thread>
#include "input/gamepad_device.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "cfg/option.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#ifndef LIBRETRO
#include "ui/settings_new.h"
#endif
#include "net_platform.h"
#include <algorithm>

namespace ggpo
{

bool inRollback;

static void getLocalInput(MapleInputState inputState[4])
{
	if (!config::ThreadedRendering)
		os_UpdateInputState();
#ifndef LIBRETRO
	const bool captureInput = SettingsNew::liveTuneActive();
#endif
	std::lock_guard<std::mutex> lock(relPosMutex);
	for (int player = 0; player < 4; player++)
	{
		MapleInputState& state = inputState[player];
		state.kcode = kcode[player];
		state.halfAxes[PJTI_L] = lt[player];
		state.halfAxes[PJTI_R] = rt[player];
		state.halfAxes[PJTI_L2] = lt2[player];
		state.halfAxes[PJTI_R2] = rt2[player];
		state.fullAxes[PJAI_X1] = joyx[player];
		state.fullAxes[PJAI_Y1] = joyy[player];
		state.fullAxes[PJAI_X2] = joyrx[player];
		state.fullAxes[PJAI_Y2] = joyry[player];
		state.fullAxes[PJAI_X3] = joy3x[player];
		state.fullAxes[PJAI_Y3] = joy3y[player];
		state.mouseButtons = mo_buttons[player];
		state.absPos.x = mo_x_abs[player];
		state.absPos.y = mo_y_abs[player];
		state.keyboard.shift = kb_shift[player];
		memcpy(state.keyboard.key, kb_key[player], sizeof(kb_key[player]));
		int relX = std::round(mo_x_delta[player]);
		int relY = std::round(mo_y_delta[player]);
		int wheel = std::round(mo_wheel_delta[player]);
		state.relPos.x += relX;
		state.relPos.y += relY;
		state.relPos.wheel += wheel;
		mo_x_delta[player] -= relX;
		mo_y_delta[player] -= relY;
		mo_wheel_delta[player] -= wheel;
#ifndef LIBRETRO
		// Keep raw controls available to ImGui, but discard gameplay input and mouse
		// deltas while tuning so they cannot leak into the game or accumulate on resume.
		if (captureInput)
			state = {};
#endif
	}
}

}

#ifdef USE_GGPO
#include "ggponet.h"
#include "emulator.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "hw/mem/mem_watch.h"
#include <string.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <numeric>
#include <limits>
#include "imgui.h"
#include "miniupnp.h"
#include "hw/naomi/naomi_cart.h"

#ifdef HOLLYCAST_GGPO_FRAME_TRACE
#include "hw/sh4/sh4_sched.h"
#include "nowide/cstdio.hpp"
#endif

//#define SYNC_TEST 1

#include <xxhash.h>
#include "ggpo_adaptive_delay.h"
#include "ggpo_resume_barrier.h"

namespace ggpo
{
using namespace std::chrono;

constexpr int MAX_PLAYERS = 2;
constexpr int SERVER_PORT = 19713;

constexpr u32 BTN_TRIGGER_LEFT	= DC_BTN_BITMAPPED_LAST << 1;
constexpr u32 BTN_TRIGGER_RIGHT	= DC_BTN_BITMAPPED_LAST << 2;

// Protocol 8 coordinates return to play after a GGPO interruption.
static constexpr u32 GGPO_PROTOCOL_VERSION = 8;
static constexpr int INTERRUPT_NOTICE_MS = 1000;
static constexpr int RECONNECT_GRACE_MS = 60000;

#pragma pack(push, 1)
// Exchanged byte-for-byte by GGPO during initial synchronization, so all
// integer fields are fixed-width. Protocol 3 adds the input layout and the
// created Maple device topology: incompatible peers now fail verification
// before gameplay instead of starting a session that can only diverge.
// Protocol 2 peers fail verification. Baseline: Flycast Dojo protocol 2
// exchanged only the game and state digests (core/network/ggpo.cpp at
// blueminder/flycast-dojo fa518ac574b17fbc70837f97275c019b208a8740).
struct VerificationData
{
	u32 protocol = GGPO_PROTOCOL_VERSION;
	u8 gameMD5[16] { };
	u8 stateMD5[16] { };
	u16 inputSize = 0;
	u8 analogAxes = 0;
	u8 inputFlags = 0;	// 1: absolute pointer, 2: keyboard, 4: mouse
	u8 mapleDevices[MAPLE_PORTS * 6] { };
};
static_assert(sizeof(VerificationData) == 64);
#pragma pack(pop)

static GGPOSession *ggpoSession;
static int localPlayerNum;
static std::atomic<int> sessionFrameDelay{ 0 };
// Zero means idle, a positive deadline means waiting, and -1 requests cancellation.
static std::atomic<s64> reconnectDeadlineMs{ 0 };
static std::atomic<s64> reconnectCountdownEndMs{ 0 };
static std::atomic<bool> reconnectInputPending{ false };
static std::atomic<bool> sessionStopRequested{ false };
static std::atomic<bool> canHoldSession{ false };
static std::atomic<bool> libraryHoldRequested{ false };
static std::atomic<bool> libraryHoldAtBoundary{ false };
static bool libraryHoldApplied;
static ResumeBarrier resumeBarrier;
static time_point<steady_clock> nextResumeStatus;
static bool startupProbeActive;
static GGPOPlayerHandle localPlayer;
static GGPOPlayerHandle remotePlayer;
static bool synchronized;
static std::recursive_mutex ggpoMutex;
static std::array<int, 5> msPerFrame;
static int msPerFrameIndex;
static time_point<steady_clock> lastFrameTime;
static int msPerFrameAvg;
static bool _endOfFrame;
static MiniUPnP miniupnp;
static int analogAxes;
static bool absPointerPos;
static bool keyboardGame;
static bool mouseGame;
static int inputSize;
static void (*chatCallback)(int playerNum, const std::string& msg);

struct MemPages
{
	void load()
	{
		memwatch::ramWatcher.getPages(ram);
		memwatch::vramWatcher.getPages(vram);
		memwatch::aramWatcher.getPages(aram);
		memwatch::elanWatcher.getPages(elanram);
	}
	memwatch::PageMap ram;
	memwatch::PageMap vram;
	memwatch::PageMap aram;
	memwatch::PageMap elanram;
};
static std::unordered_map<int, MemPages> deltaStates;
static int lastSavedFrame = -1;

static u64 guestRamChecksum()
{
	// mem_b references guest RAM in both the mapped and allocated memory layouts.
	const size_t size = mem_b.getSize();
	return size != 0 ? XXH3_64bits(&mem_b[0], size) : 0;
}

struct FrameChecksums
{
	u64 full = 0;
	u64 ram = 0;
};
static std::unordered_map<int, FrameChecksums> localChecksums;
static std::unordered_map<int, FrameChecksums> remoteChecksums;
static constexpr int CHECKSUM_INTERVAL = 60;
static constexpr int CHECKSUM_HISTORY = 600;
static int lastChecksumFrameSent;
static bool desyncReported;
static bool snapshotDifferenceLogged;

static void compareChecksums(int frame, const FrameChecksums& local, const FrameChecksums& remote)
{
	if (local.ram == remote.ram)
	{
		// SH4 rollback snapshots include native pointers and padding.
		if (local.full != remote.full && !snapshotDifferenceLogged)
		{
			snapshotDifferenceLogged = true;
			INFO_LOG(NETWORK, "GGPO rollback snapshot bytes differ at boundary %d; guest RAM agrees", frame);
		}
		return;
	}
	if (desyncReported)
		return;
	desyncReported = true;
	WARN_LOG(NETWORK, "GGPO guest RAM differs after confirmed frame %d (local %llx remote %llx)", frame - 1, local.ram, remote.ram);
	os_notify(i18n::T("Connection desynchronized"), 3000);
}

static void resetChecksumState()
{
	localChecksums.clear();
	remoteChecksums.clear();
	lastChecksumFrameSent = 0;
	desyncReported = false;
	snapshotDifferenceLogged = false;
}

static int timesyncOccurred;

#pragma pack(push, 1)
struct Inputs
{
	u32 kcode:20;
	u32 mouseButtons:4;
	u32 kbModifiers:8;

	union {
		struct {
			u8 x;
			u8 y;
		} analog;
		struct {
			s16 x;
			s16 y;
		} absPos;
		struct {
			s16 x;
			s16 y;
			s16 wheel;
		} relPos;
		u8 keys[6];
	} u;
};
static_assert(sizeof(Inputs) == 10, "wrong Inputs size");
static_assert(BTN_TRIGGER_RIGHT < (1 << 20));

struct GameEvent
{
	enum : char {
		Chat,
		VF4Card,
		StateSync,
		DelayFeedback,
		ResumeStatus
	} type;
	union {
		struct {
			u8 playerNum;
			char message[512 - sizeof(playerNum) - sizeof(type)];
		} chat;
		struct {
			u8 playerNum;
			u8 data[128];
		} card;
		struct {
			u32 frame;
			u64 checksum;
			u64 ramChecksum;
		} sync;
		struct {
			u16 stallMs;
			u8 episodes;
			u8 quiet;
		} delay;
		struct {
			u32 generation;
			u32 acknowledged;
			u8 flags;
		} resume;
	} u;

	constexpr static int chatMessageLen(int len) { return len - sizeof(u.chat.playerNum) - sizeof(type); }
};
#pragma pack(pop)

static struct
{
	AdaptiveDelayPolicy policy;
	AdaptiveDelayWindow window;
	time_point<steady_clock> lastFeedback;
	int peerStallMs = 0;
	int pendingChange = 0;
	Inputs previousInput{};
	bool hasPreviousInput = false;
	bool interrupted = false;
} adaptiveDelay;

#ifdef HOLLYCAST_GGPO_FRAME_TRACE
// Diagnostic builds use isolated profiles. An empty ggpo-trace.enable file
// records a session. Copy ggpo-replay.out to ggpo-replay.in to run it offline.
using WireInputs = std::array<u8, sizeof(Inputs) * MAX_PLAYERS>;
struct AppliedInputs
{
	WireInputs bytes{};
	int polls = 0;
};

static std::unique_ptr<FILE, decltype(&fclose)> frameTrace(nullptr, fclose);
static std::unique_ptr<FILE, decltype(&fclose)> replayOutput(nullptr, fclose);
static std::unique_ptr<FILE, decltype(&fclose)> replayInput(nullptr, fclose);
static std::unordered_map<int, AppliedInputs> finalInputs;
static AppliedInputs replayInputs;
static int traceFrame;
static int nextConfirmedInput;
static int replayPolls;
static bool replayFinished;

static void openFrameTrace()
{
	replayInput.reset(nowide::fopen(get_writable_data_path("ggpo-replay.in").c_str(), "rb"));
	auto *enable = nowide::fopen(get_writable_data_path("ggpo-trace.enable").c_str(), "rb");
	if (enable == nullptr && !replayInput)
		return;
	if (enable != nullptr)
		fclose(enable);
	frameTrace.reset(nowide::fopen(get_writable_data_path("ggpo-frames.log").c_str(), "w"));
	if (!replayInput)
		replayOutput.reset(nowide::fopen(get_writable_data_path("ggpo-replay.out").c_str(), "wb"));
	if (!frameTrace || (!replayInput && !replayOutput))
		throw FlycastException("Cannot open GGPO frame trace");
	traceFrame = nextConfirmedInput = replayPolls = 0;
	replayFinished = false;
}

static std::array<u64, 4> traceGuestState(u64 ram)
{
	// Hash named guest registers, including raw FPU bits, without context
	// padding, host callbacks or dynarec temporaries.
	const auto& ctx = Sh4cntx;
	const u32 regs[] = {ctx.pc, ctx.sr.getFull(), ctx.gbr, ctx.ssr, ctx.spc, ctx.sgr,
		ctx.dbr, ctx.vbr, ctx.pr, ctx.mac.l, ctx.mac.h, ctx.interrupt_pend};
	const u32 fpu[] = {ctx.fpscr.full, ctx.fpul};
	return { ram,
		XXH3_64bits(regs, sizeof(regs)) ^ XXH3_64bits(ctx.r, sizeof(ctx.r))
			^ XXH3_64bits_withSeed(ctx.r_bank, sizeof(ctx.r_bank), 1),
		XXH3_64bits(fpu, sizeof(fpu)) ^ XXH3_64bits(ctx.fr, sizeof(ctx.fr))
			^ XXH3_64bits_withSeed(ctx.xf, sizeof(ctx.xf), 1),
		sh4_sched_now64() };
}

static void traceBoundary(int frame, u64 ram)
{
	const auto guest = traceGuestState(ram);
	fprintf(frameTrace.get(), "S %d %d %016llx %016llx %016llx %llu\n",
			frame, (int)inRollback, guest[0], guest[1], guest[2], guest[3]);
}

static bool readReplayFrame()
{
	u8 polled;
	if (fread(&polled, 1, 1, replayInput.get()) != 1)
	{
		if (ferror(replayInput.get()))
			throw FlycastException("Could not read GGPO replay");
		return false;
	}
	if (polled > 1 || fread(replayInputs.bytes.data(), inputSize * MAX_PLAYERS, 1, replayInput.get()) != 1)
		throw FlycastException("Truncated GGPO replay frame");
	replayInputs.polls = polled;
	replayPolls = 0;
	return true;
}

static void startFrameTrace(const VerificationData& verification)
{
	const u32 timing[] = { (u32)config::Cable.get(), (u32)config::Broadcast.get(),
		(u32)config::Region.get(), (u32)config::Language.get(), (u32)config::Sh4Clock.get(),
		(u32)config::UseReios.get(), settings.platform.ram_size };
	const auto start = traceGuestState(guestRamChecksum());
	const u64 header[] = { 0x3145434152545047ULL, XXH3_64bits(&verification, sizeof(verification)),
		XXH3_64bits(timing, sizeof(timing)), start[0], start[1], start[2], start[3] };
	fprintf(frameTrace.get(), "H %s port=%d wire=%d axes=%d dynarec=%d game_bios_flash_vmu=%016llx timing=%016llx\n",
			settings.content.gameId.c_str(), localPlayerNum, inputSize, analogAxes, (int)config::DynarecEnabled.get(),
			header[1], header[2]);
	traceBoundary(0, start[0]);
	if (replayInput)
	{
		u64 savedHeader[std::size(header)];
		if (fread(savedHeader, sizeof(savedHeader), 1, replayInput.get()) != 1
				|| memcmp(header, savedHeader, sizeof(header)) != 0)
			throw FlycastException("GGPO replay starting state or settings differ");
		if (!readReplayFrame())
			throw FlycastException("GGPO replay has no confirmed inputs");
	}
	else if (fwrite(header, sizeof(header), 1, replayOutput.get()) != 1)
		throw FlycastException("Could not write GGPO replay header");
}

static void confirmTracedFrames(int snapshot)
{
	// Boundary N closes input N-1. Earlier forward attempts have already
	// been superseded by any rollback driven by this poll.
	for (; nextConfirmedInput < snapshot; ++nextConfirmedInput)
	{
		AppliedInputs applied;
		const auto it = finalInputs.find(nextConfirmedInput);
		if (it != finalInputs.end())
		{
			applied = it->second;
			finalInputs.erase(it);
		}
		const u8 polled = applied.polls != 0;
		if (fwrite(&polled, 1, 1, replayOutput.get()) != 1
				|| fwrite(applied.bytes.data(), inputSize * MAX_PLAYERS, 1, replayOutput.get()) != 1)
			throw FlycastException("Could not write GGPO replay frames");
		fprintf(frameTrace.get(), "C %d %d\n", nextConfirmedInput, applied.polls);
	}
	if (snapshot % 60 == 0)
	{
		fflush(frameTrace.get());
		fflush(replayOutput.get());
	}
}
#endif

/*
 * begin_game callback - This callback has been deprecated.  You must
 * implement it, but should ignore the 'game' parameter.
 */
static bool begin_game(const char *)
{
	DEBUG_LOG(NETWORK, "Game begin");
	rend_allow_rollback();
	return true;
}

static s64 monotonicMilliseconds()
{
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int reconnectSecondsRemaining()
{
	const s64 deadline = reconnectDeadlineMs.load();
	if (deadline <= 0 || reconnectCountdownEndMs.load() > 0)
		return -1;
	return static_cast<int>(std::max<s64>(0, deadline - monotonicMilliseconds() + 999) / 1000);
}

int reconnectUnpauseSecondsRemaining()
{
	const s64 countdownEnd = reconnectCountdownEndMs.load();
	if (countdownEnd <= 0 || reconnectDeadlineMs.load() <= 0)
		return -1;
	return static_cast<int>(std::max<s64>(0, countdownEnd - monotonicMilliseconds() + 999) / 1000);
}

bool holdSessionForLibrary()
{
	if (!canHoldSession || libraryHoldRequested)
		return false;
	libraryHoldAtBoundary = false;
	libraryHoldRequested = true;
	return true;
}

bool sessionHeldForLibrary()
{
	return libraryHoldRequested && libraryHoldAtBoundary;
}

void returnToHeldSession()
{
	libraryHoldRequested = false;
}

void cancelReconnect()
{
	s64 deadline = reconnectDeadlineMs.load();
	if (deadline > 0)
		reconnectDeadlineMs.compare_exchange_strong(deadline, -1);
}

bool reconnectFramePending()
{
	// Network polling can call back into the emulator to replay guest frames.
	return !inRollback && reconnectInputPending.load();
}

/*
 * on_event - Notification that something has happened.  See the GGPOEventCode
 * structure for more information.
 */
static bool on_event(GGPOEvent *info)
{
	using namespace i18n;
	switch (info->code) {
	case GGPO_EVENTCODE_CONNECTED_TO_PEER:
		if (localPlayerNum != 0)
		{
			GGPONetworkStats stats{};
			if (ggpo_get_network_stats(ggpoSession, info->u.connected.player, &stats) == GGPO_OK)
				startupProbeActive = stats.network.auto_frame_delay;
		}
		INFO_LOG(NETWORK, "Connected to peer %d", info->u.connected.player);
		os_notify(T("Connected to peer"), 2000);
		break;
	case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
		INFO_LOG(NETWORK, "Synchronizing with peer %d", info->u.synchronizing.player);
		os_notify(T("Synchronizing with peer"), 2000);
		break;
	case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
		INFO_LOG(NETWORK, "Synchronized with peer %d", info->u.synchronized.player);
		os_notify(T("Synchronized with peer"), 2000);
		break;
	case GGPO_EVENTCODE_RUNNING:
		INFO_LOG(NETWORK, "Running");
		os_notify(T("Running"), 2000);
		synchronized = true;
		canHoldSession = true;
		break;
	case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
	{
		const s64 deadline = reconnectDeadlineMs.exchange(0);
		NOTICE_LOG(NETWORK, "Disconnected from peer %d", info->u.disconnected.player);
		if (deadline < 0)
			throw FlycastException("");
		if (deadline > 0)
			throw FlycastException(Ts("Reconnect timed out"));
		throw FlycastException(Ts("Disconnected from peer"));
	}
	case GGPO_EVENTCODE_TIMESYNC:
		INFO_LOG(NETWORK, "Timesync: %d frames ahead", info->u.timesync.frames_ahead);
		timesyncOccurred += 5;
		std::this_thread::sleep_for(std::chrono::milliseconds(1000 / (msPerFrameAvg >= 25 ? 30 : 60)));
		break;
	case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
		WARN_LOG(NETWORK, "Connection interrupted with player %d", info->u.connection_interrupted.player);
		adaptiveDelay.interrupted = true;
		adaptiveDelay.pendingChange = 0;
		resumeBarrier.begin();
		reconnectCountdownEndMs = 0;
		{
			s64 idle = 0;
			reconnectDeadlineMs.compare_exchange_strong(idle,
					monotonicMilliseconds() + info->u.connection_interrupted.disconnect_timeout);
		}
		break;
	case GGPO_EVENTCODE_CONNECTION_RESUMED:
		NOTICE_LOG(NETWORK, "Connection reachable again for player %d", info->u.connection_resumed.player);
		adaptiveDelay.interrupted = false;
		break;
	}
	return true;
}

/*
 * advance_frame - Called during a rollback.  You should advance your game
 * state by exactly one frame.  Before each frame, call ggpo_synchronize_input
 * to retrieve the inputs you should use for that frame.  After each frame,
 * you should call ggpo_advance_frame to notify GGPO.net that you're
 * finished.
 *
 * The flags parameter is reserved.  It can safely be ignored at this time.
 */
static bool advance_frame(int)
{
	INFO_LOG(NETWORK, "advance_frame");
	settings.aica.muteAudio = true;
	rend_enable_renderer(false);
	inRollback = true;

	emu.run();
	ggpo_advance_frame(ggpoSession);

	settings.aica.muteAudio = false;
	rend_enable_renderer(true);
	inRollback = false;
	_endOfFrame = false;

	return true;
}

/*
 * load_game_state - GGPO.net will call this function at the beginning
 * of a rollback.  The buffer and len parameters contain a previously
 * saved state returned from the save_game_state function.  The client
 * should make the current game state match the state contained in the
 * buffer.
 */
static bool load_game_state(unsigned char *buffer, int len)
{
	INFO_LOG(NETWORK, "load_game_state");

	rend_start_rollback();
	// FIXME dynarecs
	Deserializer deser(buffer, len, true);
	int frame;
	deser >> frame;
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (frameTrace)
	{
		fprintf(frameTrace.get(), "R %d %d\n", lastSavedFrame, frame);
		for (int f = frame; f < lastSavedFrame; ++f)
			finalInputs.erase(f);
		traceFrame = frame;
	}
#endif
	memwatch::unprotect();
	for (int f = lastSavedFrame - 1; f >= frame; f--)
	{
		const MemPages& pages = deltaStates[f];
		for (const auto& pair : pages.ram)
			memcpy(memwatch::ramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.vram)
			memcpy(memwatch::vramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.aram)
			memcpy(memwatch::aramWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		for (const auto& pair : pages.elanram)
			memcpy(memwatch::elanWatcher.getMemPage(pair.first), &pair.second.data[0], PAGE_SIZE);
		DEBUG_LOG(NETWORK, "Restored frame %d pages: %d ram, %d vram, %d eram, %d aica ram", f, (u32)pages.ram.size(),
					(u32)pages.vram.size(), (u32)pages.elanram.size(), (u32)pages.aram.size());
	}
	dc_deserialize(deser);
	if (deser.size() != (u32)len)
	{
		ERROR_LOG(NETWORK, "load_game_state len %d used %d", len, (int)deser.size());
		die("fatal");
	}
	rend_allow_rollback();	// ggpo might load another state right after this one
	memwatch::reset();
	memwatch::protect();
	return true;
}

/*
 * save_game_state - The client should allocate a buffer, copy the
 * entire contents of the current game state into it, and copy the
 * length into the *len parameter.  Optionally, the client can compute
 * a checksum of the data and store it in the *checksum argument.
 */
static bool save_game_state(unsigned char **buffer, int *len, int *checksum, int frame)
{
	verify(!emu.getSh4Executor()->IsCpuRunning());
	lastSavedFrame = frame;
	// TODO this is way too much memory
	size_t allocSize = settings.platform.isNaomi() ? 20_MB : 10_MB;
	*buffer = (unsigned char *)malloc(allocSize);
	if (*buffer == nullptr)
	{
		WARN_LOG(NETWORK, "Memory alloc failed");
		*len = 0;
		return false;
	}
	Serializer ser(*buffer, allocSize, true);
	try {
		ser << frame;
		dc_serialize(ser);
		*len = ser.size();
	} catch (const Serializer::Exception& e) {
		WARN_LOG(NETWORK, "Save state failed: %s", e.what());
		*len = 0;
		return false;
	}
	const bool exchangeChecksum = frame > 0 && frame % CHECKSUM_INTERVAL == 0;
#ifdef SYNC_TEST
	const u64 fullChecksum = XXH3_64bits(*buffer, *len);
#else
	const u64 fullChecksum = exchangeChecksum ? XXH3_64bits(*buffer, *len) : 0;
#endif
	*checksum = (int)fullChecksum;
	bool hashRam = exchangeChecksum;
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	hashRam = hashRam || frameTrace != nullptr;
#endif
	const u64 ramChecksum = hashRam ? guestRamChecksum() : 0;
	if (exchangeChecksum)
		localChecksums[frame] = { fullChecksum, ramChecksum };
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (frameTrace)
	{
		traceFrame = frame;
		traceBoundary(frame, ramChecksum);
	}
#endif
	{
		for (auto it = localChecksums.begin(); it != localChecksums.end();)
		{
			if (it->first < frame - CHECKSUM_HISTORY)
				it = localChecksums.erase(it);
			else
				++it;
		}
		for (auto it = remoteChecksums.begin(); it != remoteChecksums.end();)
		{
			if (it->first < frame - CHECKSUM_HISTORY)
				it = remoteChecksums.erase(it);
			else
				++it;
		}
	}
	memwatch::protect();
	if (frame > 0)
	{
#ifdef SYNC_TEST
		if (deltaStates.count(frame - 1) != 0)
		{
			MemPages memPages;
			memPages.load();
			const MemPages& savedPages = deltaStates[frame - 1];
			//verify(memPages.ram.size() == savedPages.ram.size());
			if (memPages.ram.size() != savedPages.ram.size())
			{
				ERROR_LOG(NETWORK, "old ram size %d new %d", (u32)savedPages.ram.size(), (u32)memPages.ram.size());
				if (memPages.ram.size() > savedPages.ram.size())
					for (const auto& pair : memPages.ram)
					{
						if (savedPages.ram.count(pair.first) == 0)
							ERROR_LOG(NETWORK, "new page @ %x", pair.first);
						else
							DEBUG_LOG(NETWORK, "page ok @ %x", pair.first);
					}
				die("fatal");
			}
			for (const auto& pair : memPages.ram)
			{
				verify(savedPages.ram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.ram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
			verify(memPages.vram.size() == savedPages.vram.size());
			for (const auto& pair : memPages.vram)
			{
				verify(savedPages.vram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.vram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
			//verify(memPages.aram.size() == savedPages.aram.size());
			if (memPages.aram.size() != savedPages.aram.size())
			{
				ERROR_LOG(NETWORK, "old aram size %d new %d", (u32)savedPages.aram.size(), (u32)memPages.aram.size());
				die("fatal");
			}
			for (const auto& pair : memPages.aram)
			{
				verify(savedPages.aram.count(pair.first) == 1);
				verify(memcmp(&pair.second[0], &savedPages.aram.find(pair.first)->second[0], PAGE_SIZE) == 0);
			}
		}
#endif
		// Save the delta to frame-1
		deltaStates[frame - 1].load();
		DEBUG_LOG(NETWORK, "Saved frame %d pages: %d ram, %d vram, %d eram, %d aica ram", frame - 1, (u32)deltaStates[frame - 1].ram.size(),
				(u32)deltaStates[frame - 1].vram.size(), (u32)deltaStates[frame - 1].elanram.size(), (u32)deltaStates[frame - 1].aram.size());
	}

	return true;
}

/*
 * log_game_state - Used in diagnostic testing.  The client should use
 * the ggpo_log function to write the contents of the specified save
 * state in a human readible form.
 */
static bool log_game_state(char *filename, unsigned char *buffer, int len)
{
#ifdef SYNC_TEST
	static int lastLoggedFrame = -1;
	static u8 *lastState;
	int frame = *(u32 *)buffer;
	DEBUG_LOG(NETWORK, "log_game_state frame %d len %d", frame, len);
	if (lastLoggedFrame == frame) {
		for (int i = 0; i < len; i++)
			if (buffer[i] != lastState[i])
			{
				WARN_LOG(NETWORK, "States for frame %d differ at offset %d: now %x prev %x", frame, i, *(u32 *)&buffer[i & ~3], *(u32 *)&lastState[i & ~3]);
				break;
			}
	}
	lastState = buffer;
	lastLoggedFrame = frame;
#endif

	return true;
}

/*
 * free_buffer - Frees a game state allocated in save_game_state.  You
 * should deallocate the memory contained in the buffer.
 */
static void free_buffer(void *buffer)
{
	if (buffer != nullptr)
	{
		Deserializer deser(buffer, 1_MB, true);
		int frame;
		deser >> frame;
		deltaStates.erase(frame);
		free(buffer);
	}
}

static void on_message(u8 *msg, int len)
{
	if (len == 0)
		return;
	GameEvent *event = (GameEvent *)msg;
	switch (event->type)
	{
	case GameEvent::Chat:
		if (chatCallback != nullptr && GameEvent::chatMessageLen(len) > 0)
			chatCallback(event->u.chat.playerNum, std::string(event->u.chat.message, GameEvent::chatMessageLen(len)));
		break;

	case GameEvent::StateSync:
		if (len == sizeof(event->type) + sizeof(event->u.sync))
		{
			const s64 frame = event->u.sync.frame;
			if (frame > 0 && frame % CHECKSUM_INTERVAL == 0
					&& frame >= (s64)lastSavedFrame - CHECKSUM_HISTORY
					&& frame <= (s64)lastSavedFrame + CHECKSUM_HISTORY
					&& frame <= std::numeric_limits<int>::max())
				remoteChecksums[static_cast<int>(frame)] = { event->u.sync.checksum, event->u.sync.ramChecksum };
		}
		break;

	case GameEvent::VF4Card:
		setRfidCardData(event->u.card.playerNum, event->u.card.data);
		break;

	case GameEvent::DelayFeedback:
		if (len == sizeof(event->type) + sizeof(event->u.delay) && startupProbeActive && synchronized && !mouseGame
				&& !adaptiveDelay.interrupted
				&& event->u.delay.stallMs <= AdaptiveDelayWindow::WindowMs && event->u.delay.quiet <= 1
				&& (!event->u.delay.quiet || (event->u.delay.stallMs == 0 && event->u.delay.episodes == 0)))
		{
			adaptiveDelay.lastFeedback = steady_clock::now();
			adaptiveDelay.peerStallMs = event->u.delay.stallMs;
			adaptiveDelay.pendingChange = adaptiveDelay.policy.observe(adaptiveDelay.lastFeedback, event->u.delay.stallMs,
					event->u.delay.episodes, event->u.delay.quiet != 0, sessionFrameDelay.load());
		}
		break;
	case GameEvent::ResumeStatus:
		if (synchronized && len == sizeof(event->type) + sizeof(event->u.resume))
			resumeBarrier.receive({ event->u.resume.generation, event->u.resume.acknowledged, event->u.resume.flags },
					steady_clock::now());
		break;
	default:
		WARN_LOG(NETWORK, "Unknown app message type %d", event->type);
		break;
	}
}

// --- Host-to-guest session state (memory cards + console flash) ---
// The guest adopts the host's images in memory so both sides verify and
// emulate from the same initial state. The guest uses in-memory VMUs and
// restores its original images in memory when the session stops.
static bool sessionStateAdopted;
static std::vector<std::pair<int, int>> guestAdoptedVmus;
static std::vector<std::vector<u8>> guestVmuBackups;

static std::vector<sessionstate::Item> collectSessionState()
{
	std::vector<sessionstate::Item> items;
	const u8* flash = nvmem::getFlashData();
	if (flash != nullptr)
	{
		sessionstate::Item item;
		item.kind = sessionstate::KIND_FLASH;
		item.data.assign(flash, flash + settings.platform.flash_size);
		items.push_back(std::move(item));
	}
	for (int bus = 0; bus < MAPLE_PORTS; bus++)
		for (int port = 0; port < 6; port++)
		{
			const auto& device = MapleDevices[bus][port];
			if (device == nullptr || device->get_device_type() != MDT_SegaVMU)
				continue;
			size_t size = 0;
			const void* data = device->getData(size);
			if (data == nullptr || size == 0)
				continue;
			sessionstate::Item item;
			item.kind = sessionstate::KIND_VMU;
			item.bus = (u8)bus;
			item.port = (u8)port;
			item.data.assign((const u8*)data, (const u8*)data + size);
			items.push_back(std::move(item));
		}
	return items;
}

static std::atomic<bool> sessionStateServeStop{ true };
static std::thread sessionStateThread;

static void serveSessionState(int statePort, in_addr guestAddress, const std::vector<sessionstate::Item>& items)
{
	sock_t listener = socket(AF_INET, SOCK_STREAM, 0);
	if (!VALID(listener))
	{
		WARN_LOG(NETWORK, "Session state server: socket failed");
		return;
	}
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons((u_short)statePort);
	if (bind(listener, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0)
	{
		WARN_LOG(NETWORK, "Session state server: bind/listen on port %d failed", statePort);
		closesocket(listener);
		return;
	}
	// Allow the configured guest to retry while the GGPO handshake is pending.
	while (!sessionStateServeStop)
	{
		fd_set readFds;
		FD_ZERO(&readFds);
		FD_SET(listener, &readFds);
		timeval timeout{ 0, 500000 };
		if (select((int)listener + 1, &readFds, nullptr, nullptr, &timeout) <= 0)
			continue;
		sockaddr_in peer{};
		socklen_t peerSize = sizeof(peer);
		sock_t conn = accept(listener, (sockaddr*)&peer, &peerSize);
		if (!VALID(conn))
			continue;
		if (sessionStateServeStop || peer.sin_addr.s_addr != guestAddress.s_addr)
		{
			closesocket(conn);
			continue;
		}
		set_recv_timeout(conn, 10000);
		set_send_timeout(conn, 10000);
		u32 protocol = 0;
		if (sessionstate::receiveRequest(conn, protocol) && protocol == GGPO_PROTOCOL_VERSION && !sessionStateServeStop)
		{
			if (sessionstate::sendItems(conn, items))
				NOTICE_LOG(NETWORK, "Served session state to guest (%d items)", (int)items.size());
			else
				WARN_LOG(NETWORK, "Session state server: sending to guest failed");
		}
		else
			WARN_LOG(NETWORK, "Session state server: bad request (protocol %d)", protocol);
		closesocket(conn);
	}
	closesocket(listener);
}

static void stopSessionStateServer()
{
	sessionStateServeStop = true;
	if (sessionStateThread.joinable())
		sessionStateThread.join();
}

static void startSessionStateServer(int statePort, in_addr guestAddress)
{
	stopSessionStateServer();
	// Capture the same initial state for every guest attempt while emulation is stopped.
	auto items = collectSessionState();
	sessionStateServeStop = false;
	sessionStateThread = std::thread(serveSessionState, statePort, guestAddress, std::move(items));
}

static std::vector<sessionstate::Item> fetchSessionState(const std::string& hostIp, int statePort)
{
	NOTICE_LOG(NETWORK, "Fetching host session state from %s:%d", hostIp.c_str(), statePort);
	const char* stage = "connect";
	const auto deadline = steady_clock::now() + std::chrono::seconds(60);
	while (steady_clock::now() < deadline)
	{
		stage = "connect";
		sock_t sock = sessionstate::connectWithTimeout(hostIp, statePort, 1000);
		if (VALID(sock))
		{
			set_recv_timeout(sock, 10000);
			set_send_timeout(sock, 10000);
			std::vector<sessionstate::Item> items;
			stage = "send-request";
			bool ok = sessionstate::sendRequest(sock, GGPO_PROTOCOL_VERSION);
			if (ok)
			{
				stage = "receive-items";
				ok = sessionstate::receiveItems(sock, items);
			}
			closesocket(sock);
			if (ok)
			{
				NOTICE_LOG(NETWORK, "Received host session state (%d items)", (int)items.size());
				return items;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(250));
	}
	WARN_LOG(NETWORK, "Fetching host session state failed at stage %s", stage);
	throw FlycastException("Could not receive the host's memory card and console settings. Make sure the host uses the same HollyCast version and is reachable.");
}

static maple_sega_vmu* getSessionVmu(int bus, int port)
{
	if (bus >= MAPLE_PORTS || port >= 6 || MapleDevices[bus][port] == nullptr)
		return nullptr;
	return dynamic_cast<maple_sega_vmu*>(MapleDevices[bus][port].get());
}

static void applySessionState(const std::vector<sessionstate::Item>& items)
{
	// Validate the bundle and allocate backups before changing the guest's data.
	const sessionstate::Item* flashItem = nullptr;
	std::vector<std::pair<int, int>> adoptedVmus;
	std::vector<std::vector<u8>> vmuBackups;
	for (const sessionstate::Item& item : items)
	{
		if (item.kind == sessionstate::KIND_FLASH)
		{
			if (item.data.size() != settings.platform.flash_size)
				throw FlycastException("The host's console settings have an unexpected size");
			flashItem = &item;
		}
		else if (item.kind == sessionstate::KIND_VMU)
		{
			maple_sega_vmu* vmu = getSessionVmu(item.bus, item.port);
			if (vmu == nullptr)
				throw FlycastException("The host uses a memory card in a slot that is empty on this device. Match the host's controller setup and try again.");
			if (item.data.size() != sizeof(vmu->flash_data))
				throw FlycastException("The host's memory card has an unexpected size");
			adoptedVmus.emplace_back(item.bus, item.port);
			vmuBackups.emplace_back(vmu->flash_data, vmu->flash_data + sizeof(vmu->flash_data));
		}
	}
	if (flashItem == nullptr)
		throw FlycastException("The host did not send its console settings");

	nvmem::adoptSessionFlash(flashItem->data.data());
	guestAdoptedVmus.swap(adoptedVmus);
	guestVmuBackups.swap(vmuBackups);
	sessionStateAdopted = true;
	for (const sessionstate::Item& item : items)
	{
		if (item.kind != sessionstate::KIND_VMU)
			continue;
		maple_sega_vmu* vmu = getSessionVmu(item.bus, item.port);
		std::memcpy(vmu->flash_data, item.data.data(), item.data.size());
	}
	mcfg_UpdateVmuDigest();
	NOTICE_LOG(NETWORK, "Adopted the host's memory card and console settings for this session");
}

static void restoreSessionState()
{
	sessionStateAdopted = false;
	nvmem::restoreSessionFlash();
	for (size_t i = 0; i < guestAdoptedVmus.size(); i++)
	{
		if (maple_sega_vmu* vmu = getSessionVmu(guestAdoptedVmus[i].first, guestAdoptedVmus[i].second))
			std::memcpy(vmu->flash_data, guestVmuBackups[i].data(), guestVmuBackups[i].size());
	}
	guestAdoptedVmus.clear();
	guestVmuBackups.clear();
}

static void syncSessionState(int localPort, int playerNum, const std::string& peerIp, u32 peerPort, in_addr peerAddress)
{
	if (playerNum == 0)
	{
		startSessionStateServer(localPort + 1, peerAddress);
	}
	else
	{
		applySessionState(fetchSessionState(peerIp, (int)peerPort + 1));
	}
}

static void resetReconnectState()
{
	reconnectDeadlineMs = 0;
	reconnectCountdownEndMs = 0;
	reconnectInputPending = false;
	canHoldSession = false;
	libraryHoldRequested = false;
	libraryHoldAtBoundary = false;
	libraryHoldApplied = false;
	resumeBarrier = {};
	nextResumeStatus = {};
}

void startSession(int localPort, int localPlayerNum)
{
	sessionFrameDelay = config::GGPODelay.get();
	resetReconnectState();
	sessionStopRequested = false;
	startupProbeActive = false;
	adaptiveDelay = {};
	resetChecksumState();
	lastSavedFrame = -1;
	// A previous arcade session may have used a different wire layout.
	analogAxes = 0;
	absPointerPos = false;
	keyboardGame = false;
	mouseGame = false;

	GGPOSessionCallbacks cb{};
	cb.begin_game      = begin_game;
	cb.advance_frame   = advance_frame;
	cb.load_game_state = load_game_state;
	cb.save_game_state = save_game_state;
	cb.free_buffer     = free_buffer;
	cb.on_event        = on_event;
	cb.log_game_state  = log_game_state;
	cb.on_message      = on_message;

#ifdef SYNC_TEST
	inputSize = sizeof(kcode[0]);
	GGPOErrorCode result = ggpo_start_synctest(&ggpoSession, &cb, settings.content.gameId.c_str(), MAX_PLAYERS, inputSize, 1);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO start sync session failed: %d", result);
		ggpoSession = nullptr;
		throw FlycastException("GGPO start sync session failed");
	}
	ggpo_idle(ggpoSession, 0);
	ggpo::localPlayerNum = localPlayerNum;
	GGPOPlayer player{ sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, localPlayerNum + 1 };
	result = ggpo_add_player(ggpoSession, &player, &localPlayer);
	player.player_num = (1 - localPlayerNum) + 1;
	result = ggpo_add_player(ggpoSession, &player, &remotePlayer);
	synchronized = true;
	NOTICE_LOG(NETWORK, "GGPO synctest session started");
#else
	if (settings.platform.isConsole())
		analogAxes = config::GGPOAnalogAxes;
	else
	{
		if (settings.input.lightgunGame)
			absPointerPos = true;
		else if (settings.input.keyboardGame)
			keyboardGame = true;
		else if (settings.input.mouseGame)
			mouseGame = true;
		else if (NaomiGameInputs != nullptr)
		{
			for (const auto& axis : NaomiGameInputs->axes)
			{
				if (axis.name == nullptr)
					break;
				if (axis.type == Full)
					analogAxes = std::max(analogAxes, (int)axis.axis + 1);
			}
		}
		NOTICE_LOG(NETWORK, "GGPO: Using %d full analog axes", analogAxes);
	}
	// The wire format carries at most two analog axes (see Inputs).
	if (analogAxes < 0 || analogAxes > 2)
		throw FlycastException("GGPO analog axes must be between 0 and 2");
	const size_t sessionInputSize = sizeof(kcode[0]) + analogAxes + (int)absPointerPos * sizeof(Inputs::u.absPos)
		+ (int)keyboardGame * sizeof(Inputs::u.keys) + (int)mouseGame * sizeof(Inputs::u.relPos);
	if (sessionInputSize > sizeof(Inputs))
		throw FlycastException("GGPO input layout exceeds the wire buffer");
	inputSize = static_cast<int>(sessionInputSize);

#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	openFrameTrace();
#endif

	size_t colon = config::NetworkServer.get().find(':');
	std::string peerIp = config::NetworkServer.get().substr(0, colon);
	if (peerIp.empty())
		peerIp = "127.0.0.1";
	u32 peerPort;
	if (colon == std::string::npos)
	{
		if (peerIp == "127.0.0.1")
			peerPort = localPort ^ 1;
		else
			peerPort = SERVER_PORT;
	}
	else
	{
		peerPort = atoi(config::NetworkServer.get().substr(colon + 1).c_str());
	}

	// Resolve once so the save transfer and GGPO use the same peer address.
	in_addr peerAddr{};
	if (inet_pton(AF_INET, peerIp.c_str(), &peerAddr) != 1)
	{
		addrinfo hints {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo *result = nullptr;
		if (getaddrinfo(peerIp.c_str(), nullptr, &hints, &result) != 0 || result == nullptr)
			throw FlycastException(strprintf(i18n::T("GGPO: unknown peer address %s"), peerIp.c_str()));
		peerAddr = ((sockaddr_in *)result->ai_addr)->sin_addr;
		freeaddrinfo(result);
		char address[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &peerAddr, address, sizeof(address));
		NOTICE_LOG(NETWORK, "GGPO: peer %s resolved to %s", peerIp.c_str(), address);
		peerIp = address;
	}

	// The guest adopts the host's memory card and console flash before
	// verification, so both sides start from the same saved state.
	if (settings.platform.isConsole()
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
			&& !replayInput
#endif
		)
		syncSessionState(localPort, localPlayerNum, peerIp, peerPort, peerAddr);

	VerificationData verif;
	verif.inputSize = static_cast<u16>(inputSize);
	verif.analogAxes = static_cast<u8>(analogAxes);
	verif.inputFlags = static_cast<u8>((absPointerPos ? 1 : 0) | (keyboardGame ? 2 : 0) | (mouseGame ? 4 : 0));
	mcfg_GetDeviceTypes(verif.mapleDevices);
	MD5Sum().add(settings.network.md5.bios)
			.add(settings.network.md5.game)
			// The Dreamcast guest can read the cable type directly.
			.add(settings.platform.isConsole() ? static_cast<u32>(config::Cable.get()) : 0u)
			.getDigest(verif.gameMD5);
	auto& digest = settings.network.md5.savestate;
	if (std::find_if(std::begin(digest), std::end(digest), [](u8 b) { return b != 0; }) != std::end(digest))
		memcpy(verif.stateMD5, digest, sizeof(digest));
	else
	{
		MD5Sum().add(settings.network.md5.nvmem)
				.add(settings.network.md5.nvmem2)
				.add(settings.network.md5.eeprom)
				.add(settings.network.md5.vmu)
				.getDigest(verif.stateMD5);
	}

#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (frameTrace)
	{
		ggpo::localPlayerNum = localPlayerNum;
		startFrameTrace(verif);
		if (replayInput)
		{
			begin_game(nullptr);
			synchronized = true;
			return;
		}
	}
#endif

	GGPOErrorCode result = ggpo_start_session(&ggpoSession, &cb, settings.content.gameId.c_str(), MAX_PLAYERS, inputSize, localPort,
			&verif, sizeof(verif));
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO start session failed: %d", result);
		ggpoSession = nullptr;
		throw FlycastException("GGPO network initialization failed");
	}

	// automatically disconnect clients after 3000 ms and start our count-down timer
	// for disconnects after 1000 ms.   To completely disable disconnects, simply use
	// a value of 0 for ggpo_set_disconnect_timeout.
	ggpo_set_disconnect_timeout(ggpoSession, INTERRUPT_NOTICE_MS + RECONNECT_GRACE_MS);
	ggpo_set_disconnect_notify_start(ggpoSession, INTERRUPT_NOTICE_MS);

	ggpo::localPlayerNum = localPlayerNum;
	startupProbeActive = localPlayerNum == 0 && config::GGPOAutoDelay;
	if (startupProbeActive && ggpo_set_auto_frame_delay(ggpoSession, true) != GGPO_OK)
	{
		stopSession();
		throw FlycastException("GGPO network test initialization failed");
	}
	GGPOPlayer player{ sizeof(GGPOPlayer), GGPO_PLAYERTYPE_LOCAL, localPlayerNum + 1 };
	result = ggpo_add_player(ggpoSession, &player, &localPlayer);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO cannot add local player: %d", result);
		stopSession();
		throw FlycastException("GGPO cannot add local player");
	}
	ggpo_set_frame_delay(ggpoSession, localPlayer, sessionFrameDelay.load());

	player.type = GGPO_PLAYERTYPE_REMOTE;
	strcpy(player.u.remote.ip_address, peerIp.c_str());
	player.u.remote.port = peerPort;
	player.player_num = (1 - localPlayerNum) + 1;
	result = ggpo_add_player(ggpoSession, &player, &remotePlayer);
	if (result != GGPO_OK)
	{
		WARN_LOG(NETWORK, "GGPO cannot add remote player: %d", result);
		stopSession();
		throw FlycastException("GGPO cannot add remote player");
	}
	DEBUG_LOG(NETWORK, "GGPO session started");
#endif
}

void stopSession()
{
	sessionStopRequested = true;
	canHoldSession = false;
	stopSessionStateServer();
	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
	resetReconnectState();
	resetChecksumState();
	lastSavedFrame = -1;
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (replayInput)
		emu.setNetworkState(false);
	replayInput.reset();
	replayOutput.reset();
	frameTrace.reset();
	finalInputs.clear();
#endif
	if (sessionStateAdopted)
		restoreSessionState();
	if (ggpoSession == nullptr)
		return;
	ggpo_close_session(ggpoSession);
	ggpoSession = nullptr;
	miniupnp.Term();
	emu.setNetworkState(false);
	memwatch::unprotect();
	memwatch::reset();
}

void getInput(MapleInputState inputState[4])
{
	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
	if (!active())
	{
		getLocalInput(inputState);
		return;
	}
	for (int player = 0; player < 4; player++)
		inputState[player] = {};

	std::array<u8, sizeof(Inputs) * MAX_PLAYERS> inputData{};
	const int wireSize = inputSize * MAX_PLAYERS;
	GGPOErrorCode error;
	// should not call any callback
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	int predicted = 0;
	if (replayInput)
	{
		inputData = replayInputs.bytes;
		++replayPolls;
		error = GGPO_OK;
	}
	else if (frameTrace)
		error = ggpo_synchronize_input_with_prediction(ggpoSession, inputData.data(), wireSize, &predicted);
	else
#endif
		error = ggpo_synchronize_input(ggpoSession, inputData.data(), wireSize, nullptr);
	if (error != GGPO_OK)
	{
		stopSession();
		throw FlycastException("GGPO error");
	}

	for (int player = 0; player < MAX_PLAYERS; player++)
	{
		MapleInputState& state = inputState[player];
		const Inputs *inputs = (Inputs *)&inputData[player * inputSize];
		state.kcode = ~inputs->kcode;
		if (analogAxes > 0)
		{
			state.fullAxes[PJAI_X1] = inputs->u.analog.x << 8;
			if (analogAxes >= 2)
				state.fullAxes[PJAI_Y1] = inputs->u.analog.y << 8;
		}
		else if (absPointerPos)
		{
			state.absPos.x = inputs->u.absPos.x;
			state.absPos.y = inputs->u.absPos.y;
		}
		else if (keyboardGame)
		{
			memcpy(state.keyboard.key, inputs->u.keys, sizeof(state.keyboard.key));
			state.keyboard.shift = inputs->kbModifiers;
		}
		else if (mouseGame)
		{
			state.relPos.x = inputs->u.relPos.x;
			state.relPos.y = inputs->u.relPos.y;
			state.relPos.wheel = inputs->u.relPos.wheel;
			state.mouseButtons = ~inputs->mouseButtons;
		}
		state.halfAxes[PJTI_R] = (state.kcode & BTN_TRIGGER_RIGHT) == 0 ? 0xffff : 0;
		state.halfAxes[PJTI_L] = (state.kcode & BTN_TRIGGER_LEFT) == 0 ? 0xffff : 0;
	}
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (frameTrace)
	{
		if (!replayInput)
		{
			auto& applied = finalInputs[traceFrame];
			applied.bytes = inputData;
			++applied.polls;
		}
		fprintf(frameTrace.get(), "I %d %d %x %llu ", traceFrame, (int)inRollback,
				(unsigned)predicted, sh4_sched_now64());
		for (int i = 0; i < wireSize; ++i)
			fprintf(frameTrace.get(), "%02x", (unsigned)inputData[i]);
		for (int port = 0; port < MAX_PLAYERS; ++port)
		{
			const auto& state = inputState[port];
			fprintf(frameTrace.get(), " port=%d/buttons=%08x/x=%d/y=%d/l=%u/r=%u", port, (unsigned)state.kcode,
					(int)state.fullAxes[PJAI_X1], (int)state.fullAxes[PJAI_Y1],
					(unsigned)state.halfAxes[PJTI_L], (unsigned)state.halfAxes[PJTI_R]);
		}
		fputc('\n', frameTrace.get());
	}
#endif
}

static void sampleAdaptiveDelay(time_point<steady_clock> now, int predicted, steady_clock::duration waited, bool newStall)
{
	const auto report = adaptiveDelay.window.observe(now, waited, newStall, predicted >= GGPO_MAX_PREDICTION_FRAMES - 3);
	if (!report || adaptiveDelay.interrupted)
		return;

	// The peer owns the input stream that can arrive earlier when delayed.
	GameEvent event;
	event.type = GameEvent::DelayFeedback;
	event.u.delay.stallMs = static_cast<u16>(report->stallMs);
	event.u.delay.episodes = static_cast<u8>(std::min(report->episodes, 255));
	event.u.delay.quiet = report->quiet;
	ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.delay), false);
}

static void updateReconnect()
{
	const auto now = steady_clock::now();
	const s64 nowMs = duration_cast<milliseconds>(now.time_since_epoch()).count();
	const bool holdInLibrary = libraryHoldRequested;
	if (holdInLibrary && !libraryHoldApplied)
	{
		resumeBarrier.begin();
		NOTICE_LOG(NETWORK, "GGPO match held for return from library");
	}
	libraryHoldApplied = holdInLibrary;

	// Check expiry before allowing a newly received packet to finish recovery.
	const s64 waitDeadline = reconnectDeadlineMs.load();
	const s64 previousCountdown = reconnectCountdownEndMs.load();
	if (waitDeadline > 0 && nowMs >= std::max(waitDeadline, previousCountdown > 0 ? previousCountdown + 2000 : 0))
		throw FlycastException(i18n::Ts("Reconnect timed out"));

	resumeBarrier.update(now, !holdInLibrary && !adaptiveDelay.interrupted);
	if (resumeBarrier.waiting())
	{
		s64 idle = 0;
		reconnectDeadlineMs.compare_exchange_strong(idle, nowMs + RECONNECT_GRACE_MS);
		const s64 countdown = duration_cast<milliseconds>(resumeBarrier.countdownEnd().time_since_epoch()).count();
		if (countdown > 0 && previousCountdown == 0)
			NOTICE_LOG(NETWORK, "GGPO reconnected, starting five-second countdown");
		reconnectCountdownEndMs = countdown;
	}
	else if (waitDeadline > 0)
	{
		s64 waiting = waitDeadline;
		if (reconnectDeadlineMs.compare_exchange_strong(waiting, 0))
			NOTICE_LOG(NETWORK, "GGPO resuming play after reconnect countdown");
		reconnectCountdownEndMs = 0;
	}

	if (resumeBarrier.shouldSend(now) && now >= nextResumeStatus)
	{
		const auto status = resumeBarrier.status();
		GameEvent event{};
		event.type = GameEvent::ResumeStatus;
		event.u.resume = { status.generation, status.acknowledged, status.flags };
		ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.resume), false);
		nextResumeStatus = now + milliseconds(100);
	}
}

static bool readyAfterReconnect()
{
	for (;;)
	{
		if (sessionStopRequested)
			return false;
		updateReconnect();
		const s64 deadline = reconnectDeadlineMs.load();
		if (deadline == 0)
		{
			libraryHoldAtBoundary = false;
			return true;
		}
		if (deadline < 0)
		{
			NOTICE_LOG(NETWORK, "Reconnect canceled");
			// Let the UI unload and close the session after the emulation thread exits.
			throw FlycastException("");
		}
		if (libraryHoldApplied)
			libraryHoldAtBoundary = true;
		if (!config::ThreadedRendering)
			return false;

		std::this_thread::sleep_for(milliseconds(5));
		if (ggpo_idle(ggpoSession, 0) != GGPO_OK)
		{
			stopSession();
			throw FlycastException("GGPO error");
		}
	}
}

bool nextFrame()
{
	const bool pendingInput = reconnectInputPending.load();
	if (!_endOfFrame && !pendingInput)
		return false;
	_endOfFrame = false;
	if (inRollback)
		return true;
	auto now = std::chrono::steady_clock::now();
	if (!pendingInput && lastFrameTime != time_point<steady_clock>())
	{
		msPerFrame[msPerFrameIndex++] = duration_cast<milliseconds>(now - lastFrameTime).count();
		if (msPerFrameIndex >= (int)msPerFrame.size())
			msPerFrameIndex = 0;
		msPerFrameAvg = std::accumulate(msPerFrame.begin(), msPerFrame.end(), 0) / msPerFrame.size();
	}
	lastFrameTime = now;

	std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (replayInput)
	{
		if (replayFinished)
			return false;
		++traceFrame;
		traceBoundary(traceFrame, guestRamChecksum());
		if ((replayPolls != 0) != (replayInputs.polls != 0))
			fprintf(frameTrace.get(), "P %d expected_poll=%d actual=%d\n",
					traceFrame - 1, replayInputs.polls, replayPolls);
		if (!readReplayFrame())
		{
			replayFinished = true;
			fflush(frameTrace.get());
			NOTICE_LOG(NETWORK, "GGPO offline replay completed %d frames", traceFrame);
			return false;
		}
		emu.getSh4Executor()->Start();
		return true;
	}
#endif
	if (ggpoSession == nullptr)
		return false;
	// will call save_game_state
	// Shared-thread rendering may return to the UI with this frame's input
	// still pending. Poll again on the next pass without advancing twice.
	GGPOErrorCode error = pendingInput ? GGPO_OK : ggpo_advance_frame(ggpoSession);

	// may rollback
	if (error == GGPO_OK)
		error = ggpo_idle(ggpoSession, 0);
	if (error != GGPO_OK)
	{
		stopSession();
		if (error == GGPO_ERRORCODE_INPUT_SIZE_DIFF)
			throw FlycastException("GGPO analog settings are different from peer");
		else
			throw FlycastException("GGPO error");
	}

	// Snapshot N is the state before input N. Poll has completed any replay
	// before publishing the latest confirmed input frame.
	const int confirmedSnapshot = std::min(lastSavedFrame, ggpo_get_last_confirmed_frame(ggpoSession) + 1);
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (replayOutput)
		confirmTracedFrames(confirmedSnapshot);
#endif
	for (auto remote = remoteChecksums.begin(); remote != remoteChecksums.end();)
	{
		if (remote->first > confirmedSnapshot)
		{
			++remote;
			continue;
		}
		const auto local = localChecksums.find(remote->first);
		if (local != localChecksums.end())
			compareChecksums(remote->first, local->second, remote->second);
		remote = remoteChecksums.erase(remote);
	}
	const int frame = confirmedSnapshot / CHECKSUM_INTERVAL * CHECKSUM_INTERVAL;
	if (frame > lastChecksumFrameSent)
	{
		auto local = localChecksums.find(frame);
		if (local != localChecksums.end())
		{
			GameEvent event;
			event.type = GameEvent::StateSync;
			event.u.sync.frame = frame;
			event.u.sync.checksum = local->second.full;
			event.u.sync.ramChecksum = local->second.ram;
			if (ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.sync), true) == GGPO_OK)
				lastChecksumFrameSent = frame;
		}
	}

	// Relative mouse samples are movement deltas. Repeating a prior sample to
	// bridge an increase would inject extra movement.
	const bool adaptDelay = startupProbeActive && !mouseGame;
	bool retriedInput = false;
	// may call save_game_state
	do {
		if (!readyAfterReconnect())
		{
			if (sessionStopRequested)
				return false;
			reconnectInputPending = true;
			return true;
		}
		if (!config::ThreadedRendering)
			os_UpdateInputState();
		Inputs inputs{};
		inputs.kcode = ~kcode[0];
		if (rt[0] >= 0x4000)
			inputs.kcode |= BTN_TRIGGER_RIGHT;
		else
			inputs.kcode &= ~BTN_TRIGGER_RIGHT;
		if (lt[0] >= 0x4000)
			inputs.kcode |= BTN_TRIGGER_LEFT;
		else
			inputs.kcode &= ~BTN_TRIGGER_LEFT;
		inputs.mouseButtons = 0;
		inputs.kbModifiers = 0;
		if (analogAxes > 0)
		{
			inputs.u.analog.x = joyx[0] >> 8;
			if (analogAxes >= 2)
				inputs.u.analog.y = joyy[0] >> 8;
		}
		else if (absPointerPos)
		{
			inputs.u.absPos.x = mo_x_abs[0];
			inputs.u.absPos.y = mo_y_abs[0];
		}
		else if (keyboardGame)
		{
			inputs.kbModifiers = kb_shift[0];
			memcpy(inputs.u.keys, kb_key[0], sizeof(kb_key[0]));
		}
		else if (mouseGame)
		{
			std::lock_guard<std::mutex> lock(relPosMutex);
			inputs.mouseButtons = ~mo_buttons[0];
			inputs.u.relPos.x = std::round(mo_x_delta[0]);
			inputs.u.relPos.y = std::round(mo_y_delta[0]);
			inputs.u.relPos.wheel = std::round(mo_wheel_delta[0]);
			mo_x_delta[0] -= inputs.u.relPos.x;
			mo_y_delta[0] -= inputs.u.relPos.y;
			mo_wheel_delta[0] -= inputs.u.relPos.wheel;
		}
		int delayChange = 0;
		if (adaptDelay && !adaptiveDelay.interrupted)
		{
			const auto sampleTime = steady_clock::now();
			GGPONetworkStats stats{};
			if (ggpo_get_network_stats(ggpoSession, remotePlayer, &stats) == GGPO_OK)
			{
				sampleAdaptiveDelay(sampleTime, stats.sync.predicted_frames, {}, false);
				const int pending = adaptiveDelay.pendingChange;
				// Reduce only at a forward input boundary with a duplicate sample.
				// GGPO skips that duplicate to close the one-frame gap.
				if (pending != 0 && adaptiveDelay.hasPreviousInput
						&& sampleTime - adaptiveDelay.lastFeedback <= 2 * AdaptiveDelayWindow::SamplePeriod
						&& stats.sync.predicted_frames < GGPO_MAX_PREDICTION_FRAMES
						&& (pending > 0 || memcmp(&inputs, &adaptiveDelay.previousInput, inputSize) == 0)
						&& ggpo_set_frame_delay(ggpoSession, localPlayer, sessionFrameDelay.load() + pending) == GGPO_OK)
					delayChange = pending;
			}
		}
		error = ggpo_add_local_input(ggpoSession, localPlayer, &inputs, inputSize);
		if (error == GGPO_OK)
		{
			reconnectInputPending = false;
			if (adaptDelay)
			{
				adaptiveDelay.previousInput = inputs;
				adaptiveDelay.hasPreviousInput = true;
				if (delayChange != 0)
				{
					const int nextDelay = sessionFrameDelay.load() + delayChange;
					sessionFrameDelay = nextDelay;
					adaptiveDelay.policy.applied(steady_clock::now(), delayChange, adaptiveDelay.peerStallMs);
					adaptiveDelay.pendingChange = 0;
					NOTICE_LOG(NETWORK, "GGPO adaptive delay: local delay %d -> %d frames (peer waited %d ms in 5 s)",
							nextDelay - delayChange, nextDelay, adaptiveDelay.peerStallMs);
				}
			}
			break;
		}
		if (error != GGPO_ERRORCODE_PREDICTION_THRESHOLD)
		{
			WARN_LOG(NETWORK, "ggpo_add_local_input failed %d", error);
			stopSession();
			throw FlycastException("GGPO error");
		}
		DEBUG_LOG(NETWORK, "ggpo_add_local_input prediction barrier reached");
		const auto beforeWait = adaptDelay ? steady_clock::now() : time_point<steady_clock>{};
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		const auto afterWait = adaptDelay ? steady_clock::now() : time_point<steady_clock>{};
		error = ggpo_idle(ggpoSession, 0);
		if (error != GGPO_OK)
		{
			stopSession();
			throw FlycastException("GGPO error");
		}
		if (adaptDelay && active())
		{
			// Measure the time spent waiting beyond the prediction limit.
			// Polling can run rollback work, so exclude it and cap sleep overshoot.
			const auto waited = std::min(afterWait - beforeWait, duration_cast<steady_clock::duration>(milliseconds(25)));
			sampleAdaptiveDelay(steady_clock::now(), GGPO_MAX_PREDICTION_FRAMES, waited, !retriedInput);
			retriedInput = true;
		}
	} while (active());
#ifdef SYNC_TEST
	u32 input = ~kcode[1 - localPlayerNum];
	GGPOErrorCode result = ggpo_add_local_input(ggpoSession, remotePlayer, &input, sizeof(input));
	if (result != GGPO_OK)
		WARN_LOG(NETWORK, "ggpo_add_local_input(2) failed %d", result);
#endif
	if (active()) {
		emu.getSh4Executor()->Start();
		return true;
	}
	return false;
}

bool active()
{
#ifdef HOLLYCAST_GGPO_FRAME_TRACE
	if (replayInput)
		return !replayFinished;
#endif
	return ggpoSession != nullptr;
}

std::future<bool> startNetwork()
{
	synchronized = false;
	return std::async(std::launch::async, []{
		{
			ThreadName _("GGPO-start");
			std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
#ifdef SYNC_TEST
			startSession(0, 0);
#else
			if (config::EnableUPnP)
			{
				miniupnp.Init();
				miniupnp.AddPortMapping(SERVER_PORT, false);
			}

			try {
				// The Dreamcast role must match the save ownership chosen at load.
				const bool host = settings.platform.isConsole() ? !emu.isGgpoGuestGame() : config::ActAsServer.get();
				if (host)
					startSession(SERVER_PORT, 0);
				else
					// Use SERVER_PORT-1 as local port if connecting to ourselves
					startSession(config::NetworkServer.get().empty() || config::NetworkServer.get() == "127.0.0.1" ? SERVER_PORT - 1 : SERVER_PORT, 1);
			} catch (...) {
				miniupnp.Term();
				throw;
			}
#endif
		}
		while (!synchronized && active())
		{
			{
				std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
				if (ggpoSession == nullptr)
					break;
				GGPOErrorCode result = ggpo_idle(ggpoSession, 0);
				if (result == GGPO_ERRORCODE_VERIFICATION_ERROR)
				{
					WARN_LOG(NETWORK, "Peer verification failed. Local profile: input %d bytes, %d analog axes, pointer %d keyboard %d mouse %d", inputSize, analogAxes, (int)absPointerPos, (int)keyboardGame, (int)mouseGame);
					throw FlycastException("Peer verification failed: incompatible game, saved state, controller or input settings");
				}
				else if (result != GGPO_OK)
				{
					WARN_LOG(NETWORK, "ggpo_idle failed %d", result);
					throw FlycastException("GGPO error");
				}
			}
			if (!synchronized)
				std::this_thread::sleep_for(std::chrono::milliseconds(startupProbeActive ? 2 : 20));
		}
		// Rejoins reuse the live session and never fetch another starting image.
		sessionStateServeStop = true;
#ifndef SYNC_TEST
		{
			std::lock_guard<std::recursive_mutex> lock(ggpoMutex);
			GGPONetworkStats stats{};
			if (synchronized && startupProbeActive && ggpoSession != nullptr
					&& ggpo_get_network_stats(ggpoSession, remotePlayer, &stats) == GGPO_OK
					&& stats.network.auto_frame_delay && stats.network.startup_rtt_ms >= 0)
			{
				const int rtt = stats.network.startup_rtt_ms;
				// Estimate one-way latency, rounded to a 60 Hz frame and capped at eight frames.
				const int delay = std::min((std::min(rtt, 1000) * 60 + 1000) / 2000, AdaptiveDelayPolicy::MaxDelay);
				if (ggpo_set_frame_delay(ggpoSession, localPlayer, delay) == GGPO_OK)
				{
					sessionFrameDelay = delay;
					NOTICE_LOG(NETWORK, "GGPO connection test: RTT %d ms, local delay %d frames", rtt, delay);
				}
			}
		}
#endif
#ifdef SYNC_TEST
		// save initial state (frame 0)
		if (active())
		{
			MapleInputState state[4];
			getInput(state);
		}
#endif
		if (active() && (settings.content.gameId == "VIRTUA FIGHTER 4 JAPAN"
				|| settings.content.gameId == "VF4 EVOLUTION JAPAN"
				|| settings.content.gameId == "VF4 FINAL TUNED JAPAN"))
		{
			// Send the local P1 card
			const u8 *cardData = getRfidCardData(0);
			if (cardData != nullptr)
			{
				GameEvent event;
				event.type = GameEvent::VF4Card;
				event.u.card.playerNum = config::ActAsServer ? 0 : 1;
				memcpy(event.u.card.data, cardData, sizeof(event.u.card.data));
				ggpo_send_message(ggpoSession, &event, sizeof(event.type) + sizeof(event.u.card), true);
			}
		}
		emu.setNetworkState(active());
		return active();
	});
}

void displayStats()
{
	if (ggpoSession == nullptr || reconnectDeadlineMs.load() != 0)
		return;
	GGPONetworkStats stats{};
	if (ggpo_get_network_stats(ggpoSession, remotePlayer, &stats) != GGPO_OK)
		return;

	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(10, 10));
	ImGui::SetNextWindowSize(ScaledVec2(95, 0));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin("##ggpostats", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs);
	ImguiStyleColor _2(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));

	// Send Queue
	ImGui::Text("Send Q");
	ImGui::ProgressBar(stats.network.send_queue_len / 10.f, ImVec2(-1, uiScaled(10.f)), "");

	// Frame Delay
	ImGui::Text("Delay");
	std::string delay = std::to_string(sessionFrameDelay.load());
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(delay.c_str()).x);
	ImGui::Text("%s", delay.c_str());

	// Ping
	ImGui::Text("Ping");
	std::string ping = std::to_string(stats.network.ping);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ping.c_str()).x);
	ImGui::Text("%s", ping.c_str());

	// Predicted Frames
	if (stats.sync.predicted_frames >= 7)
		// red
	    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(1, 0, 0, 1));
	else if (stats.sync.predicted_frames >= 5)
		// yellow
	    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(.9f, .9f, .1f, 1));
	ImGui::Text("Predicted");
	ImGui::ProgressBar(stats.sync.predicted_frames / 7.f, ImVec2(-1, uiScaled(10.f)), "");
	if (stats.sync.predicted_frames >= 5)
		ImGui::PopStyleColor();

	// Frames behind
	int timesync = timesyncOccurred;
	if (timesync > 0)
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
	ImGui::Text("Behind");
	ImGui::ProgressBar(0.5f + stats.timesync.local_frames_behind / 16.f, ImVec2(-1, uiScaled(10.f)), "");
	if (timesync > 0)
	{
		ImGui::PopStyleColor();
		timesyncOccurred--;
	}

	ImGui::End();
}

void endOfFrame()
{
	if (active())
	{
		_endOfFrame = true;
		emu.getSh4Executor()->Stop();
	}
}

void sendChatMessage(int playerNum, const std::string& msg) {
	if (ggpoSession == nullptr)
		return;
	GameEvent event;
	event.type = GameEvent::Chat;
	event.u.chat.playerNum = playerNum;
	size_t msgLen = std::min(msg.length(), sizeof(event.u.chat.message));
	memcpy(event.u.chat.message, msg.c_str(), msgLen);
	ggpo_send_message(ggpoSession, &event, sizeof(GameEvent) - sizeof(event.u.chat.message) + msgLen, true);
}

void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg))
{
	chatCallback = callback;
}

}

#else // LIBRETRO

namespace ggpo
{

void stopSession() {
}

void getInput(MapleInputState inputState[4])
{
	getLocalInput(inputState);
}

bool nextFrame() {
	return true;
}

bool active() {
	return false;
}

int reconnectSecondsRemaining() {
	return -1;
}

int reconnectUnpauseSecondsRemaining() {
	return -1;
}

void cancelReconnect() {
}

bool reconnectFramePending() {
	return false;
}

bool holdSessionForLibrary() {
	return false;
}

bool sessionHeldForLibrary() {
	return false;
}

void returnToHeldSession() {
}

std::future<bool> startNetwork() {
	return std::async(std::launch::deferred, []{ return false; });
}

void displayStats() {
}

void endOfFrame() {
}

void sendChatMessage(int playerNum, const std::string& msg) {
}

void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg)) {
}

}
#endif
