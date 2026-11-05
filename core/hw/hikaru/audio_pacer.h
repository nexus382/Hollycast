// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace hikaru {

// Producer-thread-only pitch-preserving output pacing for the Hikaru backend.
// The optional clock makes active-time measurement deterministic in tests.
class AudioPacer {
public:
    using Sink = void (*)(void*, int16_t, int16_t);
    using Clock = double (*)(void*);
    AudioPacer(Sink sink, void* opaque, Clock clock = nullptr, void* clockOpaque = nullptr);
    ~AudioPacer();
    AudioPacer(const AudioPacer&) = delete;
    AudioPacer& operator=(const AudioPacer&) = delete;

    // Call once at the start of every active guest frame. A mode change clears
    // processing history and returns true so the host can discard queued PCM.
    bool beginFrame(bool enabled, unsigned queuedFrames = 2600, double totalBlockedSeconds = 0);
    void submit(int16_t left, int16_t right);
    void reset();
    double tempo() const { return currentTempo; }
    unsigned pendingFrames() const;
    unsigned bufferedInputFrames() const;
    unsigned maxPendingFrames() const { return maxPending; }
    uint64_t outputFrames() const { return emittedFrames; }
    uint64_t discardedFrames() const { return discarded; }

private:
    struct Processor;
    std::unique_ptr<Processor> processor;
    Sink sink;
    void* opaque;
    Clock clock;
    void* clockOpaque;
    std::array<float, 512> input{};
    unsigned inputFrames = 0, frameSamples = 0, fadeRemaining = 0;
    double previousTime = 0, previousBlocked = 0, smoothedGuestTime = 0, smoothedWallTime = 0, currentTempo = 1;
    bool haveTime = false, haveRate = false, active = false;
    unsigned maxPending = 0;
    uint64_t emittedFrames = 0, discarded = 0;
    void emit(int16_t left, int16_t right);
};

// Muted playback still feeds silence to the host clock. Fast-forward may
// explicitly suppress output to avoid waiting for the audio device.
inline void submitOutputSample(AudioPacer& pacer, int16_t left, int16_t right,
                               bool muted, bool fastForward, bool fastForwardAudio)
{
    if (fastForward && !fastForwardAudio)
        return;
    pacer.submit(muted ? 0 : left, muted ? 0 : right);
}

} // namespace hikaru
