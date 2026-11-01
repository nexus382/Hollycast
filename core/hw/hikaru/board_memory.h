// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include "bridge.h"

#include <array>
#include <cstddef>
#include <vector>

namespace hikaru {

// Development model of the RAM/bridge portion of Hikaru. This is not a
// complete machine: CPUs, graphics, audio, ROM boards and I/O are not attached.
// Addresses here are physical bus addresses; SH4 virtual/MMU translation
// belongs to the CPU, not this component.
class BoardMemory {
public:
    enum class Cpu { Master, Slave };
    struct Devices {
        std::function<uint32_t(Cpu, uint32_t address, uint32_t mask)> read;
        std::function<void(Cpu, uint32_t address, uint32_t data, uint32_t mask)> write;
        std::function<uint32_t(Cpu, uint32_t address, uint32_t mask)> configRead;
        std::function<void(Cpu, uint32_t address, uint32_t data, uint32_t mask)> configWrite;
    };
    static constexpr uint32_t BootRomSize = 2 * 1024 * 1024;
    static constexpr uint32_t CpuRamSize = 32 * 1024 * 1024;
    static constexpr uint32_t SharedRamSize = 4 * 1024 * 1024;

    BoardMemory();
    // Bridge callbacks reference this instance, so it must keep its address.
    BoardMemory(const BoardMemory&) = delete;
    BoardMemory& operator=(const BoardMemory&) = delete;
    BoardMemory(BoardMemory&&) = delete;
    BoardMemory& operator=(BoardMemory&&) = delete;

    // A raw, little-endian 2 MiB image, loaded by the caller. An invalid image
    // throws before changing the current ROM. No Sega image is bundled.
    void loadBootRom(const std::vector<uint8_t>& image);
    void loadProgramRom(std::vector<uint8_t> image);
    void loadDataRom(std::vector<uint8_t> image);
    void setDevices(Devices devices);
    void reset(); // Clears volatile RAM/bridges, retaining the loaded boot ROM.

    // Naturally aligned accesses of 1, 2 or 4 bytes. Invalid sizes/addresses
    // throw; unmapped reads return all ones in the requested lanes and writes
    // are ignored. Unimplemented devices do not report successful boot status.
    uint32_t read(Cpu cpu, uint32_t address, unsigned size = 4) const;
    void write(Cpu cpu, uint32_t address, uint32_t data, unsigned size = 4);

    Bridge& bridge(Cpu cpu);
    const Bridge& bridge(Cpu cpu) const;

private:
    static std::size_t cpuIndex(Cpu cpu);
    uint32_t readCpuWord(Cpu cpu, uint32_t address, uint32_t mask) const;
    void writeCpuWord(Cpu cpu, uint32_t address, uint32_t data, uint32_t mask);
    uint32_t readLocalWord(Cpu cpu, uint32_t address, uint32_t mask) const;
    void writeLocalWord(Cpu cpu, uint32_t address, uint32_t data, uint32_t mask);

    std::vector<uint8_t> bootRom_;
    std::array<std::vector<uint8_t>, 2> cpuRam_;
    std::vector<uint8_t> sharedRam_;
    std::vector<uint8_t> programRom_;
    std::vector<uint8_t> dataRom_;
    Devices devices_;
    std::array<Bridge, 2> bridges_;
};

} // namespace hikaru
