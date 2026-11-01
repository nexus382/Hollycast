// SPDX-License-Identifier: BSD-3-Clause
// Copyright 2026 The Hollycast Authors
#include "protection.h"
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
namespace {
bool validSize(unsigned size) { return size==1 || size==2 || size==4 || size==8; }
}
uint64_t Protection::read(uint32_t address, unsigned size)
{
    if (!validSize(size) || (address & (size-1)))
        throw std::invalid_argument("Hikaru protection invalid read width/alignment");
    // MAME's base local-bus map installs neither protection RAM nor the
    // 315-5881 window; both are installed only by localbus_map_5881.
    if (!crypto_ && address >= 0x0a000000 && address <= 0x0a010010-size)
        return size == 8 ? UINT64_MAX : (uint64_t{1} << (size * 8)) - 1;
    if (address >= 0x0a000000 && address <= 0x0a010000-size) {
        uint64_t result=0;
        for(unsigned i=0;i<size;++i) result|=uint64_t(ram_[address-0x0a000000+i])<<(i*8);
        return result;
    }
    if (address >= 0x0a010000 && address <= 0x0a010010-size) {
        if(size!=2) throw std::invalid_argument("315-5881 registers require 16-bit reads");
        if(address==0x0a01000e) return crypto_->read();
        if(address==0x0a010000) return 0; // Synchronous operation is ready.
        throw std::out_of_range("315-5881 unmapped register read");
    }
    throw std::out_of_range("Hikaru protection unmapped read");
}
void Protection::write(uint32_t address, uint64_t data, unsigned size)
{
    if (!validSize(size) || (address & (size-1)))
        throw std::invalid_argument("Hikaru protection invalid write width/alignment");
    if (!crypto_ && address >= 0x0a000000 && address <= 0x0a010010-size) return;
    if (address >= 0x0a000000 && address <= 0x0a010000-size) {
        for(unsigned i=0;i<size;++i) ram_[address-0x0a000000+i]=static_cast<uint8_t>(data>>(i*8));
        return;
    }
    if (address >= 0x0a010000 && address <= 0x0a010010-size) {
        if(size!=2) throw std::invalid_argument("315-5881 registers require 16-bit writes");
        switch(address & 15) {
        case 8: crypto_->writeAddressLow(static_cast<uint16_t>(data)); break;
        case 10: crypto_->writeAddressHigh(static_cast<uint16_t>(data)); break;
        case 12: crypto_->writeSubkey(static_cast<uint16_t>(data)); break;
        default: throw std::out_of_range("315-5881 unmapped register write");
        }
        return;
    }
    throw std::out_of_range("Hikaru protection unmapped write");
}
} // namespace hikaru
