// SPDX-License-Identifier: BSD-3-Clause
// Based on MAME's Sega 315-6154 device, copyright Samuele Zannoli.
// https://github.com/mamedev/mame/blob/43b995da4a2af46c9f48c6a5536953439adfb1ab/src/mame/sega/315-6154.cpp
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace hikaru {

// Sega 315-6154 SH-4/local-bus bridge. Addresses and masks are byte-based;
// accesses are aligned 32-bit words, with one bits in mask selecting active
// lanes. The caller places byte/halfword data in the corresponding lanes.
class Bridge {
public:
    enum class Mode : uint32_t { Master = 0, Slave = 0xffffffffu };
    using Read = std::function<uint32_t(uint32_t address, uint32_t mask)>;
    using Write = std::function<void(uint32_t address, uint32_t data, uint32_t mask)>;
    using Irq = std::function<void(bool asserted)>;
    struct Bus {
        Read read;
        Write write;
        Read configRead;
        Write configWrite;
    };

    explicit Bridge(Mode mode, Bus bus = {}, Irq irq = {});
    void reset();

    // Register offsets cover 0x00..0xff. Invalid or unaligned reads return
    // all ones; invalid or unaligned writes are ignored.
    uint32_t registerRead(uint32_t offset) const;
    void registerWrite(uint32_t offset, uint32_t data, uint32_t mask = 0xffffffffu);

    // Apertures consist of four 16 MiB pages. Invalid indices, offsets, or
    // unaligned accesses read as all ones and writes are ignored.
    uint32_t apertureRead(unsigned aperture, uint32_t offset, uint32_t mask = 0xffffffffu) const;
    void apertureWrite(unsigned aperture, uint32_t offset, uint32_t data, uint32_t mask = 0xffffffffu) const;

    // Raise status bits in the low half of register 0x04. A write-one to a
    // status bit acknowledges it; its corresponding high-half bit masks IRQ.
    void setIrqStatus(uint16_t bits);
    bool irqAsserted() const;

    // Advance an active DMA by at most maxWords 32-bit words. Timing is not
    // modeled. Source and destination are captured on start; writes to DMA
    // registers while busy are ignored. A nonempty transfer needs both local
    // bus callbacks. Returns the number of words transferred.
    uint32_t advanceDma(uint32_t maxWords);

private:
    uint32_t targetAddress(unsigned aperture, uint32_t offset) const;
    bool isConfig(unsigned aperture, uint32_t offset) const;
    void updateIrq();

    Mode mode_;
    Bus bus_;
    Irq irq_;
    std::array<uint32_t, 64> registers_{};
    bool irqAsserted_ = false;
    uint32_t dmaSource_ = 0;
    uint32_t dmaDestination_ = 0;
    uint32_t dmaRemaining_ = 0;
};

} // namespace hikaru
