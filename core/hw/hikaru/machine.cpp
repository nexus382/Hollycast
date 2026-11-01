// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "machine.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace hikaru {
namespace {
constexpr uint32_t full = 0xffffffffu;
std::runtime_error deviceError(const char* operation, uint32_t address)
{
    std::ostringstream text;
    text << operation << " at 0x" << std::hex << std::setw(8) << std::setfill('0') << address;
    return std::runtime_error(text.str());
}
void validate(uint32_t address, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4) || (address & (size - 1)))
        throw deviceError("Invalid native SH-4 access size/alignment", address);
}
uint32_t mask(unsigned size) { return full >> ((4 - size) * 8); }
bool dmacRegister(uint32_t address) { return address >= 0x1fa00000 && address < 0x1fa00044; }
bool cpuMapping(Machine::Cpu cpu, uint32_t address)
{
    return address < 0x04000000
        || (address >= 0x04000000 && address < 0x04000100)
        || (address >= 0x0c000000 && address < 0x0e000000)
        || (cpu == Machine::Cpu::Slave && address >= 0x10000000 && address < 0x10400000)
        || (address >= 0x14000000 && address < 0x1c000000);
}
template<std::size_t N>
uint32_t readBytes(const std::array<uint8_t, N>& bytes, uint32_t offset, unsigned size)
{
    uint32_t value = 0;
    for (unsigned lane = 0; lane < size; ++lane)
        value |= uint32_t(bytes[offset + lane]) << (lane * 8);
    return value;
}
template<std::size_t N>
void writeBytes(std::array<uint8_t, N>& bytes, uint32_t offset, uint32_t value, unsigned size)
{
    for (unsigned lane = 0; lane < size; ++lane)
        bytes[offset + lane] = static_cast<uint8_t>(value >> (lane * 8));
}
} // namespace

Machine::Fault::Fault(Cpu cpuValue, uint32_t pcValue, uint16_t opcodeValue, const std::string& reason)
    : std::runtime_error(reason), cpu(cpuValue), pc(pcValue), opcode(opcodeValue) {}

unsigned Machine::index(Cpu cpu)
{
    if (cpu == Cpu::Master) return 0;
    if (cpu == Cpu::Slave) return 1;
    throw std::invalid_argument("Invalid Hikaru CPU");
}

Machine::Machine()
    : peripherals_{Sh4Peripherals{}, Sh4Peripherals{}},
      dma_{Sh4Dma{}, Sh4Dma{}},
      cpus_{
          Sh4Cpu({[this](uint32_t a, unsigned s) { return read(Cpu::Master, a, s); },
                  [this](uint32_t a, uint32_t d, unsigned s) { write(Cpu::Master, a, d, s); },
                  [this](uint32_t a) { flushStoreQueue(Cpu::Master, a); }}),
          Sh4Cpu({[this](uint32_t a, unsigned s) { return read(Cpu::Slave, a, s); },
                  [this](uint32_t a, uint32_t d, unsigned s) { write(Cpu::Slave, a, d, s); },
                  [this](uint32_t a) { flushStoreQueue(Cpu::Slave, a); }})}
{
    memory_.setDevices({
        [](Cpu, uint32_t a, uint32_t) -> uint32_t { throw deviceError("Unimplemented Hikaru local-bus read", a); },
        [](Cpu, uint32_t a, uint32_t, uint32_t) { throw deviceError("Unimplemented Hikaru local-bus write", a); },
        [](Cpu, uint32_t a, uint32_t) -> uint32_t { throw deviceError("Unimplemented Hikaru PCI configuration read", a); },
        [](Cpu, uint32_t a, uint32_t, uint32_t) { throw deviceError("Unimplemented Hikaru PCI configuration write", a); }
    });
    for (Cpu id : {Cpu::Master, Cpu::Slave}) {
        dma(id).setBus({
            [this, id](uint32_t a, unsigned s) { return readPhysical(id, a & 0x1fffffffu, s); },
            [this, id](uint32_t a, uint32_t d, unsigned s) { writePhysical(id, a & 0x1fffffffu, d, s); }
        });
        peripherals(id).setPortA([this, id]() -> uint16_t {
            return id == Cpu::Master ? uint16_t(0x3c0 | (mainEeprom_.dataOut() ? 0x20 : 0)) : 0x3c0;
        },
            [this, id](uint16_t value) { writePortA(id, value); });
    }
}

Sh4Cpu& Machine::cpu(Cpu id) { return cpus_[index(id)]; }
const Sh4Cpu& Machine::cpu(Cpu id) const { return cpus_[index(id)]; }
Sh4Peripherals& Machine::peripherals(Cpu id) { return peripherals_[index(id)]; }
const Sh4Peripherals& Machine::peripherals(Cpu id) const { return peripherals_[index(id)]; }
Sh4Dma& Machine::dma(Cpu id) { return dma_[index(id)]; }

void Machine::loadImages(RomImages images)
{
    if (images.boot.size() != BoardMemory::BootRomSize || images.program.empty()
            || images.program.size() > 0x04000000 || images.data.empty() || images.data.size() > 0x10000000)
        throw std::invalid_argument("Native Hikaru requires a 2 MiB boot ROM and nonempty program/data regions within board limits");
    memory_.loadBootRom(images.boot);
    memory_.loadProgramRom(std::move(images.program));
    memory_.loadDataRom(std::move(images.data));
    reset();
}

void Machine::reset()
{
    memory_.reset();
    for (auto& p : peripherals_) p.reset();
    for (auto& d : dma_) d.reset();
    for (auto& c : cpus_) c.reset();
    for (auto& ram : onChipRam_) ram.fill(0);
    for (auto& queue : storeQueues_) queue.fill(0);
    communicationIrq_.fill(false);
    mainEeprom_.resetPins(); // A board reset is not an EEPROM power cycle.
    mainEepromTicks_ = 0;
    lastOpcode_.fill(0xffff);
    for (auto& history : trace_) history.clear();
}

uint32_t Machine::physical(Cpu id, uint32_t address)
{
    if (address >= 0x80000000 && address < 0xc0000000)
        return address & 0x1fffffffu; // P1/P2 bypass address translation.
    if (address < 0x80000000) {
        if (peripherals(id).read(0xff000010, 4) & 1)
            throw deviceError("Unsupported P0 access with SH-4 MMU enabled", address);
        return address & 0x1fffffffu; // MMU-off P0 drives the same 29 external address lines.
    }
    throw deviceError("Unsupported native SH-4 virtual address", address);
}

uint32_t Machine::readPhysical(Cpu id, uint32_t address, unsigned size)
{
    validate(address, size);
    if (!cpuMapping(id, address))
        throw deviceError("Unmapped Hikaru CPU-bus read", address);
    return memory_.read(id, address, size);
}
void Machine::writePhysical(Cpu id, uint32_t address, uint32_t data, unsigned size)
{
    validate(address, size);
    if (!cpuMapping(id, address))
        throw deviceError("Unmapped Hikaru CPU-bus write", address);
    memory_.write(id, address, data, size);
}

uint32_t Machine::read(Cpu id, uint32_t address, unsigned size)
{
    validate(address, size);
    uint32_t value;
    if (peripherals(id).handles(address)) {
        const uint32_t p = address & 0x1fffffffu;
        const unsigned shift = (p & 3) * 8;
        if (dmacRegister(p))
            value = (dma(id).readRegister(p & ~3u, mask(size) << shift) >> shift) & mask(size);
        else if ((p & ~3u) == 0x1f000028)
            value = (cpu(id).state().intevt >> shift) & mask(size);
        else
            value = peripherals(id).read(address, size);
    }
    else if (address >= 0x7c000000 && address < 0x7c002000)
        value = readBytes(onChipRam_[index(id)], address - 0x7c000000, size);
    else if (address >= 0xe0000000 && address < 0xe4000000)
        throw deviceError("Unsupported store-queue read", address);
    else
        value = readPhysical(id, physical(id, address), size);
    if (address == cpu(id).state().pc && size == 2)
        lastOpcode_[index(id)] = static_cast<uint16_t>(value);
    return value;
}

void Machine::write(Cpu id, uint32_t address, uint32_t data, unsigned size)
{
    validate(address, size);
    if (peripherals(id).handles(address)) {
        const uint32_t p = address & 0x1fffffffu;
        if (dmacRegister(p)) {
            const unsigned shift = (p & 3) * 8;
            dma(id).writeRegister(p & ~3u, data << shift, mask(size) << shift);
        }
        else if ((p & ~3u) == 0x1f000028)
            throw deviceError("Guest write to CPU-owned INTEVT", address);
        else
            peripherals(id).write(address, data, size);
    }
    else if (address >= 0x7c000000 && address < 0x7c002000)
        writeBytes(onChipRam_[index(id)], address - 0x7c000000, data, size);
    else if (address >= 0xe0000000 && address < 0xe4000000)
        writeBytes(storeQueues_[index(id)], address & 63, data, size);
    else
        writePhysical(id, physical(id, address), data, size);
}

void Machine::flushStoreQueue(Cpu id, uint32_t address)
{
    const unsigned queue = (address >> 5) & 1;
    if (peripherals(id).read(0xff000010, 4) & 1)
        throw deviceError("Unsupported store-queue translation with MMU enabled", address);
    const uint32_t qacr = peripherals(id).read(0xff000038 + queue * 4, 4);
    const uint32_t destination = ((qacr & 0x1c) << 24) | (address & 0x03ffffe0);
    for (unsigned word = 0; word < 8; ++word)
        writePhysical(id, destination + word * 4,
            readBytes(storeQueues_[index(id)], queue * 32 + word * 4, 4), 4);
}

void Machine::writePortA(Cpu id, uint16_t value)
{
    if (id == Cpu::Master)
        mainEeprom_.setPins((value & 0x10) != 0, (value & 8) != 0, (value & 4) != 0);
    // GPIO pin 0 is only driven when its PCTRA function is output.
    if (peripherals(id).read(0xff80002c, 4) & 1)
        communicationIrq_[1 - index(id)] = !(value & 1);
}

void Machine::updateInterrupts(Cpu id)
{
    unsigned priority = 0;
    uint32_t event = 0;
    const auto candidate = [&](uint32_t code, unsigned level) {
        if (level > priority) { priority = level; event = code; }
    };
    const bool bridgeIrq = memory_.bridge(id).irqAsserted();
    const bool communication = communicationIrq_[index(id)];
    if (bridgeIrq || communication) {
        if (!(peripherals(id).read(0xffd00000, 2) & 0x80))
            throw std::runtime_error("Unsupported encoded IRL mode for active Hikaru board interrupt");
        // Renesas SH7750 hardware manual table 19.4, independent IRL mode.
        if (bridgeIrq) candidate(0x2a0, 10); // bridge drives /IRL1
        if (communication) candidate(0x360, 4); // other CPU's PA0 drives /IRL3
    }
    const unsigned dmaPriority = (peripherals(id).read(0xffd0000c, 2) >> 8) & 15;
    const uint8_t dmaPending = dma(id).pending();
    for (unsigned channel = 0; channel < 4; ++channel)
        if (dmaPending & (1u << channel)) candidate(0x640 + channel * 0x20, dmaPriority);
    if (dma(id).addressError()) candidate(0x6c0, dmaPriority);
    const uint16_t ipra = static_cast<uint16_t>(peripherals(id).read(0xffd00004, 2));
    const uint8_t timerPending = peripherals(id).pendingTmu();
    for (unsigned channel = 0; channel < 3; ++channel)
        if (timerPending & (1u << channel)) candidate(0x400 + channel * 0x20, (ipra >> (12 - channel * 4)) & 15);
    if (priority != 0)
        cpu(id).interrupt(event, priority);
}

Sh4Cpu::Result Machine::step(Cpu id)
{
    const unsigned i = index(id);
    uint32_t faultPc = cpu(id).state().pc;
    lastOpcode_[i] = 0xffff;
    try {
        updateInterrupts(id);
        const uint32_t pc = cpu(id).state().pc;
        faultPc = pc;
        trace_[i].push_back(pc);
        if (trace_[i].size() > 32) trace_[i].pop_front();
        lastOpcode_[i] = 0xffff;
        const Sh4Cpu::Result result = cpu(id).step();
        if (result.status != Sh4Cpu::Status::Executed)
            throw Fault(id, result.pc, result.opcode,
                result.status == Sh4Cpu::Status::UnsupportedDelaySlot ? "Unsupported SH-4 delay-slot instruction"
                : result.status == Sh4Cpu::Status::MissingBus ? "Disconnected SH-4 bus" : "Unsupported SH-4 opcode");
        peripherals(id).advance(1); // Placeholder model tick, not an instruction's cycle count.
        if (id == Cpu::Master && ++mainEepromTicks_ == 200) {
            // The provisional one-tick/instruction budget uses a 200 MHz
            // master clock. Slave instructions do not double the board time.
            mainEeprom_.advanceMicros(1);
            mainEepromTicks_ = 0;
        }
        dma(id).advance(1);
        memory_.bridge(id).advanceDma(1);
        return result;
    }
    catch (const Fault&) { throw; }
    catch (const std::exception& error) {
        throw Fault(id, faultPc, lastOpcode_[i], error.what());
    }
}

void Machine::runSteps(uint64_t pairedSteps)
{
    for (uint64_t step = 0; step < pairedSteps; ++step) {
        this->step(Cpu::Master);
        this->step(Cpu::Slave);
    }
}
std::vector<uint32_t> Machine::trace(Cpu id) const
{
    const auto& history = trace_[index(id)];
    return {history.begin(), history.end()};
}
} // namespace hikaru
