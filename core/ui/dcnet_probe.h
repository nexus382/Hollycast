// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <chrono>
#include <string>

namespace dcnet
{
// Availability only: these handshakes neither join a game nor publish counts.
// A failed check means unreachable from this client, not necessarily offline.
bool probePba(const std::string& host, unsigned short port = 7654,
		std::chrono::milliseconds timeout = std::chrono::seconds(15));
bool probeSegaRally(const std::string& host, unsigned short port = 7657,
		std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
