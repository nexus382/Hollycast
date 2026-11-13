// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// The phone is a browser client; hosting is available in standalone desktop builds.
#if !defined(LIBRETRO) && !defined(TARGET_UWP) && !defined(__ANDROID__) \
	&& !defined(TARGET_IPHONE) && !defined(__SWITCH__) \
	&& (defined(_WIN32) || defined(__linux__) || defined(__APPLE__))
#define HAVE_PHONE_VMU
#endif

namespace phone_vmu
{
#ifdef HAVE_PHONE_VMU
struct Status
{
	bool running = false;
	bool connected = false;
	std::vector<std::string> urls;
	std::string error;
};

// Lifecycle and status are owned by the UI thread. Sharing is opt-in per session.
void start();
void stop();
Status status();

// Called by the emulation thread even while sharing is off, so pairing mid-game
// immediately displays the latest LCD. Never performs network or UI work.
void publishScreen(int index, const std::uint32_t *pixels, bool active);
// Independent copies for browser mini-games; callers retain ownership.
void publishCard(int index, const std::uint8_t *data, std::size_t size);
void updateCard(int index, std::size_t offset, const std::uint8_t *data, std::size_t size);
#endif
}
