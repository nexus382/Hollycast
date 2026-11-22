// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "network/dcnet_endpoint.h"
#include <chrono>
#include <string>

namespace dcnet
{
// Availability only: these handshakes neither join a game nor publish counts.
// A failed check means unreachable from this client, not necessarily offline.
bool probePba(const std::string& host = PublicHost, unsigned short port = PppPort,
		std::chrono::milliseconds timeout = std::chrono::seconds(15));
bool probeSegaRally(const std::string& host = PublicHost, unsigned short port = SegaRallyPort,
		std::chrono::milliseconds timeout = std::chrono::seconds(5));
}
