// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
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
//   request:  "HCRQ" + u32 session protocol (3)
//   response: "HCS1" + u16 version (1) + u16 item count, then per item
//             u8 kind + u8 bus + u8 port + u32 length + payload bytes.
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

bool encode(const std::vector<Item>& items, std::vector<u8>& out);
bool decode(const u8* data, size_t len, std::vector<Item>& items);

bool sendRequest(sock_t sock, u32 protocol);
bool receiveRequest(sock_t sock, u32& protocol);
bool sendItems(sock_t sock, const std::vector<Item>& items);
bool receiveItems(sock_t sock, std::vector<Item>& items);

// Connects to ip:port (numeric IP or host name), waiting at most timeoutMs.
// Returns a blocking socket, or INVALID_SOCKET on failure.
sock_t connectWithTimeout(const std::string& host, int port, int timeoutMs);

}
