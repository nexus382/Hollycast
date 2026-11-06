// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "hw/hikaru/roms.h"
#include <iostream>
#include <stdexcept>

int main()
{
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool result, const char* message) {
        ++checks;
        if (!result) { ++failures; std::cerr << message << '\n'; }
    };
    const auto rejects = [&](auto action, const char* message) {
        try { action(); check(false, message); }
        catch (const std::exception&) { check(true, message); }
    };
    const std::vector<uint8_t> standard{'1','2','3','4','5','6','7','8','9'};
    check(hikaru::romCrc32(standard) == 0xcbf43926, "standard CRC32 check vector");
    check(hikaru::romCrc32({}) == 0, "empty CRC32");
    const hikaru::RomFile expected{"generated", 9, 0xcbf43926};
    hikaru::validateRom(expected, standard);
    auto damaged = standard; damaged[3] ^= 1;
    rejects([&] { hikaru::validateRom(expected, damaged); }, "wrong CRC rejected");
    damaged.push_back(0);
    rejects([&] { hikaru::validateRom(expected, damaged); }, "wrong size rejected");

    std::vector<uint8_t> region(16, 0);
    hikaru::interleaveRomWords(region, 0, {0x11,0x22,0x33,0x44});
    hikaru::interleaveRomWords(region, 2, {0x55,0x66,0x77,0x88});
    check(region == std::vector<uint8_t>({0x11,0x22,0x55,0x66,0x33,0x44,0x77,0x88,0,0,0,0,0,0,0,0}), "32-bit word lanes preserve chip byte order");
    hikaru::interleaveRomWords(region, 10, {0xaa,0xbb,0xcc,0xdd});
    check(region[10] == 0xaa && region[11] == 0xbb && region[14] == 0xcc && region[15] == 0xdd,
          "word interleave accepts exact region end and second bank");
    std::vector<uint8_t> sparseRegion(32, 0);
    hikaru::interleaveRomWords(sparseRegion, 0, {0x10,0x11,0x12,0x13});
    hikaru::interleaveRomWords(sparseRegion, 2, {0x20,0x21,0x22,0x23});
    hikaru::interleaveRomWords(sparseRegion, 16, {0x30,0x31,0x32,0x33});
    hikaru::interleaveRomWords(sparseRegion, 18, {0x40,0x41,0x42,0x43});
    check(sparseRegion[0] == 0x10 && sparseRegion[1] == 0x11
            && sparseRegion[2] == 0x20 && sparseRegion[3] == 0x21
            && sparseRegion[4] == 0x12 && sparseRegion[5] == 0x13
            && sparseRegion[6] == 0x22 && sparseRegion[7] == 0x23,
          "paired ROM lanes interleave within a bank");
    check(sparseRegion[8] == 0 && sparseRegion[15] == 0
            && sparseRegion[16] == 0x30 && sparseRegion[17] == 0x31
            && sparseRegion[18] == 0x40 && sparseRegion[19] == 0x41
            && sparseRegion[20] == 0x32 && sparseRegion[22] == 0x42
            && sparseRegion[28] == 0 && sparseRegion[31] == 0,
          "ROM interleave preserves zero-filled gaps and subsequent banks");
    const auto previous = region;
    rejects([&] { hikaru::interleaveRomWords(region, 12, {1,2,3,4}); }, "out of range chip rejected");
    rejects([&] { hikaru::interleaveRomWords(region, 1, {1,2}); }, "unaligned chip rejected");
    rejects([&] { hikaru::interleaveRomWords(region, 0, {1,2,3}); }, "incomplete chip word rejected");
    check(region == previous, "invalid chips leave region unchanged");
    rejects([&] { hikaru::loadAirTrix({}); }, "missing ROM reader rejected");
    rejects([&] { hikaru::loadAirTrix([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); }, "unknown BIOS revision rejected");
    rejects([&] { hikaru::loadAirTrix([](const auto&) { return std::vector<uint8_t>{}; }); }, "missing BIOS data rejected");
    check(hikaru::findGame("AIRTRIX") == &hikaru::AirTrixGame
            && hikaru::AirTrixGame.securityKey == 0x291b02c7,
          "Air Trix descriptor keeps its title and encryption metadata");
    check(hikaru::findGame("braveff") == &hikaru::BraveFirefightersGame
            && !hikaru::BraveFirefightersGame.securityKey,
          "Brave Firefighters descriptor has no security chip encryption");
    check(hikaru::findGame("hikaru") == nullptr && hikaru::findGame("unknown") == nullptr,
          "only supported game basenames are allowlisted");
    check(hikaru::BraveFirefightersProgramRoms.size() == 6
            && hikaru::BraveFirefightersDataRoms.size() == 24,
          "Brave Firefighters manifests contain all 30 MAME chips");
    check(hikaru::BraveFirefightersProgramRoms[0].offset == 0
            && hikaru::BraveFirefightersProgramRoms[1].offset == 2
            && hikaru::BraveFirefightersProgramRoms[2].offset == 0x1000000
            && hikaru::BraveFirefightersProgramRoms[5].offset == 0x2000002,
          "Brave Firefighters EPROM manifest uses MAME program offsets");
    check(hikaru::BraveFirefightersDataRoms[0].offset == 0
            && hikaru::BraveFirefightersDataRoms[1].offset == 2
            && hikaru::BraveFirefightersDataRoms[2].offset == 0x1000000
            && hikaru::BraveFirefightersDataRoms[23].offset == 0xb000002,
          "Brave Firefighters MASKROM manifest uses 16 MiB bank strides");
    check(std::string(hikaru::BraveFirefightersProgramRoms[0].file.name) == "epr-21994.ic29"
            && hikaru::BraveFirefightersProgramRoms[0].file.size == 0x200000
            && hikaru::BraveFirefightersProgramRoms[0].file.crc32 == 0x31b0a754
            && std::string(hikaru::BraveFirefightersDataRoms[23].file.name) == "mpr-22023.ic60s"
            && hikaru::BraveFirefightersDataRoms[23].file.size == 0x800000
            && hikaru::BraveFirefightersDataRoms[23].file.crc32 == 0x07f00869,
          "Brave Firefighters manifest endpoints match the pinned MAME data");
    rejects([&] { hikaru::loadBraveFirefighters({}); }, "missing Brave Firefighters ROM reader rejected");
    rejects([&] { hikaru::loadBraveFirefighters([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); },
            "unknown Brave Firefighters BIOS revision rejected");
    rejects([&] { hikaru::loadBraveFirefighters([](const auto&) { return std::vector<uint8_t>{}; }); },
            "missing Brave Firefighters BIOS data rejected");
    check(hikaru::findGame("PHARRIER") == &hikaru::PlanetHarriersGame
            && hikaru::PlanetHarriersGame.securityKey == 0x2912c68a,
          "Planet Harriers selects its own protection key");
    check(hikaru::PlanetHarriersProgramRoms.size() == 8
            && hikaru::PlanetHarriersDataRoms.size() == 16,
          "Planet Harriers manifest contains all 24 MAME chips");
    for (std::size_t i = 0; i < hikaru::PlanetHarriersProgramRoms.size(); ++i) {
        const auto& chip = hikaru::PlanetHarriersProgramRoms[i];
        check(chip.offset == (i / 2) * 0x1000000 + (i % 2) * 2 && chip.file.size == 0x400000,
              "Planet Harriers program pairs retain 16 MiB socket slots");
    }
    for (std::size_t i = 0; i < hikaru::PlanetHarriersDataRoms.size(); ++i) {
        const auto& chip = hikaru::PlanetHarriersDataRoms[i];
        check(chip.offset == (i / 2) * 0x2000000 + (i % 2) * 2 && chip.file.size == 0x1000000,
              "Planet Harriers data pairs fill the 256 MiB region");
    }
    check(std::string(hikaru::PlanetHarriersProgramRoms[0].file.name) == "epr-23565a.ic29"
            && hikaru::PlanetHarriersProgramRoms[0].file.crc32 == 0xca9af8a7
            && std::string(hikaru::PlanetHarriersDataRoms[15].file.name) == "mpr-23564.ic66"
            && hikaru::PlanetHarriersDataRoms[15].file.crc32 == 0x255724b6,
          "Planet Harriers manifest endpoints match pinned MAME data");
    rejects([&] { hikaru::loadPlanetHarriers({}); }, "missing Planet Harriers ROM reader rejected");
    rejects([&] { hikaru::loadPlanetHarriers([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); },
            "unknown Planet Harriers BIOS revision rejected");
    rejects([&] { hikaru::loadPlanetHarriers([](const auto&) { return std::vector<uint8_t>{}; }); },
            "missing Planet Harriers BIOS data rejected");
    check(hikaru::findGame("VON4") == &hikaru::VirtualOn4Game && !hikaru::VirtualOn4Game.securityKey,
          "Virtual-On Force is allowlisted with no 315-5881 chip");
    check(hikaru::VirtualOn4ProgramRoms.size() == 4 && hikaru::VirtualOn4DataRoms.size() == 16,
          "VON4 manifest contains all 20 chips");
    for (std::size_t i = 0; i < hikaru::VirtualOn4ProgramRoms.size(); ++i) {
        const auto& chip = hikaru::VirtualOn4ProgramRoms[i];
        check(chip.offset == (i / 2) * 0x1000000 + (i % 2) * 2 && chip.file.size == 0x400000,
              "VON4 program pairs retain 16 MiB socket slots");
    }
    for (std::size_t i = 0; i < hikaru::VirtualOn4DataRoms.size(); ++i) {
        const auto& chip = hikaru::VirtualOn4DataRoms[i];
        check(chip.offset == (i / 2) * 0x2000000 + (i % 2) * 2 && chip.file.size == 0x1000000,
              "VON4 data pairs fill the 256 MiB region");
    }
    check(hikaru::VirtualOn4ProgramRoms[0].file.crc32 == 0x188e93bc
            && hikaru::VirtualOn4ProgramRoms[3].file.crc32 == 0x34e95687
            && hikaru::VirtualOn4DataRoms[15].file.crc32 == 0xd904250f,
          "VON4 manifest endpoints match pinned MAME data");
    rejects([&] { hikaru::loadVirtualOn4({}); }, "missing VON4 reader rejected");
    rejects([&] { hikaru::loadVirtualOn4([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); },
            "unknown VON4 BIOS rejected");
    rejects([&] { hikaru::loadVirtualOn4([](const auto&) { return std::vector<uint8_t>{}; }); },
            "missing VON4 BIOS data rejected");
    check(hikaru::findGame("SGNASCAR") == &hikaru::SegaNascarGame && !hikaru::SegaNascarGame.securityKey,
          "NASCAR Rev A selects its Actel board without a 315-5881 key");
    check(hikaru::SegaNascarProgramRoms.size() == 2 && hikaru::SegaNascarDataRoms.size() == 16,
          "NASCAR Rev A manifest contains the populated program pair and all data chips");
    check(hikaru::SegaNascarProgramRoms[0].offset == 0 && hikaru::SegaNascarProgramRoms[1].offset == 2
            && hikaru::SegaNascarProgramRoms[0].file.size == 0x400000
            && hikaru::SegaNascarProgramRoms[1].file.size == 0x400000
            && hikaru::SegaNascarProgramRoms[0].file.crc32 == 0x1072f531
            && hikaru::SegaNascarProgramRoms[1].file.crc32 == 0x02d4aab6,
          "NASCAR Rev A uses the correct program lanes and revision");
    for (std::size_t i = 0; i < hikaru::SegaNascarDataRoms.size(); ++i) {
        const auto& chip = hikaru::SegaNascarDataRoms[i];
        check(chip.offset == (i / 2) * 0x2000000 + (i % 2) * 2 && chip.file.size == 0x1000000,
              "NASCAR data pairs fill the 256 MiB region");
    }
    check(std::string(hikaru::SegaNascarDataRoms[0].file.name) == "mpr-23469.ic19"
            && hikaru::SegaNascarDataRoms[0].file.crc32 == 0x89cbad8d
            && std::string(hikaru::SegaNascarDataRoms[15].file.name) == "mpr-23484.ic34"
            && hikaru::SegaNascarDataRoms[15].file.crc32 == 0x2ebe1aa1,
          "NASCAR data manifest matches the pinned MAME socket layout");
    rejects([&] { hikaru::loadSegaNascar({}); }, "missing NASCAR reader rejected");
    rejects([&] { hikaru::loadSegaNascar([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); },
            "unknown NASCAR BIOS rejected");
    rejects([&] { hikaru::loadSegaNascar([](const auto&) { return std::vector<uint8_t>{}; }); },
            "missing NASCAR BIOS data rejected");
    check(hikaru::findGame("SWRACER") == &hikaru::StarWarsRacerGame
            && hikaru::StarWarsRacerGame.securityKey == 0x2903dad5,
          "Star Wars Racer selects its 315-5881 protection key");
    check(hikaru::StarWarsRacerProgramRoms.size() == 4 && hikaru::StarWarsRacerDataRoms.size() == 32,
          "Star Wars Racer manifest contains all 36 chips");
    for (std::size_t i = 0; i < hikaru::StarWarsRacerProgramRoms.size(); ++i) {
        const auto& chip = hikaru::StarWarsRacerProgramRoms[i];
        check(chip.offset == (i / 2) * 0x1000000 + (i % 2) * 2 && chip.file.size == 0x400000,
              "Star Wars Racer program pairs retain 16 MiB socket slots");
    }
    for (std::size_t i = 0; i < hikaru::StarWarsRacerDataRoms.size(); ++i) {
        const auto& chip = hikaru::StarWarsRacerDataRoms[i];
        check(chip.offset == (i / 2) * 0x1000000 + (i % 2) * 2 && chip.file.size == 0x800000,
              "Star Wars Racer SOP44 pairs fill contiguous 16 MiB banks");
    }
    check(hikaru::StarWarsRacerProgramRoms[0].file.crc32 == 0xeae62b46
            && hikaru::StarWarsRacerProgramRoms[3].file.crc32 == 0x7a5e3f0f
            && std::string(hikaru::StarWarsRacerDataRoms[15].file.name) == "mpr-23101.ic52"
            && std::string(hikaru::StarWarsRacerDataRoms[16].file.name) == "mpr-23102.ic53s"
            && hikaru::StarWarsRacerDataRoms[16].file.crc32 == 0x080c5bcb
            && std::string(hikaru::StarWarsRacerDataRoms[31].file.name) == "mpr-23117.ic68s"
            && hikaru::StarWarsRacerDataRoms[31].file.crc32 == 0x9d4d3529,
          "Star Wars Racer program and underside socket metadata match pinned MAME data");
    rejects([&] { hikaru::loadStarWarsRacer({}); }, "missing Star Wars Racer reader rejected");
    rejects([&] { hikaru::loadStarWarsRacer([](const auto&) { return std::vector<uint8_t>{}; }, "unknown"); },
            "unknown Star Wars Racer BIOS rejected");
    rejects([&] { hikaru::loadStarWarsRacer([](const auto&) { return std::vector<uint8_t>{}; }); },
            "missing Star Wars Racer BIOS data rejected");
    std::cout << checks << " ROM checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
