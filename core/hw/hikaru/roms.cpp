// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "roms.h"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace hikaru {
const GameDescriptor AirTrixGame{"airtrix", "Air Trix", true};
const GameDescriptor BraveFirefightersGame{"braveff", "Brave Firefighters", false};

const GameDescriptor* findGame(const std::string& basename)
{
    std::string normalized = basename;
    for (char& ch : normalized)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (normalized == AirTrixGame.identifier)
        return &AirTrixGame;
    if (normalized == BraveFirefightersGame.identifier)
        return &BraveFirefightersGame;
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
} // namespace hikaru
