// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 The Hollycast Authors
#include "protection.h"
#include "state_io.h"
#include <stdexcept>

namespace hikaru {
Protection::Protection(std::optional<uint32_t> key)
{
    if (key) crypto_.emplace(*key, [this](uint32_t address) {
        const unsigned offset = (address & 0x7fff) * 2;
        // The chip consumes the RAM halfword with the byte swap used by
        // MAME hikaru_state::prot_ram_r, independently of the SH-4 bus width.
        return static_cast<uint16_t>((ram_[offset] << 8) | ram_[offset+1]);
    });
}
void Protection::reset() { ram_.fill(0); if (crypto_) crypto_->reset(); }

void Protection::saveState(StateWriter& writer) const
{
    writer.value(crypto_.has_value());
    writer.bytes(ram_.data(), ram_.size());
    if (crypto_)
        crypto_->saveState(writer);
}

void Protection::loadState(StateReader& reader)
{
    bool hasCrypto;
    reader.value(hasCrypto);
    StateReader::require(hasCrypto == crypto_.has_value(),
        "Hikaru protection chip does not match configured board");
    std::array<uint8_t, 65536> stagedRam;
    reader.bytes(stagedRam.data(), stagedRam.size());
    if (crypto_)
        crypto_->loadState(reader);
    ram_ = stagedRam;
}
namespace {
bool validSize(unsigned size) { return size==1 || size==2 || size==4 || size==8; }
}
uint64_t Protection::read(uint32_t address, unsigned size)
{
    if (!validSize(size) || (address & (size-1)))
        throw std::invalid_argument("Hikaru protection invalid read width/alignment");
    if (address < 0x0a000000 || address > 0x0b000000-size)
        throw std::out_of_range("Hikaru protection read outside local-bus aperture");
    // MAME's localbus_map[_5881] leaves holes at the address space's default
    // unmapped value (zero). Boards without the chip map neither RAM nor regs.
    if (!crypto_) return 0;
    if (address <= 0x0a010000-size) {
        uint64_t result = 0;
        for (unsigned i = 0; i < size; ++i)
            result |= uint64_t(ram_[address-0x0a000000+i]) << (i*8);
        return result;
    }
    uint64_t result = 0;
    // A wide CPU read touches each 16-bit register once. Only the data port
    // advances the cipher, including when just one of its byte lanes is read.
    for (unsigned i = 0; i < size; i += 2) {
        const uint32_t reg = (address+i) & ~1u;
        const uint16_t value = reg == 0x0a01000e ? crypto_->read() : 0;
        const uint64_t lane = size == 1 ? (value >> ((address & 1)*8)) & 0xff : value;
        result |= lane << (i*8);
    }
    return result;
}
void Protection::write(uint32_t address, uint64_t data, unsigned size)
{
    if (!validSize(size) || (address & (size-1)))
        throw std::invalid_argument("Hikaru protection invalid write width/alignment");
    if (address < 0x0a000000 || address > 0x0b000000-size)
        throw std::out_of_range("Hikaru protection write outside local-bus aperture");
    if (!crypto_) return;
    if (address <= 0x0a010000-size) {
        for (unsigned i = 0; i < size; ++i)
            ram_[address-0x0a000000+i] = static_cast<uint8_t>(data >> (i*8));
        return;
    }
    // iomap_le installs only these write handlers; reads of write-only regs
    // and writes to unused/read-only regs have no device side effects.
    for (unsigned i = 0; i < size; i += 2) {
        const uint32_t reg = (address+i) & ~1u;
        const uint16_t value = size == 1 ? uint16_t((data & 0xff) << ((address & 1)*8))
                                       : uint16_t(data >> (i*8));
        switch (reg) {
        case 0x0a010008: crypto_->writeAddressLow(value); break;
        case 0x0a01000a: crypto_->writeAddressHigh(value); break;
        case 0x0a01000c: crypto_->writeSubkey(value); break;
        default: break;
        }
    }
}
} // namespace hikaru
