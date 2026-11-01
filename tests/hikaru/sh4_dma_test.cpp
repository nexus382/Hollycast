// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/sh4_dma.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using hikaru::Sh4Dma;
namespace {
struct Memory {
    std::array<uint8_t, 1024> bytes{};
    uint32_t read(uint32_t a, unsigned s) {
        if (a > bytes.size() || s > bytes.size() - a) throw std::out_of_range("DMA read");
        uint32_t v = 0; for (unsigned i = 0; i < s; ++i) v |= uint32_t(bytes[a+i]) << (i*8); return v;
    }
    void write(uint32_t a, uint32_t v, unsigned s) {
        if (a > bytes.size() || s > bytes.size() - a) throw std::out_of_range("DMA write");
        for (unsigned i = 0; i < s; ++i) bytes[a+i] = uint8_t(v >> (i*8));
    }
    Sh4Dma::Bus bus() { return {[this](uint32_t a, unsigned s) { return read(a,s); }, [this](uint32_t a, uint32_t v, unsigned s) { write(a,v,s); }}; }
};
uint32_t crc32(const uint8_t* data, unsigned count)
{
    uint32_t crc = 0xffffffffu;
    for (unsigned i = 0; i < count; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
}
int main()
{
    constexpr uint32_t base = 0xffa00000, dmaor = base + 0x40;
    unsigned errors = 0;
    auto check = [&](bool condition, const char* text) { if (!condition) { ++errors; std::cerr << text << '\n'; } };
    Memory memory; Sh4Dma dma(memory.bus());
    const std::string source = "123456789";
    for (unsigned i = 0; i < source.size(); ++i) memory.bytes[0x100 + i] = source[i];
    dma.writeRegister(base, 0x100); dma.writeRegister(base + 4, 0x200); dma.writeRegister(base + 8, 9);
    dma.writeRegister(base + 12, 0x00005415); // auto request, increment source/destination, byte, IE, DE
    check(dma.advance(3) == 0 && memory.bytes[0x200] == 0, "master disable prevents transfer");
    dma.writeRegister(dmaor, 1);
    check(dma.advance(3) == 3 && dma.readRegister(base) == 0x103 && dma.readRegister(base+4) == 0x203 && dma.readRegister(base+8) == 6, "bounded transfer updates registers");
    check(dma.pending() == 0 && memory.bytes[0x203] == 0, "partial copy has no completion interrupt");
    check(dma.advance(100) == 6 && crc32(memory.bytes.data() + 0x200, 9) == 0xcbf43926, "byte copy CRC and count");
    check(dma.pending() == 1, "completion asserts TE and IE");
    dma.readRegister(base+12, 0xff000000); dma.writeRegister(base+12, 0, 2);
    check(dma.pending() == 1, "reading another lane does not arm TE acknowledgement");
    check((dma.readRegister(base+12) & 2), "completion is visible in CHCR");
    dma.writeRegister(base+12, 0, 2); check(dma.pending() == 0, "read then write zero acknowledges TE");
    dma.reset(); dma.writeRegister(base, 0x100); dma.writeRegister(base+4, 0x200); dma.writeRegister(base+8, 0);
    dma.writeRegister(base+12, 0x5411); dma.writeRegister(dmaor, 1);
    check(dma.advance(1) == 1 && dma.readRegister(base+8) == 0x00ffffff && !(dma.readRegister(base+12) & 2), "zero count starts sixteen million transfers");
    dma.reset(); memory.write(0x100, 0x12345678, 4);
    dma.writeRegister(base+0x10, 0x100); dma.writeRegister(base+0x14, 0x204); dma.writeRegister(base+0x18, 1);
    dma.writeRegister(base+0x1c, 0x00008435); dma.writeRegister(dmaor, 1); // fixed src, decrement dest, long
    check(dma.advance(1) == 1 && memory.read(0x204,4) == 0x12345678 && dma.readRegister(base+0x14) == 0x200 && dma.pending() == 2, "longword transfer and decrement mode");
    dma.reset(); dma.writeRegister(base, 0x101); dma.writeRegister(base+4, 0x200); dma.writeRegister(base+8, 1);
    dma.writeRegister(base+12, 0x5421); dma.writeRegister(dmaor, 1);
    check(dma.advance(1) == 0 && dma.addressError() && !dma.pending(), "misaligned transfer sets address error");
    dma.writeRegister(dmaor, 1, 4); check(dma.addressError(), "unread AE cannot clear");
    check((dma.readRegister(dmaor) & 4) != 0, "AE is visible");
    dma.writeRegister(dmaor, 1, 4); check(!dma.addressError(), "read then zero clears AE");
    dma.signalNmi(); check(dma.advance(1) == 0 && (dma.readRegister(dmaor) & 2), "NMI suspends transfer");
    dma.writeRegister(dmaor, 1, 2); check((dma.readRegister(dmaor) & 2) == 0, "read then zero clears NMI");
    dma.reset(); dma.writeRegister(base+12, 0x401); dma.writeRegister(dmaor, 1);
    bool rejected = false; try { dma.advance(1); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected && !dma.pending(), "unsupported quadword fails without false completion");
    dma.reset();
    for (unsigned ch = 0; ch < 4; ++ch) {
        const uint32_t regs = base + ch * 16;
        memory.bytes[0x100 + ch] = static_cast<uint8_t>(0x10 + ch);
        dma.writeRegister(regs, 0x100 + ch); dma.writeRegister(regs + 4, 0x300 + ch);
        dma.writeRegister(regs + 8, 1); dma.writeRegister(regs + 12, 0x405);
        dma.writeRegister(regs + 12, 0x10, 0x70); // byte width via lane mask
    }
    dma.writeRegister(dmaor, 0x201); // CH2 > CH0 > CH1 > CH3
    check(dma.advance(1) == 1 && memory.bytes[0x302] == 0x12 && dma.pending() == 4, "fixed priority chooses channel 2 first");
    check(dma.advance(3) == 3 && dma.pending() == 15 && memory.bytes[0x303] == 0x13, "all four channels complete independently");
    std::cout << "SH-4 DMA synthetic checks: " << errors << " failures\n";
    return errors ? 1 : 0;
}
