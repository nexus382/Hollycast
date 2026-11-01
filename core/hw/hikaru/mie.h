// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once
#include "jvs.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace hikaru {
// Sega 315-6146: real Z80 execution with the host's even-byte memory/port
// windows and a JVS I/O node. No internal ROM or uploaded firmware is bundled.
class Mie {
public:
    using GpioRead = std::function<uint8_t(unsigned port)>;
    using GpioWrite = std::function<void(unsigned port, uint8_t value)>;
    explicit Mie(const std::vector<uint8_t>& internalRom);
    ~Mie();
    Mie(const Mie&) = delete;
    Mie& operator=(const Mie&) = delete;
    void reset();
    // Cabinet JVS inputs. Motherboard switches are separate GPIO pins and can
    // be supplied through GpioRead (port 6, active-low bits 2 and 3).
    void setInputs(const JvsNode::Inputs& inputs);
    void setGpio(GpioRead read, GpioWrite write = {});
    // AICA asserts MIE pending bit 4. Return its level after port-C masking.
    bool setSoundIrq(bool asserted);
    uint32_t readHost(uint32_t address, unsigned size);
    void writeHost(uint32_t address, uint32_t data, unsigned size);
    void advanceTicks(uint32_t z80Ticks);
    uint16_t programCounter() const;
    bool addressed() const;
    // Restore only before execution; reset intentionally preserves EEPROM.
    std::array<uint8_t, 128> eepromContents() const;
    void restoreEeprom(const std::vector<uint8_t>& contents);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hikaru
