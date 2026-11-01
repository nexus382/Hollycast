// SPDX-License-Identifier: GPL-2.0-or-later
#include "hw/hikaru/sh4_peripherals.h"

#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using hikaru::Sh4Peripherals;
struct Checks {
    unsigned count = 0;
    unsigned failed = 0;
    void equal(uint32_t actual, uint32_t expected, const std::string& label)
    {
        ++count;
        if (actual != expected) {
            ++failed;
            std::cerr << label << ": expected 0x" << std::hex << expected
                      << ", got 0x" << actual << std::dec << '\n';
        }
    }
    void throws(const std::function<void()>& action, const std::string& address, const std::string& label)
    {
        ++count;
        try {
            action();
            ++failed;
            std::cerr << label << ": expected runtime_error\n";
        }
        catch (const std::runtime_error& error) {
            if (std::string(error.what()).find(address) == std::string::npos) {
                ++failed;
                std::cerr << label << ": error missing address: " << error.what() << '\n';
            }
        }
    }
};

constexpr uint32_t tstr = 0xffd80004;
constexpr uint32_t tcor(unsigned channel) { return 0xffd80008 + channel * 12; }
constexpr uint32_t tcnt(unsigned channel) { return 0xffd8000c + channel * 12; }
constexpr uint32_t tcr(unsigned channel) { return 0xffd80010 + channel * 12; }

void resetAndMasks(Checks& checks)
{
    Sh4Peripherals p;
    checks.equal(p.handles(0xff000000), true, "P4 registers claimed");
    checks.equal(p.handles(0xffffffff), true, "P4 range end claimed");
    checks.equal(p.handles(0x1fffffff), true, "physical range end claimed");
    checks.equal(p.handles(0xbf000000), false, "other virtual aliases not claimed");
    checks.equal(p.handles(0x1effffff), false, "address below peripheral range");
    checks.equal(p.handles(0), false, "RAM not claimed");
    checks.equal(p.read(0xff000030, 4), 0x040205c1, "CPU version");
    checks.equal(p.read(0x1f000030, 4), 0x040205c1, "CPU version physical alias");
    checks.equal(p.read(0xff000030, 1), 0xc1, "CPU version low byte");
    checks.equal(p.read(0xff000031, 1), 5, "CPU version second byte");
    checks.equal(p.read(0xff000032, 2), 0x0402, "CPU version high halfword");
    checks.equal(p.read(0xff800004, 2), 0x3ffc, "BCR2 reset");
    checks.equal(p.read(0xff800008, 4), 0x77777777, "WCR1 reset");
    checks.equal(p.read(0xff80000c, 4), 0xfffeefff, "WCR2 reset");
    checks.equal(p.read(0xff800010, 4), 0x07777777, "WCR3 reset");
    checks.equal(p.read(0xff800028, 2), 0, "RFCR not a fabricated ready flag");
    checks.equal(p.read(0xffe00004, 1), 0xff, "SCI baud reset");
    checks.equal(p.read(0xffe0000c, 1), 0xff, "SCI data reset");
    checks.equal(p.read(0xffe00010, 1), 0x84, "SCI status reset");
    checks.equal(p.read(0xffe80004, 1), 0xff, "SCIF baud reset");
    checks.equal(p.read(0xffe80010, 2), 0x60, "SCIF status reset");
    checks.equal(p.read(0xffe8001c, 2), 0, "SCIF FIFO reset");
    checks.equal(p.read(0xffc00000, 2), 0xe0a, "clock mode reset has CPU/bus/peripheral 1/2/4 dividers");
    checks.equal(p.read(0xffc00008, 1), 0, "watchdog counter reset");
    checks.equal(p.read(0xffc0000c, 1), 0, "watchdog control reset");
    p.write(0xffc00008, 0xa534, 2);
    checks.equal(p.read(0xffc00008, 1), 0, "watchdog counter ignores wrong key");
    p.write(0xffc00008, 0x5a34, 2);
    checks.equal(p.read(0xffc00008, 1), 0x34, "watchdog counter keyed configuration");
    p.write(0xffc0000c, 0xa507, 2);
    checks.equal(p.read(0xffc0000c, 1), 7, "disabled watchdog control keyed configuration");
    p.advance(100000);
    checks.equal(p.read(0xffc00008, 1), 0x34, "disabled watchdog does not count");
    checks.equal(p.pendingTmu(), 0, "no pending reset timers");
    for (unsigned channel = 0; channel < 3; ++channel) {
        checks.equal(p.read(tcor(channel), 4), 0xffffffff, "timer reload reset");
        checks.equal(p.read(tcnt(channel), 4), 0xffffffff, "timer count reset");
        checks.equal(p.read(tcr(channel), 2), 0, "timer control reset");
    }

    p.write(0xff000008, 0x12345678, 4);
    p.write(0x1f000009, 0xab, 1);
    checks.equal(p.read(0xff000008, 4), 0x1234ab78, "physical alias byte merge");
    p.write(0xff00000a, 0xcdef, 2);
    checks.equal(p.read(0x1f000008, 4), 0xcdefab78, "high halfword merge");
    p.write(0xff000000, 0xffffffff, 4);
    checks.equal(p.read(0xff000000, 4), 0xfffffcff, "PTEH reserved mask");
    p.write(0xff000004, 0xffffffff, 4);
    checks.equal(p.read(0xff000004, 4), 0x1ffffdff, "PTEL reserved mask");
    p.write(0xff000010, 0xffffffff, 4);
    checks.equal(p.read(0xff000010, 4), 0xfcfcff01, "MMU invalidate self-clears");
    p.write(0xff00001c, 0xffffffff, 4);
    checks.equal(p.read(0xff00001c, 4), 0x81a7, "instruction/operand cache invalidate self-clear");
    p.write(0xff00001c, 8, 1);
    checks.equal(p.read(0xff00001c, 4), 0x8100, "cache low-byte write preserves high byte");
    p.write(0xff000038, 0xffffffff, 4);
    checks.equal(p.read(0xff000038, 4), 0x1c, "store queue area mask");
    p.write(0xff800008, 0xffffffff, 4);
    checks.equal(p.read(0xff800008, 4), 0x77777777, "bus wait control reserved bits");
    p.write(0xff800004, 0xffff, 2);
    checks.equal(p.read(0xff800004, 2), 0x3ffd, "BCR2 writable mask");
    p.write(0xff800020, 0xa57e, 2);
    checks.equal(p.read(0xff800020, 2), 0x7e, "refresh count byte mask");
    p.write(0xffd00000, 0xffff, 2);
    checks.equal(p.read(0xffd00000, 2), 0x4380, "interrupt control mask");
    p.write(0xffd00004, 0x1234, 2);
    p.write(0xffd00005, 0x9a, 1);
    checks.equal(p.read(0xffd00004, 2), 0x9a34, "interrupt priority partial write");
    p.write(0xffe80000, 0xffff, 2);
    checks.equal(p.read(0xffe80000, 2), 0x7b, "SCIF mode mask");
    p.write(0xffe80008, 0xffff, 2);
    checks.equal(p.read(0xffe80008, 2), 0xfa, "SCIF control mask");
    p.write(0xff900042, 0, 1);
    p.write(0x1f940084, 0, 1);
    p.reset();
    checks.equal(p.read(0xff000008, 4), 0, "reset clears stored register");
    checks.equal(p.read(0xffe80010, 2), 0x60, "reset restores SCIF status");
}

void timerSequence(Checks& checks)
{
    Sh4Peripherals p;
    p.write(tcor(0), 4, 4);
    p.write(tcnt(0), 2, 4);
    p.write(tcr(0), 0x20, 2);
    p.advance(1000);
    checks.equal(p.read(tcnt(0), 4), 2, "stopped timer does not count");
    p.write(tstr, 1, 1);
    p.advance(15);
    checks.equal(p.read(tcnt(0), 4), 2, "timer retains fractional CPU cycles");
    p.advance(1);
    checks.equal(p.read(tcnt(0), 4), 1, "default timer tick every 16 CPU cycles");
    p.advance(16);
    checks.equal(p.read(tcnt(0), 4), 0, "count zero is not underflow");
    checks.equal(p.pendingTmu(), 0, "no early interrupt at zero");
    p.advance(15);
    checks.equal(p.read(tcnt(0), 4), 0, "zero holds until next whole tick");
    p.advance(1);
    checks.equal(p.read(tcnt(0), 4), 4, "underflow reloads TCOR");
    checks.equal(p.read(tcr(0), 2), 0x120, "underflow latches UNF");
    checks.equal(p.pendingTmu(), 1, "enabled underflow pending");
    p.advance(16 * 12);
    checks.equal(p.read(tcnt(0), 4), 2, "multiple underflows preserve remainder ticks");
    checks.equal(p.pendingTmu(), 1, "underflow remains latched");
    p.write(tcr(0), 0, 1);
    checks.equal(p.read(tcr(0), 2), 0x100, "low byte disables IRQ preserving high UNF");
    checks.equal(p.pendingTmu(), 0, "disabled timer IRQ not pending");
    p.write(tcr(0), 0x20, 1);
    checks.equal(p.pendingTmu(), 1, "enable exposes already pending underflow");
    p.write(tcr(0) + 1, 0, 1);
    checks.equal(p.read(tcr(0), 2), 0x20, "high-byte zero clears UNF only");
    checks.equal(p.pendingTmu(), 0, "clearing UNF clears pending");
    p.write(tcr(0) + 1, 1, 1);
    checks.equal(p.read(tcr(0), 2), 0x20, "software cannot set UNF");
    p.write(tstr, 0, 1);
    p.advance(100000);
    checks.equal(p.read(tcnt(0), 4), 2, "stop freezes active count");
    p.write(tstr, 1, 1);
    p.advance(16);
    checks.equal(p.read(tcnt(0), 4), 1, "restart resumes count");

    for (uint32_t mode = 0; mode < 5; ++mode) {
        p.reset();
        const uint32_t divisor = 16u << (mode * 2);
        p.write(tcnt(0), 1, 4);
        p.write(tcor(0), 3, 4);
        p.write(tcr(0), mode, 2);
        p.write(tstr, 1, 1);
        p.advance(divisor - 1);
        checks.equal(p.read(tcnt(0), 4), 1, "prescaler before first tick " + std::to_string(mode));
        p.advance(1);
        checks.equal(p.read(tcnt(0), 4), 0, "prescaler first tick " + std::to_string(mode));
        p.advance(divisor);
        checks.equal(p.read(tcnt(0), 4), 3, "prescaler underflow " + std::to_string(mode));
        checks.equal(p.read(tcr(0), 2), mode | 0x100, "disabled IRQ still sets UNF");
        checks.equal(p.pendingTmu(), 0, "UNF without UNIE not pending");
    }

    p.reset();
    p.write(tstr, 1, 1);
    p.advance(0xffffffff);
    checks.equal(p.read(tcnt(0), 4), 0xf0000000, "large elapsed interval avoids signed overflow");
    p.advance(1);
    checks.equal(p.read(tcnt(0), 4), 0xefffffff, "large interval keeps fractional cycle");
    p.write(tcnt(0), 0, 4);
    p.advance(16);
    checks.equal(p.read(tcnt(0), 4), 0xffffffff, "maximal reload has 2^32 tick period");
    p.advance(16);
    checks.equal(p.read(tcnt(0), 4), 0xfffffffe, "maximal reload advances normally");
    p.write(tcor(0), 0, 4);
    p.write(tcnt(0), 0, 4);
    p.advance(0xffffffff);
    checks.equal(p.read(tcnt(0), 4), 0, "zero reload handles many one-tick underflows");
    checks.equal(p.read(tcr(0), 2), 0x100, "zero reload sets UNF");
}

void timerIsolation(Checks& checks)
{
    Sh4Peripherals p;
    Sh4Peripherals other;
    for (unsigned channel = 0; channel < 3; ++channel) {
        p.write(tcnt(channel), channel, 4);
        p.write(tcor(channel), (channel + 1) * 10, 4);
        p.write(tcr(channel), 0x20, 2);
    }
    p.write(tstr, 7, 1);
    p.advance(16);
    checks.equal(p.read(tcnt(0), 4), 10, "channel zero reload");
    checks.equal(p.read(tcnt(1), 4), 0, "channel one reaches zero");
    checks.equal(p.read(tcnt(2), 4), 1, "channel two still counting");
    checks.equal(p.pendingTmu(), 1, "only first channel pending");
    p.advance(16);
    checks.equal(p.pendingTmu(), 3, "second channel underflows independently");
    p.advance(16);
    checks.equal(p.pendingTmu(), 7, "third channel underflows independently");
    checks.equal(p.read(tcnt(0), 4), 8, "channel zero continues");
    checks.equal(p.read(tcnt(1), 4), 19, "channel one continues");
    checks.equal(p.read(tcnt(2), 4), 30, "channel two reload");
    checks.equal(other.read(tcnt(0), 4), 0xffffffff, "another CPU's timer unaffected");
    checks.equal(other.pendingTmu(), 0, "another CPU's IRQ unaffected");
    other.write(0xff000008, 0x12345678, 4);
    checks.equal(p.read(0xff000008, 4), 0, "ordinary register state isolated");
    p.reset();
    checks.equal(p.read(tcnt(0), 4), 0xffffffff, "reset discards timer count");
    checks.equal(p.pendingTmu(), 0, "reset discards timer IRQ");
    checks.equal(other.read(0xff000008, 4), 0x12345678, "reset does not affect other CPU");
    p.advance(100000);
    checks.equal(p.read(tcnt(0), 4), 0xffffffff, "reset timer is stopped");
}

void refreshAndClockDividers(Checks& checks)
{
    Sh4Peripherals p;
    constexpr uint32_t status = 0xff80001c;
    constexpr uint32_t count = 0xff800020;
    constexpr uint32_t compare = 0xff800024;
    constexpr uint32_t refreshes = 0xff800028;
    p.write(compare, 0xa503, 2);
    p.advance(1000);
    checks.equal(p.read(count, 2), 0, "disabled refresh clock stays stopped");
    checks.equal(p.read(refreshes, 2), 0, "no fabricated refresh completion");
    p.write(status, 0xa508, 2); // CKIO/4, default CKIO = CPU/2.
    p.advance(23);
    checks.equal(p.read(count, 2), 2, "refresh increments from elapsed CKIO");
    checks.equal(p.read(refreshes, 2), 0, "refresh no early compare");
    checks.equal(p.read(status, 2), 8, "refresh no early CMF");
    p.advance(1);
    checks.equal(p.read(count, 2), 0, "compare resets RTCNT");
    checks.equal(p.read(refreshes, 2), 1, "compare increments RFCR");
    checks.equal(p.read(status, 2), 0x88, "compare sets CMF");
    p.advance(48);
    checks.equal(p.read(refreshes, 2), 3, "batch advance counts multiple refreshes");
    p.write(status, 0xa588, 2);
    checks.equal(p.read(status, 2), 0x88, "writing CMF one preserves status");
    p.write(status, 0xa508, 2);
    checks.equal(p.read(status, 2), 8, "writing CMF zero clears status");
    p.write(status, 0xa58c, 2);
    checks.equal(p.read(status, 2), 8, "software cannot set refresh status flags");
    p.write(compare, 0x0001, 2);
    checks.equal(p.read(compare, 2), 3, "invalid refresh key ignored");
    p.write(refreshes, 0x03ff, 2);
    checks.equal(p.read(refreshes, 2), 3, "invalid RFCR key ignored");
    p.write(refreshes, 0xa7ff, 2);
    p.write(compare, 0xa501, 2);
    p.advance(8);
    checks.equal(p.read(refreshes, 2), 0, "1024th refresh wraps RFCR");
    checks.equal(p.read(status, 2), 0x8c, "RFCR overflow sets OVF");
    p.write(status, 0xa509, 2);
    p.write(refreshes, 0xa5ff, 2);
    p.advance(8);
    checks.equal(p.read(refreshes, 2), 0, "LMTS selects 512-refresh limit");
    checks.equal(p.read(status, 2), 0x8d, "512-refresh limit sets OVF");
    p.write(status, 0xa509, 2);
    p.write(count, 0xa5fe, 2);
    p.write(compare, 0xa501, 2);
    p.advance(16);
    checks.equal(p.read(count, 2), 0, "refresh count can wrap before matching lower RTCOR");
    checks.equal(p.read(refreshes, 2), 0, "wrapped count has not reached RTCOR yet");
    p.advance(8);
    checks.equal(p.read(refreshes, 2), 1, "refresh comparison after counter wrap");
    Sh4Peripherals other;
    checks.equal(other.read(refreshes, 2), 0, "refresh counter isolated between CPUs");
    p.reset();
    checks.equal(p.read(refreshes, 2), 0, "reset clears accumulated refreshes");
    p.write(status, 0xa508, 2);
    p.advance(8 * 255);
    checks.equal(p.read(count, 2), 255, "RTCOR zero waits for counter wrap");
    checks.equal(p.read(refreshes, 2), 0, "RTCOR zero does not compare early");
    p.advance(8);
    checks.equal(p.read(refreshes, 2), 1, "RTCOR zero compares on wrap");

    const uint32_t prescalers[] = {0, 4, 16, 64, 256, 1024, 2048, 4096};
    for (unsigned mode = 1; mode < 8; ++mode) {
        p.reset();
        p.write(compare, 0xa501, 2);
        p.write(status, 0xa500 | (mode << 3), 2);
        p.advance(2 * prescalers[mode] - 1);
        checks.equal(p.read(refreshes, 2), 0, "refresh prescaler before tick " + std::to_string(mode));
        p.advance(1);
        checks.equal(p.read(refreshes, 2), 1, "refresh prescaler tick " + std::to_string(mode));
    }
    p.reset();
    p.write(0xffc00000, 0xe08, 2); // Peripheral divider 2 rather than 4.
    p.write(tcnt(0), 1, 4);
    p.write(tstr, 1, 1);
    p.advance(8);
    checks.equal(p.read(tcnt(0), 4), 0, "FRQCR peripheral clock changes TMU rate");
    p.reset();
    p.write(0xffc00000, 0xe1a, 2); // Bus divider 4 rather than 2.
    p.write(compare, 0xa501, 2);
    p.write(status, 0xa508, 2);
    p.advance(15);
    checks.equal(p.read(refreshes, 2), 0, "FRQCR slower bus clock delays refresh");
    p.advance(1);
    checks.equal(p.read(refreshes, 2), 1, "FRQCR bus divider drives refresh");
    p.reset();
    p.write(0xffc00000, 0xe8a, 2); // CPU divider 3; peripheral divider 4.
    p.write(tcnt(0), 1, 4);
    p.write(tstr, 1, 1);
    p.advance(5);
    checks.equal(p.read(tcnt(0), 4), 1, "rational CPU/peripheral ratio retains fraction");
    p.advance(1);
    checks.equal(p.read(tcnt(0), 4), 0, "rational CPU/peripheral ratio advances at sixth CPU cycle");
}

void serial(Checks& checks)
{
    std::vector<uint8_t> output;
    Sh4Peripherals p([&output](uint8_t byte) { output.push_back(byte); });
    Sh4Peripherals other;
    p.write(0xffe80004, 0, 1); // 10-bit frames take 1280 CPU cycles.
    p.write(0xffe8000c, 'A', 1);
    checks.equal(static_cast<uint32_t>(output.size()), 0, "TE disabled does not transmit");
    checks.equal(p.read(0xffe8001c, 2), 0x100, "disabled transmitter queues one byte");
    p.write(0xffe80008, 0x20, 2);
    checks.equal(static_cast<uint32_t>(output.size()), 1, "enabling transmitter starts queued byte");
    if (!output.empty())
        checks.equal(output[0], 'A', "first serial byte");
    checks.equal(p.read(0xffe80010, 2), 0x20, "transmit in progress clears TEND");
    checks.equal(p.read(0xffe8001c, 2), 0, "active shift register not counted in FIFO");
    p.write(0xffe8000c, 'B', 1);
    p.write(0xffe8000c, 'C', 1);
    checks.equal(p.read(0xffe8001c, 2), 0x200, "busy transmitter queues subsequent bytes");
    p.advance(1279);
    checks.equal(static_cast<uint32_t>(output.size()), 1, "serial baud delay before next byte");
    p.advance(1);
    checks.equal(static_cast<uint32_t>(output.size()), 2, "one frame advances second byte");
    p.advance(1280);
    checks.equal(static_cast<uint32_t>(output.size()), 3, "second frame advances third byte");
    if (output.size() == 3) {
        checks.equal(output[1], 'B', "FIFO preserves second byte");
        checks.equal(output[2], 'C', "FIFO preserves third byte");
    }
    checks.equal(p.read(0xffe80010, 2), 0x20, "last byte still shifting");
    p.advance(1280);
    checks.equal(p.read(0xffe80010, 2), 0x60, "finished transmitter sets TEND and TDFE");
    checks.equal(p.read(0xffe8001c, 2), 0, "finished transmit FIFO empty");
    checks.equal(other.read(0xffe80010, 2), 0x60, "other CPU serial unaffected");
    checks.equal(p.read(0xffe80014, 1), 0, "unconnected receive FIFO empty");
    checks.equal(p.read(0xffe80010, 2) & 3, 0, "no invented receive readiness");
    p.write(0xffe80010, 0, 2);
    checks.equal(p.read(0xffe80010, 2), 0x20, "read-before-zero clears TEND but not empty-FIFO TDFE");
    p.reset();
    p.write(0xffe80010, 0, 2);
    checks.equal(p.read(0xffe80010, 2), 0x60, "status cannot clear without preceding read");
    p.write(0xffe80010, 0xffff, 2);
    checks.equal(p.read(0xffe80010, 2), 0x60, "writing ones cannot create receive flags");
    p.write(0xffe80008, 0x20, 2);
    p.write(0xffe80018, 4, 2);
    p.write(0xffe8000c, 'D', 1);
    checks.equal(static_cast<uint32_t>(output.size()), 3, "FIFO reset suppresses writes");
    p.write(0xffe80018, 0, 2);
    p.write(0xffe80004, 0, 1);
    p.write(0xffe80000, 0x40, 2);
    p.write(0xffe8000c, 0xff, 1);
    checks.equal(static_cast<uint32_t>(output.size()), 4, "transmit enabled byte callback");
    if (output.size() == 4)
        checks.equal(output[3], 0x7f, "seven-bit serial mode masks data");
    p.write(0xffe8000c, 'E', 1);
    p.reset();
    p.advance(1000000);
    checks.equal(static_cast<uint32_t>(output.size()), 4, "reset drops pending serial FIFO");
    checks.equal(p.read(0xffe80010, 2), 0x60, "reset serial flags restored");
}

void gpioAndFailures(Checks& checks)
{
    Sh4Peripherals p;
    Sh4Peripherals other;
    uint16_t input = 0xa55a;
    std::vector<uint16_t> writes;
    p.setPortA([&input]() { return input; }, [&writes](uint16_t value) { writes.push_back(value); });
    checks.equal(p.read(0xff800030, 2), input, "GPIO input callback");
    checks.equal(p.read(0xff800031, 1), 0xa5, "GPIO high byte callback");
    p.write(0xff800030, 0x1234, 2);
    p.write(0xff800031, 0x56, 1);
    checks.equal(static_cast<uint32_t>(writes.size()), 2, "GPIO output callback count");
    if (writes.size() == 2) {
        checks.equal(writes[0], 0x1234, "GPIO initial output");
        checks.equal(writes[1], 0x5634, "GPIO masked output uses latch not input");
    }
    checks.equal(other.read(0xff800030, 2), 0, "GPIO callback isolated");
    p.reset();
    input = 0x789a;
    checks.equal(p.read(0xff800030, 2), input, "reset retains board GPIO wiring");
    p.setPortA({});
    checks.equal(p.read(0xff800030, 2), 0, "reset clears GPIO latch");
    checks.throws([&p]() { p.read(0xff00002c, 4); }, "ff00002c", "unknown register");
    checks.throws([&p]() { p.write(0x1fffffff, 1, 1); }, "1fffffff", "unknown physical register");
    checks.throws([&p]() { p.read(0xbf000030, 4); }, "bf000030", "unsupported alias");
    checks.throws([&p]() { p.read(0xff000001, 2); }, "ff000001", "unaligned access");
    checks.throws([&p]() { p.write(0xff000000, 1, 3); }, "ff000000", "invalid access width");
    checks.throws([&p]() { p.read(0xff000000, 0); }, "ff000000", "zero access width");
    checks.throws([&p]() { p.read(0xffe80004, 2); }, "ffe80004", "byte register rejects halfword read");
    checks.throws([&p]() { p.write(0xffe80005, 1, 1); }, "ffe80005", "byte register rejects upper byte lane");
    checks.throws([&p]() { p.read(0xffd80006, 1); }, "ffd80006", "TSTR rejects padded lane");
    checks.throws([&p]() { p.write(0xffd00004, 1, 4); }, "ffd00004", "halfword register rejects full word write");
    checks.throws([&p]() { p.write(0xff800020, 1, 1); }, "ff800020", "refresh register requires keyed word write");
    checks.throws([&p]() { p.write(0xffc00000, 0x0e07, 2); }, "1fc00000", "reserved clock divider rejected");
    checks.throws([&p]() { p.read(0xffc00008, 2); }, "ffc00008", "watchdog requires byte reads");
    checks.throws([&p]() { p.write(0xffc0000c, 0, 1); }, "ffc0000c", "watchdog requires keyed word writes");
    checks.throws([&p]() { p.write(0xffc0000c, 0xa580, 2); }, "ffc0000c", "unimplemented active watchdog stops explicitly");
    checks.equal(p.read(0xffc0000c, 1), 0, "rejected watchdog enable retains disabled state");
    checks.equal(p.read(0xffc00000, 2), 0xe0a, "rejected clock write preserves old clock");
    checks.throws([&p]() { p.write(0xff000030, 1, 4); }, "ff000030", "CPU version read only");
    checks.throws([&p]() { p.read(0xffe8000c, 1); }, "ffe8000c", "SCFTDR write only");
    checks.throws([&p]() { p.write(0xffe80014, 1, 1); }, "ffe80014", "SCFRDR read only");
    checks.throws([&p]() { p.read(0xff900000, 1); }, "ff900000", "SDRAM mode no readback");
    checks.throws([&p]() { p.write(0xff900000, 1, 2); }, "ff900000", "SDRAM mode byte access only");
    p.write(tcr(0), 6, 2);
    p.write(tstr, 1, 1);
    checks.throws([&p]() { p.advance(16); }, "1fd80010", "unsupported timer clock does not fake ticking");
    checks.equal(p.read(tcnt(0), 4), 0xffffffff, "unsupported timer does not change counter");
    p.reset();
    p.write(0xffa00040, 1, 4);
    checks.throws([&p]() { p.write(0xffa0000c, 1, 4); }, "ffa0000c", "unconnected DMA start fails");
    checks.equal(p.read(0xffa0000c, 4), 0, "rejected DMA does not set completion");
}
} // namespace

int main()
{
    Checks checks;
    try {
        resetAndMasks(checks);
        timerSequence(checks);
        timerIsolation(checks);
        refreshAndClockDividers(checks);
        serial(checks);
        gpioAndFailures(checks);
    }
    catch (const std::exception& error) {
        ++checks.failed;
        std::cerr << "Unexpected exception: " << error.what() << '\n';
    }
    std::cout << "Hikaru SH-4 peripherals: " << checks.count << " checks, " << checks.failed << " failures\n";
    return checks.failed == 0 ? 0 : 1;
}
