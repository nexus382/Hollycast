// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "hw/hikaru/mie.h"
#include "hw/hikaru/state_io.h"
#include "deps/chips/z80.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>

namespace hikaru { uint64_t testMieSkippedClocks(); }

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
    // The card-reader adapter is a separate serial block, not the JVS UART.
    hikaru::Mie serial(rom);
    auto putPort = [&](uint8_t port, uint8_t value) { serial.writeHost(0x00800000u + unsigned(port) * 2, value, 1); };
    auto getPort = [&](uint8_t port) { return serial.readHost(0x00800000u + unsigned(port) * 2, 1); };
    check(getPort(0x0d) == 0xff, "serial adapter remains absent for cabinets without a reader");
    std::vector<uint8_t> sent;
    std::deque<uint8_t> received{0x06, 0x02};
    serial.setSerialDevice([&](uint8_t value) { sent.push_back(value); },
        [&] { auto value = received.front(); received.pop_front(); return value; },
        [&] { return !received.empty(); });
    putPort(0x0a, 0xa5); putPort(0x09, 0x55);
    check(getPort(0x0d) == 3, "adapter reports busy before loopback completion");
    serial.advanceTicks(159);
    check(getPort(0x0d) == 3, "adapter completion advances only on MIE clocks");
    serial.advanceTicks(1);
    check(getPort(0x0d) == 4 && getPort(0x0b) == 0x55 && getPort(0x0c) == 0xa5,
          "loopback returns both adapter bytes with its completion flag");
    check(getPort(0x0d) == 0 && sent.empty(), "reading the reply acknowledges it without transmitting loopback bytes");
    putPort(0x0a, 0xaa); putPort(0x09, 1); putPort(0x0a, 0xbb);
    serial.advanceTicks(160);
    check(sent == std::vector<uint8_t>{0xaa} && getPort(0x0d) == 8 && getPort(0x0b) == 1 && getPort(0x0c) == 0xaa,
          "transmit latches the in-flight byte and returns its echo");

    putPort(0x0a, 0x80); putPort(0x09, 0x80); serial.advanceTicks(160);
    check(getPort(0x0b) == 0x80 && getPort(0x0c) == 0x40 && received.size() == 2,
          "status request sees waiting reader bytes without consuming them");
    putPort(0x0a, 0x81); putPort(0x09, 0x81); serial.advanceTicks(160);
    check(getPort(0x0c) == 6 && received.size() == 1, "receive request consumes exactly one reader byte");
    received.clear();
    putPort(0x0a, 0x80); putPort(0x09, 0x80); serial.advanceTicks(160);
    check(getPort(0x0c) == 0, "empty reader status reports no waiting byte");
    putPort(0x0a, 0x81); putPort(0x09, 0x81); serial.advanceTicks(160);
    check(getPort(0x0c) == 0, "empty receive does not call a reader with no available byte");
    putPort(0x0a, 0xcc); putPort(0x09, 1); putPort(0x0d, 0); serial.advanceTicks(160);
    check(sent.size() == 1 && getPort(0x0d) == 0, "adapter reset cancels an unfinished exchange");
    putPort(0x0a, 0xdd); putPort(0x09, 1); serial.reset(); serial.advanceTicks(160);
    check(sent.size() == 1 && getPort(0x0d) == 0, "MIE reset clears exchanges while retaining the connection");
    putPort(0x0a, 0xcc); putPort(0x09, 1); serial.advanceTicks(80);
    hikaru::JvsNode::Inputs shifterInputs;
    shifterInputs.players[1] = 0x1800;
    serial.setInputs(shifterInputs);
    hikaru::StateWriter serialState;
    serial.saveState(serialState);
    hikaru::Mie resumedSerial(rom);
    std::vector<uint8_t> resumedSent;
    resumedSerial.setSerialDevice([&](uint8_t value) { resumedSent.push_back(value); },
        [] { return uint8_t(0); }, [] { return false; });
    hikaru::StateReader serialReader(serialState.data());
    resumedSerial.loadState(serialReader);
    serialReader.finish();
    check(resumedSerial.inputs().players[1] == 0x1800, "MIE state restores latched cabinet gear inputs");
    auto resumedPort = [&](uint8_t port) {
        return resumedSerial.readHost(0x00800000u + unsigned(port) * 2, 1);
    };
    serial.advanceTicks(80); resumedSerial.advanceTicks(80);
    check(sent.back() == 0xcc && resumedSent == std::vector<uint8_t>{0xcc}
        && getPort(0x0d) == 8 && resumedPort(0x0d) == 8
        && getPort(0x0c) == 0xcc && resumedPort(0x0c) == 0xcc,
        "MIE state restores serial adapter countdown and reply");
    resumedSerial.reset();
    check(resumedSerial.inputs().players[1] == 0x1800, "MIE reset preserves the physical shifter position");
    auto truncatedSerial = serialState.data();
    truncatedSerial.pop_back();
    hikaru::StateWriter beforeRejectedLoad;
    resumedSerial.saveState(beforeRejectedLoad);
    bool serialStateRejected = false;
    try {
        hikaru::StateReader broken(truncatedSerial);
        resumedSerial.loadState(broken);
    } catch (const std::exception&) {
        serialStateRejected = true;
    }
    hikaru::StateWriter afterRejectedLoad;
    resumedSerial.saveState(afterRejectedLoad);
    check(serialStateRejected && beforeRejectedLoad.data() == afterRejectedLoad.data(),
        "rejected MIE state preserves CPU and adapter state");

    serial.setSerialDevice({}, {}, {});
    check(getPort(0x0d) == 0xff, "detaching the reader restores open-bus port behavior");

    // Compare complete serialized state against single-clock execution, which
    // cannot skip a loop. This includes decoder latches, both pin images, R,
    // device countdowns, and RAM rather than just final architectural registers.
    const std::array<uint8_t, 10> pollCode{0x3a,0x00,0xc0,0xfe,0x0c,0x30,0x17,0xb7,0x28,0xf6};
    constexpr uint16_t pollStart = 0x8046;
    std::vector<uint8_t> pollRom(2048, 0);
    const uint8_t boot[] = {0x31,0x00,0xff,0xed,0x56,0xfb,0xc3,0x46,0x80}; // SP; IM 1; EI; JP poll.
    const uint8_t handler[] = {0xf5,0x3e,0x01,0x32,0x01,0xc0,0xf1,0xfb,0xed,0x4d};
    std::copy_n(boot, sizeof(boot), pollRom.begin());
    std::copy_n(handler, sizeof(handler), pollRom.begin() + 0x38);
    auto writeRam = [](hikaru::Mie& target, uint16_t address, uint8_t value) {
        target.writeHost(0x00820000u + uint32_t(address) * 2, value, 1);
    };
    auto writePort = [](hikaru::Mie& target, uint8_t port, uint8_t value) {
        target.writeHost(0x00800000u + uint32_t(port) * 2, value, 1);
    };
    auto save = [](const hikaru::Mie& target) {
        hikaru::StateWriter state;
        target.saveState(state);
        return state.data();
    };
    auto load = [](hikaru::Mie& target, const std::vector<uint8_t>& state) {
        hikaru::StateReader reader(state);
        target.loadState(reader);
        reader.finish();
    };
    auto cpuState = [](const std::vector<uint8_t>& state) {
        z80_t cpu{};
        hikaru::StateReader reader(state);
        reader.pod(cpu);
        return cpu;
    };
    auto singleTicks = [](hikaru::Mie& target, uint32_t count) {
        for (uint32_t tick = 0; tick < count; ++tick) target.advanceTicks(1);
    };
    hikaru::Mie seed(pollRom), batched(pollRom), reference(pollRom);
    for (unsigned i = 0; i < pollCode.size(); ++i) writeRam(seed, uint16_t(pollStart + i), pollCode[i]);
    // Both command exits store A, making changes to the polled byte observable.
    const uint8_t lowExit[] = {0x32,0x02,0xc0,0x76}, highExit[] = {0x32,0x03,0xc0,0x76};
    for (unsigned i = 0; i < sizeof(lowExit); ++i) {
        writeRam(seed, uint16_t(pollStart + 10 + i), lowExit[i]);
        writeRam(seed, uint16_t(pollStart + 30 + i), highExit[i]);
    }
    unsigned seedTicks = 0;
    bool foundPoll = false;
    while (seedTicks < 256) {
        seed.advanceTicks(1);
        ++seedTicks;
        z80_t cpu = cpuState(save(seed));
        if (cpu.pc == pollStart + 1 && cpu.opcode == 0x28 && z80_opdone(&cpu)) {
            foundPoll = true;
            break;
        }
    }
    check(foundPoll, "generated command poll reaches its repeated fetch phase");
    const auto pollState = save(seed);
    auto compareTicks = [&](uint32_t count, const char* description) {
        batched.advanceTicks(count);
        const uint64_t skipped = hikaru::testMieSkippedClocks();
        singleTicks(reference, count);
        check(hikaru::testMieSkippedClocks() == skipped, "single-clock oracle never skips execution");
        const bool equal = save(batched) == save(reference);
        if (!equal) std::cerr << "command poll batch length " << count << '\n';
        check(equal, description);
    };
    const uint32_t batchSizes[] = {0,1,42,43,44,85,86,87,128,129,130,490,1024,4097};
    for (uint8_t refresh : {uint8_t(0x7d), uint8_t(0xfd)}) {
        auto refreshState = pollState;
        z80_t cpu = cpuState(refreshState);
        cpu.r = refresh; // Exercise wraparound with each value of the retained high bit.
        std::memcpy(refreshState.data(), &cpu, sizeof(cpu));
        for (unsigned phase = 0; phase < 43; ++phase) {
            load(seed, refreshState);
            singleTicks(seed, phase);
            const auto phaseState = save(seed);
            for (uint32_t count : batchSizes) {
                load(batched, phaseState);
                load(reference, phaseState);
                const uint64_t skipped = hikaru::testMieSkippedClocks();
                compareTicks(count, "command polling preserves full state at every cycle phase");
                if (count >= 490)
                    check(hikaru::testMieSkippedClocks() > skipped, "long idle batches execute the shortcut");
            }
        }
    }
    // Recognition is based on instructions, including both legal RAM edges.
    for (uint16_t start : {uint16_t(0x7800), uint16_t(0x9100), uint16_t(0xfff6)}) {
        auto relocatedRom = pollRom;
        relocatedRom[7] = uint8_t(start);
        relocatedRom[8] = uint8_t(start >> 8);
        hikaru::Mie relocated(relocatedRom), relocatedReference(relocatedRom);
        for (unsigned i = 0; i < pollCode.size(); ++i) {
            writeRam(relocated, uint16_t(start + i), pollCode[i]);
            writeRam(relocatedReference, uint16_t(start + i), pollCode[i]);
        }
        relocated.advanceTicks(4097);
        singleTicks(relocatedReference, 4097);
        check(save(relocated) == save(relocatedReference), "relocated RAM command poll preserves full state");
    }
    for (uint8_t command : {uint8_t(1), uint8_t(0x0c)}) {
        load(batched, pollState);
        load(reference, pollState);
        compareTicks(490, "command poll is stable before a host command");
        writeRam(batched, 0xc000, command);
        writeRam(reference, 0xc000, command);
        compareTicks(490, "host command change ends polling on the next slice");
        const uint32_t result = command < 0x0c ? 0x00838004u : 0x00838006u;
        check(batched.readHost(result, 1) == command, "command poll takes the correct command exit");
    }
    load(batched, pollState);
    load(reference, pollState);
    compareTicks(490, "command poll is stable before replacing its code");
    writeRam(batched, pollStart + 7, 0x3c); // INC A replaces OR A and exits with A=1.
    writeRam(reference, pollStart + 7, 0x3c);
    compareTicks(490, "host code replacement cannot reuse a prior loop proof");
    check(batched.readHost(0x00838004, 1) == 1, "replacement instruction executes its side effect");

    load(batched, pollState);
    load(reference, pollState);
    compareTicks(490, "command poll is stable before an interrupt");
    for (hikaru::Mie* target : {&batched, &reference}) {
        writePort(*target, 0x30, 0x40);
        writePort(*target, 0x70, 0x40);
    }
    compareTicks(490, "IRQ between slices preserves acknowledgement and resume timing");
    check(batched.readHost(0x00838002, 1) == 1, "command poll services the IM1 interrupt handler");
    load(batched, pollState);
    load(reference, pollState);
    compareTicks(17, "partial command iteration preserves its internal phase");
    const auto midPoll = save(batched);
    load(batched, midPoll);
    load(reference, midPoll);
    compareTicks(4097, "save/load resumes command polling without cached recognition");

    load(batched, pollState);
    load(reference, pollState);
    std::vector<uint8_t> batchedSent, referenceSent;
    batched.setSerialDevice([&](uint8_t value) { batchedSent.push_back(value); }, {}, {});
    reference.setSerialDevice([&](uint8_t value) { referenceSent.push_back(value); }, {}, {});
    for (hikaru::Mie* target : {&batched, &reference}) {
        writePort(*target, 0x0a, 0xa6);
        writePort(*target, 0x09, 1);
    }
    compareTicks(159, "active serial exchange prevents polling from crossing its deadline");
    check(batchedSent.empty() && referenceSent.empty() && batched.readHost(0x0080001a, 1) == 3,
        "command polling keeps serial busy through tick 159");
    compareTicks(1, "serial exchange completes on clock 160 during command polling");
    check(batchedSent == std::vector<uint8_t>{0xa6} && batchedSent == referenceSent,
        "command polling sends exactly the scheduled serial byte");
    compareTicks(490, "command polling resumes after serial completion");

    load(batched, pollState);
    load(reference, pollState);
    compareTicks((16 - (seedTicks & 15)) & 15, "align EEPROM divider before its programming deadline");
    for (hikaru::Mie* target : {&batched, &reference}) {
        writePort(*target, 8, 0xdf); // GPIO F output: CS=8, CLK=1, DI=2.
        for (const char* command : {"1001100000", "101000000010100110"}) { // EWEN; WRITE 0,A6.
            writePort(*target, 5, 8);
            for (const char* bit = command; *bit; ++bit) {
                const uint8_t value = uint8_t(8 | (*bit == '1' ? 2 : 0));
                writePort(*target, 5, value);
                writePort(*target, 5, value | 1);
            }
            writePort(*target, 5, 0);
        }
    }
    check(batched.eepromWritePending() && reference.eepromWritePending(), "polling fixture starts EEPROM programming");
    compareTicks(79999, "busy EEPROM prevents command polling from skipping its programming clocks");
    check(batched.eepromWritePending() && batched.eepromContents()[0] == 0xff,
        "EEPROM remains unchanged one MIE clock before programming completes");
    compareTicks(1, "EEPROM programming completes at its exact MIE clock deadline");
    check(!batched.eepromWritePending() && batched.eepromContents()[0] == 0xa6,
        "command polling preserves the completed EEPROM write");
    compareTicks(490, "command polling resumes after EEPROM programming");
    std::cout << checks << " MIE checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
