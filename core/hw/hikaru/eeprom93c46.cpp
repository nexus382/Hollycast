// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// Wire protocol: ST M93C46 datasheet DS1077, sections 5–7 and table 3.
// https://www.st.com/resource/en/datasheet/m93c46-w.pdf
// Edge/dummy-bit cross-check: MAME machine/eepromser.cpp (BSD-3-Clause).
#include "eeprom93c46.h"
#include "state_io.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

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
void Eeprom93c46::saveState(StateWriter& writer) const
{
    writer.bytes(memory_.data(), memory_.size());
    writer.value(cs_);
    writer.value(clock_);
    writer.value(output_);
    writer.value(writeEnabled_);
    writer.value(static_cast<uint8_t>(stage_));
    writer.value(static_cast<uint8_t>(armed_));
    writer.value(static_cast<uint8_t>(pending_));
    writer.value(command_);
    writer.value(static_cast<uint32_t>(bits_));
    writer.value(static_cast<uint32_t>(readBit_));
    writer.value(address_);
    writer.value(data_);
    writer.value(pendingAddress_);
    writer.value(pendingData_);
    writer.value(busyMicros_);
}

void Eeprom93c46::loadState(StateReader& reader)
{
    Eeprom93c46 staged = *this;
    reader.bytes(staged.memory_.data(), staged.memory_.size());
    reader.value(staged.cs_);
    reader.value(staged.clock_);
    reader.value(staged.output_);
    reader.value(staged.writeEnabled_);
    uint8_t stage, armed, pending;
    uint32_t bits, readBit;
    reader.value(stage);
    reader.value(armed);
    reader.value(pending);
    reader.value(staged.command_);
    reader.value(bits);
    reader.value(readBit);
    reader.value(staged.address_);
    reader.value(staged.data_);
    reader.value(staged.pendingAddress_);
    reader.value(staged.pendingData_);
    reader.value(staged.busyMicros_);

    StateReader::require(stage <= static_cast<uint8_t>(Stage::Complete), "Invalid 93C46 state stage");
    StateReader::require(armed <= static_cast<uint8_t>(Operation::EraseAll), "Invalid 93C46 armed operation");
    StateReader::require(pending <= static_cast<uint8_t>(Operation::EraseAll), "Invalid 93C46 pending operation");
    StateReader::require(staged.command_ <= 0x1ff && bits <= 9 && readBit < 8,
        "Invalid 93C46 command cursor");
    StateReader::require(staged.address_ < Size && staged.pendingAddress_ < Size
        && staged.busyMicros_ <= WriteCycleMicros, "Invalid 93C46 address or busy cycle");
    StateReader::require((pending == static_cast<uint8_t>(Operation::None)) == (staged.busyMicros_ == 0),
        "Invalid 93C46 pending write cycle");
    staged.stage_ = static_cast<Stage>(stage);
    staged.armed_ = static_cast<Operation>(armed);
    staged.pending_ = static_cast<Operation>(pending);
    staged.bits_ = bits;
    staged.readBit_ = readBit;
    StateReader::require(staged.cs_ || staged.stage_ == Stage::Start,
        "Invalid 93C46 deselected command");
    StateReader::require(staged.stage_ != Stage::Command || (bits < 9
        && staged.armed_ == Operation::None && readBit == 0),
        "Invalid 93C46 command bit count");
    StateReader::require(staged.stage_ != Stage::Data || (bits < 8 && readBit == 0
        && (staged.armed_ == Operation::Write || staged.armed_ == Operation::WriteAll)),
        "Invalid 93C46 data command");
    StateReader::require(staged.stage_ != Stage::Reading || (staged.cs_ && bits == 0
        && staged.armed_ == Operation::None),
        "Invalid 93C46 read command");
    StateReader::require(staged.stage_ != Stage::Start || (bits == 0 && readBit == 0
        && staged.armed_ == Operation::None),
        "Invalid 93C46 start state");
    StateReader::require(staged.stage_ != Stage::Complete || (readBit == 0 && (bits == 0 || bits == 8)),
        "Invalid 93C46 completed command");
    *this = std::move(staged);
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
