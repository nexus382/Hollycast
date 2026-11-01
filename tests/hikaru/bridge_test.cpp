// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone, ROM-free regression checks for the Sega 315-6154 bridge.
#include "hw/hikaru/bridge.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {
using hikaru::Bridge;
constexpr uint32_t allLanes = 0xffffffffu;
constexpr uint32_t dmaBusy = 0x01000000u;
constexpr uint32_t dmaDone = 0x00001000u;

struct Checks {
    unsigned failures = 0;
    unsigned checks = 0;

    void equal(uint32_t actual, uint32_t expected, const std::string& label)
    {
        ++checks;
        if (actual != expected) {
            ++failures;
            std::cerr << label << ": expected 0x" << std::hex << expected
                      << ", got 0x" << actual << std::dec << '\n';
        }
    }
};

enum class Access { Read, Write, ConfigRead, ConfigWrite };
struct Event {
    Access access;
    uint32_t address;
    uint32_t data;
    uint32_t mask;
};

struct BusProbe {
    std::vector<Event> events;
    std::map<uint32_t, uint32_t> memory;
    std::map<uint32_t, uint32_t> config;

    uint32_t read(Access access, uint32_t address, uint32_t mask)
    {
        const auto& values = access == Access::ConfigRead ? config : memory;
        const auto found = values.find(address);
        const uint32_t value = found == values.end() ? 0xdecafbad : found->second;
        events.push_back({access, address, value, mask});
        return value;
    }

    void write(Access access, uint32_t address, uint32_t data, uint32_t mask)
    {
        events.push_back({access, address, data, mask});
        auto& values = access == Access::ConfigWrite ? config : memory;
        values[address] = (values[address] & ~mask) | (data & mask);
    }

    Bridge::Bus callbacks()
    {
        return {
            [this](uint32_t address, uint32_t mask) { return read(Access::Read, address, mask); },
            [this](uint32_t address, uint32_t data, uint32_t mask) { write(Access::Write, address, data, mask); },
            [this](uint32_t address, uint32_t mask) { return read(Access::ConfigRead, address, mask); },
            [this](uint32_t address, uint32_t data, uint32_t mask) { write(Access::ConfigWrite, address, data, mask); }
        };
    }

    void event(Checks& checks, unsigned index, Access access, uint32_t address,
               uint32_t data, uint32_t mask, const std::string& label) const
    {
        checks.equal(index < events.size(), true, label + " callback exists");
        if (index >= events.size())
            return;
        const auto& actual = events[index];
        checks.equal(static_cast<uint32_t>(actual.access), static_cast<uint32_t>(access), label + " callback kind");
        checks.equal(actual.address, address, label + " address");
        checks.equal(actual.data, data, label + " data");
        checks.equal(actual.mask, mask, label + " lanes");
    }
};

void registersAndResetDefaults(Checks& checks)
{
    Bridge master(Bridge::Mode::Master);
    Bridge slave(Bridge::Mode::Slave);
    checks.equal(master.registerRead(0), 0, "master ID");
    checks.equal(slave.registerRead(0), allLanes, "slave ID");
    master.registerWrite(0, allLanes);
    slave.registerWrite(0, 0);
    checks.equal(master.registerRead(0), 0, "master ID unaffected by writes");
    checks.equal(slave.registerRead(0), allLanes, "slave ID unaffected by writes");
    for (uint32_t offset = 4; offset < 0x100; offset += 4)
        checks.equal(master.registerRead(offset), 0, "initial register " + std::to_string(offset));

    master.registerWrite(0x20, 0x12345678);
    master.registerWrite(0x20, 0xaabbccdd, 0x00ff00ff);
    checks.equal(master.registerRead(0x20), 0x12bb56dd, "register byte lanes combine");
    master.registerWrite(0x20, 0x98760000, 0xffff0000);
    checks.equal(master.registerRead(0x20), 0x987656dd, "register high halfword combine");
    master.registerWrite(0x20, allLanes, 0);
    checks.equal(master.registerRead(0x20), 0x987656dd, "zero register mask");
    master.reset();
    slave.reset();
    for (uint32_t offset = 4; offset < 0x100; offset += 4)
        checks.equal(master.registerRead(offset), 0, "reset register " + std::to_string(offset));
    checks.equal(slave.registerRead(0), allLanes, "reset preserves slave mode");
}

void aperturePages(Checks& checks)
{
    BusProbe probe;
    Bridge bridge(Bridge::Mode::Master, probe.callbacks());
    // Explicit expected bases cover both sides of the provisional high-bit
    // normalization boundary. Each byte selects one 16 MiB page, low first.
    const std::array<uint32_t, 3> mappings = {0xc0bf807f, 0xfe0181ff, 0x00c14282};
    const std::array<std::array<uint32_t, 4>, 3> bases = {{
        {{0x7f000000, 0x00000000, 0x3f000000, 0xc0000000}},
        {{0xff000000, 0x01000000, 0x01000000, 0xfe000000}},
        {{0x02000000, 0x42000000, 0xc1000000, 0x00000000}}
    }};
    for (unsigned aperture = 0; aperture < 3; ++aperture) {
        bridge.registerWrite(0x10 + aperture * 4, mappings[aperture]);
        for (unsigned page = 0; page < 4; ++page) {
            for (uint32_t low : {0u, 0x00123454u, 0x00fffffcu}) {
                const uint32_t offset = (page << 24) | low;
                const uint32_t target = bases[aperture][page] | low;
                const std::string label = "aperture " + std::to_string(aperture)
                    + " page " + std::to_string(page) + " offset " + std::to_string(low);
                probe.events.clear();
                probe.memory[target] = 0xa1234567;
                checks.equal(bridge.apertureRead(aperture, offset, 0x00ff0000), 0xa1234567, label + " read result");
                bridge.apertureWrite(aperture, offset, 0x98765432, 0x0000ffff);
                checks.equal(static_cast<uint32_t>(probe.events.size()), 2, label + " callback count");
                probe.event(checks, 0, Access::Read, target, 0xa1234567, 0x00ff0000, label + " read");
                probe.event(checks, 1, Access::Write, target, 0x98765432, 0x0000ffff, label + " write");
                checks.equal(probe.memory[target], 0xa1235432, label + " callback lane merge");
            }
        }
    }
    bridge.registerWrite(0x10, 0x01020304);
    bridge.registerWrite(0x10, 0x00c00000, 0x00ff0000);
    probe.events.clear();
    bridge.apertureRead(0, 0x02000004);
    probe.event(checks, 0, Access::Read, 0xc0000004, 0xdecafbad, allLanes, "mapping lane update");
}

void configurationAndLaneMasks(Checks& checks)
{
    BusProbe probe;
    Bridge bridge(Bridge::Mode::Master, probe.callbacks());
    for (unsigned aperture = 0; aperture < 3; ++aperture)
        bridge.registerWrite(0x10 + aperture * 4, 0xc4c3c2c1);
    bridge.registerWrite(0x1c, 7);
    const std::array<uint32_t, 8> masks = {
        0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000,
        0x0000ffff, 0xffff0000, allLanes, 0
    };
    for (unsigned aperture = 0; aperture < 3; ++aperture) {
        for (unsigned page = 0; page < 4; ++page) {
            for (uint32_t mask : masks) {
                const bool config = aperture < 2 && page == 0;
                const uint32_t offset = (page << 24) | 0x00000804;
                const uint32_t target = config ? 0x804 : ((0xc1u + page) << 24) | 0x804;
                const std::string label = "config routing aperture " + std::to_string(aperture)
                    + " page " + std::to_string(page) + " mask " + std::to_string(mask);
                probe.events.clear();
                probe.memory[target] = 0x11223344;
                probe.config[target] = 0x55667788;
                const uint32_t value = config ? 0x55667788 : 0x11223344;
                checks.equal(bridge.apertureRead(aperture, offset, mask), value, label + " result");
                bridge.apertureWrite(aperture, offset, 0xaabbccdd, mask);
                checks.equal(static_cast<uint32_t>(probe.events.size()), 2, label + " callback count");
                probe.event(checks, 0, config ? Access::ConfigRead : Access::Read, target, value, mask, label + " read");
                probe.event(checks, 1, config ? Access::ConfigWrite : Access::Write, target, 0xaabbccdd, mask, label + " write");
                checks.equal(config ? probe.config[target] : probe.memory[target],
                             (value & ~mask) | (0xaabbccdd & mask), label + " merged write");
            }
        }
    }
    bridge.registerWrite(0x1c, 0, 1);
    probe.events.clear();
    bridge.apertureRead(0, 0x804);
    bridge.apertureRead(1, 0x804);
    probe.event(checks, 0, Access::Read, 0xc1000804, probe.memory[0xc1000804], allLanes, "config aperture 0 disabled independently");
    probe.event(checks, 1, Access::ConfigRead, 0x804, probe.config[0x804], allLanes, "config aperture 1 remains enabled");
}

void interrupts(Checks& checks)
{
    std::vector<bool> transitions;
    Bridge bridge(Bridge::Mode::Master, {}, [&transitions](bool state) { transitions.push_back(state); });
    bridge.setIrqStatus(0x1101);
    checks.equal(bridge.registerRead(4), 0x00001101, "status latches while disabled");
    checks.equal(bridge.irqAsserted(), false, "disabled status does not assert IRQ");
    checks.equal(static_cast<uint32_t>(transitions.size()), 0, "no inactive IRQ callback");
    bridge.registerWrite(4, 0x10000000, 0xff000000);
    checks.equal(bridge.irqAsserted(), true, "enable pending DMA IRQ");
    bridge.setIrqStatus(0x1000);
    checks.equal(static_cast<uint32_t>(transitions.size()), 1, "unchanged IRQ does not repeat callback");
    bridge.registerWrite(4, 0x1100, 0x000000ff);
    checks.equal(bridge.registerRead(4), 0x10001101, "W1C honors low byte mask");
    bridge.registerWrite(4, 1, 0x000000ff);
    checks.equal(bridge.registerRead(4), 0x10001100, "W1C clears selected status bit");
    bridge.registerWrite(4, 0x1000, 0x0000ff00);
    checks.equal(bridge.registerRead(4), 0x10000100, "W1C preserves other status and enable");
    checks.equal(bridge.irqAsserted(), false, "acknowledgement deasserts IRQ");
    bridge.registerWrite(4, 0x01000000, 0xff000000);
    checks.equal(bridge.irqAsserted(), true, "other pending enabled status asserts IRQ");
    bridge.registerWrite(4, 0, 0xffff0000);
    checks.equal(bridge.registerRead(4), 0x100, "disable preserves pending status");
    checks.equal(bridge.irqAsserted(), false, "disable deasserts IRQ");
    bridge.registerWrite(4, 0x01000100);
    checks.equal(bridge.registerRead(4), 0x01000000, "combined enable and W1C update");
    checks.equal(bridge.irqAsserted(), false, "cleared status does not assert from enable write");
    bridge.registerWrite(4, allLanes, 0);
    checks.equal(bridge.registerRead(4), 0x01000000, "zero IRQ lane mask");
    checks.equal(static_cast<uint32_t>(transitions.size()), 4, "IRQ transition count");
    if (transitions.size() == 4) {
        checks.equal(transitions[0], true, "IRQ first assertion");
        checks.equal(transitions[1], false, "IRQ first deassertion");
        checks.equal(transitions[2], true, "IRQ second assertion");
        checks.equal(transitions[3], false, "IRQ second deassertion");
    }
}

void incrementalDma(Checks& checks)
{
    BusProbe probe;
    std::vector<bool> transitions;
    Bridge bridge(Bridge::Mode::Master, probe.callbacks(), [&transitions](bool state) { transitions.push_back(state); });
    const std::array<uint32_t, 3> data = {0x01234567, 0x89abcdef, 0x55aa00ff};
    for (unsigned word = 0; word < data.size(); ++word)
        probe.memory[0x100 + word * 4] = data[word];
    bridge.registerWrite(0x1c, 3); // DMA always uses memory, even in config mode.
    bridge.registerWrite(4, dmaDone << 16);
    bridge.registerWrite(0x30, 0x80000100);
    bridge.registerWrite(0x34, 0xb0000200);
    bridge.registerWrite(0x38, dmaBusy | 3, 0x00ffffff);
    checks.equal(bridge.registerRead(0x38), 3, "masked start only programs length");
    checks.equal(bridge.advanceDma(20), 0, "masked start does not transfer");
    checks.equal(bridge.registerRead(4), dmaDone << 16, "masked start does not complete");
    bridge.registerWrite(0x38, dmaBusy, 0xff000000);
    checks.equal(bridge.registerRead(0x38), dmaBusy | 3, "start preserves programmed length");
    checks.equal(static_cast<uint32_t>(probe.events.size()), 0, "DMA start has no immediate bus activity");
    checks.equal(bridge.advanceDma(0), 0, "zero DMA budget");
    checks.equal(bridge.registerRead(0x38), dmaBusy | 3, "zero budget retains busy");
    checks.equal(bridge.irqAsserted(), false, "DMA has no early completion IRQ");
    checks.equal(bridge.advanceDma(1), 1, "first incremental DMA word");
    checks.equal(bridge.registerRead(4), dmaDone << 16, "partial DMA has no done status");
    checks.equal(bridge.registerRead(0x38), dmaBusy | 3, "partial DMA retains control length");
    bridge.registerWrite(0x30, 0xc0000000);
    bridge.registerWrite(0x34, 0xf0000000);
    bridge.registerWrite(0x38, dmaBusy | 99);
    checks.equal(bridge.registerRead(0x30), 0x80000100, "busy freezes source register");
    checks.equal(bridge.registerRead(0x34), 0xb0000200, "busy freezes destination register");
    checks.equal(bridge.registerRead(0x38), dmaBusy | 3, "busy ignores restart");
    checks.equal(bridge.advanceDma(1), 1, "second incremental DMA word");
    checks.equal(bridge.irqAsserted(), false, "second word has no early IRQ");
    checks.equal(bridge.advanceDma(100), 1, "DMA caps oversized budget at remaining length");
    checks.equal(bridge.registerRead(0x38), 3, "completion clears busy only");
    checks.equal(bridge.registerRead(4), (dmaDone << 16) | dmaDone, "completion latches done status");
    checks.equal(bridge.irqAsserted(), true, "completion asserts enabled IRQ");
    checks.equal(static_cast<uint32_t>(probe.events.size()), 6, "DMA exactly one read/write per word");
    for (unsigned word = 0; word < data.size(); ++word) {
        probe.event(checks, word * 2, Access::Read, 0x100 + word * 4, data[word], allLanes, "local DMA source " + std::to_string(word));
        probe.event(checks, word * 2 + 1, Access::Write, 0x30000200 + word * 4, data[word], allLanes, "local DMA destination " + std::to_string(word));
        checks.equal(probe.memory[0x30000200 + word * 4], data[word], "local DMA copied value " + std::to_string(word));
    }
    checks.equal(bridge.advanceDma(100), 0, "completed DMA stays idle");
    checks.equal(static_cast<uint32_t>(probe.events.size()), 6, "completed DMA has no repeated bus activity");
    checks.equal(static_cast<uint32_t>(transitions.size()), 1, "DMA completion IRQ fires once");
    bridge.registerWrite(4, dmaDone, 0xffff);
    checks.equal(bridge.irqAsserted(), false, "DMA done acknowledgement");

    // Both ends at or above 0xc0000000 retain bit 31 for PCI addressing.
    probe.events.clear();
    probe.memory[0xc0000100] = 0x10203040;
    probe.memory[0xc0000104] = 0x50607080;
    bridge.registerWrite(0x30, 0xc0000100);
    bridge.registerWrite(0x34, 0xfffffff8);
    bridge.registerWrite(0x38, dmaBusy | 2);
    checks.equal(bridge.advanceDma(2), 2, "PCI DMA transfer count");
    probe.event(checks, 0, Access::Read, 0xc0000100, 0x10203040, allLanes, "PCI DMA threshold source");
    probe.event(checks, 1, Access::Write, 0xfffffff8, 0x10203040, allLanes, "PCI DMA high destination");
    probe.event(checks, 2, Access::Read, 0xc0000104, 0x50607080, allLanes, "PCI DMA source increment");
    probe.event(checks, 3, Access::Write, 0xfffffffc, 0x50607080, allLanes, "PCI DMA destination increment");
}

void resetDuringDmaAndMapping(Checks& checks)
{
    BusProbe probe;
    std::vector<bool> transitions;
    Bridge bridge(Bridge::Mode::Slave, probe.callbacks(), [&transitions](bool state) { transitions.push_back(state); });
    probe.memory[0x100] = 0x12345678;
    bridge.registerWrite(0x10, 0xc4c3c2c1);
    bridge.registerWrite(0x14, 0xd4d3d2d1);
    bridge.registerWrite(0x18, 0xe4e3e2e1);
    bridge.registerWrite(0x1c, 3);
    bridge.registerWrite(4, 0x10000000);
    bridge.setIrqStatus(0x1000);
    bridge.registerWrite(0x30, 0x80000100);
    bridge.registerWrite(0x34, 0x80000200);
    bridge.registerWrite(0x38, dmaBusy | 3);
    checks.equal(bridge.advanceDma(1), 1, "pre-reset DMA word");
    bridge.reset();
    checks.equal(bridge.registerRead(0), allLanes, "reset during DMA preserves mode");
    checks.equal(bridge.irqAsserted(), false, "reset deasserts active IRQ");
    checks.equal(static_cast<uint32_t>(transitions.size()), 2, "reset IRQ callback count");
    if (transitions.size() == 2)
        checks.equal(transitions[1], false, "reset emits IRQ deassertion");
    for (uint32_t offset = 4; offset < 0x100; offset += 4)
        checks.equal(bridge.registerRead(offset), 0, "reset clears active register " + std::to_string(offset));
    probe.events.clear();
    checks.equal(bridge.advanceDma(100), 0, "reset cancels DMA");
    checks.equal(static_cast<uint32_t>(probe.events.size()), 0, "reset DMA has no residual callbacks");
    checks.equal(probe.memory.count(0x204), 0, "reset cancels remaining destination writes");
    for (unsigned aperture = 0; aperture < 3; ++aperture) {
        for (unsigned page = 0; page < 4; ++page) {
            probe.events.clear();
            bridge.apertureRead(aperture, (page << 24) | 0x100);
            probe.event(checks, 0, Access::Read, 0x100, 0x12345678, allLanes,
                        "reset clears mapping and config " + std::to_string(aperture) + "/" + std::to_string(page));
        }
    }
    // The bridge remains usable after reset rather than retaining stale DMA state.
    bridge.registerWrite(0x30, 0x100);
    bridge.registerWrite(0x34, 0x300);
    bridge.registerWrite(0x38, dmaBusy | 1);
    checks.equal(bridge.advanceDma(1), 1, "fresh DMA after reset");
    checks.equal(probe.memory[0x300], 0x12345678, "fresh DMA uses new destination");
}

void missingCallbacksAndEmptyDma(Checks& checks)
{
    Bridge empty(Bridge::Mode::Master);
    for (unsigned aperture = 0; aperture < 3; ++aperture) {
        checks.equal(empty.apertureRead(aperture, 0), allLanes, "unconnected memory bus reads open");
        empty.apertureWrite(aperture, 0, 1);
    }
    empty.registerWrite(0x1c, 3);
    checks.equal(empty.apertureRead(0, 0), allLanes, "unconnected config bus reads open");
    empty.apertureWrite(1, 0, 1);
    empty.registerWrite(0x38, dmaBusy | 1);
    checks.equal(empty.advanceDma(10), 0, "missing DMA callbacks make no progress");
    checks.equal(empty.registerRead(0x38), dmaBusy | 1, "missing DMA callbacks retain busy");
    checks.equal(empty.registerRead(4), 0, "missing DMA callbacks do not complete");
    empty.reset();
    empty.registerWrite(0x38, dmaBusy);
    checks.equal(empty.registerRead(4), 0, "empty DMA does not complete during start write");
    checks.equal(empty.advanceDma(0), 0, "empty DMA copies zero words");
    checks.equal(empty.registerRead(0x38), 0, "empty DMA clears busy when advanced");
    checks.equal(empty.registerRead(4), dmaDone, "empty DMA sets done without callbacks");

    for (bool readOnly : {false, true}) {
        BusProbe probe;
        auto callbacks = probe.callbacks();
        if (readOnly)
            callbacks.write = {};
        else
            callbacks.read = {};
        Bridge partial(Bridge::Mode::Master, callbacks);
        partial.registerWrite(0x38, dmaBusy | 1);
        checks.equal(partial.advanceDma(1), 0, "DMA requires both memory callbacks");
        checks.equal(static_cast<uint32_t>(probe.events.size()), 0, "partial bus has no DMA side effects");
        checks.equal(partial.registerRead(4), 0, "partial bus DMA has no false completion");
        checks.equal(partial.registerRead(0x38), dmaBusy | 1, "partial bus DMA stays busy");
    }
}

void invalidAccesses(Checks& checks)
{
    BusProbe probe;
    Bridge bridge(Bridge::Mode::Master, probe.callbacks());
    bridge.registerWrite(0x10, 0xc4c3c2c1);
    for (uint32_t offset : {1u, 2u, 3u, 0xffu, 0x100u, 0x104u, allLanes}) {
        checks.equal(bridge.registerRead(offset), allLanes, "invalid register read " + std::to_string(offset));
        bridge.registerWrite(offset, allLanes);
    }
    checks.equal(bridge.registerRead(0x10), 0xc4c3c2c1, "invalid writes preserve valid register");
    for (unsigned aperture : {0u, 1u, 2u, 3u, 99u}) {
        for (uint32_t offset : {0u, 1u, 2u, 3u, 0x03ffffffu, 0x04000000u, allLanes}) {
            if (aperture < 3 && offset == 0)
                continue;
            checks.equal(bridge.apertureRead(aperture, offset), allLanes, "invalid aperture read");
            bridge.apertureWrite(aperture, offset, allLanes);
        }
    }
    checks.equal(static_cast<uint32_t>(probe.events.size()), 0, "invalid apertures never call bus");
}
} // namespace

int main()
{
    Checks checks;
    registersAndResetDefaults(checks);
    aperturePages(checks);
    configurationAndLaneMasks(checks);
    interrupts(checks);
    incrementalDma(checks);
    resetDuringDmaAndMapping(checks);
    missingCallbacksAndEmptyDma(checks);
    invalidAccesses(checks);
    std::cout << "Hikaru bridge: " << checks.checks << " checks, " << checks.failures << " failures\n";
    return checks.failures == 0 ? 0 : 1;
}
