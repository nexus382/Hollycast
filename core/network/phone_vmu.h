// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>

// The VMU page is a browser client; standalone desktop and Android builds can host it.
#if !defined(LIBRETRO) && !defined(TARGET_UWP) && !defined(TARGET_IPHONE) && !defined(__SWITCH__) \
	&& (defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__ANDROID__))
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
	// Last successful phone save-back, for the settings status line.
	int lastSyncSlot = -1;
	std::string lastSyncTime;
};

// Lifecycle and status are owned by the UI thread. Sharing is opt-in per session.
void start();
void stop();
Status status();
// Supplies the persisted pairing identity before start(). An empty token is
// generated once and retained until regeneratePairing(); port 0 asks the OS
// for an available port. The UI layer owns saving these values.
void setPairing(const std::string& token, int port);
std::string pairingToken();
// Revokes every paired phone by replacing the pairing token, then restarts
// sharing if it was running. Returns the new token for the caller to persist.
std::string regeneratePairing();

// Called by the emulation thread even while sharing is off, so pairing mid-game
// immediately displays the latest LCD. Never performs network or UI work.
void publishScreen(int index, const std::uint32_t *pixels, bool active);
// The emulation thread owns live cards. Network handlers only queue complete copies.
void publishCard(int index, const std::uint8_t *data, std::size_t size, bool writable = true);
void updateCard(int index, std::size_t offset, const std::uint8_t *data, std::size_t size);
void unpublishCard(int index, const std::uint8_t *data);
// Returns true while the phone has removed this card from the virtual controller.
// Persist must back up the original on the first write and report flushed disk writes.
bool syncCard(int index, std::uint8_t *data, std::size_t size, bool writable,
		const std::function<bool(const std::uint8_t *, std::size_t, bool)>& persist);
#endif
}
