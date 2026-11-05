// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>

namespace hikaru {

// Instance-local SH7750 register model. Addresses use either the P4 FFxxxxxx
// range or its physical 1Fxxxxxx alias. Data is unshifted, little endian;
// naturally aligned accesses may be 1, 2, or 4 bytes. Unknown registers and
// unsupported operations throw rather than manufacturing device readiness.
class Sh4Peripherals {
public:
    // The sink observes a byte when its shift-register transmission starts,
    // matching the existing Flycast serial callback convention. Baud timing
    // controls FIFO draining and TEND; this is not an electrical serial line.
    using Serial = std::function<void(uint8_t)>;
    using PortARead = std::function<uint16_t()>;
    using PortAWrite = std::function<void(uint16_t)>;

    explicit Sh4Peripherals(Serial serial = {});
    bool handles(uint32_t address) const;
    uint32_t read(uint32_t address, unsigned size);
    void write(uint32_t address, uint32_t data, unsigned size);
    void advance(uint32_t cpuCycles);
    void reset();
    void setPortA(PortARead read, PortAWrite write = {});

    // Bit n means timer n has both underflow status and interrupt enable set.
    // Delivery and priority selection belong to the owning CPU, not this class.
    uint8_t pendingTmu() const;

private:
    std::map<uint32_t, uint32_t> registers_;
    std::array<uint64_t, 3> timerRemainder_{};
    uint64_t refreshRemainder_ = 0;
    Serial serial_;
    PortARead portARead_;
    PortAWrite portAWrite_;
    uint16_t serialStatusRead_ = 0;
    std::deque<uint8_t> transmitFifo_;
    uint64_t transmitCycles_ = 0;
    void startSerialTransmit();
    void advanceSerial(uint32_t cpuCycles);
    void advanceRefresh(uint32_t cpuCycles);
};

} // namespace hikaru
