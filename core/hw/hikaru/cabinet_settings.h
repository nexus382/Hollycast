// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <array>
#include <cstdint>

namespace hikaru {
enum class CabinetRegion { Saved = -1, Japan = 0, Usa = 1, Export = 2 };
enum class CabinetRegionResult { Unchanged, Changed, PendingInitialization, Invalid };

// Country is part of two checksummed MIE EEPROM records, separate from scores
// and calibration. Incomplete records are left for the real BIOS to initialize.
CabinetRegionResult applyCabinetRegion(std::array<uint8_t, 128>& eeprom,
    const std::array<uint8_t, 4>& cabinetId, CabinetRegion region);
// Only an entirely erased chip may adopt the BIOS's two matching working
// copies. Existing or damaged saved settings must never be replaced by defaults.
// A nonzero player count supplies a title-required fresh cabinet default; Saved
// keeps the BIOS country when only the player count needs initialization.
CabinetRegionResult initializeErasedCabinetRegion(std::array<uint8_t, 128>& eeprom,
    const std::array<uint8_t, 128>& biosDefaults, const std::array<uint8_t, 4>& cabinetId,
    CabinetRegion region, unsigned players = 0);
} // namespace hikaru
