// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "audio_pacer.h"
#include "SoundTouch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace hikaru {
namespace {
constexpr double Rate = 44100.0;
constexpr unsigned FadeFrames = 441;
double steadyTime(void*) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

struct AudioPacer::Processor { soundtouch::SoundTouch touch; };

AudioPacer::AudioPacer(Sink sink, void* opaque, Clock clock, void* clockOpaque)
    : processor(std::make_unique<Processor>()), sink(sink), opaque(opaque),
      clock(clock ? clock : steadyTime), clockOpaque(clockOpaque)
{
    auto& touch = processor->touch;
    touch.setSampleRate(44100);
    touch.setChannels(2);
    touch.setRate(1);
    touch.setPitch(1);
    touch.setTempo(1);
    touch.setSetting(SETTING_SEQUENCE_MS, 40);
    touch.setSetting(SETTING_SEEKWINDOW_MS, 12);
    touch.setSetting(SETTING_OVERLAP_MS, 8);
    touch.setSetting(SETTING_USE_QUICKSEEK, 1);
}

AudioPacer::~AudioPacer() = default;

void AudioPacer::reset()
{
    processor->touch.clear();
    inputFrames = frameSamples = 0;
    fadeRemaining = FadeFrames;
    previousTime = previousBlocked = 0;
    smoothedGuestTime = smoothedWallTime = 0;
    currentTempo = 1;
    haveTime = haveRate = false;
    processor->touch.setTempo(1);
}

bool AudioPacer::beginFrame(bool enabled, unsigned queuedFrames, double totalBlockedSeconds)
{
    bool changed = enabled != active;
    if (changed) { reset(); active = enabled; }
    const double now = clock(clockOpaque);
    if (active && haveTime) {
        const double elapsed = now - previousTime - std::max(0.0, totalBlockedSeconds - previousBlocked);
        if (elapsed > 0.5 || elapsed < 0) {
            reset(); // A prolonged stall must not become queued delayed audio.
            changed = true;
        } else if (elapsed > 0 && frameSamples) {
            // Smooth guest and active wall time separately before dividing.
            // Clamping each frame's instantaneous speed biases variable frame
            // times downward even when their average is exactly real-time.
            const double decay = haveRate ? std::exp(-elapsed / 0.5) : 0.0;
            smoothedGuestTime = decay * smoothedGuestTime + frameSamples / Rate;
            smoothedWallTime = decay * smoothedWallTime + elapsed;
            const double smoothedSpeed = smoothedGuestTime / smoothedWallTime;
            // Aim for ~59 ms, within the default 64 ms backend buffer. Below
            // one SDL callback of queued audio, recover more promptly; the
            // correction is bounded and does not alter the emulated clock.
            double correction = 0;
            if (queuedFrames != std::numeric_limits<unsigned>::max()) {
                correction = 0.03 * std::clamp((2600.0 - queuedFrames) / 2600.0, -1.0, 1.0);
                if (queuedFrames < 1024)
                    correction += (0.08 - 0.03) * (1024.0 - queuedFrames) / 1024.0;
            }
            currentTempo = std::clamp(smoothedSpeed < 0.98 ? smoothedSpeed * (1.0 - correction) : 1.0, 0.25, 1.0);
            haveRate = true;
            processor->touch.setTempo(currentTempo);
        }
    }
    previousTime = now;
    previousBlocked = totalBlockedSeconds;
    frameSamples = 0;
    haveTime = true;
    return changed;
}

void AudioPacer::emit(int16_t left, int16_t right)
{
    if (!sink) return;
    if (fadeRemaining) {
        const float gain = float(FadeFrames - fadeRemaining) / FadeFrames;
        left = int16_t(left * gain); right = int16_t(right * gain);
        --fadeRemaining;
    }
    sink(opaque, left, right);
    ++emittedFrames;
}

void AudioPacer::submit(int16_t left, int16_t right)
{
    ++frameSamples;
    if (!active) { emit(left, right); return; }
    input[2 * inputFrames] = left / 32768.0f;
    input[2 * inputFrames + 1] = right / 32768.0f;
    if (++inputFrames != input.size() / 2) return;
    processor->touch.putSamples(input.data(), inputFrames);
    inputFrames = 0;
    constexpr unsigned MaxProcessedFrames = 4410; // 100 ms at 44.1 kHz
    const unsigned ready = processor->touch.numSamples();
    if (ready > MaxProcessedFrames) {
        const unsigned excess = ready - MaxProcessedFrames;
        processor->touch.receiveSamples(excess);
        discarded += excess;
    }
    maxPending = std::max(maxPending, processor->touch.numSamples());
    // Drain each input block immediately; no unbounded processed-output queue.
    std::array<float, 1024> output;
    unsigned n;
    while ((n = processor->touch.receiveSamples(output.data(), output.size() / 2)) != 0)
        for (unsigned i = 0; i < n; ++i) {
            auto convert = [](float v) { return int16_t(std::clamp(std::lround(v * 32768.0f), -32768l, 32767l)); };
            emit(convert(output[2 * i]), convert(output[2 * i + 1]));
        }
}

unsigned AudioPacer::pendingFrames() const
{
    return inputFrames + processor->touch.numSamples();
}

unsigned AudioPacer::bufferedInputFrames() const
{
    return inputFrames + processor->touch.numUnprocessedSamples();
}

} // namespace hikaru
