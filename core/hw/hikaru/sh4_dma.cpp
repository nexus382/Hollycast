// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Register fields and auto-request operation follow the Renesas SH7750 manual
// section 14 and core/hw/sh4/modules/dmac.cpp.
#include "sh4_dma.h"

#include <stdexcept>
#include <utility>

namespace hikaru {
namespace {
uint32_t offset(uint32_t address)
{
    if ((address >> 24) != 0xff && (address >> 24) != 0x1f)
        throw std::invalid_argument("invalid SH-4 DMAC address");
    const uint32_t physical = address & 0x1fffffffu;
    if (physical < 0x1fa00000 || physical > 0x1fa00040 || (physical & 3))
        throw std::invalid_argument("invalid SH-4 DMAC register");
    return physical - 0x1fa00000;
}
}
Sh4Dma::Sh4Dma(Bus bus) : bus_(std::move(bus)) {}
void Sh4Dma::setBus(Bus bus) { bus_ = std::move(bus); }
void Sh4Dma::reset()
{
    channels_ = {};
    dmaor_ = dmaorRead_ = 0;
    roundRobin_ = 0;
}
uint32_t Sh4Dma::readRegister(uint32_t address, uint32_t mask)
{
    const uint32_t reg = offset(address);
    if (reg == 0x40) { dmaorRead_ |= dmaor_ & mask & 6; return dmaor_; }
    Channel& c = channels_[reg / 16];
    switch (reg & 15) {
    case 0: return c.sar;
    case 4: return c.dar;
    case 8: return c.count;
    default: c.teRead |= (c.control & mask & 2) != 0; return c.control;
    }
}
void Sh4Dma::writeRegister(uint32_t address, uint32_t data, uint32_t mask)
{
    const uint32_t reg = offset(address);
    auto merge = [data, mask](uint32_t old, uint32_t writable) {
        const uint32_t selected = mask & writable;
        return (old & ~selected) | (data & selected);
    };
    if (reg == 0x40) {
        const uint32_t flags = dmaor_ & 6u & ~(dmaorRead_ & mask & ~data);
        dmaor_ = (merge(dmaor_, 0x8301u) & ~6u) | flags;
        dmaorRead_ &= flags;
        return;
    }
    Channel& c = channels_[reg / 16];
    switch (reg & 15) {
    case 0: c.sar = merge(c.sar, 0xffffffffu); break;
    case 4: c.dar = merge(c.dar, 0xffffffffu); break;
    case 8: c.count = merge(c.count, 0x00ffffffu); break;
    default: {
        const uint32_t te = (c.control & 2u) && !(c.teRead && (mask & 2u) && !(data & 2u)) ? 2u : 0u;
        c.control = (merge(c.control, reg < 0x20 ? 0xff0ffff5u : 0xff0afff5u) & ~2u) | te;
        if (!te) c.teRead = false;
        break;
    }
    }
}
uint32_t Sh4Dma::advance(uint32_t maxUnits)
{
    static constexpr unsigned order[3][4] = {{0,1,2,3}, {0,2,3,1}, {2,0,1,3}};
    uint32_t transferred = 0;
    while (transferred < maxUnits && (dmaor_ & 7u) == 1) {
        unsigned selected = 4;
        const unsigned mode = (dmaor_ >> 8) & 3;
        for (unsigned rank = 0; rank < 4; ++rank) {
            const unsigned index = mode == 3 ? (roundRobin_ + rank) & 3 : order[mode][rank];
            if ((channels_[index].control & 3u) == 1) { selected = index; break; }
        }
        if (selected == 4) break;
        Channel& c = channels_[selected];
        const uint32_t control = c.control;
        const unsigned request = (control >> 8) & 15, widthCode = (control >> 4) & 7;
        const unsigned sourceMode = (control >> 12) & 3, destinationMode = (control >> 14) & 3;
        if ((dmaor_ & 0x8000u) || request != 4 || widthCode < 1 || widthCode > 3 || sourceMode == 3 || destinationMode == 3)
            throw std::runtime_error("unsupported SH-4 DMA mode or transfer width");
        if (!bus_.read || !bus_.write)
            throw std::runtime_error("SH-4 DMA bus is not connected");
        const unsigned size = 1u << (widthCode - 1);
        if ((c.sar & (size - 1)) || (c.dar & (size - 1))) { dmaor_ |= 4; dmaorRead_ &= ~4u; break; }
        const uint32_t data = bus_.read(c.sar, size);
        bus_.write(c.dar, data, size);
        if (sourceMode == 1) c.sar += size;
        else if (sourceMode == 2) c.sar -= size;
        if (destinationMode == 1) c.dar += size;
        else if (destinationMode == 2) c.dar -= size;
        c.count = (c.count - 1) & 0x00ffffffu;
        if (!c.count) { c.control |= 2; c.teRead = false; }
        ++transferred;
        if (mode == 3 && (!(control & 0x80) || !c.count)) roundRobin_ = (selected + 1) & 3;
    }
    return transferred;
}
uint8_t Sh4Dma::pending() const
{
    uint8_t result = 0;
    for (unsigned i = 0; i < 4; ++i)
        if ((channels_[i].control & 6u) == 6u) result |= static_cast<uint8_t>(1u << i);
    return result;
}
bool Sh4Dma::addressError() const { return (dmaor_ & 4) != 0; }
void Sh4Dma::signalNmi() { dmaor_ |= 2; dmaorRead_ &= ~2u; }
} // namespace hikaru
