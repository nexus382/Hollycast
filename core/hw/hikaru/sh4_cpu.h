// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace hikaru {

// An isolated, incomplete SH-4 interpreter for synthetic bring-up. It does not
// implement MMU, internal peripherals, synchronous exceptions or timing.
// Includes integer instructions, interrupt entry, and partial single-precision
// FPU execution. Enabled FPU traps and unsupported modes stop explicitly.
// Bus callbacks receive virtual addresses; translation belongs to the caller.
class Sh4Cpu {
public:
    struct Bus {
        std::function<uint32_t(uint32_t address, unsigned size)> read;
        std::function<void(uint32_t address, uint32_t data, unsigned size)> write;
        // Required for PREF to the SH-4 store queue range; the bus owner
        // translates QACR and performs the 32-byte transfer.
        std::function<void(uint32_t address)> prefetch{};
    };
    struct State {
        std::array<uint32_t, 16> r{};
        std::array<uint32_t, 8> rBank{};
        uint32_t pc = 0xa0000000;
        uint32_t pr = 0;
        uint32_t sr = 0x700000f0;
        uint32_t gbr = 0;
        uint32_t vbr = 0;
        uint32_t mach = 0;
        uint32_t macl = 0;
        uint32_t ssr = 0;
        uint32_t spc = 0;
        uint32_t sgr = 0;
        uint32_t dbr = 0;
        uint32_t fpul = 0;
        uint32_t fpscr = 0x00040001;
        uint32_t intevt = 0;
        std::array<uint32_t, 16> fr{};
        std::array<uint32_t, 16> xf{};
    };
    enum class Status { Executed, UnsupportedOpcode, UnsupportedDelaySlot, MissingBus };
    struct Result {
        Status status;
        uint32_t pc;
        uint16_t opcode;
    };

    explicit Sh4Cpu(Bus bus);
    void reset();
    State& state() { return state_; }
    const State& state() const { return state_; }
    bool inDelaySlot() const { return delayed_; }
    Result step();
    // Attempts delivery at an instruction boundary. Masked interrupts and
    // interrupts between a branch and its delay slot are not accepted. NMI
    // bypasses IMASK but, like the existing core, is blocked by SR.BL.
    bool interrupt(uint32_t eventCode, unsigned priority, bool nmi = false);

private:
    void setSr(uint32_t value);
    void setFpscr(uint32_t value);
    Bus bus_;
    State state_;
    bool delayed_ = false;
    uint32_t branchTarget_ = 0;
    bool delayedLink_ = false;
    uint32_t linkValue_ = 0;
    bool delayedRestoreSr_ = false;
    uint32_t restoreSr_ = 0;
};

} // namespace hikaru
