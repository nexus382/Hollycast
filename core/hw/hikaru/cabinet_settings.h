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
} // namespace hikaru
