// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/eeprom93c46.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using hikaru::Eeprom93c46;
struct Checks {
    unsigned count = 0, failures = 0;
    void equal(unsigned actual, unsigned expected, const std::string& label)
    {
        ++count;
        if (actual != expected) {
            ++failures;
            std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        }
    }
};
struct Wire {
    Eeprom93c46& chip;
    void begin() { chip.setPins(false, false, false); chip.setPins(true, false, false); }
    void end() { chip.setPins(false, false, false); }
    void bit(bool value) { chip.setPins(true, false, value); chip.setPins(true, true, value); }
    void bits(const char* text) { for (; *text; ++text) bit(*text == '1'); }
    uint8_t byte()
    {
        uint8_t result = 0;
        for (unsigned i = 0; i < 8; ++i) { bit(false); result = static_cast<uint8_t>((result << 1) | unsigned(chip.dataOut())); }
        return result;
    }
    void command(const char* literal) { begin(); bits(literal); end(); }
};

void pinEdgesAndReads(Checks& checks)
{
    std::vector<uint8_t> initial(128, 0xff);
    initial[0x25] = 0xa6;
    initial[0x7f] = 0x80;
    initial[0] = 1;
    initial[1] = 0xe7;
    Eeprom93c46 chip(initial);
    Wire wire{chip};
    checks.equal(chip.dataOut(), true, "CS low DO is pulled high");
    checks.equal(chip.writeEnabled(), false, "power-up disables erase/write");
    wire.begin();
    wire.bits("0000"); // Zeroes before the start bit are ignored.
    wire.bits("1100100101"); // Datasheet READ: start 1, opcode 10, address 0100101.
    checks.equal(chip.dataOut(), false, "dummy zero after final address edge");
    chip.setPins(true, false, false);
    checks.equal(chip.dataOut(), false, "falling CLK does not shift dummy bit");
    wire.bit(false);
    checks.equal(chip.dataOut(), true, "first rising read edge outputs D7");
    chip.setPins(true, true, false);
    checks.equal(chip.dataOut(), true, "steady high CLK does not shift output");
    const bool remaining[] = {false, true, false, false, true, true, false};
    for (bool expected : remaining) {
        wire.bit(false);
        checks.equal(chip.dataOut(), expected, "READ A6 follows MSB-first data sheet order");
        chip.setPins(true, false, true);
        checks.equal(chip.dataOut(), expected, "DO stable on falling clock despite DI change");
    }
    wire.end();
    checks.equal(chip.dataOut(), true, "CS falling returns DO to pull-up");
    wire.begin();
    wire.bits("1101111111"); // READ address 127.
    checks.equal(chip.dataOut(), false, "sequential read initial dummy");
    checks.equal(wire.byte(), 0x80, "sequential read last byte");
    checks.equal(wire.byte(), 1, "sequential read wraps to zero without another dummy");
    checks.equal(wire.byte(), 0xe7, "sequential read advances to next address");
    wire.end();
    chip.setPins(true, true, true); // Coincident CS/CLK rising is not a start clock.
    wire.bits("000000000");
    checks.equal(chip.dataOut(), true, "coincident CS/CLK rising edge ignored");
    wire.end();
    wire.begin(); wire.bits("110010"); wire.end(); // Aborted header.
    wire.begin(); wire.bits("1100100101");
    checks.equal(wire.byte(), 0xa6, "CS clears partial command before next read");
    wire.end();
    initial[0x25] = 0;
    checks.equal(chip.contents()[0x25], 0xa6, "external initial contents copied");
}

void writeEnableBusyAndErase(Checks& checks)
{
    Eeprom93c46 chip;
    Wire wire{chip};
    for (uint8_t byte : chip.contents()) checks.equal(byte, 0xff, "default erased contents");
    wire.begin(); wire.bits("1010100101"); wire.bits("10100110"); wire.end();
    checks.equal(chip.busy(), false, "WRITE without EWEN does not start programming");
    chip.advanceMicros(10000);
    checks.equal(chip.contents()[0x25], 0xff, "disabled WRITE leaves memory untouched");
    wire.command("1001100000"); // EWEN: start 1, opcode 00, sub-op 11, five don't-care bits.
    checks.equal(chip.writeEnabled(), true, "EWEN latches write enable");
    wire.begin(); wire.bits("1010100101"); wire.bits("10100110");
    checks.equal(chip.busy(), false, "complete data still waits for CS falling");
    checks.equal(chip.contents()[0x25], 0xff, "no early memory commit");
    wire.end();
    checks.equal(chip.busy(), true, "CS falling starts self-timed programming");
    checks.equal(chip.dataOut(), true, "busy chip with CS low has high-impedance pull-up");
    chip.setPins(true, false, false);
    checks.equal(chip.dataOut(), false, "selected busy chip drives DO low");
    wire.bits("1000000000"); // EWDS during busy is ignored.
    for (unsigned i = 0; i < 200; ++i) wire.bit(true);
    checks.equal(chip.busy(), true, "serial clocks cannot complete internal programming");
    checks.equal(chip.writeEnabled(), true, "busy ignores attempted commands");
    chip.advanceMicros(0);
    checks.equal(chip.contents()[0x25], 0xff, "zero elapsed time cannot commit");
    chip.advanceMicros(4999);
    checks.equal(chip.busy(), true, "busy remains until complete 5 ms model interval");
    checks.equal(chip.contents()[0x25], 0xff, "memory unchanged one microsecond before completion");
    chip.advanceMicros(1);
    checks.equal(chip.busy(), false, "programming completes at modeled interval");
    checks.equal(chip.dataOut(), true, "DO switches from BUSY to READY");
    checks.equal(chip.contents()[0x25], 0xa6, "programmed byte committed after elapsed time");
    // Datasheet permits a new start while CS remains high after READY.
    wire.bits("1100100101");
    checks.equal(wire.byte(), 0xa6, "new command after READY with CS held high");
    wire.end();
    wire.command("1000000000"); // EWDS.
    checks.equal(chip.writeEnabled(), false, "EWDS clears write-enable latch");
    wire.command("1110100101"); // ERASE address 0x25.
    checks.equal(chip.busy(), false, "disabled ERASE does not run");
    checks.equal(chip.contents()[0x25], 0xa6, "disabled ERASE preserves memory");
    wire.command("1001111111"); // EWEN with nonzero don't-care bits.
    wire.command("1110100101");
    checks.equal(chip.busy(), true, "ERASE begins a real internal cycle");
    checks.equal(chip.contents()[0x25], 0xa6, "ERASE does not commit immediately");
    chip.advanceMicros(5000);
    checks.equal(chip.contents()[0x25], 0xff, "ERASE commits erased byte");
}

void allMemoryAndAbortedCommands(Checks& checks)
{
    Eeprom93c46 chip;
    Wire wire{chip};
    wire.command("1001100000");
    wire.begin(); wire.bits("1000100000"); wire.bits("01011010"); wire.end(); // WRAL 5A.
    checks.equal(chip.busy(), true, "WRAL begins timed programming");
    checks.equal(chip.contents()[0], 0xff, "WRAL has no early effect");
    chip.advanceMicros(5000);
    for (uint8_t byte : chip.contents()) checks.equal(byte, 0x5a, "WRAL writes all 128 bytes");
    wire.command("1001000000"); // ERAL.
    checks.equal(chip.busy(), true, "ERAL begins timed erase");
    checks.equal(chip.contents()[127], 0x5a, "ERAL has no early effect");
    chip.advanceMicros(5000);
    for (uint8_t byte : chip.contents()) checks.equal(byte, 0xff, "ERAL erases all bytes");
    wire.begin(); wire.bits("1010100101"); wire.bits("0000000"); wire.end();
    checks.equal(chip.busy(), false, "short WRITE data aborted by CS falling");
    wire.begin(); wire.bits("1010100101"); wire.bits("00000000"); wire.bit(false); wire.end();
    checks.equal(chip.busy(), false, "overclocked WRITE rejected by pulse count");
    wire.begin(); wire.bits("1110100101"); wire.bit(false); wire.end();
    checks.equal(chip.busy(), false, "overclocked ERASE rejected by pulse count");
    wire.begin(); wire.bits("1000100000"); wire.bits("0000"); wire.end();
    checks.equal(chip.busy(), false, "short WRAL data aborted");
    chip.advanceMicros(0xffffffff);
    for (uint8_t byte : chip.contents()) checks.equal(byte, 0xff, "aborted writes cannot alter memory later");
}

void resetAndIsolation(Checks& checks)
{
    Eeprom93c46 chip, other;
    Wire wire{chip};
    wire.command("1001100000");
    wire.begin(); wire.bits("1010000000"); wire.bits("00010010"); wire.end(); // WRITE address zero, 12.
    chip.advanceMicros(1000);
    chip.resetPins();
    checks.equal(chip.writeEnabled(), true, "pin reset retains write enable");
    checks.equal(chip.busy(), true, "pin reset retains an already-running internal cycle");
    checks.equal(chip.dataOut(), true, "pin reset deasserts CS");
    chip.advanceMicros(3999);
    checks.equal(chip.contents()[0], 0xff, "pin reset cannot force completion");
    chip.advanceMicros(1);
    checks.equal(chip.contents()[0], 0x12, "internal write finishes after pin reset");
    checks.equal(other.contents()[0], 0xff, "EEPROM instances isolated");
    checks.equal(other.writeEnabled(), false, "write-enable latches isolated");
    wire.begin(); wire.bits("1010000001"); wire.bits("00110100");
    chip.resetPins(); // Completed serial command has not yet seen a falling CS edge.
    checks.equal(chip.busy(), false, "pin-reset API discards an uncommitted command");
    checks.equal(chip.contents()[1], 0xff, "pin reset does not synthesize a write edge");
    wire.begin(); wire.bits("1010000001"); wire.bits("00110100"); wire.end();
    chip.powerReset();
    checks.equal(chip.busy(), false, "power reset discards interrupted programming model");
    checks.equal(chip.writeEnabled(), false, "power reset disables erase/write");
    chip.advanceMicros(5000);
    checks.equal(chip.contents()[0], 0x12, "power reset retains existing EEPROM contents");
    checks.equal(chip.contents()[1], 0xff, "power reset cannot finish canceled programming later");
    bool rejected = false;
    try { Eeprom93c46 invalid(std::vector<uint8_t>(127)); }
    catch (const std::invalid_argument&) { rejected = true; }
    checks.equal(rejected, true, "initial contents size validated");
}
} // namespace

int main()
{
    Checks checks;
    pinEdgesAndReads(checks);
    writeEnableBusyAndErase(checks);
    allMemoryAndAbortedCommands(checks);
    resetAndIsolation(checks);
    std::cout << "93C46 serial EEPROM: " << checks.count << " checks, " << checks.failures << " failures\n";
    return checks.failures == 0 ? 0 : 1;
}
