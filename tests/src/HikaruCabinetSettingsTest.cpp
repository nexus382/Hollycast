// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "hw/hikaru/cabinet_settings.h"
#include <algorithm>

namespace {
using namespace hikaru;
struct CabinetFixture {
    std::array<uint8_t, 4> id;
    std::array<std::array<uint16_t, 2>, 3> checksums;
};
// Synthetic records: byte i = i * 17 + salt (salts 3 and 11), then ID and
// country fields. Fixed expected checksums use the verified BIOS format.
const CabinetFixture Cabinets[] = {
    {{{'B','C','O','0'}}, {{{0x5261,0x5626}, {0x7754,0x7313}, {0x180b,0x1c4c}}}},
    {{{'B','A','K','0'}}, {{{0x75e6,0x71a1}, {0x50d3,0x5494}, {0x3f8c,0x3bcb}}}},
    {{{'B','C','N',0}},   {{{0x74bc,0x70fb}, {0x5189,0x55ce}, {0x3ed6,0x3a91}}}},
};
std::array<uint8_t, 128> fixture(const CabinetFixture& cabinet, unsigned country = 0)
{
    std::array<uint8_t, 128> result{};
    for (unsigned copy = 0; copy < 2; ++copy) {
        auto* record = result.data() + copy * 64;
        for (unsigned i = 0; i < 62; ++i) record[i] = uint8_t(i * 17 + (copy ? 11 : 3));
        std::copy(cabinet.id.begin(), cabinet.id.end(), record);
        record[5] = uint8_t(country);
        const auto checksum = cabinet.checksums[country][copy];
        record[62] = uint8_t(checksum);
        record[63] = uint8_t(checksum >> 8);
    }
    return result;
}

TEST(HikaruCabinetSettingsTest, EnglishCountryChangesOnlyCountryAndChecksumsForEveryGame)
{
    for (const auto& cabinet : Cabinets) {
        const auto original = fixture(cabinet);
        for (unsigned country : {1u, 2u}) {
            auto changed = original;
            ASSERT_EQ(applyCabinetRegion(changed, cabinet.id, static_cast<CabinetRegion>(country)), CabinetRegionResult::Changed);
            EXPECT_EQ(changed, fixture(cabinet, country));
            for (unsigned i = 0; i < original.size(); ++i) {
                const unsigned offset = i % 64;
                if (offset != 5 && offset < 62) EXPECT_EQ(changed[i], original[i]) << "EEPROM byte " << i;
            }
        }
    }
}

TEST(HikaruCabinetSettingsTest, RepeatedSelectionIsIdempotentAndJapanRestoresOriginalBytes)
{
    for (const auto& cabinet : Cabinets) {
        auto changed = fixture(cabinet);
        ASSERT_EQ(applyCabinetRegion(changed, cabinet.id, CabinetRegion::Usa), CabinetRegionResult::Changed);
        const auto english = changed;
        EXPECT_EQ(applyCabinetRegion(changed, cabinet.id, CabinetRegion::Usa), CabinetRegionResult::Unchanged);
        EXPECT_EQ(changed, english);
        ASSERT_EQ(applyCabinetRegion(changed, cabinet.id, CabinetRegion::Japan), CabinetRegionResult::Changed);
        EXPECT_EQ(changed, fixture(cabinet));
    }
}

TEST(HikaruCabinetSettingsTest, SavedModeLeavesAnyExistingSettingsUntouched)
{
    for (const auto& cabinet : Cabinets) {
        auto data = fixture(cabinet);
        data[21] ^= 0x20; // Preserve even an invalid record when no override is requested.
        const auto original = data;
        EXPECT_EQ(applyCabinetRegion(data, cabinet.id, CabinetRegion::Saved), CabinetRegionResult::Unchanged);
        EXPECT_EQ(data, original);
    }
}

TEST(HikaruCabinetSettingsTest, BlankCabinetWaitsForBiosInitializationWithoutSeedingAnImage)
{
    std::array<uint8_t, 128> blank;
    blank.fill(0xff);
    const auto original = blank;
    EXPECT_EQ(applyCabinetRegion(blank, Cabinets[0].id, CabinetRegion::Usa), CabinetRegionResult::PendingInitialization);
    EXPECT_EQ(blank, original);
}

TEST(HikaruCabinetSettingsTest, AnySingleBitCorruptionWaitsWithoutUpdatingEitherRecord)
{
    for (const auto& cabinet : Cabinets) {
        for (unsigned byte = 0; byte < 128; ++byte) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                auto data = fixture(cabinet);
                data[byte] ^= uint8_t(1u << bit);
                const auto original = data;
                const auto result = applyCabinetRegion(data, cabinet.id, CabinetRegion::Usa);
                ASSERT_TRUE(result == CabinetRegionResult::PendingInitialization || result == CabinetRegionResult::Invalid);
                ASSERT_EQ(data, original);
            }
        }
    }
}

TEST(HikaruCabinetSettingsTest, WrongGameOrUnsupportedRegionNeverChangesData)
{
    auto data = fixture(Cabinets[0]);
    const auto original = data;
    EXPECT_EQ(applyCabinetRegion(data, Cabinets[1].id, CabinetRegion::Usa), CabinetRegionResult::Invalid);
    EXPECT_EQ(data, original);
    EXPECT_EQ(applyCabinetRegion(data, Cabinets[0].id, static_cast<CabinetRegion>(999)), CabinetRegionResult::Invalid);
    EXPECT_EQ(data, original);
}

TEST(HikaruCabinetSettingsTest, ValidRecordsFromDifferentGamesAreNotCombined)
{
    auto data = fixture(Cabinets[0]);
    const auto other = fixture(Cabinets[1]);
    std::copy(other.begin() + 64, other.end(), data.begin() + 64);
    const auto original = data;
    EXPECT_EQ(applyCabinetRegion(data, Cabinets[0].id, CabinetRegion::Usa), CabinetRegionResult::Invalid);
    EXPECT_EQ(data, original);
}
} // namespace
