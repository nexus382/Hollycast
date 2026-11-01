// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Hardware register reference: MAME src/mame/sega/mie.cpp, Olivier Galibert,
// BSD-3-Clause. Firmware is executed from external inputs, never patched here.
#include "mie.h"
#include "eeprom93c46.h"
#define CHIPS_IMPL
#include "deps/chips/z80.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace hikaru {
struct Mie::Impl {
    z80_t cpu{};
    uint64_t pins = 0;
    std::array<uint8_t, 0x800> rom{};
    std::array<uint8_t, 0x8800> ram{}; // 0x7800..0xffff
    std::array<uint8_t, 8> gpio{};
    uint8_t direction = 0xff, lcr = 0, control = 0, irqEnable = 0, irqPending = 0, mapleIrqLevel = 0xff;
    std::array<uint8_t, 516> transmit{};
    std::size_t transmitSize = 0;
    bool transmitOverflow = false;
    JvsNode node;
    JvsNode::Inputs inputs;
    Eeprom93c46 eeprom;
    unsigned eepromTicks = 0;
    GpioRead gpioRead;
    GpioWrite gpioWrite;
    bool soundIrq = false;
    uint8_t boardIrqMask = 0;

    uint8_t memoryRead(uint16_t address) const {
        if (address < rom.size()) return rom[address];
        if (address >= 0x7800) return ram[address - 0x7800];
        return address < 0x7000 ? 0xff : 0; // Unconnected Maple reply/control area.
    }
    void memoryWrite(uint16_t address, uint8_t value) {
        if (address >= 0x7800) ram[address - 0x7800] = value;
        // ROM and unmapped memory have no writable backing.
    }
    uint8_t portRead(uint8_t port) {
        if (port < 8) {
            if (!(direction & (1u << port))) return gpio[port];
            if (port == 6) {
                uint8_t value = gpioRead ? gpioRead(port) : 0xff;
                value = uint8_t((value & ~1u) | (eeprom.dataOut() ? 1 : 0));
                // GPIO G bits 2/3 are motherboard switches, distinct from
                // the JVS cabinet's Test and Service inputs.
                return value;
            }
            if (port == 1) return uint8_t((gpioRead ? gpioRead(port) : 0) | (soundIrq ? 0x10 : 0));
            if (gpioRead) return gpioRead(port);
            return 0xff;
        }
        switch (port) {
        case 0x08: return direction;
        case 0x0f: return 0; // MIE ADC is unpopulated on this board.
        case 0x10: { uint8_t value = 0; if (!(lcr & 0x80)) node.popByte(value); return value; }
        case 0x15: return uint8_t(0x60 | (node.queuedBytes() ? 1 : 0));
        case 0x30: return irqEnable;
        case 0x50: return mapleIrqLevel;
        case 0x70: return irqPending;
        case 0x91: return uint8_t(0x8c | (node.addressed() ? 2 : 0));
        default: return 0xff;
        }
    }
    void gpioOutput(unsigned port, uint8_t value) {
        if (port == 2) boardIrqMask = value;
        if (port == 5) eeprom.setPins((value & 8) != 0, (value & 1) != 0, (value & 2) != 0);
        if (gpioWrite) gpioWrite(port, value);
    }
    void portWrite(uint8_t port, uint8_t value) {
        if (port < 8) {
            gpio[port] = value;
            if (!(direction & (1u << port))) gpioOutput(port, value);
            return;
        }
        switch (port) {
        case 0x08: {
            const uint8_t outputs = (direction ^ value) & ~value;
            direction = value;
            for (unsigned i = 0; i < 8; ++i)
                if (outputs & (1u << i)) gpioOutput(i, gpio[i]);
            break;
        }
        case 0x10:
            if (!(lcr & 0x80)) {
                if (transmitSize < transmit.size()) transmit[transmitSize++] = value;
                else transmitOverflow = true;
            }
            break;
        case 0x13: lcr = value; break;
        case 0x30: irqEnable = value; break;
        case 0x50: mapleIrqLevel = value; break;
        case 0x70: irqPending = value; break;
        case 0x90:
            if ((control & 1) && !(value & 1)) {
                uint8_t discarded;
                while (node.popByte(discarded)) {}
                if (!transmitOverflow) for (std::size_t i = 0; i < transmitSize; ++i)
                    node.pushByte(transmit[i]);
                transmitSize = 0;
                transmitOverflow = false;
            }
            control = value;
            break;
        default: break; // Unpopulated secondary UART/external I/O ports.
        }
    }
    void ticks(uint32_t count) {
        while (count--) {
            if (++eepromTicks == 16) { eepromTicks = 0; eeprom.advanceMicros(1); }
            const uint8_t pending = irqEnable & irqPending & 0x7f;
            if (pending) pins |= Z80_INT; else pins &= ~Z80_INT;
            pins = z80_tick(&cpu, pins);
            const uint16_t address = Z80_GET_ADDR(pins);
            if (pins & Z80_MREQ) {
                if (pins & Z80_RD) { Z80_SET_DATA(pins, memoryRead(address)); }
                else if (pins & Z80_WR) memoryWrite(address, Z80_GET_DATA(pins));
            } else if (pins & Z80_IORQ) {
                if (pins & Z80_M1) {
                    unsigned level = 0;
                    while (level < 7 && !(pending & (1u << level))) ++level;
                    if (level < 7) {
                        irqPending &= uint8_t(~(1u << level));
                        Z80_SET_DATA(pins, uint8_t(0xf2 + 2 * level));
                    } else { Z80_SET_DATA(pins, 0xff); }
                } else if (pins & Z80_RD) { Z80_SET_DATA(pins, portRead(uint8_t(address))); }
                else if (pins & Z80_WR) portWrite(uint8_t(address), Z80_GET_DATA(pins));
            }
        }
    }
};

Mie::Mie(const std::vector<uint8_t>& internalRom) : impl_(std::make_unique<Impl>())
{
    if (internalRom.size() != impl_->rom.size())
        throw std::invalid_argument("MIE internal ROM must contain exactly 2048 bytes");
    std::copy(internalRom.begin(), internalRom.end(), impl_->rom.begin());
    reset();
}
Mie::~Mie() = default;
void Mie::reset()
{
    impl_->ram.fill(0);
    impl_->gpio.fill(0);
    impl_->direction = 0xff;
    impl_->lcr = impl_->control = impl_->irqEnable = impl_->irqPending = 0;
    impl_->mapleIrqLevel = 0xff;
    impl_->transmitSize = 0;
    impl_->transmitOverflow = false;
    impl_->node.reset(impl_->inputs);
    impl_->eeprom.resetPins();
    impl_->eepromTicks = 0;
    impl_->soundIrq = false;
    impl_->boardIrqMask = 0;
    impl_->pins = z80_init(&impl_->cpu);
}
void Mie::setInputs(const JvsNode::Inputs& inputs) { impl_->inputs = inputs; impl_->node.setInputs(inputs); }
void Mie::setGpio(GpioRead read, GpioWrite write) { impl_->gpioRead = std::move(read); impl_->gpioWrite = std::move(write); }
bool Mie::setSoundIrq(bool asserted) { impl_->soundIrq = asserted; return asserted && (impl_->boardIrqMask & 0x10); }

namespace {
void validateHost(uint32_t address, unsigned size) {
    if ((size != 1 && size != 2 && size != 4) || (address & (size - 1))
        || address < 0x00800000 || uint64_t(address) + size > 0x00840000)
        throw std::invalid_argument("Invalid Hikaru MIE host access");
}
}
uint32_t Mie::readHost(uint32_t address, unsigned size)
{
    validateHost(address, size);
    uint32_t value = 0;
    for (unsigned byte = 0; byte < size; ++byte) {
        const uint32_t lane = address + byte;
        if (lane & 1) {
            // The 315-6146 occupies only the low byte of each 16-bit host
            // lane. The unconnected high byte is pulled up (open bus).
            value |= uint32_t(0xff) << (byte * 8);
            continue;
        }
        const uint8_t data = lane < 0x00820000
            ? impl_->portRead(uint8_t(lane >> 1)) : impl_->memoryRead(uint16_t(lane >> 1));
        value |= uint32_t(data) << (byte * 8);
    }
    return value;
}
void Mie::writeHost(uint32_t address, uint32_t data, unsigned size)
{
    validateHost(address, size);
    for (unsigned byte = 0; byte < size; ++byte) {
        const uint32_t lane = address + byte;
        if (lane & 1) continue;
        if (lane < 0x00820000) impl_->portWrite(uint8_t(lane >> 1), uint8_t(data >> (byte * 8)));
        else impl_->memoryWrite(uint16_t(lane >> 1), uint8_t(data >> (byte * 8)));
    }
}
void Mie::advanceTicks(uint32_t z80Ticks) { impl_->ticks(z80Ticks); }
uint16_t Mie::programCounter() const { return impl_->cpu.pc; }
bool Mie::addressed() const { return impl_->node.addressed(); }
std::array<uint8_t, 128> Mie::eepromContents() const { return impl_->eeprom.contents(); }
void Mie::restoreEeprom(const std::vector<uint8_t>& contents) { impl_->eeprom = Eeprom93c46(contents); }
} // namespace hikaru
