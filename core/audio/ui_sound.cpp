// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui_sound.h"
#include "audiostream.h"
#include "oslib/oslib.h"
#include "stdclass.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>

namespace
{
constexpr u32 SampleRate = 44100;
using Clock = std::chrono::steady_clock;
constexpr auto IdleTimeout = std::chrono::seconds(3);
constexpr u32 LeadFrames = 2048; // Sent ahead of playback, to ride out a slow frame.
constexpr u32 FlushFrames = 1024; // Trailing silence: a backend can hold back a partial device buffer.
Clock::time_point lastPlayed, queueStart;
bool playedSinceIdle;
UiSoundQueue queue;

u32 readLe(const u8* data, int bytes)
{
	u32 value = 0;
	for (int i = 0; i < bytes; ++i)
		value |= static_cast<u32>(data[i]) << (i * 8);
	return value;
}
}

bool decodeUiSound(const u8* data, size_t size, std::vector<s16>& stereo)
{
	stereo.clear();
	if (size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
		return false;
	u32 channels = 0;
	for (size_t pos = 12; pos + 8 <= size;)
	{
		const u32 chunkSize = readLe(data + pos + 4, 4);
		const size_t body = pos + 8;
		if (chunkSize > size - body)
			return false;
		if (std::memcmp(data + pos, "fmt ", 4) == 0)
		{
			if (chunkSize < 16 || readLe(data + body, 2) != 1 || readLe(data + body + 4, 4) != SampleRate
					|| readLe(data + body + 14, 2) != 16)
				return false;
			channels = readLe(data + body + 2, 2);
			if (channels != 1 && channels != 2)
				return false;
		}
		else if (std::memcmp(data + pos, "data", 4) == 0)
		{
			if (channels == 0 || chunkSize == 0)
				return false;
			const size_t frames = chunkSize / (channels * 2);
			stereo.reserve(frames * 2);
			for (size_t frame = 0; frame < frames; ++frame)
			{
				const u8* sample = data + body + frame * channels * 2;
				const s16 left = static_cast<s16>(readLe(sample, 2));
				stereo.push_back(left);
				stereo.push_back(channels == 2 ? static_cast<s16>(readLe(sample + 2, 2)) : left);
			}
			return !stereo.empty();
		}
		pos = body + chunkSize + (chunkSize & 1);
	}
	return false;
}

void UiSoundQueue::add(const std::vector<s16>& stereo)
{
	if (empty())
	{
		pending.clear();
		position = 0;
		framesTaken = 0;
	}
	const size_t needed = position + stereo.size() + FlushFrames * 2;
	if (pending.size() < needed)
		pending.resize(needed);
	for (size_t i = 0; i < stereo.size(); ++i)
	{
		const int mixed = pending[position + i] + stereo[i];
		pending[position + i] = static_cast<s16>(std::clamp(mixed, -32768, 32767));
	}
}

u32 UiSoundQueue::take(double elapsedSeconds, s16* stereo, u32 maxFrames)
{
	if (empty())
		return 0;
	const u64 due = static_cast<u64>(elapsedSeconds * SampleRate) + LeadFrames;
	if (framesTaken >= due)
		return 0;
	const u32 frames = static_cast<u32>(std::min<size_t>(maxFrames, (pending.size() - position) / 2));
	std::copy_n(pending.data() + position, frames * 2, stereo);
	position += frames * 2;
	framesTaken += frames;
	return frames;
}

static void pumpUiSound()
{
	static s16 chunk[SAMPLE_COUNT * 2];
	const double elapsed = std::chrono::duration<double>(Clock::now() - queueStart).count();
	while (const u32 frames = queue.take(elapsed, chunk, SAMPLE_COUNT))
		PushUiAudio(chunk, frames);
}

void playUiSound(const std::string& path)
{
	static std::map<std::string, std::vector<s16>> sounds;
	auto sound = sounds.find(path);
	if (sound == sounds.end())
	{
		std::vector<s16> decoded;
		std::ifstream file(path, std::ios::binary);
		const std::vector<u8> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		if (!decodeUiSound(bytes.data(), bytes.size(), decoded))
			WARN_LOG(AUDIO, "Cannot play interface sound %s", path.c_str());
		sound = sounds.emplace(path, std::move(decoded)).first;
	}
	if (sound->second.empty())
		return;
	if (queue.empty())
		queueStart = Clock::now();
	queue.add(sound->second);
	pumpUiSound();
	lastPlayed = Clock::now();
	playedSinceIdle = true;
}

void tickUiSound()
{
	if (!queue.empty())
	{
		pumpUiSound();
		lastPlayed = Clock::now();
	}
	else if (playedSinceIdle && Clock::now() - lastPlayed > IdleTimeout)
	{
		playedSinceIdle = false;
		StopUiAudio();
	}
}

void cancelUiSound()
{
	queue = UiSoundQueue();
	playedSinceIdle = false;
	StopUiAudio();
}
