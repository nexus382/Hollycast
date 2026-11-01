// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace hikaru {
// 93C46 in 128 x 8 organization. DI and DO advance on rising CLK edges;
// READ has a dummy zero followed by MSB-first sequential bytes. Programming
// starts at CS falling and commits after an explicit 5 ms model interval.
// High-impedance DO is represented by its external pull-up (true).
class Eeprom93c46 {
public:
    static constexpr unsigned Size = 128;
    static constexpr uint32_t WriteCycleMicros = 5000;
    Eeprom93c46();
    explicit Eeprom93c46(const std::vector<uint8_t>& contents);
    void setPins(bool cs, bool clock, bool data);
    bool dataOut() const;
    void advanceMicros(uint32_t micros);
    bool busy() const { return busyMicros_ != 0; }
    bool writeEnabled() const { return writeEnabled_; }
    const std::array<uint8_t, Size>& contents() const { return memory_; }
    // Discards an unfinished command, retaining the write-enable latch and any
    // already-running internal write cycle. No CS edge/command is generated.
    void resetPins();
    // Device power-on reset: disables writes. An interrupted programming cycle
    // is discarded without modifying memory (analog power failure is unmodeled).
    void powerReset();

private:
    enum class Stage { Start, Command, Data, Reading, Complete };
    enum class Operation { None, Write, Erase, WriteAll, EraseAll };
    void risingEdge(bool data);
    void decodeCommand();
    void clearCommand();
    std::array<uint8_t, Size> memory_{};
    bool cs_ = false, clock_ = false, output_ = true, writeEnabled_ = false;
    Stage stage_ = Stage::Start;
    Operation armed_ = Operation::None, pending_ = Operation::None;
    uint16_t command_ = 0;
    unsigned bits_ = 0, readBit_ = 0;
    uint8_t address_ = 0, data_ = 0, pendingAddress_ = 0, pendingData_ = 0;
    uint32_t busyMicros_ = 0;
};
} // namespace hikaru
