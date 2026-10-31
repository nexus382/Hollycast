// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "types.h"

#include <cstddef>
#include <string>
#include <vector>

// Decodes 44.1 kHz 16-bit PCM mono or stereo WAV data into interleaved stereo.
// Returns false for any other format, or for damaged data.
bool decodeUiSound(const u8* data, size_t size, std::vector<s16>& stereo);

// Holds sounds still to be played and releases them at the rate they play, because a
// backend's buffer can be shorter than a sound. Sounds added while others play are mixed in.
class UiSoundQueue
{
public:
	void add(const std::vector<s16>& stereo);
	// Copies the frames due by this many seconds after the queue became active, at most
	// maxFrames. Returns the number of frames copied, or 0 when nothing is due.
	u32 take(double elapsedSeconds, s16* stereo, u32 maxFrames);
	bool empty() const { return position >= pending.size(); }

private:
	std::vector<s16> pending;
	size_t position = 0;
	u64 framesTaken = 0;
};

// Plays a validated sound through the audio backend. Does nothing while a game is running.
void playUiSound(const std::string& path);
// Call once per frame: closes the audio device after a few idle seconds, so that an
// open device does not keep the machine awake.
void tickUiSound();
// Drops anything still playing and closes the audio device. Call before a game starts.
void cancelUiSound();
