// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace hikaru {

// Bounded JVS 837-13551-compatible I/O node. Input switch words are active
// high in JVS wire order: 0x8000 start, 0x4000 service, 0x2000 up,
// 0x1000 down, 0x0800 left, 0x0400 right; lower bits are game buttons.
class JvsNode {
public:
    struct Inputs {
        bool test = false;
        bool service = false; // player 1 service switch
        std::array<uint16_t, 2> players{};
        std::array<bool, 2> coin{};
        std::array<uint16_t, 8> analog{{0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000}};
    };
    void reset();
    void reset(const Inputs& inputs);
    void setInputs(const Inputs& inputs);
    void pushByte(uint8_t byte);
    bool popByte(uint8_t& byte);
    bool addressed() const { return node_ != 0; }
    uint8_t address() const { return node_; }
    uint32_t outputs() const { return outputs_; }
    std::size_t queuedBytes() const { return response_.size(); }

private:
    void processFrame();
    void clearParser();
    Inputs inputs_{};
    std::array<uint16_t, 2> coins_{};
    std::array<bool, 2> previousCoin_{};
    std::array<uint8_t, 257> frame_{};
    std::size_t frameSize_ = 0;
    std::size_t expected_ = 0;
    bool receiving_ = false;
    bool escape_ = false;
    uint8_t node_ = 0;
    uint32_t outputs_ = 0;
    std::deque<uint8_t> response_;
};
} // namespace hikaru
