// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Address-map reference: MAME src/mame/sega/hikaru.cpp (BSD-3-Clause).
#include "board_memory.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace hikaru {
namespace {
constexpr uint32_t cpuRamBase = 0x0c000000;
constexpr uint32_t sharedCpuBase = 0x10000000;
constexpr uint32_t slaveRamLocalBase = 0x40000000;
constexpr uint32_t sharedLocalBase = 0x48000000;
constexpr uint32_t masterRamLocalBase = 0x70000000;

bool inRange(uint32_t address, uint32_t base, uint32_t size)
{
    return address >= base && address - base < size;
}

void validateAccess(uint32_t address, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) || address % size != 0)
        throw std::invalid_argument("Hikaru bus access must be aligned and 1, 2 or 4 bytes");
}

uint32_t sizeMask(unsigned size)
{
    return size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
}

uint32_t readWord(const std::vector<uint8_t>& memory, uint32_t offset, uint32_t mask)
{
    uint32_t value = 0;
    for (unsigned lane = 0; lane < 4; ++lane)
        value |= uint32_t(memory[offset + lane]) << (lane * 8);
    return value & mask;
}

void writeWord(std::vector<uint8_t>& memory, uint32_t offset, uint32_t data, uint32_t mask)
{
    for (unsigned lane = 0; lane < 4; ++lane)
    {
        const uint8_t byteMask = mask >> (lane * 8);
        const uint8_t byteData = data >> (lane * 8);
        memory[offset + lane] = (memory[offset + lane] & ~byteMask) | (byteData & byteMask);
    }
}
}

BoardMemory::BoardMemory()
    : bootRom_(BootRomSize, 0xff),
      cpuRam_{std::vector<uint8_t>(CpuRamSize), std::vector<uint8_t>(CpuRamSize)},
      sharedRam_(SharedRamSize),
      bridges_{
          Bridge(Bridge::Mode::Master, {
              [this](uint32_t address, uint32_t mask) { return readLocalWord(Cpu::Master, address, mask); },
              [this](uint32_t address, uint32_t data, uint32_t mask) { writeLocalWord(Cpu::Master, address, data, mask); },
              [this](uint32_t address, uint32_t mask) { return devices_.configRead ? devices_.configRead(Cpu::Master, address, mask) : mask; },
              [this](uint32_t address, uint32_t data, uint32_t mask) { if (devices_.configWrite) devices_.configWrite(Cpu::Master, address, data, mask); }}),
          Bridge(Bridge::Mode::Slave, {
              [this](uint32_t address, uint32_t mask) { return readLocalWord(Cpu::Slave, address, mask); },
              [this](uint32_t address, uint32_t data, uint32_t mask) { writeLocalWord(Cpu::Slave, address, data, mask); },
              [this](uint32_t address, uint32_t mask) { return devices_.configRead ? devices_.configRead(Cpu::Slave, address, mask) : mask; },
              [this](uint32_t address, uint32_t data, uint32_t mask) { if (devices_.configWrite) devices_.configWrite(Cpu::Slave, address, data, mask); }})}
{
}

std::size_t BoardMemory::cpuIndex(Cpu cpu)
{
    switch (cpu)
    {
    case Cpu::Master: return 0;
    case Cpu::Slave: return 1;
    }
    throw std::invalid_argument("Invalid Hikaru CPU");
}

Bridge& BoardMemory::bridge(Cpu cpu)
{
    return bridges_[cpuIndex(cpu)];
}

const Bridge& BoardMemory::bridge(Cpu cpu) const
{
    return bridges_[cpuIndex(cpu)];
}

void BoardMemory::loadBootRom(const std::vector<uint8_t>& image)
{
    if (image.size() != BootRomSize)
        throw std::invalid_argument("Hikaru boot ROM must contain exactly 2 MiB");
    bootRom_ = image;
}

void BoardMemory::loadProgramRom(std::vector<uint8_t> image)
{
    if (image.empty() || image.size() > 0x04000000)
        throw std::invalid_argument("Hikaru program ROM region must contain 1..64 MiB");
    image.resize(0x04000000, 0);
    programRom_ = std::move(image);
}

void BoardMemory::loadDataRom(std::vector<uint8_t> image)
{
    if (image.empty() || image.size() > 0x10000000)
        throw std::invalid_argument("Hikaru data ROM region must contain 1..256 MiB");
    image.resize(0x10000000, 0);
    dataRom_ = std::move(image);
}

void BoardMemory::setDevices(Devices devices)
{
    devices_ = std::move(devices);
}

void BoardMemory::reset()
{
    for (auto& ram : cpuRam_)
        std::fill(ram.begin(), ram.end(), 0);
    std::fill(sharedRam_.begin(), sharedRam_.end(), 0);
    for (auto& bridge : bridges_)
        bridge.reset();
}

uint32_t BoardMemory::read(Cpu cpu, uint32_t address, unsigned size) const
{
    cpuIndex(cpu);
    validateAccess(address, size);
    const unsigned shift = (address & 3) * 8;
    const uint32_t mask = sizeMask(size) << shift;
    return (readCpuWord(cpu, address & ~3u, mask) >> shift) & sizeMask(size);
}

void BoardMemory::write(Cpu cpu, uint32_t address, uint32_t data, unsigned size)
{
    cpuIndex(cpu);
    validateAccess(address, size);
    const unsigned shift = (address & 3) * 8;
    writeCpuWord(cpu, address & ~3u, data << shift, sizeMask(size) << shift);
}

uint32_t BoardMemory::readCpuWord(Cpu cpu, uint32_t address, uint32_t mask) const
{
    const Bridge& cpuBridge = bridge(cpu);
    if (address < BootRomSize)
        return readWord(bootRom_, address, mask);
    if (address < 0x04000000)
        return cpuBridge.apertureRead(2, address, mask);
    if (inRange(address, 0x04000000, 0x100))
        return cpuBridge.registerRead(address - 0x04000000) & mask;
    if (inRange(address, cpuRamBase, CpuRamSize))
        return readWord(cpuRam_[cpuIndex(cpu)], address - cpuRamBase, mask);
    if (cpu == Cpu::Slave && inRange(address, sharedCpuBase, SharedRamSize))
        return readWord(sharedRam_, address - sharedCpuBase, mask);
    if (inRange(address, 0x14000000, 0x04000000))
        return cpuBridge.apertureRead(0, address - 0x14000000, mask);
    if (inRange(address, 0x18000000, 0x04000000))
        return cpuBridge.apertureRead(1, address - 0x18000000, mask);
    return mask;
}

void BoardMemory::writeCpuWord(Cpu cpu, uint32_t address, uint32_t data, uint32_t mask)
{
    Bridge& cpuBridge = bridge(cpu);
    if (address < BootRomSize)
        return;
    if (address < 0x04000000)
        cpuBridge.apertureWrite(2, address, data, mask);
    else if (inRange(address, 0x04000000, 0x100))
        cpuBridge.registerWrite(address - 0x04000000, data, mask);
    else if (inRange(address, cpuRamBase, CpuRamSize))
        writeWord(cpuRam_[cpuIndex(cpu)], address - cpuRamBase, data, mask);
    else if (cpu == Cpu::Slave && inRange(address, sharedCpuBase, SharedRamSize))
        writeWord(sharedRam_, address - sharedCpuBase, data, mask);
    else if (inRange(address, 0x14000000, 0x04000000))
        cpuBridge.apertureWrite(0, address - 0x14000000, data, mask);
    else if (inRange(address, 0x18000000, 0x04000000))
        cpuBridge.apertureWrite(1, address - 0x18000000, data, mask);
}

uint32_t BoardMemory::readLocalWord(Cpu cpu, uint32_t address, uint32_t mask) const
{
    // The bridge can be programmed with unaligned DMA addresses. Reject those
    // rather than indexing beyond a RAM bank or pretending to emulate a fault.
    if (address & 3)
        throw std::invalid_argument("Unaligned Hikaru local-bus read");
    if (inRange(address, slaveRamLocalBase, CpuRamSize))
        return readWord(cpuRam_[1], address - slaveRamLocalBase, mask);
    if (inRange(address, sharedLocalBase, SharedRamSize))
        return readWord(sharedRam_, address - sharedLocalBase, mask);
    if (inRange(address, masterRamLocalBase, CpuRamSize))
        return readWord(cpuRam_[0], address - masterRamLocalBase, mask);
    if (!programRom_.empty() && inRange(address, 0x10000000, uint32_t(programRom_.size())))
        return readWord(programRom_, address - 0x10000000, mask);
    if (!dataRom_.empty() && inRange(address, 0x20000000, uint32_t(dataRom_.size())))
        return readWord(dataRom_, address - 0x20000000, mask);
    return devices_.read ? devices_.read(cpu, address, mask) : mask;
}

void BoardMemory::writeLocalWord(Cpu cpu, uint32_t address, uint32_t data, uint32_t mask)
{
    if (address & 3)
        throw std::invalid_argument("Unaligned Hikaru local-bus write");
    if (inRange(address, slaveRamLocalBase, CpuRamSize))
        writeWord(cpuRam_[1], address - slaveRamLocalBase, data, mask);
    else if (inRange(address, sharedLocalBase, SharedRamSize))
        writeWord(sharedRam_, address - sharedLocalBase, data, mask);
    else if (inRange(address, masterRamLocalBase, CpuRamSize))
        writeWord(cpuRam_[0], address - masterRamLocalBase, data, mask);
    else if ((!programRom_.empty() && inRange(address, 0x10000000, uint32_t(programRom_.size())))
          || (!dataRom_.empty() && inRange(address, 0x20000000, uint32_t(dataRom_.size()))))
        return;
    else if (devices_.write)
        devices_.write(cpu, address, data, mask);
}

} // namespace hikaru
