// Portions Copyright 2026 The Hollycast Authors
#ifdef HOLLYCAST_HAVE_WEBRTC
#include "gtest/gtest.h"
#include "ui/remoteplay/opus_encoder.h"
#include "audio/audiostream.h"
#include "cfg/option.h"
#include <opus.h>
#include <array>
#include <cmath>

TEST(RemotePlayAudioTest, HostVolumeDoesNotMuteTheRemoteStereoSamples)
{
	static std::vector<s16> captured;
	captured.clear();
	const int volume = config::AudioVolume;
	struct Cleanup {
		int volume;
		~Cleanup() { SetAudioOutputTap(nullptr); config::AudioVolume.set(volume); config::AudioVolume.calcDbPower(); }
	} cleanup { volume };
	config::AudioVolume.set(0);
	config::AudioVolume.calcDbPower();
	SetAudioOutputTap([](const void *frames, u32 count) {
		captured.resize(count * 2);
		std::memcpy(captured.data(), frames, count * 2 * sizeof(s16));
	});
	for (u32 i = 0; i < SAMPLE_COUNT * 3; i++) WriteSample(-2345, 1234);
	ASSERT_EQ(captured.size(), SAMPLE_COUNT * 2);
	for (size_t frame = 0; frame < SAMPLE_COUNT; frame++) {
		EXPECT_EQ(captured[frame * 2], 1234);
		EXPECT_EQ(captured[frame * 2 + 1], -2345);
	}
}

TEST(RemotePlayAudioTest, ResamplesContinuousStereoWithoutDroppingChunkBoundaries)
{
	int error;
	OpusDecoder *decoder = opus_decoder_create(48000, 2, &error);
	ASSERT_EQ(error, OPUS_OK);
	struct Cleanup { OpusDecoder *decoder; ~Cleanup() { opus_decoder_destroy(decoder); } } cleanup { decoder };
	std::vector<s16> decoded;
	int packets = 0;
	auto encoder = remoteplay::OpusAudioEncoder::create([&](const std::vector<u8>& packet) {
		std::array<s16, 960 * 2> pcm;
		int frames = opus_decode(decoder, packet.data(), packet.size(), pcm.data(), 960, 0);
		EXPECT_EQ(frames, 960);
		if (frames > 0) decoded.insert(decoded.end(), pcm.begin(), pcm.begin() + frames * 2);
		packets++;
	});
	ASSERT_NE(encoder, nullptr);
	std::array<s16, 512 * 2> pcm;
	for (int start = 0; start < 44100; start += 512)
	{
		int count = std::min(512, 44100 - start);
		for (int i = 0; i < count; i++) {
			pcm[i * 2] = std::sin((start + i) * 440.0 * 6.2831853 / 44100) * 16000;
			pcm[i * 2 + 1] = std::sin((start + i) * 880.0 * 6.2831853 / 44100) * 8000;
		}
		encoder->encode(pcm.data(), count);
	}
	// SDL retains a short resampler tail; the live stream never flushes it.
	EXPECT_GE(packets, 48);
	EXPECT_LE(packets, 50);
	ASSERT_GT(decoded.size(), 48000u);
	for (int channel = 0; channel < 2; channel++)
	{
		int crossings = 0;
		for (size_t frame = 4800; frame < decoded.size() / 2; frame++)
			if (decoded[(frame - 1) * 2 + channel] < 0 && decoded[frame * 2 + channel] >= 0) crossings++;
		double frequency = crossings * 48000.0 / (decoded.size() / 2 - 4800);
		EXPECT_NEAR(frequency, channel == 0 ? 440 : 880, 4) << "Stereo channels must retain their sample rate and order";
	}
}
#endif
