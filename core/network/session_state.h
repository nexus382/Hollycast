// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include "net_platform.h"

// Temporary session state that the GGPO host shares with the guest before a
// session starts: the console flash image and the memory-card (VMU) images.
// The guest adopts them in memory only for the duration of the session; its
// own files are never overwritten (see ggpo.cpp). This replaces the manual
// save-file copying that matching GGPO verification digests used to require.
//
// Wire format (all integers little-endian, fixed width):
//   request:  "HCRQ" + u32 GGPO protocol version
//   host sends five zero bytes and a 16-byte challenge over TCP. The guest echoes it over UDP
//   from its gameplay socket, allowing the host to identify its translated port.
//   response: "HCS1" + u16 version (1) + u16 item count, then per item
//             u8 kind + u8 bus + u8 port + u32 length + payload bytes.
//   the guest acknowledges the bundle after adoption with a single byte (1).
// Kind 1 is the console flash, kind 2 a VMU in the given bus/slot. Payloads
// are bounded and validated before use; anything malformed is rejected.
namespace sessionstate
{

constexpr u8 KIND_FLASH = 1;
constexpr u8 KIND_VMU = 2;

struct Item
{
	u8 kind = 0;
	u8 bus = 0;
	u8 port = 0;
	std::vector<u8> data;
};

class Socket
{
public:
	explicit Socket(sock_t sock = INVALID_SOCKET) : handle(static_cast<uintptr_t>(sock)) {}
	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;
	~Socket() { if (VALID(get())) closesocket(get()); }
	sock_t get() const { return static_cast<sock_t>(handle); }

	// GGPO takes ownership by replacing this handle with INVALID_SOCKET.
	uintptr_t handle;
};

bool encode(const std::vector<Item>& items, std::vector<u8>& out);
bool decode(const u8* data, size_t len, std::vector<Item>& items);

bool waitForReadable(sock_t sock, int timeoutMs);
enum class ServeResult { Retry, Complete, IncompatibleVersion };

ServeResult serveGuest(sock_t tcp, sock_t udp, sockaddr_in& guest, u32 protocol,
		const std::vector<Item>& items, const std::atomic<bool>& stop);
bool fetchFromHost(sock_t tcp, sock_t udp, const sockaddr_in& host, u32 protocol,
		std::vector<Item>& items, const std::atomic<bool>& stop);

// Uses the same TCP socket after fetchFromHost and successful local adoption.
bool acknowledgeHost(sock_t tcp, const std::atomic<bool>& stop);

// Connects to a resolved endpoint, waiting at most timeoutMs.
// Returns a blocking socket, or INVALID_SOCKET on failure.
sock_t connectWithTimeout(const sockaddr_in& target, int timeoutMs, const std::atomic<bool>& stop);

}
