// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Wire protocol: ST M93C46 datasheet DS1077, sections 5–7 and table 3.
// https://www.st.com/resource/en/datasheet/m93c46-w.pdf
// Edge/dummy-bit cross-check: MAME machine/eepromser.cpp (BSD-3-Clause).
#include "eeprom93c46.h"

#include <algorithm>
#include <stdexcept>

namespace hikaru {
Eeprom93c46::Eeprom93c46()
{
    memory_.fill(0xff);
}
Eeprom93c46::Eeprom93c46(const std::vector<uint8_t>& contents)
{
    if (contents.size() != Size)
        throw std::invalid_argument("93C46 in 8-bit organization requires exactly 128 bytes");
    std::copy(contents.begin(), contents.end(), memory_.begin());
}
void Eeprom93c46::clearCommand()
{
    stage_ = Stage::Start;
    armed_ = Operation::None;
    command_ = 0;
    bits_ = readBit_ = 0;
    address_ = data_ = 0;
    output_ = true;
}
void Eeprom93c46::resetPins()
{
    cs_ = clock_ = false;
    clearCommand();
}
void Eeprom93c46::powerReset()
{
    writeEnabled_ = false;
    busyMicros_ = 0;
    pending_ = Operation::None;
    resetPins();
}
bool Eeprom93c46::dataOut() const
{
    return !cs_ || (busyMicros_ == 0 && output_);
}
void Eeprom93c46::setPins(bool cs, bool clock, bool data)
{
    const bool wasCs = cs_, wasClock = clock_;
    cs_ = cs;
    clock_ = clock;
    if (!cs) {
        if (wasCs && stage_ == Stage::Complete && armed_ != Operation::None && writeEnabled_ && !busy()) {
            pending_ = armed_;
            pendingAddress_ = address_;
            pendingData_ = data_;
            busyMicros_ = WriteCycleMicros;
        }
        clearCommand();
        return;
    }
    if (!wasCs) {
        clearCommand();
        return; // Ignore a clock rise coincident with CS assertion.
    }
    if (!busy() && !wasClock && clock)
        risingEdge(data);
}
void Eeprom93c46::decodeCommand()
{
    const unsigned opcode = (command_ >> 7) & 3;
    address_ = command_ & 0x7f;
    bits_ = 0;
    if (opcode == 2) {
        stage_ = Stage::Reading;
        readBit_ = 0;
        output_ = false; // Dummy zero directly after the final address edge.
    }
    else if (opcode == 1) {
        armed_ = Operation::Write;
        stage_ = Stage::Data;
    }
    else if (opcode == 3) {
        armed_ = Operation::Erase;
        stage_ = Stage::Complete;
    }
    else {
        switch (address_ >> 5) {
        case 0: writeEnabled_ = false; stage_ = Stage::Complete; break; // EWDS
        case 1: armed_ = Operation::WriteAll; stage_ = Stage::Data; break; // WRAL
        case 2: armed_ = Operation::EraseAll; stage_ = Stage::Complete; break; // ERAL
        case 3: writeEnabled_ = true; stage_ = Stage::Complete; break; // EWEN
        }
    }
}
void Eeprom93c46::risingEdge(bool data)
{
    switch (stage_) {
    case Stage::Start:
        if (data) { stage_ = Stage::Command; command_ = 0; bits_ = 0; }
        break;
    case Stage::Command:
        command_ = static_cast<uint16_t>((command_ << 1) | unsigned(data));
        if (++bits_ == 9) decodeCommand();
        break;
    case Stage::Data:
        data_ = static_cast<uint8_t>((data_ << 1) | unsigned(data));
        if (++bits_ == 8) stage_ = Stage::Complete;
        break;
    case Stage::Reading:
        output_ = (memory_[address_] & (0x80u >> readBit_)) != 0;
        if (++readBit_ == 8) {
            readBit_ = 0;
            address_ = (address_ + 1) & 0x7f;
        }
        break;
    case Stage::Complete:
        // DS1077 clock-pulse counter: extra clocks before CS falling abort a
        // programming command, rather than programming an unintended address.
        armed_ = Operation::None;
        break;
    }
}
void Eeprom93c46::advanceMicros(uint32_t micros)
{
    if (!busy())
        return;
    if (micros < busyMicros_) {
        busyMicros_ -= micros;
        return;
    }
    switch (pending_) {
    case Operation::Write: memory_[pendingAddress_] = pendingData_; break;
    case Operation::Erase: memory_[pendingAddress_] = 0xff; break;
    case Operation::WriteAll: memory_.fill(pendingData_); break;
    case Operation::EraseAll: memory_.fill(0xff); break;
    case Operation::None: break;
    }
    busyMicros_ = 0;
    pending_ = Operation::None;
    clearCommand(); // READY accepts a new start bit, including while CS is high.
}
} // namespace hikaru
