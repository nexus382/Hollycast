// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hikaru {
struct RomFile {
    const char* name;
    uint32_t size;
    uint32_t crc32;
};

struct InterleavedRomFile {
    RomFile file;
    uint32_t offset;
};

struct GameDescriptor {
    const char* identifier;
    const char* title;
    bool encrypted;
};

extern const GameDescriptor AirTrixGame;
extern const GameDescriptor BraveFirefightersGame;
const GameDescriptor* findGame(const std::string& basename);

// File facts from MAME's Hikaru driver, revision
// 43b995da4a2af46c9f48c6a5536953439adfb1ab. These are manifests, not ROM data.
extern const std::array<RomFile, 3> BootRoms;
extern const std::array<RomFile, 10> AirTrixRoms;
extern const std::array<InterleavedRomFile, 6> BraveFirefightersProgramRoms;
extern const std::array<InterleavedRomFile, 24> BraveFirefightersDataRoms;
extern const RomFile MieBootRom;

uint32_t romCrc32(const std::vector<uint8_t>& data);
void validateRom(const RomFile& expected, const std::vector<uint8_t>& data);
// ROM_LOAD32_WORD: consecutive 16-bit little-endian words occupy one half
// of each 32-bit bus word. Offset selects the board bank and low/high lane.
void interleaveRomWords(std::vector<uint8_t>& region, uint32_t offset,
                        const std::vector<uint8_t>& chip);

struct RomImages {
    std::vector<uint8_t> boot;
    std::vector<uint8_t> program;
    std::vector<uint8_t> data;
    static constexpr uint32_t AirTrixSecurityKey = 0x291b02c7;
};

using RomReader = std::function<std::vector<uint8_t>(const RomFile&)>;
RomImages loadAirTrix(const RomReader& reader, const std::string& bootName = "epr-23400.ic94");
RomImages loadBraveFirefighters(const RomReader& reader, const std::string& bootName = "epr-23400.ic94");
} // namespace hikaru
