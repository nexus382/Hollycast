// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "roms.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace hikaru {
const GameDescriptor AirTrixGame{"airtrix", "Air Trix", RomImages::AirTrixSecurityKey};
const GameDescriptor BraveFirefightersGame{"braveff", "Brave Firefighters", std::nullopt};
const GameDescriptor PlanetHarriersGame{"pharrier", "Planet Harriers", RomImages::PlanetHarriersSecurityKey};
const GameDescriptor VirtualOn4Game{"von4", "Virtual-On Force", std::nullopt, CardReaderModel::SanwaCRP1231BR10};
const GameDescriptor SegaNascarGame{"sgnascar", "NASCAR Arcade (Rev A)", std::nullopt};
const GameDescriptor StarWarsRacerGame{"swracer", "Star Wars: Racer Arcade", RomImages::StarWarsRacerSecurityKey};

const GameDescriptor* findGame(const std::string& basename)
{
    std::string normalized = basename;
    for (char& ch : normalized)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (normalized == AirTrixGame.identifier)
        return &AirTrixGame;
    if (normalized == BraveFirefightersGame.identifier)
        return &BraveFirefightersGame;
    if (normalized == PlanetHarriersGame.identifier)
        return &PlanetHarriersGame;
    if (normalized == VirtualOn4Game.identifier)
        return &VirtualOn4Game;
    if (normalized == SegaNascarGame.identifier)
        return &SegaNascarGame;
    if (normalized == StarWarsRacerGame.identifier)
        return &StarWarsRacerGame;
    return nullptr;
}

const RomFile MieBootRom{"315-6146.bin", 0x800, 0x9b197e35};
const std::array<RomFile, 3> BootRoms = {{
    {"epr-23400a.ic94", 0x200000, 0x2aa906a7},
    {"epr-23400.ic94",  0x200000, 0x3d557104},
    {"epr-21904.ic94",  0x200000, 0xd96298b6},
}};

const std::array<RomFile, 10> AirTrixRoms = {{
    {"epr-23601a.ic29", 0x0400000, 0xcd3ccc05},
    {"epr-23602a.ic30", 0x0400000, 0x24f1bca9},
    {"mpr-23573.ic37",  0x1000000, 0xe22a0734},
    {"mpr-23577.ic38",  0x1000000, 0xd007680d},
    {"mpr-23574.ic41",  0x1000000, 0xa77034a5},
    {"mpr-23578.ic42",  0x1000000, 0xdb612dd6},
    {"mpr-23575.ic45",  0x1000000, 0xfe660f06},
    {"mpr-23579.ic46",  0x1000000, 0x55e656d2},
    {"mpr-23576.ic49",  0x1000000, 0xc01e0329},
    {"mpr-23580.ic50",  0x1000000, 0xd260f39c},
}};

// MAME hikaru.cpp at 43b995da4a2af46c9f48c6a5536953439adfb1ab.
// ROM_LOAD32_WORD offsets are byte positions within the zero-filled regions.
const std::array<InterleavedRomFile, 6> BraveFirefightersProgramRoms = {{
    {{"epr-21994.ic29", 0x200000, 0x31b0a754}, 0x0000000},
    {{"epr-21995.ic30", 0x200000, 0xbcccb56b}, 0x0000002},
    {{"epr-21996.ic31", 0x200000, 0xa8f88e17}, 0x1000000},
    {{"epr-21997.ic32", 0x200000, 0x36641a7f}, 0x1000002},
    {{"epr-21998.ic33", 0x200000, 0xbd1df696}, 0x2000000},
    {{"epr-21999.ic34", 0x200000, 0x9425eee0}, 0x2000002},
}};

const std::array<InterleavedRomFile, 24> BraveFirefightersDataRoms = {{
    {{"mpr-22000.ic37", 0x800000, 0x53d641d6}, 0x0000000},
    {{"mpr-22001.ic38", 0x800000, 0x234bc48f}, 0x0000002},
    {{"mpr-22002.ic39", 0x800000, 0xd8f3aa9e}, 0x01000000},
    {{"mpr-22003.ic40", 0x800000, 0x2560fe98}, 0x01000002},
    {{"mpr-22004.ic41", 0x800000, 0x4e24d71d}, 0x02000000},
    {{"mpr-22005.ic42", 0x800000, 0x2b96c97f}, 0x02000002},
    {{"mpr-22006.ic43", 0x800000, 0xf793a3ba}, 0x03000000},
    {{"mpr-22007.ic44", 0x800000, 0x62616e31}, 0x03000002},
    {{"mpr-22008.ic45", 0x800000, 0xe6905de8}, 0x04000000},
    {{"mpr-22009.ic46", 0x800000, 0xc37dfa5c}, 0x04000002},
    {{"mpr-22010.ic47", 0x800000, 0xb570b46c}, 0x05000000},
    {{"mpr-22011.ic48", 0x800000, 0xd1f5fb58}, 0x05000002},
    {{"mpr-22012.ic49", 0x800000, 0x3ab79029}, 0x06000000},
    {{"mpr-22013.ic50", 0x800000, 0x42d8d00b}, 0x06000002},
    {{"mpr-22014.ic51", 0x800000, 0x0f49c00f}, 0x07000000},
    {{"mpr-22015.ic52", 0x800000, 0xd3696e61}, 0x07000002},
    {{"mpr-22016.ic53s", 0x800000, 0xc1015e00}, 0x08000000},
    {{"mpr-22017.ic54s", 0x800000, 0x222a7cb0}, 0x08000002},
    {{"mpr-22018.ic55s", 0x800000, 0xf160e115}, 0x09000000},
    {{"mpr-22019.ic56s", 0x800000, 0x468b2f10}, 0x09000002},
    {{"mpr-22020.ic57s", 0x800000, 0x0c018d8a}, 0x0a000000},
    {{"mpr-22021.ic58s", 0x800000, 0x43b08604}, 0x0a000002},
    {{"mpr-22022.ic59s", 0x800000, 0xabd3d888}, 0x0b000000},
    {{"mpr-22023.ic60s", 0x800000, 0x07f00869}, 0x0b000002},
}};

// MAME hikaru.cpp at 43b995da4a2af46c9f48c6a5536953439adfb1ab.
// Each program pair occupies 8 MiB in a zero-filled 16 MiB socket slot.
const std::array<InterleavedRomFile, 8> PlanetHarriersProgramRoms = {{
    {{"epr-23565a.ic29", 0x0400000, 0xca9af8a7}, 0x0000000},
    {{"epr-23566a.ic30", 0x0400000, 0xaad0057c}, 0x0000002},
    {{"epr-23567.ic31", 0x0400000, 0xf0e3dcdc}, 0x1000000},
    {{"epr-23568.ic32", 0x0400000, 0x6eee734c}, 0x1000002},
    {{"epr-23569.ic33", 0x0400000, 0x867c7064}, 0x2000000},
    {{"epr-23570.ic34", 0x0400000, 0x556ff58b}, 0x2000002},
    {{"epr-23571.ic35", 0x0400000, 0x5a75fa92}, 0x3000000},
    {{"epr-23572.ic36", 0x0400000, 0x46054067}, 0x3000002},
}};

const std::array<InterleavedRomFile, 16> PlanetHarriersDataRoms = {{
    {{"mpr-23549.ic37", 0x1000000, 0xed764200}, 0x0000000},
    {{"mpr-23553.ic38", 0x1000000, 0x5e70ae78}, 0x0000002},
    {{"mpr-23550.ic41", 0x1000000, 0x841b4d3b}, 0x2000000},
    {{"mpr-23554.ic42", 0x1000000, 0x5cce99de}, 0x2000002},
    {{"mpr-23551.ic45", 0x1000000, 0x71f61d04}, 0x4000000},
    {{"mpr-23555.ic46", 0x1000000, 0x582e5453}, 0x4000002},
    {{"mpr-23552.ic49", 0x1000000, 0x32487181}, 0x6000000},
    {{"mpr-23556.ic50", 0x1000000, 0x45002955}, 0x6000002},
    {{"mpr-23557.ic53", 0x1000000, 0xc20dff1b}, 0x8000000},
    {{"mpr-23561.ic54", 0x1000000, 0x01237844}, 0x8000002},
    {{"mpr-23558.ic57", 0x1000000, 0xe93cc8d7}, 0xa000000},
    {{"mpr-23562.ic58", 0x1000000, 0x85e0816c}, 0xa000002},
    {{"mpr-23559.ic61", 0x1000000, 0x1a7f2ba0}, 0xc000000},
    {{"mpr-23563.ic62", 0x1000000, 0xe3dc328b}, 0xc000002},
    {{"mpr-23560.ic65", 0x1000000, 0x24bb7072}, 0xe000000},
    {{"mpr-23564.ic66", 0x1000000, 0x255724b6}, 0xe000002},
}};

// MAME hikaru.cpp at 43b995da4a2af46c9f48c6a5536953439adfb1ab.
// VON4 has two populated program pairs; the remaining socket slots stay zero.
const std::array<InterleavedRomFile, 4> VirtualOn4ProgramRoms = {{
    {{"epr-23766e.ic29", 0x0400000, 0x188e93bc}, 0x00000000},
    {{"epr-23767e.ic30", 0x0400000, 0xd39e7ff1}, 0x00000002},
    {{"epr-23768a.ic31", 0x0400000, 0x2e1b3812}, 0x01000000},
    {{"epr-23769a.ic32", 0x0400000, 0x34e95687}, 0x01000002},
}};

const std::array<InterleavedRomFile, 16> VirtualOn4DataRoms = {{
    {{"mpr-23750.ic37", 0x1000000, 0x2e40b246}, 0x00000000},
    {{"mpr-23751.ic38", 0x1000000, 0xf345e8b3}, 0x00000002},
    {{"mpr-23752.ic41", 0x1000000, 0x2b8c33e1}, 0x02000000},
    {{"mpr-23753.ic42", 0x1000000, 0x9e928f82}, 0x02000002},
    {{"mpr-23754.ic45", 0x1000000, 0xa7129d77}, 0x04000000},
    {{"mpr-23755.ic46", 0x1000000, 0xfbfc0470}, 0x04000002},
    {{"mpr-23756.ic49", 0x1000000, 0xc5e9abb1}, 0x06000000},
    {{"mpr-23757.ic50", 0x1000000, 0xc8f1316e}, 0x06000002},
    {{"mpr-23758.ic53", 0x1000000, 0x054e408a}, 0x08000000},
    {{"mpr-23759.ic54", 0x1000000, 0xe49c6ed7}, 0x08000002},
    {{"mpr-23760.ic57", 0x1000000, 0x261c24d0}, 0x0a000000},
    {{"mpr-23761.ic58", 0x1000000, 0xbe3681fa}, 0x0a000002},
    {{"mpr-23762.ic61", 0x1000000, 0xd3aa6caf}, 0x0c000000},
    {{"mpr-23763.ic62", 0x1000000, 0x648aecb0}, 0x0c000002},
    {{"mpr-23764.ic65", 0x1000000, 0x0aa472aa}, 0x0e000000},
    {{"mpr-23765.ic66", 0x1000000, 0xd904250f}, 0x0e000002},
}};

// MAME hikaru.cpp at 43b995da4a2af46c9f48c6a5536953439adfb1ab.
// Rev A uses an Actel ROM board; its key is not a 315-5881 security key.
const std::array<InterleavedRomFile, 2> SegaNascarProgramRoms = {{
    {{"epr-23485a.ic35", 0x0400000, 0x1072f531}, 0x00000000},
    {{"epr-23486a.ic36", 0x0400000, 0x02d4aab6}, 0x00000002},
}};

const std::array<InterleavedRomFile, 16> SegaNascarDataRoms = {{
    {{"mpr-23469.ic19", 0x1000000, 0x89cbad8d}, 0x00000000},
    {{"mpr-23473.ic20", 0x1000000, 0x977b87d6}, 0x00000002},
    {{"mpr-23470.ic21", 0x1000000, 0xfaf4940f}, 0x02000000},
    {{"mpr-23474.ic22", 0x1000000, 0xfaf69ac5}, 0x02000002},
    {{"mpr-23471.ic23", 0x1000000, 0xa3aad8ac}, 0x04000000},
    {{"mpr-23475.ic24", 0x1000000, 0x5f51597c}, 0x04000002},
    {{"mpr-23472.ic25", 0x1000000, 0x2495f678}, 0x06000000},
    {{"mpr-23476.ic26", 0x1000000, 0x927cf31c}, 0x06000002},
    {{"mpr-23477.ic27", 0x1000000, 0xb4b7c477}, 0x08000000},
    {{"mpr-23481.ic28", 0x1000000, 0x27b8eb7d}, 0x08000002},
    {{"mpr-23478.ic29", 0x1000000, 0x1fac431c}, 0x0a000000},
    {{"mpr-23482.ic30", 0x1000000, 0x2e9a0420}, 0x0a000002},
    {{"mpr-23479.ic31", 0x1000000, 0x9704e393}, 0x0c000000},
    {{"mpr-23483.ic32", 0x1000000, 0xc37adebe}, 0x0c000002},
    {{"mpr-23480.ic33", 0x1000000, 0xf517b8b3}, 0x0e000000},
    {{"mpr-23484.ic34", 0x1000000, 0x2ebe1aa1}, 0x0e000002},
}};

// MAME hikaru.cpp at 43b995da4a2af46c9f48c6a5536953439adfb1ab.
// Two populated 8 MiB program pairs retain their zero-filled 16 MiB socket slots.
const std::array<InterleavedRomFile, 4> StarWarsRacerProgramRoms = {{
    {{"epr-23174.ic29", 0x0400000, 0xeae62b46}, 0x00000000},
    {{"epr-23175.ic30", 0x0400000, 0xb92da060}, 0x00000002},
    {{"epr-23176.ic31", 0x0400000, 0x2f2824a7}, 0x01000000},
    {{"epr-23177.ic32", 0x0400000, 0x7a5e3f0f}, 0x01000002},
}};

// The SOP44 board uses sixteen 16 MiB pairs, including the underside sockets.
const std::array<InterleavedRomFile, 32> StarWarsRacerDataRoms = {{
    {{"mpr-23086.ic37",  0x0800000, 0xef6f20f1}, 0x00000000},
    {{"mpr-23087.ic38",  0x0800000, 0x54389822}, 0x00000002},
    {{"mpr-23088.ic39",  0x0800000, 0x9f1a382e}, 0x01000000},
    {{"mpr-23089.ic40",  0x0800000, 0x6aae64fc}, 0x01000002},
    {{"mpr-23090.ic41",  0x0800000, 0xba857872}, 0x02000000},
    {{"mpr-23091.ic42",  0x0800000, 0x66a73e27}, 0x02000002},
    {{"mpr-23092.ic43",  0x0800000, 0x4f20a0f5}, 0x03000000},
    {{"mpr-23093.ic44",  0x0800000, 0xe74d7d64}, 0x03000002},
    {{"mpr-23094.ic45",  0x0800000, 0x90f04c14}, 0x04000000},
    {{"mpr-23095.ic46",  0x0800000, 0xcc67cb5b}, 0x04000002},
    {{"mpr-23096.ic47",  0x0800000, 0x799ab79e}, 0x05000000},
    {{"mpr-23097.ic48",  0x0800000, 0xf68439de}, 0x05000002},
    {{"mpr-23098.ic49",  0x0800000, 0xa1e2009c}, 0x06000000},
    {{"mpr-23099.ic50",  0x0800000, 0xce36f642}, 0x06000002},
    {{"mpr-23100.ic51",  0x0800000, 0x0f966653}, 0x07000000},
    {{"mpr-23101.ic52",  0x0800000, 0x2640fbaa}, 0x07000002},
    {{"mpr-23102.ic53s", 0x0800000, 0x080c5bcb}, 0x08000000},
    {{"mpr-23103.ic54s", 0x0800000, 0x19c7758f}, 0x08000002},
    {{"mpr-23104.ic55s", 0x0800000, 0x4ca74216}, 0x09000000},
    {{"mpr-23105.ic56s", 0x0800000, 0xe2dd35ba}, 0x09000002},
    {{"mpr-23106.ic57s", 0x0800000, 0xdd325515}, 0x0a000000},
    {{"mpr-23107.ic58s", 0x0800000, 0xa527a22a}, 0x0a000002},
    {{"mpr-23108.ic59s", 0x0800000, 0x47817d9a}, 0x0b000000},
    {{"mpr-23109.ic60s", 0x0800000, 0x8c61dec4}, 0x0b000002},
    {{"mpr-23110.ic61s", 0x0800000, 0x4ddae9f1}, 0x0c000000},
    {{"mpr-23111.ic62s", 0x0800000, 0xc404cb1c}, 0x0c000002},
    {{"mpr-23112.ic63s", 0x0800000, 0xd001fe59}, 0x0d000000},
    {{"mpr-23113.ic64s", 0x0800000, 0xf241cfd5}, 0x0d000002},
    {{"mpr-23114.ic65s", 0x0800000, 0x80049d7c}, 0x0e000000},
    {{"mpr-23115.ic66s", 0x0800000, 0x4fc540fe}, 0x0e000002},
    {{"mpr-23116.ic67s", 0x0800000, 0x9f567fce}, 0x0f000000},
    {{"mpr-23117.ic68s", 0x0800000, 0x9d4d3529}, 0x0f000002},
}};

uint32_t romCrc32(const std::vector<uint8_t>& data)
{
    static const auto table = [] {
        std::array<uint32_t, 256> entries{};
        for (uint32_t byte = 0; byte < entries.size(); ++byte) {
            uint32_t crc = byte;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
            entries[byte] = crc;
        }
        return entries;
    }();
    uint32_t crc = 0xffffffffu;
    for (uint8_t byte : data)
        crc = (crc >> 8) ^ table[(crc ^ byte) & 255];
    return ~crc;
}

void validateRom(const RomFile& expected, const std::vector<uint8_t>& data)
{
    if (data.size() != expected.size)
        throw std::runtime_error(std::string(expected.name) + ": expected "
            + std::to_string(expected.size) + " bytes, got " + std::to_string(data.size()));
    const uint32_t actual = romCrc32(data);
    if (actual != expected.crc32) {
        std::ostringstream error;
        error << expected.name << ": CRC32 mismatch; expected " << std::hex
              << std::setw(8) << std::setfill('0') << expected.crc32 << ", got "
              << std::setw(8) << actual;
        throw std::runtime_error(error.str());
    }
}

void interleaveRomWords(std::vector<uint8_t>& region, uint32_t offset,
                        const std::vector<uint8_t>& chip)
{
    if ((offset & 1) || (chip.size() & 1))
        throw std::invalid_argument("Hikaru ROM chips require aligned, complete 16-bit words");
    const uint64_t end = chip.empty() ? offset : uint64_t(offset) + uint64_t(chip.size()) * 2 - 2;
    if (end > region.size())
        throw std::out_of_range("Hikaru ROM chip exceeds its interleaved region");
    for (std::size_t word = 0; word < chip.size() / 2; ++word) {
        const std::size_t destination = offset + word * 4;
        region[destination] = chip[word * 2];
        region[destination + 1] = chip[word * 2 + 1];
    }
}

RomImages loadAirTrix(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);
    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x0800000, 0);
    images.data.resize(0x08000000, 0);
    for (std::size_t index = 0; index < AirTrixRoms.size(); ++index) {
        const auto& expected = AirTrixRoms[index];
        auto chip = reader(expected);
        validateRom(expected, chip);
        if (index < 2)
            interleaveRomWords(images.program, uint32_t(index) * 2, chip);
        else {
            const uint32_t chipIndex = uint32_t(index - 2);
            interleaveRomWords(images.data, (chipIndex / 2) * 0x02000000 + (chipIndex & 1) * 2, chip);
        }
    }
    return images;
}

RomImages loadBraveFirefighters(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);

    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x04000000, 0);
    images.data.resize(0x10000000, 0);
    for (const auto& chip : BraveFirefightersProgramRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.program, chip.offset, bytes);
    }
    for (const auto& chip : BraveFirefightersDataRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.data, chip.offset, bytes);
    }
    return images;
}

RomImages loadPlanetHarriers(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);

    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x04000000, 0);
    images.data.resize(0x10000000, 0);
    for (const auto& chip : PlanetHarriersProgramRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.program, chip.offset, bytes);
    }
    for (const auto& chip : PlanetHarriersDataRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.data, chip.offset, bytes);
    }
    return images;
}
RomImages loadVirtualOn4(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);

    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x04000000, 0);
    images.data.resize(0x10000000, 0);
    for (const auto& chip : VirtualOn4ProgramRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.program, chip.offset, bytes);
    }
    for (const auto& chip : VirtualOn4DataRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.data, chip.offset, bytes);
    }
    return images;
}

RomImages loadSegaNascar(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);

    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x04000000, 0);
    images.data.resize(0x10000000, 0);
    for (const auto& chip : SegaNascarProgramRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.program, chip.offset, bytes);
    }
    for (const auto& chip : SegaNascarDataRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.data, chip.offset, bytes);
    }
    return images;
}

RomImages loadStarWarsRacer(const RomReader& reader, const std::string& bootName)
{
    if (!reader)
        throw std::invalid_argument("Hikaru ROM reader is required");
    const RomFile* boot = nullptr;
    for (const auto& candidate : BootRoms)
        if (bootName == candidate.name)
            boot = &candidate;
    if (!boot)
        throw std::invalid_argument("Unknown Hikaru BIOS revision: " + bootName);

    RomImages images;
    images.boot = reader(*boot);
    validateRom(*boot, images.boot);
    images.program.resize(0x04000000, 0);
    images.data.resize(0x10000000, 0);
    for (const auto& chip : StarWarsRacerProgramRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.program, chip.offset, bytes);
    }
    for (const auto& chip : StarWarsRacerDataRoms) {
        auto bytes = reader(chip.file);
        validateRom(chip.file, bytes);
        interleaveRomWords(images.data, chip.offset, bytes);
    }
    return images;
}
} // namespace hikaru
