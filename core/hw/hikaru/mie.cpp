// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
// Hardware register reference: MAME src/mame/sega/mie.cpp, Olivier Galibert,
// BSD-3-Clause. Firmware is executed from external inputs, never patched here.
#include "mie.h"
#include "eeprom93c46.h"
#include "state_io.h"
#define CHIPS_IMPL
#include "deps/chips/z80.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace hikaru {
#ifdef HIKARU_MIE_TESTING
// The standalone regression build verifies that equivalent state also comes
// from executing the shortcut. Normal player builds have no counter or hook.
static thread_local uint64_t mieSkippedClocks = 0;
uint64_t testMieSkippedClocks() { return mieSkippedClocks; }
#endif
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
    std::function<void(uint8_t)> serialSend;
    std::function<uint8_t()> serialReceive;
    std::function<bool()> serialPending;
    struct SerialExchange {
        uint8_t transmit = 0, command = 0, data = 0;
        uint8_t replyCommand = 0, replyData = 0, status = 0;
        unsigned ticks = 0;
    } serial;

    void finishSerial() {
        serial.replyCommand = serial.command;
        serial.status = serial.command == 0x55 ? 0x04 : 0x08;
        serial.replyData = serial.data;
        if (serial.command == 1) {
            serialSend(serial.data);
        } else if (serial.command != 0x55) {
            const bool waiting = serialPending && serialPending();
            serial.replyData = serial.data == 0x80 ? (waiting ? 0x40 : 0)
                : waiting && serialReceive ? serialReceive() : 0;
        }
    }

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
        if (serialSend && port >= 0x09 && port <= 0x0e) {
            // The game's uploaded Z80 extension polls completion, then reads
            // the adapter control/data echoes. Reading data acknowledges it.
            if (port == 0x0b) return serial.replyCommand;
            if (port == 0x0d) return serial.status;
            if (port == 0x0c) {
                serial.status &= uint8_t(~0x0c);
                return serial.replyData;
            }
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
        if (serialSend && port >= 0x09 && port <= 0x0e) {
            if (port == 0x0a) serial.transmit = value;
            else if (port == 0x0d || port == 0x0e) serial = {};
            else if (port == 0x09 && (value == 1 || value == 0x55
                    || ((value == 0x80 || value == 0x81) && (serial.transmit == 0x80 || serial.transmit == 0x81)))) {
                serial.command = value;
                serial.data = serial.transmit;
                serial.status = 0x03;
                // Model adapter completion after 10 us at the fixed 16 MHz
                // MIE clock. Electrical serial timings are not cycle-exact.
                serial.ticks = 160;
            }
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
        // Uploaded firmware polls a host command byte with this exact sequence:
        // LD A,(C000); CP 0C; JR NC,+17; OR A; JR Z,-10. With a zero command,
        // one iteration takes 43 clocks and five opcode refreshes, with no I/O
        // or writes. Recognition lives only within this emulation-worker slice:
        // the SH-4 host writes MIE RAM on that same worker between ticks calls.
        static constexpr std::array<uint8_t, 10> commandPoll{
            0x3a, 0x00, 0xc0, 0xfe, 0x0c, 0x30, 0x17, 0xb7, 0x28, 0xf6
        };
        z80_t checkpoint{};
        uint64_t checkpointPins = 0;
        uint32_t checkpointRemaining = 0;
        bool haveCheckpoint = false;
        while (count--) {
            if (serial.ticks && --serial.ticks == 0) finishSerial();
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
            if (count < 43 || !z80_opdone(&cpu)) continue;
            // Do not cross serial callbacks, EEPROM programming, or an interrupt.
            // With these gates, the recognized loop cannot create any internal
            // event; idle EEPROM microsecond advances have no observable effect.
            constexpr uint64_t blockedPins = Z80_INT | Z80_NMI | Z80_WAIT | Z80_HALT | Z80_RES;
            if (serial.ticks || eeprom.busy() || irqPending || cpu.int_bits ||
                ((pins | cpu.pins) & blockedPins)) {
                haveCheckpoint = false;
                continue;
            }
            if (address < 0x7800 || address > 0x10000 - commandPoll.size() || cpu.pc != address + 1 ||
                Z80_GET_DATA(pins) != commandPoll[0] || ram[0xc000 - 0x7800] ||
                !std::equal(commandPoll.begin(), commandPoll.end(), ram.begin() + address - 0x7800)) {
                // Other instruction fetches inside the loop do not end the proof.
                // A different matching loop still has to reproduce its full state.
                continue;
            }
            // Compare named fields, not structure padding. At this post-bus fetch
            // phase both pin images hold the loop address, not a refresh address.
            const bool repeated = haveCheckpoint && checkpointRemaining - count == 43 &&
                pins == checkpointPins && cpu.step == checkpoint.step && cpu.addr == checkpoint.addr &&
                cpu.dlatch == checkpoint.dlatch && cpu.opcode == checkpoint.opcode &&
                cpu.hlx_idx == checkpoint.hlx_idx && cpu.prefix_active == checkpoint.prefix_active &&
                cpu.pins == checkpoint.pins && cpu.int_bits == checkpoint.int_bits &&
                cpu.pc == checkpoint.pc && cpu.af == checkpoint.af && cpu.bc == checkpoint.bc &&
                cpu.de == checkpoint.de && cpu.hl == checkpoint.hl && cpu.ix == checkpoint.ix &&
                cpu.iy == checkpoint.iy && cpu.wz == checkpoint.wz && cpu.sp == checkpoint.sp &&
                cpu.i == checkpoint.i && cpu.af2 == checkpoint.af2 && cpu.bc2 == checkpoint.bc2 &&
                cpu.de2 == checkpoint.de2 && cpu.hl2 == checkpoint.hl2 && cpu.im == checkpoint.im &&
                cpu.iff1 == checkpoint.iff1 && cpu.iff2 == checkpoint.iff2 &&
                cpu.r == uint8_t((checkpoint.r & 0x80) | ((checkpoint.r + 5) & 0x7f));
            if (repeated) {
                const uint32_t iterations = count / 43;
                const uint32_t skipped = iterations * 43;
#ifdef HIKARU_MIE_TESTING
                mieSkippedClocks += skipped;
#endif
                cpu.r = uint8_t((cpu.r & 0x80) | ((cpu.r + iterations * 5) & 0x7f));
                eepromTicks = (eepromTicks + (skipped & 15)) & 15;
                count -= skipped;
                haveCheckpoint = false;
            } else {
                // The first real iteration may normalize A, flags, or WZ.
                checkpoint = cpu;
                checkpointPins = pins;
                checkpointRemaining = count;
                haveCheckpoint = true;
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
    impl_->serial = {};
    impl_->pins = z80_init(&impl_->cpu);
}
void Mie::setInputs(const JvsNode::Inputs& inputs) { impl_->inputs = inputs; impl_->node.setInputs(inputs); }
const JvsNode::Inputs& Mie::inputs() const { return impl_->inputs; }
float Mie::nascarSteeringForce() const { return impl_->node.nascarSteeringForce(); }
void Mie::setJvsBoardProfile(JvsNode::BoardProfile profile) { impl_->node.setBoardProfile(profile); }
void Mie::setGpio(GpioRead read, GpioWrite write) { impl_->gpioRead = std::move(read); impl_->gpioWrite = std::move(write); }
void Mie::setSerialDevice(std::function<void(uint8_t)> send,
    std::function<uint8_t()> receive, std::function<bool()> pending) {
    impl_->serialSend = std::move(send);
    impl_->serialReceive = std::move(receive);
    impl_->serialPending = std::move(pending);
    impl_->serial = {};
}
bool Mie::setSoundIrq(bool asserted) { impl_->soundIrq = asserted; return asserted && (impl_->boardIrqMask & 0x10); }

void Mie::saveState(StateWriter& writer) const
{
    const Impl& state = *impl_;
    static_assert(std::is_trivially_copyable_v<z80_t>, "Z80 state must be an opaque POD");
    writer.pod(state.cpu);
    writer.value(state.pins);
    writer.bytes(state.ram.data(), state.ram.size());
    writer.bytes(state.gpio.data(), state.gpio.size());
    writer.value(state.direction);
    writer.value(state.lcr);
    writer.value(state.control);
    writer.value(state.irqEnable);
    writer.value(state.irqPending);
    writer.value(state.mapleIrqLevel);
    writer.bytes(state.transmit.data(), state.transmit.size());
    writer.value(static_cast<uint32_t>(state.transmitSize));
    writer.value(state.transmitOverflow);
    state.node.saveState(writer);
    state.eeprom.saveState(writer);
    writer.value(state.eepromTicks);
    writer.value(state.soundIrq);
    writer.value(state.boardIrqMask);
    writer.value(state.serial.transmit);
    writer.value(state.serial.command);
    writer.value(state.serial.data);
    writer.value(state.serial.replyCommand);
    writer.value(state.serial.replyData);
    writer.value(state.serial.status);
    writer.value(state.serial.ticks);
}

void Mie::loadState(StateReader& reader)
{
    // Keep the loaded ROM and live host callbacks attached to the staged state.
    auto staged = std::make_unique<Impl>(*impl_);
    static_assert(std::is_trivially_copyable_v<z80_t>, "Z80 state must be an opaque POD");
    reader.pod(staged->cpu);
    reader.value(staged->pins);
    reader.bytes(staged->ram.data(), staged->ram.size());
    reader.bytes(staged->gpio.data(), staged->gpio.size());
    reader.value(staged->direction);
    reader.value(staged->lcr);
    reader.value(staged->control);
    reader.value(staged->irqEnable);
    reader.value(staged->irqPending);
    reader.value(staged->mapleIrqLevel);
    reader.bytes(staged->transmit.data(), staged->transmit.size());
    uint32_t transmitSize;
    reader.value(transmitSize);
    reader.value(staged->transmitOverflow);
    staged->node.loadState(reader);
    staged->eeprom.loadState(reader);
    reader.value(staged->eepromTicks);
    reader.value(staged->soundIrq);
    reader.value(staged->boardIrqMask);
    reader.value(staged->serial.transmit);
    reader.value(staged->serial.command);
    reader.value(staged->serial.data);
    reader.value(staged->serial.replyCommand);
    reader.value(staged->serial.replyData);
    reader.value(staged->serial.status);
    reader.value(staged->serial.ticks);

    constexpr uint64_t z80PinMask = (uint64_t(1) << 40) - 1;
    constexpr uint16_t maxZ80Step = 1708; // z80_tick dispatches generated cases 0 through 1708.
    StateReader::require(staged->pins <= z80PinMask && staged->cpu.pins <= z80PinMask
        && staged->cpu.step <= maxZ80Step && staged->cpu.hlx_idx <= 2 && staged->cpu.im <= 2,
        "Invalid MIE Z80 state");
    StateReader::require(transmitSize <= staged->transmit.size() && staged->eepromTicks < 16,
        "Invalid MIE transmit or EEPROM cursor");
    StateReader::require(staged->serial.ticks <= 160
        && (staged->serial.command == 0 || staged->serial.command == 1
            || staged->serial.command == 0x55 || staged->serial.command == 0x80
            || staged->serial.command == 0x81)
        && (staged->serial.replyCommand == 0 || staged->serial.replyCommand == 1
            || staged->serial.replyCommand == 0x55 || staged->serial.replyCommand == 0x80
            || staged->serial.replyCommand == 0x81)
        && (staged->serial.status == 0 || staged->serial.status == 3
            || staged->serial.status == 4 || staged->serial.status == 8)
        && ((staged->serial.ticks != 0) == (staged->serial.status == 3))
        && (!staged->serial.ticks || (staged->serial.command && staged->serialSend)),
        "Invalid MIE serial exchange");
    staged->transmitSize = transmitSize;
    staged->inputs = staged->node.inputs();
    impl_ = std::move(staged);
}

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
bool Mie::eepromWritePending() const { return impl_->eeprom.writePending(); }
void Mie::restoreEeprom(const std::vector<uint8_t>& contents) { impl_->eeprom = Eeprom93c46(contents); }
} // namespace hikaru
