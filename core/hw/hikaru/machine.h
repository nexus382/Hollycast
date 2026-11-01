// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include "board_memory.h"
#include "eeprom93c46.h"
#include "roms.h"
#include "sh4_cpu.h"
#include "sh4_dma.h"
#include "sh4_peripherals.h"

#include <array>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

namespace hikaru {

// Native bring-up machine. Each successful instruction advances one model tick
// and one DMA unit. These deterministic budgets are NOT SH-4 hardware timing.
// GPU/audio/MIE/security devices remain explicit, unconnected device faults.
class Machine {
public:
    using Cpu = BoardMemory::Cpu;
    class Fault : public std::runtime_error {
    public:
        Fault(Cpu cpu, uint32_t pc, uint16_t opcode, const std::string& reason);
        Cpu cpu;
        uint32_t pc;
        uint16_t opcode;
    };

    Machine();
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
    void loadImages(RomImages images);
    void reset(); // Retains ROM images and board-device wiring.
    Sh4Cpu::Result step(Cpu cpu);
    // One paired step executes master then slave, stopping at the first fault.
    void runSteps(uint64_t pairedSteps);

    Sh4Cpu& cpu(Cpu cpu);
    const Sh4Cpu& cpu(Cpu cpu) const;
    Sh4Peripherals& peripherals(Cpu cpu);
    const Sh4Peripherals& peripherals(Cpu cpu) const;
    Sh4Dma& dma(Cpu cpu);
    BoardMemory& memory() { return memory_; }
    const BoardMemory& memory() const { return memory_; }
    std::vector<uint32_t> trace(Cpu cpu) const;

private:
    static unsigned index(Cpu cpu);
    uint32_t read(Cpu cpu, uint32_t address, unsigned size);
    void write(Cpu cpu, uint32_t address, uint32_t data, unsigned size);
    uint32_t physical(Cpu cpu, uint32_t address);
    uint32_t readPhysical(Cpu cpu, uint32_t address, unsigned size);
    void writePhysical(Cpu cpu, uint32_t address, uint32_t data, unsigned size);
    void flushStoreQueue(Cpu cpu, uint32_t address);
    void updateInterrupts(Cpu cpu);
    void writePortA(Cpu cpu, uint16_t value);

    BoardMemory memory_;
    Eeprom93c46 mainEeprom_;
    unsigned mainEepromTicks_ = 0;
    std::array<Sh4Peripherals, 2> peripherals_;
    std::array<Sh4Dma, 2> dma_;
    std::array<Sh4Cpu, 2> cpus_;
    std::array<std::array<uint8_t, 8192>, 2> onChipRam_{};
    std::array<std::array<uint8_t, 64>, 2> storeQueues_{};
    std::array<bool, 2> communicationIrq_{};
    std::array<uint16_t, 2> lastOpcode_{{0xffff, 0xffff}};
    std::array<std::deque<uint32_t>, 2> trace_;
};
} // namespace hikaru
