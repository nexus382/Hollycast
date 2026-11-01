// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Protocol and 837-13551 feature values follow hw/maple/maple_jvs.cpp.
#include "jvs.h"

#include <algorithm>
#include <vector>

namespace hikaru {
namespace {
constexpr uint8_t sync = 0xe0, escape = 0xd0;
constexpr std::size_t maxPayload = 254, maxQueued = 1024;
}
void JvsNode::clearParser()
{
    frameSize_ = expected_ = 0;
    receiving_ = escape_ = false;
}
void JvsNode::reset()
{
    reset(Inputs{});
}
void JvsNode::reset(const Inputs& inputs)
{
    inputs_ = inputs;
    coins_.fill(0);
    previousCoin_ = inputs.coin;
    node_ = 0;
    outputs_ = 0;
    response_.clear();
    clearParser();
}
void JvsNode::setInputs(const Inputs& inputs)
{
    for (unsigned i = 0; i < coins_.size(); ++i) {
        if (inputs.coin[i] && !previousCoin_[i] && coins_[i] < 0x3fff)
            ++coins_[i];
        previousCoin_[i] = inputs.coin[i];
    }
    inputs_ = inputs;
}
bool JvsNode::popByte(uint8_t& byte)
{
    if (response_.empty()) return false;
    byte = response_.front();
    response_.pop_front();
    return true;
}
void JvsNode::pushByte(uint8_t byte)
{
    if (byte == sync) {
        clearParser();
        receiving_ = true;
        return;
    }
    if (!receiving_) return;
    if (!escape_ && byte == escape) { escape_ = true; return; }
    if (escape_) {
        if (byte != 0xcf && byte != 0xdf) { clearParser(); return; }
        byte = static_cast<uint8_t>(byte + 1);
        escape_ = false;
    }
    if (frameSize_ >= frame_.size()) { clearParser(); return; }
    frame_[frameSize_++] = byte;
    if (frameSize_ == 2) {
        if (byte == 0) { clearParser(); return; }
        expected_ = 2 + byte;
    }
    if (expected_ && frameSize_ == expected_) {
        processFrame();
        clearParser();
    }
}
void JvsNode::processFrame()
{
    if (frameSize_ < 4) return;
    unsigned checksum = 0;
    for (std::size_t i = 0; i + 1 < frameSize_; ++i) checksum += frame_[i];
    if (static_cast<uint8_t>(checksum) != frame_[frameSize_ - 1]) return;
    const uint8_t destination = frame_[0];
    if (destination != 0xff && (!node_ || destination != node_)) return;
    const std::size_t end = frameSize_ - 1;
    if (frame_[2] == 0xf0) {
        if (destination == 0xff && end == 4 && frame_[3] == 0xd9) node_ = 0;
        return;
    }
    auto coins = coins_;
    uint32_t outputs = outputs_;
    uint8_t newNode = node_;
    std::vector<uint8_t> payload{1};
    auto append = [&](uint8_t value) {
        if (payload.size() >= maxPayload) return false;
        payload.push_back(value); return true;
    };
    auto need = [&](std::size_t pos, std::size_t count) { return pos <= end && count <= end - pos; };
    std::size_t pos = 2;
    while (pos < end) {
        const uint8_t cmd = frame_[pos++];
        if (!append(1)) return;
        switch (cmd) {
        case 0xf1:
            if (!need(pos, 1) || destination != 0xff || node_ != 0 || frame_[pos] == 0 || frame_[pos] > 31) return;
            newNode = frame_[pos++];
            break;
        case 0x10: {
            static constexpr char id[] = "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10";
            for (char c : id) if (!append(static_cast<uint8_t>(c))) return;
            break;
        }
        case 0x11: if (!append(0x11)) return; break;
        case 0x12: if (!append(0x20)) return; break;
        case 0x13: if (!append(0x10)) return; break;
        case 0x14: {
            static constexpr uint8_t features[] = {1,2,13,0, 2,2,0,0, 3,8,16,0, 0x12,6,0,0, 0};
            for (uint8_t b : features) if (!append(b)) return;
            break;
        }
        case 0x15:
            while (pos < end && frame_[pos] != 0) ++pos;
            if (pos == end) return;
            ++pos;
            break;
        case 0x20: {
            if (!need(pos, 2)) return;
            const unsigned players = frame_[pos++], bytes = frame_[pos++];
            if (players > 2 || bytes == 0 || bytes > 2) { payload.back() = 2; pos = end; break; }
            if (!append(inputs_.test ? 0x80 : 0)) return;
            for (unsigned i = 0; i < players; ++i) {
                const uint16_t bits = inputs_.players[i] | ((i == 0 && inputs_.service) ? 0x4000u : 0);
                if (!append(static_cast<uint8_t>(bits >> 8))) return;
                if (bytes == 2 && !append(static_cast<uint8_t>(bits))) return;
            }
            break;
        }
        case 0x21: {
            if (!need(pos, 1)) return;
            const unsigned slots = frame_[pos++];
            if (slots > coins.size()) { payload.back() = 2; pos = end; break; }
            for (unsigned i = 0; i < slots; ++i)
                if (!append(static_cast<uint8_t>(coins[i] >> 8)) || !append(static_cast<uint8_t>(coins[i]))) return;
            break;
        }
        case 0x22: {
            if (!need(pos, 1)) return;
            const unsigned axes = frame_[pos++];
            if (axes > inputs_.analog.size()) { payload.back() = 2; pos = end; break; }
            for (unsigned i = 0; i < axes; ++i)
                if (!append(static_cast<uint8_t>(inputs_.analog[i] >> 8)) || !append(static_cast<uint8_t>(inputs_.analog[i]))) return;
            break;
        }
        case 0x30: case 0x35: {
            if (!need(pos, 3)) return;
            const unsigned slot = frame_[pos++], amount = (unsigned(frame_[pos]) << 8) | frame_[pos + 1];
            pos += 2;
            if (slot == 0 || slot > coins.size()) { payload.back() = 2; pos = end; break; }
            uint16_t& count = coins[slot - 1];
            count = cmd == 0x30 ? static_cast<uint16_t>(count > amount ? count - amount : 0)
                                : static_cast<uint16_t>(std::min<unsigned>(0x3fff, unsigned(count) + amount));
            break;
        }
        case 0x32: {
            if (!need(pos, 1)) return;
            const unsigned count = frame_[pos++];
            if (!need(pos, count)) return;
            for (unsigned i = 0; i < count; ++i) {
                if (i < 4) outputs = (outputs & ~(0xffu << (8 * i))) | (uint32_t(frame_[pos + i]) << (8 * i));
            }
            pos += count;
            break;
        }
        default: payload.back() = 2; pos = end; break;
        }
    }
    // Build the whole escaped frame before committing state or queueing bytes.
    std::vector<uint8_t> raw{0, static_cast<uint8_t>(payload.size() + 1)};
    raw.insert(raw.end(), payload.begin(), payload.end());
    unsigned sum = 0;
    for (uint8_t b : raw) sum += b;
    raw.push_back(static_cast<uint8_t>(sum));
    std::vector<uint8_t> encoded{sync};
    for (uint8_t b : raw) {
        if (b == sync || b == escape) { encoded.push_back(escape); --b; }
        encoded.push_back(b);
    }
    if (encoded.size() > maxQueued - response_.size()) return;
    coins_ = coins; outputs_ = outputs; node_ = newNode;
    response_.insert(response_.end(), encoded.begin(), encoded.end());
}
} // namespace hikaru
