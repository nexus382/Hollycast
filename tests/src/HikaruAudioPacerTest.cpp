// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/audio_pacer.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {
constexpr double Pi = 3.14159265358979323846;
struct Harness {
    double now = 1, blockedPerPush = 0, blocked = 0;
    unsigned calls = 0;
    std::vector<std::array<int16_t, 2>> output;
    static double clock(void* p) { return static_cast<Harness*>(p)->now; }
    static void sink(void* p, int16_t l, int16_t r) {
        auto& h = *static_cast<Harness*>(p);
        h.output.push_back({l, r});
        if (++h.calls % 512 == 0) { h.now += h.blockedPerPush; h.blocked += h.blockedPerPush; }
    }
};

TEST(HikaruAudioPacerTest, SlowsTempoWhileKeepingPitchAndStereo)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    unsigned inputIndex = 0;
    for (int frame = 0; frame < 180; ++frame) {
        pacer.beginFrame(true, 2205, h.blocked);
        for (unsigned i = 0; i < 735; ++i, ++inputIndex) {
            const auto sample = int16_t(12000 * std::sin(2 * Pi * 440 * inputIndex / 44100.0));
            pacer.submit(sample, -sample);
        }
        EXPECT_LE(pacer.pendingFrames(), 4410u);
        h.now += 1.0 / 36.0; // 60 emulated frames in 100 real frames
    }
    EXPECT_NEAR(pacer.tempo(), 0.6, 0.01);
    EXPECT_EQ(pacer.discardedFrames(), 0u);
    EXPECT_LE(pacer.maxPendingFrames(), 4410u);
    // 3 seconds of input should yield about 5 seconds of stretched output,
    // excluding the algorithm's startup/ending window.
    EXPECT_GT(h.output.size(), 210000u);
    EXPECT_LT(h.output.size(), 224000u);
    unsigned crossings = 0;
    int stereoError = 0;
    for (size_t i = 22051; i < h.output.size(); ++i) {
        if (h.output[i - 1][0] <= 0 && h.output[i][0] > 0) ++crossings;
        stereoError = std::max(stereoError, std::abs(int(h.output[i][0]) + int(h.output[i][1])));
    }
    EXPECT_LE(stereoError, 2);
    const double seconds = double(h.output.size() - 22051) / 44100.0;
    EXPECT_NEAR(crossings / seconds, 440, 8);
}

TEST(HikaruAudioPacerTest, ExcludesBackendBlockingFromSpeedEstimate)
{
    Harness h;
    h.blockedPerPush = 0.01;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 100; ++frame) {
        pacer.beginFrame(true, 2205, h.blocked);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(3000, -3000);
        h.now += 1.0 / 60.0;
    }
    EXPECT_GT(pacer.tempo(), 0.98);
}

TEST(HikaruAudioPacerTest, HandlesFortyPercentSpeedAndRecoversLowQueue)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 100; ++frame) {
        pacer.beginFrame(true, 2205);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(5000, -5000);
        h.now += 1.0 / 24.0;
    }
    EXPECT_NEAR(pacer.tempo(), 0.4, 0.01);
    EXPECT_GT(h.output.size(), 170000u);
    EXPECT_LE(pacer.maxPendingFrames(), 4410u);
    EXPECT_EQ(pacer.discardedFrames(), 0u);
    for (int frame = 0; frame < 100; ++frame) {
        pacer.beginFrame(true, 0);
        if (frame == 0) EXPECT_NEAR(pacer.tempo(), 0.368, 0.01);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(5000, -5000);
        h.now += 1.0 / 24.0;
    }
    EXPECT_NEAR(pacer.tempo(), 0.368, 0.01);
    for (int frame = 0; frame < 100; ++frame) {
        pacer.beginFrame(true, 4410);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(5000, -5000);
        h.now += 1.0 / 24.0;
    }
    EXPECT_NEAR(pacer.tempo(), 0.412, 0.01);
}

TEST(HikaruAudioPacerTest, ResetDiscardsPendingAudioAndReprimes)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 15; ++frame) {
        pacer.beginFrame(true);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(10000, 10000);
        h.now += 1.0 / 36.0;
    }
    ASSERT_FALSE(h.output.empty());
    pacer.reset();
    EXPECT_EQ(pacer.pendingFrames(), 0u);
    h.output.clear();
    h.now += 5; // pause does not enter the speed estimate
    for (int frame = 0; frame < 15; ++frame) {
        pacer.beginFrame(true);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(-10000, -10000);
        h.now += 1.0 / 36.0;
    }
    ASSERT_FALSE(h.output.empty());
    EXPECT_TRUE(std::all_of(h.output.begin(), h.output.end(), [](const auto& s) { return s[0] <= 0 && s[1] <= 0; }));
    EXPECT_NEAR(pacer.tempo(), 0.6, 0.01);
    EXPECT_TRUE(pacer.beginFrame(false)); // fast-forward/setting transition
    EXPECT_EQ(pacer.pendingFrames(), 0u);
}

TEST(HikaruAudioPacerTest, LongStallClearsAndReprimes)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    pacer.beginFrame(true);
    for (unsigned i = 0; i < 1500; ++i) pacer.submit(12000, 12000);
    h.now += 1.0;
    EXPECT_TRUE(pacer.beginFrame(true));
    EXPECT_EQ(pacer.pendingFrames(), 0u);
    h.output.clear();
    for (int frame = 0; frame < 12; ++frame) {
        for (unsigned i = 0; i < 735; ++i) pacer.submit(-12000, -12000);
        h.now += 1.0 / 36.0;
        pacer.beginFrame(true);
    }
    ASSERT_FALSE(h.output.empty());
    EXPECT_TRUE(std::all_of(h.output.begin(), h.output.end(), [](const auto& s) { return s[0] <= 0 && s[1] <= 0; }));
}

TEST(HikaruAudioPacerTest, LowTempoKeepsProcessedQueueBounded)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 60; ++frame) {
        pacer.beginFrame(true);
        for (unsigned i = 0; i < 735; ++i) pacer.submit(5000, 5000);
        h.now += 1.0 / 15.0;
    }
    EXPECT_NEAR(pacer.tempo(), 0.25, 0.005);
    EXPECT_LE(pacer.maxPendingFrames(), 4410u);
    EXPECT_EQ(pacer.discardedFrames(), 0u);
    EXPECT_GT(h.output.size(), 160000u);
}
}

TEST(HikaruAudioPacerTest, MutedOutputKeepsClockAndFastForwardCanSuppressIt)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    pacer.beginFrame(false);
    for (int i = 0; i < 1024; ++i)
        hikaru::submitOutputSample(pacer, 12000, -8000, true, false, false);
    ASSERT_EQ(h.output.size(), 1024u);
    EXPECT_TRUE(std::all_of(h.output.begin(), h.output.end(), [](const auto& frame) {
        return frame[0] == 0 && frame[1] == 0;
    }));
    hikaru::submitOutputSample(pacer, 12000, -8000, true, true, false);
    EXPECT_EQ(h.output.size(), 1024u);
    hikaru::submitOutputSample(pacer, 12000, -8000, true, true, true);
    ASSERT_EQ(h.output.size(), 1025u);
    EXPECT_EQ(h.output.back()[0], 0);
    EXPECT_EQ(h.output.back()[1], 0);
    hikaru::submitOutputSample(pacer, 12000, -8000, false, false, false);
    ASSERT_EQ(h.output.size(), 1026u);
    EXPECT_EQ(h.output.back()[0], 12000);
    EXPECT_EQ(h.output.back()[1], -8000);
}

TEST(HikaruAudioPacerTest, AlternatingFrameTimesEstimateAverageSpeed)
{
    Harness h;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 240; ++frame) {
        pacer.beginFrame(true, 2600, h.blocked);
        for (int i = 0; i < 735; ++i) pacer.submit(1000, -1000);
        h.now += frame % 2 ? (1.0 / 30.0 - 0.014) : 0.014;
    }
    EXPECT_GT(pacer.tempo(), 0.985);
}

TEST(HikaruAudioPacerTest, VariableFramesPreserveSlowMeanAndExcludeBackendWait)
{
    Harness h;
    h.blockedPerPush = 0.004;
    hikaru::AudioPacer pacer(Harness::sink, &h, Harness::clock, &h);
    for (int frame = 0; frame < 240; ++frame) {
        pacer.beginFrame(true, 2600, h.blocked);
        for (int i = 0; i < 735; ++i) pacer.submit(1000, -1000);
        h.now += frame % 2 ? (0.0466666666666667 - 0.020) : 0.020;
    }
    EXPECT_NEAR(pacer.tempo(), 5.0 / 7.0, 0.01);
    EXPECT_GT(h.blocked, 0.0);
}

#ifdef USE_SDL_AUDIO
#include "audio/sdl_partial.h"
#include <SDL.h>

TEST(HikaruSdlPartialTest, NativeUnderflowConsumesAvailableFramesAndPreservesOrder)
{
    audio::SdlPartialOutput output;
    std::vector<uint32_t> queue(2000);
    for (unsigned i = 0; i < 1000; ++i) queue[i] = i + 1;
    unsigned queued = 1000;
    std::vector<uint32_t> dest(1026, 0xdeadbeef);
    auto unused = [](const uint32_t*, unsigned, uint8_t*, size_t) { return -1; };
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 1024,
                            nullptr, 0, 0, 1.0, false, unused), 24u);
    EXPECT_EQ(queued, 0u);
    for (unsigned i = 0; i < 1000; ++i) EXPECT_EQ(dest[i], i + 1);
    for (unsigned i = 1000; i < 1024; ++i) EXPECT_EQ(dest[i], 0u);
    EXPECT_EQ(dest[1024], 0xdeadbeefu);
    // A producer can now append a full block without retaining the old PCM.
    for (unsigned i = 0; i < 512; ++i) queue[queued++] = 1001 + i;
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 512,
                            nullptr, 0, 0, 1.0, false, unused), 0u);
    EXPECT_EQ(queued, 0u);
    for (unsigned i = 0; i < 512; ++i) EXPECT_EQ(dest[i], 1001 + i);
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 1024,
                            nullptr, 0, 0, 1.0, false, unused), 1024u);
    queued = 1536;
    for (unsigned i = 0; i < queued; ++i) queue[i] = i + 1;
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 1024,
                            nullptr, 0, 0, 1.0, false, unused), 0u);
    EXPECT_EQ(queued, 512u);
    EXPECT_EQ(queue[0], 1025u);
    EXPECT_EQ(queue[511], 1536u);
}

TEST(HikaruSdlPartialTest, RealConverterRetainsExcessAndCountsOnlyMissingTail)
{
    SDL_AudioCVT cvt{};
    ASSERT_EQ(SDL_BuildAudioCVT(&cvt, AUDIO_S16, 2, 44100, AUDIO_S16, 2, 48000), 1);
    const size_t capacity = 1024 * sizeof(uint32_t) * cvt.len_mult;
    std::vector<uint8_t> scratch(capacity + 16, 0xa5);
    cvt.buf = scratch.data();
    std::vector<uint32_t> queue(1024, 0x23451234);
    std::vector<uint8_t> reference(941 * sizeof(uint32_t) * cvt.len_mult);
    SDL_AudioCVT ref = cvt; ref.buf = reference.data(); ref.len = 941 * sizeof(uint32_t);
    std::memcpy(ref.buf, queue.data(), ref.len);
    ASSERT_EQ(SDL_ConvertAudio(&ref), 0);
    ASSERT_GT(ref.len_cvt, 0);
    ASSERT_EQ(ref.len_cvt % sizeof(uint32_t), 0);
    const unsigned converted = unsigned(ref.len_cvt / sizeof(uint32_t));
    const unsigned emitted = std::min(1024u, converted);
    const unsigned carried = converted - emitted;
    auto convert = [&cvt](const uint32_t* src, unsigned frames, uint8_t* dst, size_t cap) {
        const size_t bytes = size_t(frames) * sizeof(uint32_t);
        if (bytes * cvt.len_mult > cap) return -1;
        std::memcpy(dst, src, bytes); cvt.len = int(bytes);
        return SDL_ConvertAudio(&cvt) == 0 ? cvt.len_cvt : -1;
    };
    audio::SdlPartialOutput output;
    std::vector<uint8_t> dest(1024 * sizeof(uint32_t) + 16, 0xa5);
    unsigned queued = 941;
    EXPECT_EQ(output.render(queue.data(), queued, dest.data(), 1024, scratch.data(), capacity,
                            1024, cvt.len_ratio, true, convert), 1024u - emitted);
    EXPECT_EQ(queued, 0u);
    EXPECT_EQ(output.pendingBytes(), carried * sizeof(uint32_t));
    EXPECT_TRUE(std::equal(dest.begin(), dest.begin() + emitted * sizeof(uint32_t), reference.begin()));
    EXPECT_TRUE(std::all_of(dest.begin() + emitted * sizeof(uint32_t), dest.begin() + 1024 * sizeof(uint32_t), [](uint8_t v) { return v == 0; }));
    EXPECT_TRUE(std::all_of(dest.end() - 16, dest.end(), [](uint8_t v) { return v == 0xa5; }));
    EXPECT_TRUE(std::all_of(scratch.end() - 16, scratch.end(), [](uint8_t v) { return v == 0xa5; }));
    EXPECT_EQ(output.render(queue.data(), queued, dest.data(), 1024, scratch.data(), capacity,
                            1024, cvt.len_ratio, true, convert), 1024u - carried);
    EXPECT_TRUE(std::equal(dest.begin(), dest.begin() + carried * sizeof(uint32_t), reference.begin() + emitted * sizeof(uint32_t)));
    EXPECT_TRUE(std::all_of(dest.begin() + carried * sizeof(uint32_t), dest.begin() + 1024 * sizeof(uint32_t), [](uint8_t v) { return v == 0; }));
    EXPECT_EQ(output.pendingBytes(), 0u);
    // A short SDL conversion leaves a zero tail based on actual output.
    queued = 500;
    ref.len = 500 * sizeof(uint32_t);
    std::memcpy(ref.buf, queue.data(), ref.len);
    ASSERT_EQ(SDL_ConvertAudio(&ref), 0);
    ASSERT_GT(ref.len_cvt, 0);
    ASSERT_LT(ref.len_cvt, 1024 * int(sizeof(uint32_t)));
    EXPECT_EQ(output.render(queue.data(), queued, dest.data(), 1024, scratch.data(), capacity,
                            1024, cvt.len_ratio, true, convert), 1024u - unsigned(ref.len_cvt / sizeof(uint32_t)));
    EXPECT_EQ(queued, 0u);
    EXPECT_TRUE(std::equal(dest.begin(), dest.begin() + ref.len_cvt, reference.begin()));
    EXPECT_TRUE(std::all_of(dest.begin() + ref.len_cvt, dest.begin() + 1024 * sizeof(uint32_t), [](uint8_t v) { return v == 0; }));
}

TEST(HikaruSdlPartialTest, ShortConversionAndFailureAreBounded)
{
    audio::SdlPartialOutput output;
    std::array<uint32_t, 8> queue{1, 2, 3, 4, 5, 6, 7, 8};
    std::array<uint32_t, 10> dest{};
    std::array<uint32_t, 8> scratch{};
    unsigned queued = 3;
    auto shortConvert = [](const uint32_t* src, unsigned frames, uint8_t* dst, size_t) {
        std::memcpy(dst, src, frames * sizeof(uint32_t));
        return int(frames * sizeof(uint32_t));
    };
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 8,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, shortConvert), 5u);
    EXPECT_EQ(queued, 0u);
    EXPECT_EQ(dest[0], 1u); EXPECT_EQ(dest[1], 2u); EXPECT_EQ(dest[2], 3u);
    for (unsigned i = 3; i < 8; ++i) EXPECT_EQ(dest[i], 0u);
    queued = 2; queue[0] = 10; queue[1] = 11; dest[8] = 0xdeadbeef;
    auto fail = [](const uint32_t*, unsigned, uint8_t*, size_t) { return -1; };
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 8,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, fail), 8u);
    EXPECT_EQ(queued, 2u); EXPECT_EQ(queue[0], 10u); EXPECT_EQ(queue[1], 11u);
    EXPECT_EQ(dest[8], 0xdeadbeefu);

    // Preserve converted excess across callbacks and discard it on reset.
    queued = 2; queue[0] = 21; queue[1] = 22;
    auto excess = [](const uint32_t* src, unsigned frames, uint8_t* dst, size_t) {
        std::memcpy(dst, src, frames * sizeof(uint32_t));
        const uint32_t marker = 23;
        std::memcpy(dst + frames * sizeof(uint32_t), &marker, sizeof(marker));
        return int((frames + 1) * sizeof(uint32_t));
    };
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 2,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, excess), 0u);
    ASSERT_EQ(output.pendingBytes(), sizeof(uint32_t));
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 2,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, excess), 1u);
    EXPECT_EQ(dest[0], 23u); EXPECT_EQ(dest[1], 0u);
    queued = 2; queue[0] = 21; queue[1] = 22;
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 2,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, excess), 0u);
    ASSERT_EQ(output.pendingBytes(), sizeof(uint32_t));
    output.reset();
    EXPECT_EQ(output.pendingBytes(), 0u);
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 2,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 8,
                            1.0, true, excess), 2u);

    // A callback larger than conversion storage is handled in source chunks.
    queued = 8;
    for (unsigned i = 0; i < queued; ++i) queue[i] = i + 1;
    EXPECT_EQ(output.render(queue.data(), queued, reinterpret_cast<uint8_t*>(dest.data()), 8,
                            reinterpret_cast<uint8_t*>(scratch.data()), sizeof(scratch), 3,
                            1.0, true, shortConvert), 0u);
    EXPECT_EQ(queued, 0u);
    for (unsigned i = 0; i < 8; ++i) EXPECT_EQ(dest[i], i + 1);
    EXPECT_EQ(dest[8], 0xdeadbeefu);
}
#endif
