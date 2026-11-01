// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/sh4_cpu.h"
#include <array>
#include <cfenv>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using hikaru::Sh4Cpu;
struct Memory {
    std::array<uint8_t, 1024> data{};
    uint32_t read(uint32_t address, unsigned size) {
        if (address > data.size() || size > data.size() - address) throw std::out_of_range("read");
        uint32_t value = 0; for (unsigned i = 0; i < size; ++i) value |= uint32_t(data[address+i]) << (8*i); return value;
    }
    void write(uint32_t address, uint32_t value, unsigned size) {
        if (address > data.size() || size > data.size() - address) throw std::out_of_range("write");
        for (unsigned i = 0; i < size; ++i) data[address+i] = uint8_t(value >> (8*i));
    }
    Sh4Cpu::Bus bus() { return {[this](uint32_t a, unsigned s) { return read(a,s); }, [this](uint32_t a, uint32_t d, unsigned s) { write(a,d,s); }}; }
    void op(uint32_t address, uint16_t opcode) { write(address, opcode, 2); }
};
int main() {
    unsigned errors = 0;
    auto check = [&](bool condition, const char* label) { if (!condition) { ++errors; std::cerr << label << '\n'; } };
    Memory a, b;
    Sh4Cpu x(a.bus()), y(b.bus());
    check(x.state().pc == 0xa0000000 && x.state().sr == 0x700000f0, "reset registers");
    x.state().pc = y.state().pc = 0;
    a.op(0, 0xe0ff); a.op(2, 0x7002); a.op(4, 0x2112); // mov #-1,r0; add #2,r0; mov.l r1,@r1
    x.state().r[1] = 100;
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0xffffffff, "signed immediate");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 1, "wrapping add");
    check(x.step().status == Sh4Cpu::Status::Executed && a.read(100,4) == 100, "memory store");
    check(y.state().pc == 0 && y.state().r[0] == 0 && b.read(100,4) == 0, "independent state and bus");
    a.op(6, 0x2116); // mov.l r1,@-r1 stores original r1
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[1] == 96 && a.read(96,4) == 100, "predecrement alias");
    a.op(8, 0x6116); a.write(96,0x12345678,4); // mov.l @r1+,r1 (no increment)
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[1] == 0x12345678, "postincrement alias");
    a.op(10, 0xb002); a.op(12, 0xe205); a.op(18, 0x000b); a.op(20, 0xe306);
    check(x.step().status == Sh4Cpu::Status::Executed && x.inDelaySlot() && x.state().pc == 12, "bsr schedules delay");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[2] == 5 && x.state().pc == 18 && x.state().pr == 14, "bsr executes slot and links");
    check(x.step().status == Sh4Cpu::Status::Executed && x.inDelaySlot(), "rts schedules delay");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[3] == 6 && x.state().pc == 14, "rts slot then target");
    a.op(14, 0xffff);
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 14, "unknown opcode stops at PC");
    a.op(14, 0xa000); a.op(16, 0xa000);
    check(x.step().status == Sh4Cpu::Status::Executed && x.inDelaySlot(), "bra schedules slot");
    check(x.step().status == Sh4Cpu::Status::UnsupportedDelaySlot && x.state().pc == 16 && x.inDelaySlot(), "branch in slot rejected");
    x.reset(); check(x.state().pc == 0xa0000000 && !x.inDelaySlot() && x.state().r[0] == 0, "reset clears CPU and branch");
    x.state().pc = 30;
    x.state().r[0] = 0x1234; x.state().rBank[0] = 0x5678; x.state().r[8] = 0x40000000;
    a.op(30, 0x480e); // ldc r8,SR: leave the privileged bank
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0x5678 && x.state().rBank[0] == 0x1234, "SR switches register bank");
    x.state().r[8] = 0x60000000; a.op(32, 0x480e);
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0x1234, "SR switches register bank back");
    a.op(34, 0xf09d); a.op(36, 0xf01d); a.op(38, 0xfbfd); a.op(40, 0xf00d);
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x3f800000, "FLDI1 exact bits");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fpul == 0x3f800000, "FLDS raw transfer");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().xf[0] == 0x3f800000 && x.state().fr[0] == 0, "FRCHG swaps banks");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x3f800000, "FSTS raw transfer");
    a.op(42, 0xffff);
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 42, "unsupported FPU opcode stops");
    x.state().pc = 44; x.state().r[0] = 0xffffffff; x.state().r[1] = 0;
    x.state().sr = (x.state().sr & ~1u) | 1u; a.op(44, 0x301e); // addc r1,r0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0 && (x.state().sr & 1), "ADDC carry");
    x.state().r[0] = 0x80000000; x.state().r[1] = 1; a.op(46, 0x301b); // subv r1,r0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0x7fffffff && (x.state().sr & 1), "SUBV signed overflow");
    x.state().r[0] = 0x80000000; x.state().r[1] = 0xffffffff; a.op(48, 0x401c); // shad r1,r0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 0xc0000000, "SHAD arithmetic right");
    x.state().r[0] = 0xffffffff; x.state().r[1] = 2; a.op(50, 0x3015); // dmulu.l r1,r0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().mach == 1 && x.state().macl == 0xfffffffe, "DMULU double width");
    x.reset(); x.state().pc = 0x200; x.state().vbr = 0xfffffa40; x.state().sr = 0x40000041;
    x.state().r[0] = 0x1234; x.state().rBank[0] = 0x5678; x.state().r[15] = 0x900;
    check(!x.interrupt(0x320, 4) && x.state().pc == 0x200, "interrupt masked by IMASK");
    check(x.interrupt(0x320, 5) && x.state().pc == 0x40 && x.state().spc == 0x200 && x.state().ssr == 0x40000041 && x.state().sgr == 0x900 && x.state().intevt == 0x320, "interrupt saves state and vectors");
    check(x.state().r[0] == 0x5678 && !x.interrupt(0x1c0, 15, true), "interrupt bank switch and blocked NMI");
    a.op(0x40, 0x002b); a.op(0x42, 0x0129); // RTE; MOVT r1
    check(x.step().status == Sh4Cpu::Status::Executed && x.inDelaySlot(), "RTE schedules delay slot");
    check(!x.interrupt(0x320, 15), "no interrupt between branch and slot");
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().pc == 0x200 && x.state().sr == 0x40000041 && x.state().r[0] == 0x1234 && x.state().r[1] == 1 && x.state().rBank[1] == 0, "RTE slot uses restored T and register bank");
    x.state().pc = 0x200; check(x.interrupt(0x320, 5), "second interrupt accepted");
    a.op(0x42, 0xe007); // MOV #7,R0 in restored bank.
    check(x.step().status == Sh4Cpu::Status::Executed && x.step().status == Sh4Cpu::Status::Executed && x.state().r[0] == 7 && x.state().rBank[0] == 0x5678, "RTE slot writes restored register bank");
    x.state().pc = 0x200; check(x.interrupt(0x320, 5), "third interrupt accepted");
    a.op(0x42, 0xffff);
    check(x.step().status == Sh4Cpu::Status::Executed && x.step().status == Sh4Cpu::Status::UnsupportedDelaySlot && x.state().sr == 0x70000041 && x.state().r[0] == 0x5678 && x.state().rBank[0] == 7, "unsupported RTE slot restores pre-slot bank and SR");
    x.reset();
    x.state().sr = 0x400000f0;
    check(x.interrupt(0x1c0, 0, true), "NMI bypasses IMASK");
    x.reset(); x.state().pc = 80; x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0x40000000;
    a.op(80, 0xf010); // fadd fr1,fr0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x40400000, "exact FADD");
    x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0x40400000; a.op(82, 0xf013); // fdiv fr1,fr0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x3eaaaaaa && (x.state().fpscr & 0x1004) == 0x1004, "inexact FDIV rounds toward zero and sets flags");
    x.state().fr[0] = 0x40800000; a.op(84, 0xf06d); // fsqrt fr0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x40000000, "FSQRT");
    x.state().fpul = 0xfffffffe; a.op(86, 0xf02d); // float fpul,fr0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0xc0000000, "FLOAT signed conversion");
    x.state().fr[0] = 0x3fc00000; a.op(88, 0xf03d); // ftrc fr0,fpul
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fpul == 1 && (x.state().fpscr & 0x1004) == 0x1004, "FTRC truncates and sets inexact");
    x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0x40000000; x.state().fr[2] = 0x40400000;
    a.op(90, 0xf21e); // fmac fr0,fr1,fr2
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[2] == 0x40a00000, "FMAC");
    for (unsigned i = 0; i < 4; ++i) { x.state().fr[i] = 0x3f800000; x.state().fr[4+i] = 0x40000000; }
    a.op(92, 0xf1ed); // fipr fv4,fv0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[3] == 0x41000000, "FIPR dot product");
    for (unsigned i = 0; i < 16; ++i) x.state().xf[i] = (i % 5 == 0) ? 0x3f800000 : 0;
    a.op(94, 0xf1fd); // ftrv xmtrx,fv0
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x3f800000 && x.state().fr[3] == 0x41000000, "FTRV identity matrix");
    x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0; a.op(96, 0xf013);
    const int oldRounding = std::fegetround();
    check(x.step().status == Sh4Cpu::Status::Executed && x.state().fr[0] == 0x7f800000 && (x.state().fpscr & 0x8020) == 0x8020 && std::fegetround() == oldRounding, "FDIV zero sets flags and restores host rounding");
    x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0x40400000; x.state().fpscr |= 0x80; a.op(98, 0xf013);
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 98 && x.state().fr[0] == 0x3f800000, "enabled FPU traps remain unsupported");
    x.reset(); x.state().pc = 100; x.state().sr &= ~1u;
    a.op(100, 0x8d05); a.op(102, 0xa000); // BT/S not taken; branch in slot remains illegal.
    check(x.step().status == Sh4Cpu::Status::Executed && x.inDelaySlot(), "not-taken BT/S still schedules slot");
    check(x.step().status == Sh4Cpu::Status::UnsupportedDelaySlot && x.state().pc == 102, "not-taken BT/S rejects branch in slot");
    x.reset(); x.state().pc = 104; x.state().fr[0] = 0x3f800000; x.state().fr[1] = 0x3f800000; x.state().fpscr |= 0x3f004;
    a.op(104, 0xf014); // FCMP/EQ FR1,FR0
    check(x.step().status == Sh4Cpu::Status::Executed && (x.state().sr & 1) && !(x.state().fpscr & 0x3f000) && (x.state().fpscr & 4), "FCMP clears cause and retains sticky flags");
    x.state().fpscr &= ~0x40000u; x.state().fr[0] = 1; a.op(106, 0xf010);
    const uint32_t beforeFpscr = x.state().fpscr;
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 106 && x.state().fr[0] == 1 && x.state().fpscr == beforeFpscr, "DN=0 arithmetic subnormal stops unchanged");
    a.op(106, 0xf03d);
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 106 && x.state().fpscr == beforeFpscr, "DN=0 FTRC subnormal stops unchanged");
    x.state().fpscr |= 0x40000u; a.op(106, 0xf014);
    x.state().fr[1] = 0;
    check(x.step().status == Sh4Cpu::Status::Executed && (x.state().sr & 1), "DN=1 comparison flushes subnormal input");
    x.reset(); x.state().pc = 112; x.state().r[2] = 0xe0000020; a.op(112, 0x0283);
    check(x.step().status == Sh4Cpu::Status::UnsupportedOpcode && x.state().pc == 112, "store queue PREF requires bus callback");
    x.state().r[2] = 0x0c000000;
    check(x.step().status == Sh4Cpu::Status::Executed, "ordinary PREF acts as cache hint");
    auto bus = a.bus(); uint32_t flushed = 0;
    bus.prefetch = [&](uint32_t address) { flushed = address; };
    Sh4Cpu prefetch(bus); prefetch.state().pc = 112; prefetch.state().r[2] = 0xe3ffffe0;
    check(prefetch.step().status == Sh4Cpu::Status::Executed && flushed == 0xe3ffffe0, "store queue PREF calls bus with original address");
    Sh4Cpu empty({}); check(empty.step().status == Sh4Cpu::Status::MissingBus, "unconnected bus rejected");
    std::cout << "SH-4 synthetic checks: " << errors << " failures\n";
    return errors ? 1 : 0;
}
