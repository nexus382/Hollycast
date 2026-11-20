// Portions Copyright 2026 The Hollycast Authors
#include "audio_stream.h"
#ifdef HOLLYCAST_HAVE_WEBRTC
#include "audio/audiostream.h"
#include "opus_encoder.h"
#include "webrtc_peer.h"
#include "log/Log.h"
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace remoteplay {
namespace {
using Clock = std::chrono::steady_clock;
struct Chunk {
	std::array<s16, SAMPLE_COUNT * 2> pcm;
	u64 sequence;
	Clock::time_point captured;
};
// One producer, the AICA thread; one consumer, the encoder worker. Seven
// queued blocks bound latency to 81 ms even if the worker falls behind.
std::array<Chunk, 8> chunks;
std::atomic<size_t> readIndex { 0 }, writeIndex { 0 };
std::atomic<bool> running { false }, accepting { false };
std::thread worker;
std::mutex waitMutex;
std::condition_variable wake;
u64 sequence = 0; // producer only, never reset while a tap may be in flight

void capture(const void *frames, u32 count)
{
	if (!accepting || count != SAMPLE_COUNT) return;
	const u64 currentSequence = sequence++;
	size_t write = writeIndex.load(std::memory_order_relaxed);
	size_t next = (write + 1) % chunks.size();
	if (next == readIndex.load(std::memory_order_acquire)) return;
	auto& chunk = chunks[write];
	std::memcpy(chunk.pcm.data(), frames, sizeof(chunk.pcm));
	chunk.sequence = currentSequence;
	chunk.captured = Clock::now();
	writeIndex.store(next, std::memory_order_release);
	wake.notify_one();
}
bool pop(Chunk& chunk)
{
	size_t read = readIndex.load(std::memory_order_relaxed);
	if (read == writeIndex.load(std::memory_order_acquire)) return false;
	chunk = chunks[read];
	readIndex.store((read + 1) % chunks.size(), std::memory_order_release);
	return true;
}
void encodeLoop()
{
	std::unique_ptr<OpusAudioEncoder> encoder;
	const auto epoch = Clock::now();
	Clock::time_point lastCapture {};
	u64 nextSequence = 0;
	u32 timestamp = 0;
	while (running)
	{
		bool connected = rtcAnyConnected();
		accepting = connected;
		Chunk chunk;
		if (!pop(chunk)) {
			std::unique_lock<std::mutex> lock(waitMutex);
			wake.wait_for(lock, std::chrono::milliseconds(5));
			continue;
		}
		if (!connected) { encoder.reset(); continue; }
		// Skip stale queued sound. A lost block or pause also resets the
		// resampler and RTP clock, rather than playing old sound on resume.
		if (Clock::now() - chunk.captured > std::chrono::milliseconds(100)) {
			encoder.reset(); continue;
		}
		if (chunk.sequence != nextSequence || chunk.captured - lastCapture > std::chrono::milliseconds(100))
			encoder.reset();
		nextSequence = chunk.sequence + 1;
		lastCapture = chunk.captured;
		if (!encoder) {
			timestamp = (u32)(std::chrono::duration<double>(chunk.captured - epoch).count() * 48000);
			encoder = OpusAudioEncoder::create([&timestamp](const std::vector<u8>& packet) {
				rtcPushAudioSample(packet, timestamp);
				timestamp += 960;
			});
			if (!encoder) { WARN_LOG(AUDIO, "Remote Play could not initialize Opus audio"); break; }
		}
		encoder->encode(chunk.pcm.data(), SAMPLE_COUNT);
	}
	accepting = false;
}
}
void startAudioStreaming()
{
	stopAudioStreaming();
	running = true;
	worker = std::thread(encodeLoop);
	SetAudioOutputTap(capture);
}
void stopAudioStreaming()
{
	accepting = false;
	SetAudioOutputTap(nullptr);
	running = false;
	wake.notify_all();
	if (worker.joinable()) worker.join();
}
}
#else
namespace remoteplay {
void startAudioStreaming() {}
void stopAudioStreaming() {}
}
#endif
