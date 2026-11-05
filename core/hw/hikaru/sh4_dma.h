// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace hikaru {

// SH7750 four-channel DMAC. This model supports auto-request external-memory
// transfers of 1, 2 and 4 bytes. External/on-chip requests, DDT, 8/32-byte
// transfers and bus timing are not implemented and fail explicitly when used.
// DMA callbacks receive physical addresses and a natural access width.
class Sh4Dma {
public:
    struct Bus {
        std::function<uint32_t(uint32_t address, unsigned size)> read;
        std::function<void(uint32_t address, uint32_t value, unsigned size)> write;
    };
    explicit Sh4Dma(Bus bus = {});
    void setBus(Bus bus);
    void reset();
    // Registers must be aligned, full physical 1FA000xx or P4 FFA000xx
    // addresses. mask selects active data bits in their aligned lanes.
    uint32_t readRegister(uint32_t address, uint32_t mask = 0xffffffffu);
    void writeRegister(uint32_t address, uint32_t data, uint32_t mask = 0xffffffffu);
    // Executes up to maxUnits transfers, not CPU cycles. A programmed zero
    // DMATCR denotes 2^24 units. Throws for unsupported active modes or absent
    // callbacks. Misaligned addresses set DMAOR.AE and halt all channels.
    uint32_t advance(uint32_t maxUnits);
    // Bit n is set when channel n has both TE and IE asserted.
    uint8_t pending() const;
    bool addressError() const;
    void signalNmi();
private:
    struct Channel { uint32_t sar = 0, dar = 0, count = 0, control = 0; bool teRead = false; };
    std::array<Channel, 4> channels_{};
    Bus bus_;
    uint32_t dmaor_ = 0;
    uint32_t dmaorRead_ = 0;
    unsigned roundRobin_ = 0;
};
} // namespace hikaru
