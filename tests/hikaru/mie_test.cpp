// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/mie.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

int main()
{
    unsigned checks = 0, failures = 0;
    auto check = [&](bool result, const char* description) {
        ++checks;
        if (!result) { ++failures; std::cerr << description << '\n'; }
    };
    auto rejects = [&](auto action, const char* description) {
        try { action(); check(false, description); }
        catch (const std::invalid_argument&) { check(true, description); }
    };
    std::vector<uint8_t> rom(2048, 0);
    // Generated Z80 program: LD A,5A; LD (8000),A; HALT.
    rom[0] = 0x3e; rom[1] = 0x5a; rom[2] = 0x32; rom[3] = 0; rom[4] = 0x80; rom[5] = 0x76;
    hikaru::Mie mie(rom);
    auto blank = mie.eepromContents();
    for (uint8_t byte : blank) check(byte == 0xff, "new EEPROM is erased");
    std::vector<uint8_t> persistent(128);
    for (unsigned i = 0; i < persistent.size(); ++i) persistent[i] = uint8_t(i * 37);
    rejects([&] { mie.restoreEeprom(std::vector<uint8_t>(127)); }, "EEPROM image size checked");
    mie.restoreEeprom(persistent);
    mie.reset();
    const auto restored = mie.eepromContents();
    check(std::equal(restored.begin(), restored.end(), persistent.begin()), "EEPROM survives MIE reset");
    hikaru::Mie recreated(rom);
    recreated.restoreEeprom(std::vector<uint8_t>(restored.begin(), restored.end()));
    check(recreated.eepromContents() == restored, "EEPROM roundtrip across MIE recreation");
    check(mie.readHost(0x00800006, 1) == 0xff, "sound reset pin pulled high when GPIO is input");
    mie.writeHost(0x00800010, 0xf7, 2); // port 3 output
    mie.writeHost(0x00800006, 0xdf, 1);
    check((mie.readHost(0x00800006, 1) & 0x20) == 0, "SND_RES asserted through GPIO port 3");
    mie.writeHost(0x00800006, 0xff, 1);
    check((mie.readHost(0x00800006, 1) & 0x20) != 0, "SND_RES released through GPIO port 3");
    mie.reset();
    check(!mie.setSoundIrq(true), "AICA IRQ masked at reset");
    check(mie.readHost(0x00800002, 1) == 0x10, "MIE port B reports raw AICA pending level");
    mie.writeHost(0x00800010, 0xfb, 2); // port C output
    mie.writeHost(0x00800004, 0x10, 1);
    check(mie.setSoundIrq(true), "MIE port C unmasks AICA IRQ");
    check(!mie.setSoundIrq(false) && mie.readHost(0x00800002, 1) == 0, "AICA IRQ clears when source deasserts");
    mie.setSoundIrq(true);
    mie.writeHost(0x00800004, 0, 1);
    check(!mie.setSoundIrq(true), "MIE mask suppresses an asserted source");
    mie.reset();
    check(mie.readHost(0x00830000, 2) == 0xff00, "RAM initially clear");
    mie.advanceTicks(100);
    check(mie.readHost(0x00830000, 2) == 0xff5a, "actual Z80 instructions write host-visible RAM");
    check(mie.readHost(0x00830001, 1) == 0xff, "unconnected odd lane");
    mie.writeHost(0x00830004, 0x00cd00ab, 4);
    check(mie.readHost(0x00830004, 4) == 0xffcdffab, "even-byte 32-bit host window");
    mie.writeHost(0x00830005, 0xff, 1);
    check(mie.readHost(0x00830004, 4) == 0xffcdffab, "odd lane writes ignored");
    const auto firstRomByte = mie.readHost(0x00820000, 2);
    mie.writeHost(0x00820000, 0, 2);
    check(mie.readHost(0x00820000, 2) == firstRomByte, "internal ROM is read-only");
    mie.writeHost(0x0082f000, 0x80, 2);
    check(mie.readHost(0x0082f000, 2) == 0xff80, "host boot flag maps to Z80 7800");
    // MOV.W sign-extends a 16-bit input. Open high-byte bits must not
    // turn an unpressed, active-low input into phantom switch presses.
    const uint16_t dipWord = uint16_t(mie.readHost(0x00800000, 2));
    check(dipWord == 0xffff && ~int32_t(int16_t(dipWord)) == 0,
          "MOV.W of unpressed GPIO has no phantom active-low inputs");
    mie.reset();
    check(mie.readHost(0x00830000, 2) == 0xff00 && mie.programCounter() == 0, "reset clears RAM and begins at actual internal ROM");

    // Standard encoded JVS broadcast assignment, sent through real UART ports.
    mie.writeHost(0x00800120, 1, 2);
    for (uint8_t byte : {0xe0,0xff,0x03,0xf1,0x01,0xf4}) mie.writeHost(0x00800020, byte, 2);
    check(!mie.addressed(), "UART transaction waits for transmit commit");
    mie.writeHost(0x00800120, 0, 2);
    check(mie.addressed(), "JVS node accepts committed address assignment");
    check(mie.readHost(0x00800122, 2) == 0xff8e, "addressed sense line");
    check(mie.readHost(0x0080002a, 2) == 0xff61, "UART receive-ready from actual response queue");
    for (uint8_t expected : {0xe0,0x00,0x03,0x01,0x01,0x05})
        check(mie.readHost(0x00800020, 2) == (0xff00u | expected), "encoded JVS assignment response");
    check(mie.readHost(0x0080002a, 2) == 0xff60, "receive-ready clears after response consumed");

    hikaru::JvsNode::Inputs inputs;
    inputs.test = inputs.service = true;
    mie.setInputs(inputs);
    check(mie.readHost(0x0080000c, 2) == 0xffff, "JVS controls do not assert motherboard switches");
    mie.setGpio([](unsigned port) -> uint8_t { return port == 6 ? 0xf3 : 0xff; });
    check(mie.readHost(0x0080000c, 2) == 0xfff3, "mainboard test/service switches use active-low GPIO");
    mie.writeHost(0x00800010, 0, 2); // GPIO direction: output.
    mie.writeHost(0x0080000c, 0x55, 2);
    check(mie.readHost(0x0080000c, 2) == 0xff55, "GPIO output latch readback");
    rejects([&] { mie.readHost(0x00840000, 1); }, "host range bounds checked");
    rejects([&] { mie.writeHost(0x00830001, 0, 2); }, "host alignment checked");
    rejects([&] { mie.readHost(0x00830000, 8); }, "host width checked");
    rejects([&] { hikaru::Mie invalid(std::vector<uint8_t>(2047)); }, "internal ROM size checked");

    // A guest overflowing UART storage cannot terminate or corrupt its host.
    mie.writeHost(0x00800120, 1, 2);
    for (unsigned i = 0; i < 2000; ++i) mie.writeHost(0x00800020, 0xe0, 2);
    mie.writeHost(0x00800120, 0, 2);
    check(mie.readHost(0x0080002a, 2) == 0xff60, "overflowed guest UART frame dropped");
    mie.reset();
    check(!mie.addressed(), "reset removes JVS address");
    std::cout << checks << " MIE checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
