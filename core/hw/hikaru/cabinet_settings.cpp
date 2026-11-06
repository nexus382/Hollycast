// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "cabinet_settings.h"
#include <algorithm>

namespace hikaru {
namespace {
constexpr unsigned RecordSize = 64, CountryOffset = 5, ChecksumOffset = 62;

uint16_t recordChecksum(const uint8_t* record)
{
    // Derived from BIOS 0.92 at 0x74e4 (also present in 0.84/0.96): this
    // seeded, augmented calculation is not a standard CCITT preset. The
    // result is stored little-endian at bytes 62/63 of each 64-byte record.
    uint32_t state = 0x01a18104;
    for (unsigned i = 0; i <= ChecksumOffset; ++i) {
        if (i < ChecksumOffset) state |= record[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            state = (state << 1) ^ ((state & 0x80000000) ? 0x10210000u : 0);
    }
    return uint16_t(state >> 16);
}
bool validChecksum(const uint8_t* record)
{
    return recordChecksum(record) == (unsigned(record[ChecksumOffset]) | (unsigned(record[ChecksumOffset + 1]) << 8));
}
} // namespace

CabinetRegionResult applyCabinetRegion(std::array<uint8_t, 128>& eeprom,
    const std::array<uint8_t, 4>& cabinetId, CabinetRegion region)
{
    if (region == CabinetRegion::Saved) return CabinetRegionResult::Unchanged;
    if (region != CabinetRegion::Japan && region != CabinetRegion::Usa && region != CabinetRegion::Export)
        return CabinetRegionResult::Invalid;
    // Both records must be complete before changing either one. During fresh
    // initialization or BIOS copy recovery, a valid first copy is not enough.
    for (unsigned offset : {0u, RecordSize})
        if (!validChecksum(eeprom.data() + offset))
            return CabinetRegionResult::PendingInitialization;
    for (unsigned offset : {0u, RecordSize})
        if (!std::equal(cabinetId.begin(), cabinetId.end(), eeprom.begin() + offset))
            return CabinetRegionResult::Invalid;

    bool changed = false;
    for (unsigned offset : {0u, RecordSize}) {
        auto* record = eeprom.data() + offset;
        if (record[CountryOffset] == uint8_t(region)) continue;
        record[CountryOffset] = uint8_t(region);
        const auto checksum = recordChecksum(record);
        record[ChecksumOffset] = uint8_t(checksum);
        record[ChecksumOffset + 1] = uint8_t(checksum >> 8);
        changed = true;
    }
    return changed ? CabinetRegionResult::Changed : CabinetRegionResult::Unchanged;
}
CabinetRegionResult initializeErasedCabinetRegion(std::array<uint8_t, 128>& eeprom,
    const std::array<uint8_t, 128>& biosDefaults, const std::array<uint8_t, 4>& cabinetId,
    CabinetRegion region, unsigned players)
{
    if (players > 8) return CabinetRegionResult::Invalid;
    if (region == CabinetRegion::Saved && players == 0) return CabinetRegionResult::Unchanged;
    if (region != CabinetRegion::Saved && region != CabinetRegion::Japan
            && region != CabinetRegion::Usa && region != CabinetRegion::Export)
        return CabinetRegionResult::Invalid;
    if (!std::all_of(eeprom.begin(), eeprom.end(), [](uint8_t value) { return value == 0xff; }))
        return CabinetRegionResult::Invalid;
    // On erased EEPROM the BIOS builds these records in RAM, with zero CRC
    // until it saves them. Matching complete copies plus the game ID and fixed
    // system header distinguish that state from RAM still being initialized.
    if (!std::equal(biosDefaults.begin(), biosDefaults.begin() + RecordSize, biosDefaults.begin() + RecordSize)
            || !std::equal(cabinetId.begin(), cabinetId.end(), biosDefaults.begin())
            || biosDefaults[4] == 0 || biosDefaults[4] > 8 || biosDefaults[5] > 4
            || biosDefaults[8] != 0x18 || biosDefaults[10] != 4)
        return CabinetRegionResult::PendingInitialization;
    if ((biosDefaults[62] != 0 || biosDefaults[63] != 0) && !validChecksum(biosDefaults.data()))
        return CabinetRegionResult::PendingInitialization;

    auto initialized = biosDefaults;
    for (unsigned offset : {0u, RecordSize}) {
        auto* record = initialized.data() + offset;
        if (region != CabinetRegion::Saved) record[CountryOffset] = uint8_t(region);
        if (players != 0) record[4] = uint8_t(players);
        const auto checksum = recordChecksum(record);
        record[ChecksumOffset] = uint8_t(checksum);
        record[ChecksumOffset + 1] = uint8_t(checksum >> 8);
    }
    eeprom = initialized;
    return CabinetRegionResult::Changed;
}
} // namespace hikaru
