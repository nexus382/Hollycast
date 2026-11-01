// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 The Hollycast Authors
#pragma once
#include "crypto3155881.h"
#include <array>
#include <cstdint>
#include <optional>

namespace hikaru {
// ROM-board protection RAM and Sega 315-5881, on the 315-6154 local bus.
// Owns all device state; no copyrighted firmware or ROM data is supplied.
class Protection {
public:
    // A missing key represents a board without the 315-5881.
    explicit Protection(std::optional<uint32_t> key = 0x291b02c7);
    Protection(const Protection&) = delete;
    Protection& operator=(const Protection&) = delete;
    void reset();
    uint64_t read(uint32_t address, unsigned size);
    void write(uint32_t address, uint64_t data, unsigned size);
private:
    std::array<uint8_t, 65536> ram_{};
    std::optional<Crypto3155881> crypto_;
};
} // namespace hikaru
