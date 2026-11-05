// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/board_memory.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
using Board = hikaru::BoardMemory;
using Cpu = Board::Cpu;
unsigned checks = 0;
unsigned failures = 0;

void check(bool result, const std::string& description)
{
    ++checks;
    if (!result)
    {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

template <typename F> void rejects(F&& function, const char* description)
{
    try
    {
        function();
        check(false, description);
    }
    catch (const std::invalid_argument&)
    {
        check(true, description);
    }
}

void bootRom(Board& board)
{
    check(board.read(Cpu::Master, 0) == 0xffffffff, "absent BIOS has no executable substitute");
    std::vector<uint8_t> bytes(Board::BootRomSize);
    for (uint32_t offset = 0; offset < bytes.size(); ++offset)
        bytes[offset] = static_cast<uint8_t>((offset * 13) ^ (offset >> 8));
    board.loadBootRom(bytes);
    for (Cpu cpu : {Cpu::Master, Cpu::Slave})
    {
        for (uint32_t offset : {0u, 4u, Board::BootRomSize - 4})
        {
            const uint32_t expected = uint32_t(bytes[offset]) | (uint32_t(bytes[offset + 1]) << 8)
                | (uint32_t(bytes[offset + 2]) << 16) | (uint32_t(bytes[offset + 3]) << 24);
            check(board.read(cpu, offset) == expected, "both CPUs see little-endian BIOS");
            board.write(cpu, offset, ~expected);
            check(board.read(cpu, offset) == expected, "BIOS overlay is read-only");
            board.write(cpu, offset + 3, 0xff, 1);
            check(board.read(cpu, offset + 3, 1) == bytes[offset + 3], "BIOS byte writes ignored");
        }
    }
    rejects([&] { board.loadBootRom({}); }, "empty boot ROM rejected");
    rejects([&] { board.loadBootRom(std::vector<uint8_t>(Board::BootRomSize - 1)); }, "short ROM rejected");
    rejects([&] { board.loadBootRom(std::vector<uint8_t>(Board::BootRomSize + 1)); }, "long ROM rejected");
    check(board.read(Cpu::Master, 0, 1) == bytes[0], "failed ROM load preserves previous image");
}

void cpuRamAndApertures(Board& board)
{
    constexpr uint32_t ram = 0x0c000000;
    for (Cpu cpu : {Cpu::Master, Cpu::Slave})
    {
        const uint32_t value = cpu == Cpu::Master ? 0x44332211 : 0x88776655;
        board.write(cpu, ram, value);
        board.write(cpu, ram + Board::CpuRamSize - 4, value);
        check(board.read(cpu, ram) == value, "each CPU owns its RAM bank");
        check(board.read(cpu, ram + Board::CpuRamSize - 4) == value, "RAM bank last word");
        for (unsigned lane = 0; lane < 4; ++lane)
            check(board.read(cpu, ram + lane, 1) == ((value >> (lane * 8)) & 0xff), "RAM byte lanes");
        board.write(cpu, ram + 1, 0xab, 1);
        check(board.read(cpu, ram) == ((value & 0xffff00ff) | 0xab00), "byte write keeps other lanes");
        board.write(cpu, ram + 2, 0xcdef, 2);
        check(board.read(cpu, ram) == ((value & 0x000000ff) | 0xcdefab00), "halfword write keeps low lanes");
    }
    check(board.read(Cpu::Master, ram) == 0xcdefab11, "master RAM not overwritten by slave");
    check(board.read(Cpu::Slave, ram) == 0xcdefab55, "slave RAM not overwritten by master");

    // Aperture 0 pages: slave lower RAM, master lower RAM, slave upper RAM,
    // command RAM. Program via the CPU's actual register window.
    board.write(Cpu::Master, 0x04000010, 0x48417040);
    check(board.read(Cpu::Master, 0x14000000) == board.read(Cpu::Slave, ram), "page 0 reaches slave RAM");
    check(board.read(Cpu::Master, 0x15000000) == board.read(Cpu::Master, ram), "page 1 reaches master RAM");
    board.write(Cpu::Master, 0x16000010, 0x01234567);
    check(board.read(Cpu::Slave, ram + 0x01000010) == 0x01234567, "page 2 reaches upper slave bank");
    board.write(Cpu::Master, 0x17000002, 0xbbaa, 2);
    check(board.read(Cpu::Slave, 0x10000000) == 0xbbaa0000, "page 3 shares slave command RAM");
    check(board.read(Cpu::Slave, 0x14000000) == 0xffffffff, "bridge mappings are per CPU");
    check(board.read(Cpu::Master, 0x10000000) == 0xffffffff, "master has no direct command window");

    // Aperture 1 -> command RAM. Its last word must alias the last word of
    // the slave's direct window, while the next word is unmapped.
    board.write(Cpu::Master, 0x04000014, 0x48);
    board.write(Cpu::Master, 0x183fffff, 0xd3, 1);
    check(board.read(Cpu::Slave, 0x103fffff, 1) == 0xd3, "shared RAM last byte alias");
    board.write(Cpu::Slave, 0x10400000, 0x12345678);
    check(board.read(Cpu::Master, 0x18400000) == 0xffffffff, "shared RAM does not wrap");

    // Aperture 2 lies underneath the BIOS overlay. The first visible byte
    // after the overlay is at the corresponding offset in mapped master RAM.
    board.write(Cpu::Slave, 0x04000018, 0x70);
    board.write(Cpu::Slave, Board::BootRomSize, 0x98765432);
    check(board.read(Cpu::Master, ram + Board::BootRomSize) == 0x98765432, "aperture 2 beyond BIOS");
    const uint32_t romLast = board.read(Cpu::Slave, Board::BootRomSize - 4);
    board.write(Cpu::Slave, Board::BootRomSize - 4, 0xffffffff);
    check(board.read(Cpu::Slave, Board::BootRomSize - 4) == romLast, "BIOS overlay ends at exact boundary");

    check(board.read(Cpu::Master, 0x04000000) == 0, "master bridge mode ID");
    check(board.read(Cpu::Slave, 0x04000000) == 0xffffffff, "slave bridge mode ID");
    board.write(Cpu::Master, 0x04000011, 0x40, 1);
    check(board.read(Cpu::Master, 0x04000010) == 0x48414040, "byte write programs one page");
}

void dmaAndReset(Board& board)
{
    board.reset();
    for (unsigned word = 0; word < 3; ++word)
        board.write(Cpu::Master, 0x0c000000 + word * 4, 0x10203040 + word);
    board.write(Cpu::Master, 0x04000004, 1u << 28); // DMA-done IRQ enable.
    board.write(Cpu::Master, 0x04000030, 0x70000000);
    board.write(Cpu::Master, 0x04000034, 0x48000000);
    board.write(Cpu::Master, 0x04000038, 0x01000003);
    check(board.bridge(Cpu::Master).advanceDma(1) == 1, "DMA consumes word budget");
    check(board.read(Cpu::Slave, 0x10000000) == 0x10203040, "DMA delivers first command word");
    check(board.read(Cpu::Slave, 0x10000004) == 0, "DMA leaves untransferred words unchanged");
    check(!board.bridge(Cpu::Master).irqAsserted(), "DMA has no early IRQ");
    check(board.bridge(Cpu::Master).advanceDma(10) == 2, "DMA stops at captured length");
    check(board.read(Cpu::Slave, 0x10000008) == 0x10203042, "DMA delivers final word");
    check(board.read(Cpu::Slave, 0x1000000c) == 0, "DMA does not overwrite following word");
    check(board.bridge(Cpu::Master).irqAsserted(), "DMA IRQ reaches CPU's bridge");
    check(!board.bridge(Cpu::Slave).irqAsserted(), "other CPU's bridge has no spurious IRQ");
    board.write(Cpu::Master, 0x04000005, 0x10, 1); // Low-half status bit 12.
    check(!board.bridge(Cpu::Master).irqAsserted(), "byte-lane W1C acknowledges DMA");
    check(board.read(Cpu::Master, 0x04000004) == (1u << 28), "W1C retains IRQ enable");

    board.write(Cpu::Master, 0x04000010, 0x70);
    board.write(Cpu::Master, 0x04000038, 0x01000003);
    const uint32_t rom = board.read(Cpu::Master, 0);
    board.reset();
    check(board.read(Cpu::Master, 0x0c000000) == 0, "reset clears master RAM");
    check(board.read(Cpu::Slave, 0x0c000000) == 0, "reset clears slave RAM");
    check(board.read(Cpu::Slave, 0x10000000) == 0, "reset clears command RAM");
    check(board.read(Cpu::Master, 0x04000010) == 0, "reset clears mapping");
    check(board.read(Cpu::Master, 0x04000004) == 0, "reset clears IRQ state");
    check(board.bridge(Cpu::Master).advanceDma(10) == 0, "reset cancels DMA");
    check(board.read(Cpu::Master, 0) == rom, "reset retains boot ROM");
}

void invalidAndUnmapped(Board& board)
{
    for (Cpu cpu : {Cpu::Master, Cpu::Slave})
    {
        for (unsigned size : {1u, 2u, 4u})
        {
            for (uint32_t address : {0x04000100u, 0x0e000000u, 0x1c000000u, 0xfffffffcu})
            {
                board.write(cpu, address, 0, size);
                const uint32_t expected = size == 4 ? 0xffffffffu : (1u << (size * 8)) - 1;
                check(board.read(cpu, address, size) == expected, "unmapped access remains open bus");
            }
        }
        for (unsigned size : {0u, 3u, 8u, 0xffffffffu})
        {
            rejects([&] { board.read(cpu, 0, size); }, "invalid read width rejected");
            rejects([&] { board.write(cpu, 0, 0, size); }, "invalid write width rejected");
        }
        rejects([&] { board.read(cpu, 0x0c000001, 2); }, "unaligned read rejected");
        rejects([&] { board.write(cpu, 0x0c000002, 0, 4); }, "unaligned write rejected");
    }
    rejects([&] { board.read(static_cast<Cpu>(2), 0); }, "invalid CPU rejected");
    rejects([&] { board.write(static_cast<Cpu>(-1), 0, 0); }, "invalid CPU write rejected");
    check(!std::is_move_constructible<Board>::value, "board callback target cannot move");
    check(!std::is_copy_constructible<Board>::value, "board callback target cannot copy");
}
}

int main()
{
    Board board;
    bootRom(board);
    cpuRamAndApertures(board);
    dmaAndReset(board);
    invalidAndUnmapped(board);
    std::cout << checks << " board-memory checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
