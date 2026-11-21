// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstdint>
#include <string>

namespace dcnet
{
inline constexpr char PublicHost[] = "dcnet.flyca.st";
inline constexpr std::uint16_t PppPort = 7654;
inline constexpr std::uint16_t SegaRallyPort = 7657;

std::string serverHostname(const char *fallback = PublicHost);
}
