// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/machine.h"

#include <functional>
#include <iostream>
#include <string>

namespace {
using hikaru::Machine;
using Cpu = Machine::Cpu;
constexpr Cpu master = Cpu::Master, slave = Cpu::Slave;
constexpr uint32_t ram = 0x0c000000, virtualRam = 0xac000000;
struct Checks {
    unsigned count = 0, failures = 0;
    void equal(uint32_t actual, uint32_t expected, const std::string& label)
    {
        ++count;
        if (actual != expected) {
            ++failures;
            std::cerr << label << ": expected 0x" << std::hex << expected << ", got 0x" << actual << std::dec << '\n';
        }
    }
    void fault(const std::function<void()>& action, Cpu cpu, uint32_t pc, uint16_t opcode,
               const std::string& reason, const std::string& label)
    {
        ++count;
        try { action(); ++failures; std::cerr << label << ": expected machine fault\n"; }
        catch (const Machine::Fault& failure) {
            equal(static_cast<uint32_t>(failure.cpu), static_cast<uint32_t>(cpu), label + " CPU");
            equal(failure.pc, pc, label + " PC");
            equal(failure.opcode, opcode, label + " opcode");
            equal(std::string(failure.what()).find(reason) != std::string::npos, true, label + " diagnostic");
        }
    }
};
void op(Machine& machine, Cpu id, uint32_t offset, uint16_t code)
{
    machine.memory().write(id, ram + offset, code, 2);
}
void execute(Machine& machine, Cpu id, uint16_t code)
{
    op(machine, id, 0, code);
    machine.cpu(id).state().pc = virtualRam;
    machine.step(id);
}
void store(Machine& machine, Cpu id, uint32_t address, uint32_t value, unsigned size = 4)
{
    machine.cpu(id).state().r[0] = value;
    machine.cpu(id).state().r[1] = address;
    execute(machine, id, static_cast<uint16_t>(0x2100 + (size == 4 ? 2 : size == 2 ? 1 : 0)));
}
uint32_t load(Machine& machine, Cpu id, uint32_t address, unsigned size = 4)
{
    machine.cpu(id).state().r[1] = address;
    execute(machine, id, static_cast<uint16_t>(0x6210 + (size == 4 ? 2 : size == 2 ? 1 : 0)));
    return machine.cpu(id).state().r[2];
}

void communicationAndAliases(Machine& machine, Checks& checks)
{
    machine.memory().bridge(master).registerWrite(0x10, 0x48);
    op(machine, master, 0, 0x2102); // MOV.L R0,@R1
    op(machine, master, 2, 0x7001); // ADD #1,R0
    op(machine, master, 4, 0xaffc); // BRA back to offset 0
    op(machine, master, 6, 0x0009);
    op(machine, slave, 0, 0x6212); // MOV.L @R1,R2
    op(machine, slave, 2, 0x0009);
    op(machine, slave, 4, 0xaffc);
    op(machine, slave, 6, 0x0009);
    machine.cpu(master).state().pc = machine.cpu(slave).state().pc = virtualRam;
    machine.cpu(master).state().r[0] = 0x12345678;
    machine.cpu(master).state().r[1] = 0xb4000000; // P2 -> aperture 0 -> shared RAM.
    machine.cpu(slave).state().r[1] = 0xb0000000; // Slave direct shared window.
    machine.runSteps(4);
    checks.equal(machine.cpu(slave).state().r[2], 0x12345678, "paired master store visible to slave load");
    checks.equal(machine.cpu(master).state().r[0], 0x12345679, "master integer execution");
    checks.equal(machine.cpu(master).state().pc, virtualRam, "master branch and delay slot");
    checks.equal(machine.cpu(slave).state().pc, virtualRam, "slave branch and delay slot");
    checks.equal(machine.memory().read(slave, 0x10000000), 0x12345678, "shared RAM on actual board bus");
    checks.equal(machine.memory().read(master, ram, 2), 0x2102, "master own RAM program");
    checks.equal(machine.memory().read(slave, ram, 2), 0x6212, "slave own RAM program");
    machine.reset();
    store(machine, master, 0x8c000100, 0x87654321);
    checks.equal(load(machine, master, 0xac000100), 0x87654321, "P1/P2 physical RAM aliases");
    checks.equal(load(machine, master, 0x0c000100), 0x87654321, "untranslated P0 RAM");
    checks.equal(load(machine, master, 0x4c000100), 0x87654321, "MMU-off P0 upper address bits use 29-bit physical bus");
    checks.equal(load(machine, slave, 0xac000100), 0, "CPU RAM banks isolated");
    machine.peripherals(master).write(0xff000010, 1, 4);
    checks.equal(load(machine, master, 0x8c000100), 0x87654321, "P1 remains direct with MMU enabled");
    machine.cpu(master).state().r[1] = 0x0c000100;
    op(machine, master, 0, 0x6212);
    machine.cpu(master).state().pc = virtualRam;
    checks.fault([&]() { machine.step(master); }, master, virtualRam, 0x6212,
        "MMU enabled", "unimplemented P0 translation rejected");
    machine.reset();
}

void cacheAndQueues(Machine& machine, Checks& checks)
{
    store(machine, master, 0x7c001ffc, 0x11223344);
    checks.equal(load(machine, master, 0x7c001ffc), 0x11223344, "on-chip RAM last word");
    checks.equal(load(machine, slave, 0x7c001ffc), 0, "on-chip RAM per CPU");
    for (unsigned word = 0; word < 8; ++word) {
        store(machine, master, 0xe0000000 + word * 4, 0xaabb0000 + word);
        store(machine, master, 0xe0000020 + word * 4, 0xccdd0000 + word);
    }
    machine.peripherals(master).write(0xff000038, 0x0c, 4);
    machine.peripherals(master).write(0xff00003c, 0x0c, 4);
    machine.cpu(master).state().r[1] = 0xe0000100;
    execute(machine, master, 0x0183); // PREF @R1
    for (unsigned word = 0; word < 8; ++word)
        checks.equal(machine.memory().read(master, ram + 0x100 + word * 4), 0xaabb0000 + word, "queue zero flush word " + std::to_string(word));
    machine.cpu(master).state().r[1] = 0xe0000120;
    execute(machine, master, 0x0183);
    for (unsigned word = 0; word < 8; ++word)
        checks.equal(machine.memory().read(master, ram + 0x120 + word * 4), 0xccdd0000 + word, "queue one flush word " + std::to_string(word));
    checks.equal(machine.memory().read(slave, ram + 0x100), 0, "store queue flush remains CPU-local RAM");
    machine.reset();
    checks.equal(load(machine, master, 0x7c001ffc), 0, "reset clears on-chip RAM");
    machine.peripherals(master).write(0xff000038, 0x0c, 4);
    machine.cpu(master).state().r[1] = 0xe0000100;
    execute(machine, master, 0x0183);
    checks.equal(machine.memory().read(master, ram + 0x100), 0, "reset clears store queue buffer");
    machine.reset();
}

void interruptsAndDma(Machine& machine, Checks& checks)
{
    auto prepare = [&]() {
        machine.reset();
        op(machine, master, 0x100, 0x0009);
        op(machine, master, 0x102, 0x0009);
        op(machine, master, 0x104, 0x0009);
        op(machine, master, 0x600, 0x0009);
        machine.cpu(master).state().pc = virtualRam + 0x100;
        machine.cpu(master).state().vbr = virtualRam;
        machine.cpu(master).state().sr = 0x40000000;
    };
    prepare();
    machine.peripherals(master).write(0xffd00004, 0x7000, 2);
    machine.peripherals(master).write(0xffd8000c, 0, 4);
    machine.peripherals(master).write(0xffd80010, 0x20, 2);
    machine.peripherals(master).write(0xffd80004, 1, 1);
    machine.peripherals(master).advance(16);
    machine.step(master);
    checks.equal(machine.cpu(master).state().intevt, 0x400, "TMU priority routes interrupt to CPU");
    checks.equal(machine.cpu(master).state().spc, virtualRam + 0x100, "TMU interrupt saves interrupted PC");
    checks.equal(machine.cpu(master).state().pc, virtualRam + 0x602, "TMU executes first vector instruction");
    checks.equal(load(machine, master, 0xff000028), 0x400, "INTEVT reads CPU event state");

    prepare();
    machine.peripherals(master).write(0xffd0000c, 0x500, 2);
    machine.memory().write(master, ram + 0x200, 0x11223344);
    machine.memory().write(master, ram + 0x204, 0x55667788);
    machine.dma(master).writeRegister(0xffa00000, ram + 0x200);
    machine.dma(master).writeRegister(0xffa00004, ram + 0x300);
    machine.dma(master).writeRegister(0xffa00008, 2);
    machine.dma(master).writeRegister(0xffa0000c, 0x5435);
    machine.dma(master).writeRegister(0xffa00040, 1);
    machine.step(master);
    checks.equal(machine.memory().read(master, ram + 0x300), 0x11223344, "machine advances one DMA unit per step");
    checks.equal(machine.dma(master).readRegister(0xffa00008), 1, "DMA remains incremental");
    checks.equal(machine.cpu(master).state().intevt, 0, "DMA has no early IRQ");
    machine.step(master);
    checks.equal(machine.memory().read(master, ram + 0x304), 0x55667788, "machine DMA second value");
    checks.equal(machine.dma(master).pending(), 1, "DMA completion raises pending channel");
    machine.step(master);
    checks.equal(machine.cpu(master).state().intevt, 0x640, "DMA completion IRQ selected by IPRC");
    checks.equal(machine.cpu(master).state().spc, virtualRam + 0x104, "DMA interrupt boundary PC");
    machine.reset();
    store(machine, master, 0xffa00001, 0xab, 1);
    checks.equal(machine.dma(master).readRegister(0xffa00000), 0xab00, "CPU DMAC partial-lane write routed");
    checks.equal(load(machine, master, 0x1fa00000), 0xab00, "physical DMAC alias routed");

    prepare();
    machine.peripherals(master).write(0xffd00000, 0x80, 2);
    machine.memory().bridge(master).registerWrite(4, 0x10000000);
    machine.memory().bridge(master).setIrqStatus(0x1000);
    machine.step(master);
    checks.equal(machine.cpu(master).state().intevt, 0x2a0, "bridge line routes individual IRL1");
    prepare();
    machine.memory().bridge(master).registerWrite(4, 0x10000000);
    machine.memory().bridge(master).setIrqStatus(0x1000);
    checks.fault([&]() { machine.step(master); }, master, virtualRam + 0x100, 0xffff,
        "encoded IRL", "unsupported board IRQ mode reported");

    machine.reset();
    checks.equal(load(machine, master, 0xff800030, 2), 0x3e0, "main board default GPIO inputs");
    checks.equal(load(machine, slave, 0xff800030, 2), 0x3c0, "slave board default DIP inputs");
    machine.peripherals(slave).write(0xffd00000, 0x80, 2);
    machine.peripherals(master).write(0xff80002c, 1, 4);
    store(machine, master, 0xff800030, 0, 2);
    op(machine, slave, 0x100, 0x0009);
    op(machine, slave, 0x600, 0x0009);
    machine.cpu(slave).state().pc = virtualRam + 0x100;
    machine.cpu(slave).state().vbr = virtualRam;
    machine.cpu(slave).state().sr = 0x40000000;
    machine.step(slave);
    checks.equal(machine.cpu(slave).state().intevt, 0x360, "GPIO PA0 drives other CPU IRL3");
    machine.reset();
}

void faultsResetAndRoms(Machine& machine, Checks& checks)
{
    machine.memory().bridge(master).registerWrite(0x18, 0x00000100); // Aperture 2, page 1 -> local 0x01xxxxxx.
    machine.cpu(master).state().r[1] = 0xa1000100;
    op(machine, master, 0, 0x6212);
    machine.cpu(master).state().pc = virtualRam;
    checks.fault([&]() { machine.step(master); }, master, virtualRam, 0x6212,
        "01000100", "unknown GPU access reports actual local-bus address");
    checks.equal(machine.trace(master).back(), virtualRam, "fault instruction recorded in trace");
    machine.reset();
    for (unsigned i = 0; i < 40; ++i) op(machine, master, i * 2, 0x0009);
    machine.cpu(master).state().pc = virtualRam;
    for (unsigned i = 0; i < 40; ++i) machine.step(master);
    checks.equal(static_cast<uint32_t>(machine.trace(master).size()), 32, "trace bounded to last 32 instructions");
    checks.equal(machine.trace(master).front(), virtualRam + 16, "trace evicts oldest PC");
    checks.equal(machine.trace(master).back(), virtualRam + 78, "trace retains newest PC");
    machine.reset();
    checks.equal(static_cast<uint32_t>(machine.trace(master).size()), 0, "reset clears trace");
    checks.equal(machine.cpu(master).state().pc, 0xa0000000, "reset returns master to boot ROM");
    checks.equal(machine.cpu(slave).state().pc, 0xa0000000, "reset returns slave to boot ROM");
    checks.equal(machine.dma(master).pending(), 0, "reset clears DMA IRQ");
    checks.equal(machine.peripherals(master).pendingTmu(), 0, "reset clears timer IRQ");
    checks.equal(machine.memory().read(master, ram), 0, "reset clears CPU RAM");
    checks.equal(machine.memory().read(slave, 0x10000000), 0, "reset clears shared RAM");
    checks.equal(machine.memory().read(master, 0, 4), 0x00090009, "reset retains boot ROM");
    machine.memory().bridge(master).registerWrite(0x10, 0x10);
    checks.equal(machine.memory().read(master, 0x14000000), 0x44332211, "program ROM attached to local bus");
    machine.memory().bridge(master).registerWrite(0x10, 0x20);
    checks.equal(machine.memory().read(master, 0x14000000), 0x88776655, "data ROM attached to local bus");
    machine.reset();
    machine.memory().bridge(master).registerWrite(0x10, 0x10);
    checks.equal(machine.memory().read(master, 0x14000000), 0x44332211, "reset retains program ROM");
    machine.memory().bridge(master).registerWrite(0x10, 0x20);
    checks.equal(machine.memory().read(master, 0x14000000), 0x88776655, "reset retains data ROM");
}

void mainEepromGpio(Machine& machine, Checks& checks)
{
    machine.reset();
    auto pins = [&](bool cs, bool clock, bool data) {
        store(machine, master, 0xff800030,
            1u | (cs ? 0x10u : 0) | (clock ? 8u : 0) | (data ? 4u : 0), 2);
    };
    auto sample = [&]() { return (load(machine, master, 0xff800030, 2) & 0x20) != 0; };
    auto bit = [&](bool data) { pins(true, false, data); pins(true, true, data); };
    auto bits = [&](const char* text) { for (; *text; ++text) bit(*text == '1'); };
    auto begin = [&]() { pins(false, false, false); pins(true, false, false); };
    auto readByte = [&]() {
        unsigned value = 0;
        for (unsigned i = 0; i < 8; ++i) { bit(false); value = (value << 1) | unsigned(sample()); }
        return value;
    };
    auto runTicks = [&](unsigned count) {
        op(machine, master, 0, 0xaffe); // BRA to self, with NOP delay slot.
        op(machine, master, 2, 0x0009);
        machine.cpu(master).state().pc = virtualRam;
        for (unsigned i = 0; i < count; ++i) machine.step(master);
    };
    checks.equal(sample(), true, "main EEPROM DO pull-up at reset");
    begin(); bits("1100100101"); // READ address 0x25 via real PDTRA instruction callbacks.
    checks.equal(sample(), false, "GPIO DO exposes EEPROM read dummy zero");
    checks.equal(readByte(), 0xff, "main EEPROM starts blank without factory ROM data");
    pins(false, false, false);
    begin(); bits("1001100000"); pins(false, false, false); // EWEN.
    begin(); bits("1010100101"); bits("10100110"); pins(false, false, false); // WRITE 25,A6.
    pins(true, false, false);
    checks.equal(sample(), false, "main EEPROM GPIO exposes genuine write BUSY");
    machine.reset(); // Does not power cycle or finish the EEPROM write.
    pins(true, false, false);
    checks.equal(sample(), false, "machine reset preserves running EEPROM cycle");
    runTicks(999000);
    checks.equal(sample(), false, "EEPROM write remains busy before 5 ms model interval");
    runTicks(1000);
    checks.equal(sample(), true, "EEPROM model advances once per 200 master instruction ticks");
    pins(false, false, false);
    machine.reset();
    begin(); bits("1100100101");
    checks.equal(readByte(), 0xa6, "machine reset retains programmed EEPROM contents");
    pins(false, false, false);
    begin(); bits("1010100110"); bits("01011010"); pins(false, false, false); // WRITE 26,5A without another EWEN.
    pins(true, false, false);
    checks.equal(sample(), false, "machine reset retains EEPROM write-enable latch");
    machine.reset();
}

void romBoundaries(Machine& machine, Checks& checks)
{
    machine.memory().bridge(master).registerWrite(0x10, 0x13121110);
    checks.equal(machine.memory().read(master, 0x14000003, 1), 0x44, "program ROM high byte lane");
    checks.equal(machine.memory().read(master, 0x14000002, 2), 0x4433, "program ROM high halfword lane");
    machine.memory().write(master, 0x14000000, 0, 4);
    checks.equal(machine.memory().read(master, 0x14000000), 0x44332211, "program ROM writes ignored");
    checks.equal(machine.memory().read(master, 0x14800000), 0, "unpopulated program bank zero-filled per MAME region manifest");
    checks.equal(machine.memory().read(master, 0x17fffffc), 0, "program region final aligned word stays within backing");
    machine.memory().bridge(master).registerWrite(0x10, 0x14);
    checks.fault([&]() { load(machine, master, 0xb4000000); }, master, virtualRam, 0x6212,
        "14000000", "address beyond program region reaches device decoder");
    machine.memory().bridge(master).registerWrite(0x10, 0x20);
    checks.equal(machine.memory().read(master, 0x14000003, 1), 0x88, "data ROM high byte lane");
    checks.equal(machine.memory().read(master, 0x14000002, 2), 0x8877, "data ROM high halfword lane");
    machine.memory().write(master, 0x14000000, 0, 4);
    checks.equal(machine.memory().read(master, 0x14000000), 0x88776655, "data ROM writes ignored");
    machine.memory().bridge(master).registerWrite(0x10, 0x2f);
    checks.equal(machine.memory().read(master, 0x14fffffc), 0, "unpopulated data tail final word zero-filled");
    checks.equal(machine.memory().read(master, 0x14ffffff, 1), 0, "data region final byte stays within backing");
    machine.memory().write(master, 0x14ffffff, 0xff, 1);
    checks.equal(machine.memory().read(master, 0x14ffffff, 1), 0, "data ROM hole cannot become writable RAM");
    machine.memory().bridge(master).registerWrite(0x10, 0x30);
    checks.fault([&]() { load(machine, master, 0xb4000000); }, master, virtualRam, 0x6212,
        "30000000", "address beyond data region reaches device decoder");
    machine.reset();
}
} // namespace

int main()
{
    Checks checks;
    try {
        Machine machine;
        hikaru::RomImages images;
        images.boot.resize(hikaru::BoardMemory::BootRomSize, 0xff);
        images.boot[0] = images.boot[2] = 9;
        images.boot[1] = images.boot[3] = 0;
        images.program = {0x11, 0x22, 0x33, 0x44};
        images.data = {0x55, 0x66, 0x77, 0x88};
        machine.loadImages(std::move(images));
        machine.runSteps(1);
        checks.equal(machine.cpu(master).state().pc, 0xa0000002, "master boots supplied native image");
        checks.equal(machine.cpu(slave).state().pc, 0xa0000002, "slave boots supplied native image");
        machine.reset();
        communicationAndAliases(machine, checks);
        cacheAndQueues(machine, checks);
        interruptsAndDma(machine, checks);
        mainEepromGpio(machine, checks);
        faultsResetAndRoms(machine, checks);
        romBoundaries(machine, checks);
    }
    catch (const std::exception& error) {
        ++checks.failures;
        std::cerr << "Unexpected exception: " << error.what() << '\n';
    }
    std::cout << "Hikaru native machine: " << checks.count << " checks, " << checks.failures << " failures\n";
    return checks.failures == 0 ? 0 : 1;
}
