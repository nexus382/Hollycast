// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace hikaru {
class StateReader;
class StateWriter;

// Menu requests cross threads; only the emulation thread samples/resets.
// A physical release must reach JVS before the next queued rising edge.
class CoinPulseQueue {
public:
    void enqueue() { pending.fetch_add(1, std::memory_order_relaxed); }
    void saveState(StateWriter& writer) const;
    void loadState(StateReader& reader);
    void reset() { pending = 0; previous = false; }
    bool sample(bool physical) {
        bool coin = physical;
        if (!physical && !previous && pending.load(std::memory_order_relaxed)) {
            pending.fetch_sub(1, std::memory_order_relaxed);
            coin = true;
        }
        previous = coin;
        return coin;
    }
private:
    std::atomic<unsigned> pending{0};
    bool previous = false;
};

// Bounded JVS node for Sega 837-13551 and 837-13844 I/O boards. Input switch
// words are active high in JVS wire order: 0x8000 start, 0x4000 service, 0x2000 up,
// 0x1000 down, 0x0800 left, 0x0400 right; lower bits are game buttons.
class JvsNode {
public:
    enum class BoardProfile { Sega83713551, Sega83713844 };
    struct Inputs {
        bool test = false;
        bool service = false; // player 1 service switch
        std::array<uint16_t, 2> players{};
        std::array<bool, 2> coin{};
        std::array<uint16_t, 8> analog{{0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000}};
    };
    void reset();
    void reset(const Inputs& inputs);
    // Select the physical cabinet board before execution. This fixed game
    // configuration survives reset/load and is not part of the state payload.
    void setBoardProfile(BoardProfile profile) { boardProfile_ = profile; }
    void setInputs(const Inputs& inputs);
    void pushByte(uint8_t byte);
    bool popByte(uint8_t& byte);
    bool addressed() const { return node_ != 0; }
    uint8_t address() const { return node_; }
    uint32_t outputs() const { return outputs_; }
    std::size_t queuedBytes() const { return response_.size(); }
    const Inputs& inputs() const { return inputs_; }
    void saveState(StateWriter& writer) const;
    void loadState(StateReader& reader);

private:
    void processFrame();
    void clearParser();
    BoardProfile boardProfile_ = BoardProfile::Sega83713551;
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
