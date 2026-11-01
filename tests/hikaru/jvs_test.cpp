// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
#include "hw/hikaru/jvs.h"
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using hikaru::JvsNode;
namespace {
std::vector<uint8_t> encode(uint8_t destination, const std::vector<uint8_t>& commands)
{
    std::vector<uint8_t> raw{destination, static_cast<uint8_t>(commands.size() + 1)};
    raw.insert(raw.end(), commands.begin(), commands.end());
    unsigned sum = 0; for (uint8_t b : raw) sum += b; raw.push_back(static_cast<uint8_t>(sum));
    std::vector<uint8_t> result{0xe0};
    for (uint8_t b : raw) { if (b == 0xe0 || b == 0xd0) { result.push_back(0xd0); --b; } result.push_back(b); }
    return result;
}
void send(JvsNode& node, uint8_t dest, const std::vector<uint8_t>& commands)
{ for (uint8_t b : encode(dest, commands)) node.pushByte(b); }
std::vector<uint8_t> read(JvsNode& node)
{
    std::vector<uint8_t> result; uint8_t b; bool escaped = false;
    while (node.popByte(b)) {
        if (result.empty()) { result.push_back(b); continue; }
        if (!escaped && b == 0xd0) { escaped = true; continue; }
        result.push_back(static_cast<uint8_t>(b + (escaped ? 1 : 0))); escaped = false;
    }
    return result;
}
bool valid(const std::vector<uint8_t>& r)
{
    if (r.size() < 4 || r[0] != 0xe0 || r[1] != 0 || r.size() != unsigned(r[2]) + 3) return false;
    unsigned sum = 0; for (std::size_t i = 1; i + 1 < r.size(); ++i) sum += r[i];
    return static_cast<uint8_t>(sum) == r.back();
}
}
int main()
{
    unsigned checks = 0, failures = 0;
    auto check = [&](bool ok, const char* label) { ++checks; if (!ok) { ++failures; std::cerr << label << '\n'; } };
    JvsNode node;
    send(node, 0xff, {0xf0, 0xd9}); check(!node.addressed() && read(node).empty(), "broadcast reset has no reply");
    send(node, 0xff, {0xf1, 1}); auto r = read(node);
    check(node.addressed() && node.address() == 1 && valid(r) && r[3] == 1 && r[4] == 1, "address assignment");
    send(node, 1, {0x10}); r = read(node);
    check(valid(r) && std::string(r.begin()+5, r.end()-2) == "SEGA ENTERPRISES,LTD.;I/O BD JVS;837-13551 ;Ver1.00;98/10" && r[r.size()-2] == 0, "837-13551 identity");
    send(node, 1, {0x11,0x12,0x13}); r = read(node);
    check(valid(r) && r[4] == 1 && r[5] == 0x11 && r[6] == 1 && r[7] == 0x20 && r[8] == 1 && r[9] == 0x10, "batched version queries");
    JvsNode::Inputs inputs; inputs.test = true; inputs.service = true; inputs.players[0] = 0x8401; inputs.players[1] = 0x0202; inputs.coin[0] = true; inputs.analog[0] = 0xe0d0; inputs.analog[1] = 0x1234;
    node.setInputs(inputs); node.setInputs(inputs); // held switch counts once
    send(node, 1, {0x20,2,2,0x21,2,0x22,2}); r = read(node);
    check(valid(r) && r[3] == 1 && r[4] == 1 && r[5] == 0x80 && r[6] == 0xc4 && r[7] == 1 && r[8] == 2 && r[9] == 2 && r[10] == 1 && r[11] == 0 && r[12] == 1 && r[13] == 0 && r[14] == 0 && r[15] == 1 && r[16] == 0xe0 && r[17] == 0xd0 && r[18] == 0x12 && r[19] == 0x34, "digital coin analog poll and escaped data");
    inputs.coin[0] = false; node.setInputs(inputs); inputs.coin[0] = true; node.setInputs(inputs);
    send(node, 1, {0x30,1,0,1,0x21,1}); r = read(node);
    check(valid(r) && r[4] == 1 && r[5] == 1 && r[6] == 0 && r[7] == 1, "coin edge and subtraction");
    send(node, 1, {0x35,2,0xff,0xff,0x21,2}); r = read(node);
    check(valid(r) && r[4] == 1 && r[5] == 1 && r[6] == 0 && r[7] == 1 && r[8] == 0x3f && r[9] == 0xff, "coin add targets slot and saturates at 14 bits");
    send(node, 1, {0x32,2,0x5a,0xa5}); r = read(node);
    check(valid(r) && node.outputs() == 0xa55a, "output bytes");
    send(node, 2, {0x10}); check(read(node).empty(), "ignore another address");
    auto bad = encode(1, {0x30,1,0,1}); bad.back() ^= 1; for (uint8_t b : bad) node.pushByte(b);
    check(read(node).empty(), "bad checksum discarded");
    send(node, 1, {0x30,1,0}); check(read(node).empty(), "truncated command discarded");
    send(node, 1, {0x21,1}); r = read(node);
    check(valid(r) && r[5] == 0 && r[6] == 1, "malformed commands have no coin side effects");
    send(node, 1, {0x20,3,2}); r = read(node); check(valid(r) && r[4] == 2, "unsupported dimensions return command error");
    for (unsigned i = 0; i < 100; ++i) send(node, 1, {0x10});
    check(node.queuedBytes() <= 1024, "response queue bounded");
    JvsNode::Inputs held = inputs;
    held.coin = {{true, true}};
    node.reset(held);
    check(!node.addressed() && node.queuedBytes() == 0 && node.outputs() == 0,
        "reset with held inputs clears address, queued replies, and outputs");
    node.setInputs(held);
    node.setInputs(held);
    send(node, 0xff, {0xf1, 1}); r = read(node);
    check(valid(r) && node.address() == 1, "held-input reset permits normal JVS reassignment");
    send(node, 1, {0x21, 2}); r = read(node);
    check(valid(r) && r.size() == 10 && r[4] == 1 && r[5] == 0 && r[6] == 0 && r[7] == 0 && r[8] == 0,
        "coins already held at reset do not generate phantom edges or retain credits");
    send(node, 1, {0x20, 1, 2, 0x22, 1}); r = read(node);
    check(valid(r) && r.size() == 12 && r[4] == 1 && r[5] == 0x80 && r[6] == 0xc4 && r[7] == 1
            && r[8] == 1 && r[9] == 0xe0 && r[10] == 0xd0,
        "reset preserves held switch and analog input levels");
    held.coin[0] = false; node.setInputs(held);
    held.coin[0] = true; node.setInputs(held); node.setInputs(held);
    send(node, 1, {0x21, 2}); r = read(node);
    check(valid(r) && r.size() == 10 && r[5] == 0 && r[6] == 1 && r[7] == 0 && r[8] == 0,
        "first release and repress after reset counts exactly one coin in its slot");
    held.coin[1] = false; node.setInputs(held);
    held.coin[1] = true; node.setInputs(held);
    send(node, 1, {0x21, 2}); r = read(node);
    check(valid(r) && r.size() == 10 && r[5] == 0 && r[6] == 1 && r[7] == 0 && r[8] == 1,
        "second held coin slot requires its own new edge after reset");
    node.reset(held); node.setInputs(held);
    send(node, 0xff, {0xf1, 1}); read(node);
    send(node, 1, {0x21, 2}); r = read(node);
    check(valid(r) && r.size() == 10 && r[5] == 0 && r[6] == 0 && r[7] == 0 && r[8] == 0,
        "repeated reset while coins stay held never adds a phantom credit");
    node.reset(); check(!node.addressed() && node.queuedBytes() == 0, "reset clears node and response queue");
    for (unsigned seed = 0; seed < 1000; ++seed) {
        uint32_t v = seed; for (unsigned i = 0; i < 400; ++i) { v = v * 1664525u + 1013904223u; node.pushByte(v >> 24); }
        check(node.queuedBytes() <= 1024, "random malformed stream remains bounded");
    }
    std::cout << "JVS synthetic checks: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
