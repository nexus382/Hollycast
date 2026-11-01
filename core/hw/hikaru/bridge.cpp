// SPDX-License-Identifier: BSD-3-Clause
// Based on MAME's Sega 315-6154 device, copyright Samuele Zannoli.
// https://github.com/mamedev/mame/blob/43b995da4a2af46c9f48c6a5536953439adfb1ab/src/mame/sega/315-6154.cpp
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
#include "bridge.h"

#include <utility>

namespace hikaru {
namespace {
constexpr uint32_t apertureSize = 0x04000000;
constexpr uint32_t pageMask = 0x00ffffff;

bool validAperture(unsigned aperture, uint32_t offset)
{
    return aperture < 3 && offset < apertureSize && (offset & 3) == 0;
}

uint32_t normalizeAddress(uint32_t address)
{
    return address < 0xc0000000u ? address & 0x7fffffffu : address;
}
}

Bridge::Bridge(Mode mode, Bus bus, Irq irq)
    : mode_(mode), bus_(std::move(bus)), irq_(std::move(irq))
{
}

void Bridge::reset()
{
    registers_.fill(0);
    dmaSource_ = dmaDestination_ = dmaRemaining_ = 0;
    updateIrq();
}

uint32_t Bridge::registerRead(uint32_t offset) const
{
    if (offset >= 0x100 || (offset & 3) != 0)
        return 0xffffffffu;
    return offset == 0 ? static_cast<uint32_t>(mode_) : registers_[offset / 4];
}

void Bridge::registerWrite(uint32_t offset, uint32_t data, uint32_t mask)
{
    if (offset >= 0x100 || (offset & 3) != 0)
        return;
    // Freeze a transfer's programming until completion. This is an explicit
    // emulator policy; the hardware's behavior on reprogramming is unknown.
    if (offset >= 0x30 && offset <= 0x38 && (registers_[0x38 / 4] & 0x01000000u))
        return;
    uint32_t& reg = registers_[offset / 4];
    if (offset == 0x04)
    {
        reg &= ~(data & mask & 0xffffu);
        mask &= 0xffff0000u;
    }
    reg = (reg & ~mask) | (data & mask);
    if (offset == 0x04)
        updateIrq();

    if (offset == 0x38 && (mask & 0xff000000u) && (reg & 0x01000000u))
    {
        dmaSource_ = normalizeAddress(registers_[0x30 / 4]);
        dmaDestination_ = normalizeAddress(registers_[0x34 / 4]);
        dmaRemaining_ = reg & 0x00ffffffu;
    }
}

uint32_t Bridge::targetAddress(unsigned aperture, uint32_t offset) const
{
    const unsigned page = offset >> 24;
    uint32_t base = ((registers_[(0x10 / 4) + aperture] >> (page * 8)) & 0xffu) << 24;
    // MAME's provisional local-bus interpretation: clear bit 31 below
    // 0xc0000000; hardware meaning of the high address bits is not known.
    base = normalizeAddress(base);
    return base | (offset & pageMask);
}

bool Bridge::isConfig(unsigned aperture, uint32_t offset) const
{
    return (offset >> 24) == 0 && aperture < 2
        && (registers_[0x1c / 4] & (1u << aperture)) != 0;
}

uint32_t Bridge::apertureRead(unsigned aperture, uint32_t offset, uint32_t mask) const
{
    if (!validAperture(aperture, offset))
        return 0xffffffffu;
    const bool config = isConfig(aperture, offset);
    const Read& read = config ? bus_.configRead : bus_.read;
    if (!read)
        return 0xffffffffu;
    return read(config ? offset & pageMask : targetAddress(aperture, offset), mask);
}

void Bridge::apertureWrite(unsigned aperture, uint32_t offset, uint32_t data, uint32_t mask) const
{
    if (!validAperture(aperture, offset))
        return;
    const bool config = isConfig(aperture, offset);
    const Write& write = config ? bus_.configWrite : bus_.write;
    if (write)
        write(config ? offset & pageMask : targetAddress(aperture, offset), data, mask);
}

void Bridge::setIrqStatus(uint16_t bits)
{
    registers_[1] |= bits;
    updateIrq();
}

bool Bridge::irqAsserted() const
{
    return irqAsserted_;
}

uint32_t Bridge::advanceDma(uint32_t maxWords)
{
    if (!(registers_[0x38 / 4] & 0x01000000u))
        return 0;
    uint32_t transferred = 0;
    if (bus_.read && bus_.write)
    {
        while (dmaRemaining_ && transferred < maxWords)
        {
            const uint32_t value = bus_.read(dmaSource_, 0xffffffffu);
            bus_.write(dmaDestination_, value, 0xffffffffu);
            dmaSource_ += 4;
            dmaDestination_ += 4;
            --dmaRemaining_;
            ++transferred;
        }
    }
    if (!dmaRemaining_)
    {
        registers_[0x38 / 4] &= ~0x01000000u;
        setIrqStatus(1u << 12);
    }
    return transferred;
}

void Bridge::updateIrq()
{
    const bool asserted = (registers_[1] & (registers_[1] >> 16) & 0xffffu) != 0;
    if (asserted != irqAsserted_)
    {
        irqAsserted_ = asserted;
        if (irq_)
            irq_(asserted);
    }
}

} // namespace hikaru
