// SPDX-License-Identifier: GPL-2.0-or-later
// Register reset values and masks follow core/hw/sh4/sh4_mmr.h and the
// existing Flycast modules. This model owns its state and scheduler inputs.
#include "sh4_peripherals.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace hikaru {
namespace {
constexpr uint32_t full = 0xffffffffu;
enum Permission { Read = 1, Write = 2, ReadWrite = 3 };
struct Register {
    uint32_t address;
    uint32_t initial;
    uint32_t mask;
    unsigned permission = ReadWrite;
    unsigned width = 4;
};

constexpr Register definitions[] = {
    // Cache/TLB controller. Undefined reset registers use deterministic zero.
    {0x1f000000, 0, 0xfffffcff}, // PTEH
    {0x1f000004, 0, 0x1ffffdff}, // PTEL
    {0x1f000008, 0, full},       // TTB
    {0x1f00000c, 0, full},       // TEA
    {0x1f000010, 0, 0xfcfcff05}, // MMUCR
    {0x1f000014, 0, 0xff, ReadWrite, 1}, // BASRA
    {0x1f000018, 0, 0xff, ReadWrite, 1}, // BASRB
    {0x1f00001c, 0, 0x89af},     // CCR
    {0x1f000020, 0, 0x3fc},      // TRA
    {0x1f000024, 0, 0xfff},      // EXPEVT (power-on reset)
    {0x1f000028, 0, 0xfff},      // INTEVT
    {0x1f000030, 0x040205c1, 0, Read}, // CPU_VERSION
    {0x1f000034, 0, 0xf},        // PTEA
    {0x1f000038, 0, 0x1c},       // QACR0
    {0x1f00003c, 0, 0x1c},       // QACR1
    {0x1f000044, 0, 0, Read},    // PRR
    // User-break controller programming; no break comparator is connected yet.
    {0x1f200000, 0, full}, {0x1f200004, 0, 0xf, ReadWrite, 1}, {0x1f200008, 0, 0x7f, ReadWrite, 2},
    {0x1f20000c, 0, full}, {0x1f200010, 0, 0xf, ReadWrite, 1}, {0x1f200014, 0, 0x7f, ReadWrite, 2},
    {0x1f200018, 0, full}, {0x1f20001c, 0, full}, {0x1f200020, 0, 0xc4c9, ReadWrite, 2},
    // Bus-state controller. RFCR is not forced to Naomi's bootstrap value 17.
    {0x1f800000, 0, 0x033efffd},
    {0x1f800004, 0x3ffc, 0x3ffd, ReadWrite, 2},
    {0x1f800008, 0x77777777, 0x77777777},
    {0x1f80000c, 0xfffeefff, 0xfffeefff},
    {0x1f800010, 0x07777777, 0x07777777},
    {0x1f800014, 0, 0xf8bbffff},
    {0x1f800018, 0, 0xffff, ReadWrite, 2},
    {0x1f80001c, 0, 0xff, ReadWrite, 2}, {0x1f800020, 0, 0xff, ReadWrite, 2}, {0x1f800024, 0, 0xff, ReadWrite, 2},
    {0x1f800028, 0, 0x3ff, ReadWrite, 2},
    {0x1f80002c, 0, full}, {0x1f800030, 0, 0xffff, ReadWrite, 2},
    {0x1f800040, 0, 0xff}, {0x1f800044, 0, 0xf, ReadWrite, 2}, {0x1f800048, 0, 0xffff, ReadWrite, 2},
    // DMA programming is retained, but an actual enabled transfer is rejected.
    {0x1fa00000, 0, full}, {0x1fa00004, 0, full}, {0x1fa00008, 0, 0xffffff}, {0x1fa0000c, 0, 0xff0ffff7},
    {0x1fa00010, 0, full}, {0x1fa00014, 0, full}, {0x1fa00018, 0, 0xffffff}, {0x1fa0001c, 0, 0xff0ffff7},
    {0x1fa00020, 0, full}, {0x1fa00024, 0, full}, {0x1fa00028, 0, 0xffffff}, {0x1fa0002c, 0, 0xff0afff7},
    {0x1fa00030, 0, full}, {0x1fa00034, 0, full}, {0x1fa00038, 0, 0xffffff}, {0x1fa0003c, 0, 0xff0afff7},
    {0x1fa00040, 0, 0x8307},
    // Clock mode default: Ick:Bck:Pck = 1:1/2:1/4. Divider fields are modeled;
    // PLL stabilization and externally selected clock-mode pins are not.
    {0x1fc00000, 0xe0a, 0xfff, ReadWrite, 2}, {0x1fc00004, 0, 0xff, ReadWrite, 1},
    {0x1fc00008, 0, 0xff, ReadWrite, 2}, {0x1fc0000c, 0, 0xff, ReadWrite, 2}, // Watchdog keyed word writes, byte reads.
    {0x1fc00010, 0, 0x80, ReadWrite, 1},
    // Interrupt control. SH7750 has no IPRD (unlike SH7750S/R).
    {0x1fd00000, 0, 0x4380, ReadWrite, 2}, {0x1fd00004, 0, 0xffff, ReadWrite, 2},
    {0x1fd00008, 0, 0xffff, ReadWrite, 2}, {0x1fd0000c, 0, 0xffff, ReadWrite, 2}, {0x1fd00010, 0, 0, Read, 2},
    // Timer unit: TOCR, TSTR, then TCOR/TCNT/TCR for each of three channels.
    {0x1fd80000, 0, 1, ReadWrite, 1}, {0x1fd80004, 0, 7, ReadWrite, 1},
    {0x1fd80008, full, full}, {0x1fd8000c, full, full}, {0x1fd80010, 0, 0x13f, ReadWrite, 2},
    {0x1fd80014, full, full}, {0x1fd80018, full, full}, {0x1fd8001c, 0, 0x13f, ReadWrite, 2},
    {0x1fd80020, full, full}, {0x1fd80024, full, full}, {0x1fd80028, 0, 0x3ff, ReadWrite, 2},
    {0x1fd8002c, 0, 0, Read}, // TCPR2: no capture edge has occurred.
    // SCI (unconnected receiver).
    {0x1fe00000, 0, 0xff, ReadWrite, 1}, {0x1fe00004, 0xff, 0xff, ReadWrite, 1}, {0x1fe00008, 0, 0xff, ReadWrite, 1},
    {0x1fe0000c, 0xff, 0xff, ReadWrite, 1}, {0x1fe00010, 0x84, 0xf9, ReadWrite, 1},
    {0x1fe00014, 0, 0, Read, 1}, {0x1fe0001c, 0, 0x8f, ReadWrite, 1},
    // SCIF. Receive-ready/error flags are never invented for the empty input.
    {0x1fe80000, 0, 0x7b, ReadWrite, 2}, {0x1fe80004, 0xff, 0xff, ReadWrite, 1}, {0x1fe80008, 0, 0xfa, ReadWrite, 2},
    {0x1fe8000c, 0, 0xff, Write, 1}, {0x1fe80010, 0x60, 0xff, ReadWrite, 2},
    {0x1fe80014, 0, 0, Read, 1}, {0x1fe80018, 0, 0xff, ReadWrite, 2},
    {0x1fe8001c, 0, 0, Read, 2}, {0x1fe80020, 0, 0xf3, ReadWrite, 2}, {0x1fe80024, 0, 1, ReadWrite, 2}
};

[[noreturn]] void unsupported(uint32_t address, const char* operation)
{
    std::ostringstream message;
    message << "Unsupported SH-4 peripheral " << operation << " at 0x"
            << std::hex << std::setw(8) << std::setfill('0') << address;
    throw std::runtime_error(message.str());
}

const Register& definition(uint32_t address, unsigned size)
{
    if ((address >> 24) != 0xff && (address >> 24) != 0x1f)
        unsupported(address, "address");
    if ((size != 1 && size != 2 && size != 4) || (address & (size - 1)) != 0)
        unsupported(address, "access size/alignment");
    const uint32_t base = (address & 0x1fffffffu) & ~3u;
    for (const Register& reg : definitions)
        if (reg.address == base) {
            if ((address & 3) + size > reg.width)
                unsupported(address, "register access width");
            return reg;
        }
    unsupported(address, "register");
}

uint32_t accessMask(uint32_t address, unsigned size)
{
    return (full >> ((4 - size) * 8)) << ((address & 3) * 8);
}

bool sdramModeRegister(uint32_t address)
{
    return (address & 0xffff0000u) == 0x1f900000u
        || (address & 0xffff0000u) == 0x1f940000u;
}

constexpr uint32_t timerControl(unsigned channel) { return 0x1fd80010 + channel * 12; }
constexpr uint32_t timerCount(unsigned channel) { return 0x1fd8000c + channel * 12; }
constexpr uint32_t timerReload(unsigned channel) { return 0x1fd80008 + channel * 12; }

struct ClockDividers { uint32_t cpu; uint32_t bus; uint32_t peripheral; };
ClockDividers clockDividers(uint32_t frqcr)
{
    constexpr uint32_t clocks[] = {1, 2, 3, 4, 6, 8};
    constexpr uint32_t peripheral[] = {2, 3, 4, 6, 8};
    const uint32_t cpuMode = (frqcr >> 6) & 7;
    const uint32_t busMode = (frqcr >> 3) & 7;
    const uint32_t peripheralMode = frqcr & 7;
    if (cpuMode >= 6 || busMode >= 6 || peripheralMode >= 5)
        unsupported(0x1fc00000, "reserved clock divider");
    return {clocks[cpuMode], clocks[busMode], peripheral[peripheralMode]};
}
} // namespace

Sh4Peripherals::Sh4Peripherals(Serial serial) : serial_(std::move(serial))
{
    reset();
}

bool Sh4Peripherals::handles(uint32_t address) const
{
    return (address >> 24) == 0xff || (address >> 24) == 0x1f;
}

void Sh4Peripherals::reset()
{
    registers_.clear();
    for (const Register& reg : definitions)
        registers_.emplace(reg.address, reg.initial);
    timerRemainder_.fill(0);
    refreshRemainder_ = 0;
    serialStatusRead_ = 0;
    transmitFifo_.clear();
    transmitCycles_ = 0;
}

void Sh4Peripherals::setPortA(PortARead read, PortAWrite write)
{
    portARead_ = std::move(read);
    portAWrite_ = std::move(write);
}

uint32_t Sh4Peripherals::read(uint32_t address, unsigned size)
{
    const Register& reg = definition(address, size);
    if ((reg.address == 0x1fc00008 || reg.address == 0x1fc0000c) && (size != 1 || (address & 3)))
        unsupported(address, "watchdog register read width");
    if (!(reg.permission & Read))
        unsupported(address, "read of write-only register");
    uint32_t value = registers_.at(reg.address);
    if (reg.address == 0x1f800030 && portARead_)
        value = portARead_();
    else if (reg.address == 0x1fe80010)
        serialStatusRead_ |= static_cast<uint16_t>(value & accessMask(address, size));
    else if (reg.address == 0x1fe8001c)
        value = static_cast<uint32_t>(transmitFifo_.size()) << 8;
    return (value & accessMask(address, size)) >> ((address & 3) * 8);
}

void Sh4Peripherals::write(uint32_t address, uint32_t data, unsigned size)
{
    // The address strobes an SDRAM mode command; no ready/readback register.
    if (handles(address) && sdramModeRegister(address & 0x1fffffffu)) {
        if (size != 1)
            unsupported(address, "SDRAM mode access size");
        return;
    }
    const Register& reg = definition(address, size);
    if (!(reg.permission & Write))
        unsupported(address, "write of read-only register");
    const uint32_t lanes = accessMask(address, size);
    const uint32_t shifted = data << ((address & 3) * 8);
    uint32_t& value = registers_.at(reg.address);
    const uint32_t previous = value;
    uint32_t next = (previous & ~lanes) | (shifted & lanes & reg.mask);

    // SH7750 hardware manual 10.8: WTCNT uses 5Axx, WTCSR uses A5xx.
    // Disabled configuration is real storage. Active watchdog reset/interval
    // timing is not connected yet, so enabling it must stop explicitly.
    if (reg.address == 0x1fc00008 || reg.address == 0x1fc0000c) {
        if (size != 2 || (address & 3))
            unsupported(address, "watchdog register write width");
        const uint32_t key = reg.address == 0x1fc00008 ? 0x5a00 : 0xa500;
        if ((data & 0xff00) != key)
            return;
        if (reg.address == 0x1fc0000c) {
            if (data & 0x80)
                unsupported(address, "enabled watchdog timer");
            next = (data & 0xe7) | (previous & data & 0x18); // WOVF/IOVF clear on zero.
        }
    }

    // Renesas SH7750 hardware manual 13.2.11–15: the refresh registers use
    // keyed word writes; RFCR has a 6-bit key and 10-bit counter. Counting is
    // driven below by elapsed CKIO ticks, not a bootstrap-ready constant.
    if (reg.address >= 0x1f80001c && reg.address <= 0x1f800028) {
        if (size != 2 || (address & 3) != 0)
            unsupported(address, "refresh register write width");
        const uint32_t keyMask = reg.address == 0x1f800028 ? 0xfc00 : 0xff00;
        const uint32_t key = reg.address == 0x1f800028 ? 0xa400 : 0xa500;
        if ((data & keyMask) != key)
            return;
        if (reg.address == 0x1f80001c) {
            next = (data & 0x7b) | (previous & data & 0x84);
            if ((previous ^ next) & 0x38)
                refreshRemainder_ = 0;
        }
        if (reg.address == 0x1f800020)
            refreshRemainder_ = 0;
    }
    if (reg.address == 0x1fc00000) {
        if (size != 2)
            unsupported(address, "frequency register write width");
        (void)clockDividers(next);
        if ((previous ^ next) & 0x1ff) {
            timerRemainder_.fill(0);
            refreshRemainder_ = 0;
        }
    }

    if (reg.address == 0x1f00001c)
        next &= ~0x0808u; // ICI and OCI complete immediately; no cache is attached.
    else if (reg.address == 0x1f000010)
        next &= ~4u; // TI is self-clearing.
    else if (reg.address == 0x1fe80010) {
        // Status flags clear only after a read followed by writing zero.
        uint32_t clear = serialStatusRead_ & lanes & ~shifted & 0xf3;
        const unsigned threshold[] = {8, 4, 2, 1};
        if (transmitFifo_.size() <= threshold[(registers_.at(0x1fe80018) >> 4) & 3])
            clear &= ~0x20u; // An empty FIFO still satisfies its trigger level.
        value &= ~clear;
        serialStatusRead_ &= static_cast<uint16_t>(~clear);
        return;
    }

    for (unsigned channel = 0; channel < 3; ++channel) {
        if (reg.address == timerControl(channel)) {
            // UNF and ICPF are hardware-set, write-zero-to-clear status bits.
            const uint32_t flags = channel == 2 ? 0x300 : 0x100;
            next = (next & ~flags) | (previous & flags & (~lanes | shifted));
            if ((next & 7) != (previous & 7))
                timerRemainder_[channel] = 0;
        }
        if (reg.address == timerCount(channel))
            timerRemainder_[channel] = 0;
    }

    // Enabling an unimplemented DMA must fail visibly, never report completion.
    if (reg.address == 0x1fa00040 || (reg.address >= 0x1fa0000c
            && reg.address <= 0x1fa0003c && (reg.address & 0xf) == 0xc)) {
        const uint32_t dmaor = reg.address == 0x1fa00040 ? next : registers_.at(0x1fa00040);
        for (unsigned channel = 0; channel < 4; ++channel) {
            const uint32_t control = 0x1fa0000c + channel * 16;
            const uint32_t chcr = reg.address == control ? next : registers_.at(control);
            if ((dmaor & 1) && (chcr & 3) == 1)
                unsupported(address, "DMA start (bus not connected)");
        }
    }

    value = next;
    if (reg.address == 0x1fd80004) {
        for (unsigned channel = 0; channel < 3; ++channel)
            if ((previous ^ next) & (1u << channel))
                timerRemainder_[channel] = 0;
    }
    if (reg.address == 0x1f800030 && portAWrite_ && (lanes & 0xffff))
        portAWrite_(static_cast<uint16_t>(next));

    if (reg.address == 0x1fe8000c && (lanes & 0xff)) {
        if (!(registers_.at(0x1fe80018) & 4)) {
            if (transmitFifo_.size() >= 16)
                unsupported(address, "SCIF transmit FIFO overflow");
            uint8_t byte = static_cast<uint8_t>(next);
            if (registers_.at(0x1fe80000) & 0x40)
                byte &= 0x7f;
            transmitFifo_.push_back(byte);
            startSerialTransmit();
        }
    }
    else if (reg.address == 0x1fe80008) {
        if (!(next & 0x20)) {
            transmitCycles_ = 0;
            registers_.at(0x1fe80010) |= 0x40;
        }
        else
            startSerialTransmit();
    }
    else if (reg.address == 0x1fe80018 && (next & 4)) {
        transmitFifo_.clear();
        transmitCycles_ = 0;
        registers_.at(0x1fe80010) |= 0x60;
        serialStatusRead_ &= ~0x60;
    }
}

void Sh4Peripherals::startSerialTransmit()
{
    if (transmitCycles_ != 0 || transmitFifo_.empty()
            || !(registers_.at(0x1fe80008) & 0x20) || (registers_.at(0x1fe80018) & 4))
        return;
    const uint8_t data = transmitFifo_.front();
    transmitFifo_.pop_front();
    const uint32_t mode = registers_.at(0x1fe80000);
    const uint32_t frameBits = 1 + ((mode & 0x40) ? 7 : 8)
        + ((mode & 0x20) ? 1 : 0) + ((mode & 8) ? 2 : 1);
    const ClockDividers clocks = clockDividers(registers_.at(0x1fc00000));
    transmitCycles_ = uint64_t(frameBits) * (registers_.at(0x1fe80004) + 1)
        * 32 * clocks.peripheral * (1u << ((mode & 3) * 2)) / clocks.cpu;
    registers_.at(0x1fe80010) &= ~0x40u;
    const unsigned threshold[] = {8, 4, 2, 1};
    if (transmitFifo_.size() <= threshold[(registers_.at(0x1fe80018) >> 4) & 3]) {
        registers_.at(0x1fe80010) |= 0x20;
        serialStatusRead_ &= ~0x20;
    }
    if (serial_)
        serial_(data);
}

void Sh4Peripherals::advanceSerial(uint32_t cpuCycles)
{
    uint64_t remaining = cpuCycles;
    while (transmitCycles_ != 0 && remaining >= transmitCycles_) {
        remaining -= transmitCycles_;
        transmitCycles_ = 0;
        startSerialTransmit();
        if (transmitCycles_ == 0) {
            registers_.at(0x1fe80010) |= 0x60;
            serialStatusRead_ &= ~0x60;
        }
    }
    if (transmitCycles_ != 0)
        transmitCycles_ -= remaining;
}

void Sh4Peripherals::advance(uint32_t cpuCycles)
{
    const ClockDividers clocks = clockDividers(registers_.at(0x1fc00000));
    for (unsigned channel = 0; channel < 3; ++channel) {
        if (!(registers_.at(0x1fd80004) & (1u << channel)))
            continue;
        const uint32_t control = registers_.at(timerControl(channel));
        const uint32_t mode = control & 7;
        if (mode >= 5)
            unsupported(timerControl(channel), "TMU clock mode");
        // TPSC divides Pclk by 4,16,64,256,1024. Retain the rational-clock
        // remainder so non-integral CPU/Pclk ratios do not lose timer ticks.
        const uint32_t divisor = clocks.peripheral * (4u << (mode * 2));
        const uint64_t elapsed = timerRemainder_[channel] + uint64_t(cpuCycles) * clocks.cpu;
        uint64_t ticks = elapsed / divisor;
        timerRemainder_[channel] = elapsed % divisor;
        uint32_t& count = registers_.at(timerCount(channel));
        if (ticks <= count) {
            count -= static_cast<uint32_t>(ticks);
            continue;
        }
        ticks -= uint64_t(count) + 1;
        registers_.at(timerControl(channel)) |= 0x100;
        const uint32_t reload = registers_.at(timerReload(channel));
        const uint64_t period = uint64_t(reload) + 1;
        count = reload - static_cast<uint32_t>(ticks % period);
    }
    advanceRefresh(cpuCycles);
    advanceSerial(cpuCycles);
}

void Sh4Peripherals::advanceRefresh(uint32_t cpuCycles)
{
    // SH7750 manual sections 13.2.11–14 and 10.4. The refresh counter counts
    // upward, resets on RTCOR compare, then increments the ten-bit RFCR.
    const uint32_t mode = (registers_.at(0x1f80001c) >> 3) & 7;
    if (mode == 0)
        return;
    constexpr uint32_t divisors[] = {0, 4, 16, 64, 256, 1024, 2048, 4096};
    const ClockDividers clocks = clockDividers(registers_.at(0x1fc00000));
    const uint32_t divisor = clocks.bus * divisors[mode];
    const uint64_t elapsed = refreshRemainder_ + uint64_t(cpuCycles) * clocks.cpu;
    uint64_t ticks = elapsed / divisor;
    refreshRemainder_ = elapsed % divisor;
    if (ticks == 0)
        return;
    uint32_t& count = registers_.at(0x1f800020);
    const uint32_t compare = registers_.at(0x1f800024);
    uint32_t untilCompare = (compare - count) & 0xff;
    if (untilCompare == 0)
        untilCompare = 256;
    if (ticks < untilCompare) {
        count = (count + static_cast<uint32_t>(ticks)) & 0xff;
        return;
    }
    ticks -= untilCompare;
    const uint32_t period = compare == 0 ? 256 : compare;
    const uint64_t matches = 1 + ticks / period;
    count = static_cast<uint32_t>(ticks % period);
    uint32_t& status = registers_.at(0x1f80001c);
    status |= 0x80;
    uint32_t& refreshes = registers_.at(0x1f800028);
    const uint32_t limit = (status & 1) ? 512 : 1024;
    if (refreshes >= limit) {
        status |= 4;
        refreshes = static_cast<uint32_t>((matches - 1) % limit);
    }
    else {
        const uint64_t total = refreshes + matches;
        if (total >= limit)
            status |= 4;
        refreshes = static_cast<uint32_t>(total % limit);
    }
}

uint8_t Sh4Peripherals::pendingTmu() const
{
    uint8_t pending = 0;
    for (unsigned channel = 0; channel < 3; ++channel)
        if ((registers_.at(timerControl(channel)) & 0x120) == 0x120)
            pending |= 1u << channel;
    return pending;
}
} // namespace hikaru
