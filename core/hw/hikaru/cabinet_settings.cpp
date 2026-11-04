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
} // namespace hikaru
