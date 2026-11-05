// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright 2026 The Hollycast Authors
// Instruction semantics checked against Hollycast's hw/sh4/interpr/sh4_opcodes.cpp.
#include "sh4_cpu.h"

#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace hikaru {
namespace {
static_assert(sizeof(float) == sizeof(uint32_t), "SH-4 FPU requires 32-bit float storage");
uint32_t signExtend(uint32_t value, unsigned bits)
{
    const uint32_t sign = 1u << (bits - 1);
    return (value ^ sign) - sign;
}

float fromBits(uint32_t value)
{
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

[[maybe_unused]] uint32_t toBits(float value)
{
    uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

bool supportedFloat(uint32_t bits)
{
    const uint32_t exponent = bits & 0x7f800000u;
    return exponent != 0x7f800000u && (exponent != 0 || (bits & 0x007fffffu) == 0);
}

// Float operations use a temporary host environment and preserve caller state.
// Enabled traps are not emulated. Input NaNs need SH-4-specific signaling rules.
bool floatOperation(uint32_t leftBits, uint32_t rightBits, uint32_t thirdBits,
                    unsigned operation, uint32_t& fpscr, uint32_t& result)
{
#if defined(__FAST_MATH__)
    (void)leftBits; (void)rightBits; (void)thirdBits; (void)operation; (void)fpscr; (void)result;
    return false; // Fast-math may disregard the floating-point environment.
#else
    if (!std::numeric_limits<float>::is_iec559 || (fpscr & 3u) > 1 || (fpscr & 0xf80u))
        return false;
    const auto nan = [](uint32_t bits) { return (bits & 0x7f800000u) == 0x7f800000u && (bits & 0x7fffffu); };
    const auto subnormal = [](uint32_t bits) { return !(bits & 0x7f800000u) && (bits & 0x7fffffu); };
    if ((operation != 6 && nan(leftBits)) || (operation < 4 && nan(rightBits)) || (operation == 5 && (nan(rightBits) || nan(thirdBits)))) return false;
    if (!(fpscr & 0x40000u) && ((operation != 6 && subnormal(leftBits)) || (operation < 4 && subnormal(rightBits)) || (operation == 5 && (subnormal(rightBits) || subnormal(thirdBits))))) return false;
    const auto flush = [fpscr](uint32_t bits) {
        return (fpscr & 0x40000u) && !(bits & 0x7f800000u) ? bits & 0x80000000u : bits;
    };
    const float left = operation == 6 ? 0 : fromBits(flush(leftBits)), right = fromBits(flush(rightBits)), third = fromBits(flush(thirdBits));
    fenv_t environment;
    if (feholdexcept(&environment) != 0) return false;
    if (fesetround((fpscr & 3u) ? FE_TOWARDZERO : FE_TONEAREST) != 0) { fesetenv(&environment); return false; }
    volatile float a = left, b = right, c = third, value = 0;
    switch (operation) { case 0: value = a + b; break; case 1: value = a - b; break; case 2: value = a * b; break; case 3: value = a / b; break; case 4: value = std::sqrt(a); break; case 5: value = std::fma(a, b, c); break; case 6: { const int64_t signedValue = int64_t(leftBits ^ 0x80000000u) - 0x80000000ll; volatile int32_t integer = static_cast<int32_t>(signedValue); value = static_cast<float>(integer); break; } default: fesetenv(&environment); return false; }
    const int exceptions = fetestexcept(FE_ALL_EXCEPT);
    uint32_t flags = 0;
    if (exceptions & FE_INEXACT) flags |= 1;
    if (exceptions & FE_UNDERFLOW) flags |= 2;
    if (exceptions & FE_OVERFLOW) flags |= 4;
    if (exceptions & FE_DIVBYZERO) flags |= 8;
    if (exceptions & FE_INVALID) flags |= 16;
    result = toBits(value);
    if (nan(result)) result = 0x7fbfffffu;
    else if ((fpscr & 0x40000u) && !(result & 0x7f800000u) && (result & 0x7fffffu)) { result &= 0x80000000u; flags |= 3; }
    fpscr = (fpscr & ~0x3f000u) | (flags << 12) | (flags << 2);
    fesetenv(&environment);
    return true;
#endif
}

bool vectorOperation(const std::array<uint32_t, 16>& fr, const std::array<uint32_t, 16>& xf,
                     unsigned n, unsigned m, bool matrix, uint32_t& fpscr, std::array<uint32_t, 4>& output)
{
#if defined(__FAST_MATH__)
    (void)fr; (void)xf; (void)n; (void)m; (void)matrix; (void)fpscr; (void)output;
    return false;
#else
    if (!std::numeric_limits<float>::is_iec559 || (fpscr & 3u) > 1 || (fpscr & 0xf80u)) return false;
    auto read = [fpscr](uint32_t bits, float& value) {
        const uint32_t exponent = bits & 0x7f800000u;
        if (exponent == 0x7f800000u && (bits & 0x7fffffu)) return false;
        if (!(fpscr & 0x40000u) && exponent == 0 && (bits & 0x7fffffu)) return false;
        if ((fpscr & 0x40000u) && exponent == 0) bits &= 0x80000000u;
        value = fromBits(bits); return true;
    };
    float vector[4], other[16];
    for (unsigned i = 0; i < 4; ++i) if (!read(fr[n + i], vector[i])) return false;
    for (unsigned i = 0; i < (matrix ? 16u : 4u); ++i)
        if (!read(matrix ? xf[i] : fr[m + i], other[i])) return false;
    fenv_t environment;
    if (feholdexcept(&environment) != 0) return false;
    if (fesetround((fpscr & 3u) ? FE_TOWARDZERO : FE_TONEAREST) != 0) { fesetenv(&environment); return false; }
    for (unsigned row = 0; row < (matrix ? 4u : 1u); ++row) {
        volatile double sum = 0;
        for (unsigned i = 0; i < 4; ++i) sum = sum + double(vector[i]) * other[matrix ? row + i * 4 : i];
        volatile float value = static_cast<float>(sum);
        output[row] = toBits(value);
        if ((output[row] & 0x7f800000u) == 0x7f800000u && (output[row] & 0x7fffffu)) output[row] = 0x7fbfffffu;
        else if ((fpscr & 0x40000u) && !(output[row] & 0x7f800000u) && (output[row] & 0x7fffffu)) { output[row] &= 0x80000000u; feraiseexcept(FE_UNDERFLOW | FE_INEXACT); }
    }
    const int exceptions = fetestexcept(FE_ALL_EXCEPT);
    uint32_t flags = 0;
    if (exceptions & FE_INEXACT) flags |= 1;
    if (exceptions & FE_UNDERFLOW) flags |= 2;
    if (exceptions & FE_OVERFLOW) flags |= 4;
    if (exceptions & FE_DIVBYZERO) flags |= 8;
    if (exceptions & FE_INVALID) flags |= 16;
    fpscr = (fpscr & ~0x3f000u) | (flags << 12) | (flags << 2);
    fesetenv(&environment);
    return true;
#endif
}
}

Sh4Cpu::Sh4Cpu(Bus bus) : bus_(std::move(bus)) {}
void Sh4Cpu::reset()
{
    state_ = State{};
    delayed_ = false;
    branchTarget_ = 0;
    delayedLink_ = false;
    linkValue_ = 0;
    delayedRestoreSr_ = false;
    restoreSr_ = 0;
}

bool Sh4Cpu::interrupt(uint32_t eventCode, unsigned priority, bool nmi)
{
    if (delayed_ || (state_.sr & 0x10000000u) || priority > 15
        || (!nmi && priority <= ((state_.sr >> 4) & 15)))
        return false;
    state_.intevt = eventCode;
    state_.ssr = state_.sr;
    state_.spc = state_.pc;
    state_.sgr = state_.r[15];
    setSr(state_.sr | 0x70000000u);
    state_.pc = state_.vbr + 0x600;
    return true;
}

void Sh4Cpu::setSr(uint32_t value)
{
    constexpr uint32_t srMask = 0x700083f3u;
    value &= srMask;
    const bool oldBank = (state_.sr & 0x60000000u) == 0x60000000u;
    const bool newBank = (value & 0x60000000u) == 0x60000000u;
    if (oldBank != newBank)
        for (unsigned i = 0; i < 8; ++i)
            std::swap(state_.r[i], state_.rBank[i]);
    state_.sr = value;
}

void Sh4Cpu::setFpscr(uint32_t value)
{
    value &= ~0x3f000u;
    if ((state_.fpscr ^ value) & 0x00200000u)
        state_.fr.swap(state_.xf);
    state_.fpscr = value;
}

Sh4Cpu::Result Sh4Cpu::step()
{
    const uint32_t pc = state_.pc;
    if (!bus_.read || !bus_.write)
        return {Status::MissingBus, pc, 0};
    const uint16_t op = static_cast<uint16_t>(bus_.read(pc, 2));
    const bool restoreBank = delayed_ && delayedRestoreSr_;
    const uint32_t oldSr = state_.sr;
    // Only the instruction fetch uses the pre-RTE mode. The restored SR,
    // including RB and MD, is in effect for executing the delay slot.
    // SR writes in this slot need exception/privilege rules not implemented yet.
    if (restoreBank && ((op & 0xf0ff) == 0x400e || (op & 0xf0ff) == 0x4007))
        return {Status::UnsupportedDelaySlot, pc, op};
    if (restoreBank)
        setSr(restoreSr_);
    const unsigned n = (op >> 8) & 15, m = (op >> 4) & 15;
    const uint32_t imm = op & 255, disp4 = op & 15;
    auto& r = state_.r;
    const bool wasDelayed = delayed_;
    const uint32_t previousTarget = branchTarget_;
    bool control = false;
    bool supported = true;
    uint32_t nextPc = pc + 2;
    bool scheduleBranch = false;
    bool link = false;
    bool restoreSr = false;
    uint32_t target = 0;
    const auto setT = [&](bool b) { state_.sr = (state_.sr & ~1u) | (b ? 1u : 0u); };
    const auto load = [&](uint32_t address, unsigned size) {
        const uint32_t v = bus_.read(address, size);
        return size == 4 ? v : signExtend(v & (size == 1 ? 255u : 65535u), size * 8);
    };
    const auto store = [&](uint32_t address, uint32_t value, unsigned size) { bus_.write(address, value, size); };
    switch (op >> 12) {
    case 0x0:
        if (op == 0x0009) {} // NOP
        else if (op == 0x0008) setT(false);
        else if (op == 0x0018) setT(true);
        else if (op == 0x0048) state_.sr &= ~2u;
        else if (op == 0x0058) state_.sr |= 2u;
        else if (op == 0x0019) state_.sr &= ~0x301u; // DIV0U: M, Q, T
        else if (op == 0x0028) state_.mach = state_.macl = 0;
        else if (op == 0x000b) { control = scheduleBranch = true; target = state_.pr; }
        else if (op == 0x002b) { control = scheduleBranch = restoreSr = true; target = state_.spc; }
        else if ((op & 0xf0ff) == 0x0029) r[n] = state_.sr & 1;
        else if ((op & 0xf0ff) == 0x000a) r[n] = state_.mach;
        else if ((op & 0xf0ff) == 0x001a) r[n] = state_.macl;
        else if ((op & 0xf0ff) == 0x002a) r[n] = state_.pr;
        else if ((op & 0xf0ff) == 0x005a) r[n] = state_.fpul;
        else if ((op & 0xf0ff) == 0x006a) r[n] = state_.fpscr;
        else if ((op & 0xf0ff) == 0x0002) r[n] = state_.sr;
        else if ((op & 0xf0ff) == 0x0012) r[n] = state_.gbr;
        else if ((op & 0xf0ff) == 0x0022) r[n] = state_.vbr;
        else if ((op & 0xf0ff) == 0x0032) r[n] = state_.ssr;
        else if ((op & 0xf0ff) == 0x0042) r[n] = state_.spc;
        else if ((op & 0xf0ff) == 0x003a) r[n] = state_.sgr;
        else if ((op & 0xf0ff) == 0x00fa) r[n] = state_.dbr;
        else if ((op & 0xf08f) == 0x0082) r[n] = state_.rBank[m & 7];
        else if ((op & 0xf0ff) == 0x0023) { control = scheduleBranch = true; target = pc + 4 + r[n]; }
        else if ((op & 0xf0ff) == 0x0003) { control = scheduleBranch = link = true; target = pc + 4 + r[n]; }
        else if ((op & 0xf0ff) == 0x0083) {
            const uint32_t address = r[n];
            if ((address & 0xfc000000u) == 0xe0000000u) {
                if (bus_.prefetch) bus_.prefetch(address);
                else supported = false;
            }
            // Ordinary cache prefetch is an optional hint in this uncached core.
        }
        else if ((op & 15) >= 4 && (op & 15) <= 6) store(r[n] + r[0], r[m], 1u << ((op & 15) - 4));
        else if ((op & 15) == 7) state_.macl = r[n] * r[m];
        else if ((op & 15) >= 12 && (op & 15) <= 14) r[n] = load(r[m] + r[0], 1u << ((op & 15) - 12));
        else supported = false;
        break;
    case 0x1: store(r[n] + disp4 * 4, r[m], 4); break;
    case 0x2:
        switch (op & 15) {
        case 0: case 1: case 2: store(r[n], r[m], 1u << (op & 15)); break;
        case 4: case 5: case 6: { const unsigned s = 1u << ((op & 15) - 4); const uint32_t address = r[n] - s; store(address, r[m], s); r[n] = address; break; }
        case 7: { const bool q = (r[n] >> 31) != 0, mv = (r[m] >> 31) != 0; state_.sr = (state_.sr & ~0x301u) | (q ? 0x100u : 0) | (mv ? 0x200u : 0) | (q != mv ? 1u : 0); break; }
        case 8: setT((r[n] & r[m]) == 0); break;
        case 9: r[n] &= r[m]; break;
        case 10: r[n] ^= r[m]; break;
        case 11: r[n] |= r[m]; break;
        case 12: { const uint32_t v = r[n] ^ r[m]; setT(!(v & 0xff) || !(v & 0xff00) || !(v & 0xff0000) || !(v & 0xff000000)); break; }
        case 13: r[n] = (r[n] >> 16) | (r[m] << 16); break;
        case 14: state_.macl = (r[n] & 0xffffu) * (r[m] & 0xffffu); break;
        case 15: state_.macl = signExtend(r[n] & 0xffffu, 16) * signExtend(r[m] & 0xffffu, 16); break;
        default: supported = false; break;
        } break;
    case 0x3:
        switch (op & 15) {
        case 0: setT(r[n] == r[m]); break;
        case 2: setT(r[n] >= r[m]); break;
        case 3: setT((r[n] ^ 0x80000000u) >= (r[m] ^ 0x80000000u)); break;
        case 4: {
            const bool oldQ = (state_.sr & 0x100) != 0, mv = (state_.sr & 0x200) != 0;
            bool q = (r[n] >> 31) != 0;
            const uint32_t operand = r[m];
            r[n] = (r[n] << 1) | (state_.sr & 1);
            const uint32_t before = r[n];
            if (oldQ == mv) { r[n] -= operand; q ^= r[n] > before; if (oldQ) q = !q; }
            else { r[n] += operand; q ^= r[n] < before; if (!oldQ) q = !q; }
            state_.sr = (state_.sr & ~0x101u) | (q ? 0x100u : 0) | (q == mv ? 1u : 0); break;
        }
        case 5: { const uint64_t product = uint64_t(r[n]) * r[m]; state_.macl = uint32_t(product); state_.mach = uint32_t(product >> 32); break; }
        case 6: setT(r[n] > r[m]); break;
        case 7: setT((r[n] ^ 0x80000000u) > (r[m] ^ 0x80000000u)); break;
        case 8: r[n] -= r[m]; break;
        case 10: { const uint32_t before = r[n], operand = r[m], borrow = state_.sr & 1; const uint32_t partial = before - operand; r[n] = partial - borrow; setT(before < operand || partial < borrow); break; }
        case 11: { const uint32_t before = r[n], operand = r[m]; r[n] -= operand; setT(((before ^ operand) & (before ^ r[n]) & 0x80000000u) != 0); break; }
        case 12: r[n] += r[m]; break;
        case 13: { const int64_t left = int64_t(r[n] ^ 0x80000000u) - 0x80000000ll; const int64_t right = int64_t(r[m] ^ 0x80000000u) - 0x80000000ll; const uint64_t product = static_cast<uint64_t>(left * right); state_.macl = uint32_t(product); state_.mach = uint32_t(product >> 32); break; }
        case 14: { const uint64_t result = uint64_t(r[n]) + r[m] + (state_.sr & 1); r[n] = uint32_t(result); setT(result >> 32); break; }
        case 15: { const uint32_t before = r[n], operand = r[m]; r[n] += operand; setT(((~(before ^ operand)) & (before ^ r[n]) & 0x80000000u) != 0); break; }
        default: supported = false; break;
        } break;
    case 0x4:
        if ((op & 0x00ff) == 0x000b) { control = scheduleBranch = link = true; target = r[n]; }
        else if ((op & 0x00ff) == 0x002b) { control = scheduleBranch = true; target = r[n]; }
        else if ((op & 0x00ff) == 0x0010) { --r[n]; setT(r[n] == 0); }
        else if ((op & 0x00ff) == 0x0011) setT(!(r[n] & 0x80000000u));
        else if ((op & 0x00ff) == 0x0015) setT(r[n] != 0 && !(r[n] & 0x80000000u));
        else if ((op & 0x00ff) == 0x0000 || (op & 0x00ff) == 0x0020) { setT((r[n] >> 31) != 0); r[n] <<= 1; }
        else if ((op & 0x00ff) == 0x0001) { setT(r[n] & 1); r[n] >>= 1; }
        else if ((op & 0x00ff) == 0x0021) { const uint32_t old = r[n]; setT(old & 1); r[n] = (old >> 1) | (old & 0x80000000u); }
        else if ((op & 0x00ff) == 0x0004 || (op & 0x00ff) == 0x0024) { const uint32_t oldT = state_.sr & 1, bit = r[n] >> 31; r[n] = (r[n] << 1) | ((op & 0x20) ? oldT : bit); setT(bit); }
        else if ((op & 0x00ff) == 0x0005 || (op & 0x00ff) == 0x0025) { const uint32_t oldT = state_.sr & 1, bit = r[n] & 1; r[n] = (r[n] >> 1) | (((op & 0x20) ? oldT : bit) << 31); setT(bit); }
        else if ((op & 0x00ff) == 0x0008) r[n] <<= 2;
        else if ((op & 0x00ff) == 0x0009) r[n] >>= 2;
        else if ((op & 0x00ff) == 0x0018) r[n] <<= 8;
        else if ((op & 0x00ff) == 0x0019) r[n] >>= 8;
        else if ((op & 0x00ff) == 0x0028) r[n] <<= 16;
        else if ((op & 0x00ff) == 0x0029) r[n] >>= 16;
        else if ((op & 0xf00f) == 0x400c || (op & 0xf00f) == 0x400d) {
            const uint32_t count = r[m], old = r[n];
            if (!(count & 0x80000000u)) r[n] = old << (count & 31);
            else {
                const unsigned shift = ((~count) & 31) + 1;
                if ((op & 15) == 13) r[n] = shift == 32 ? 0 : old >> shift;
                else if (shift == 32) r[n] = (old & 0x80000000u) ? 0xffffffffu : 0;
                else r[n] = (old >> shift) | ((old & 0x80000000u) ? (~0u << (32 - shift)) : 0);
            }
        }
        else if ((op & 0x00ff) == 0x001b) { const uint32_t value = bus_.read(r[n], 1) & 255; setT(value == 0); store(r[n], value | 0x80, 1); }
        else if ((op & 0x00ff) == 0x000a) state_.mach = r[n];
        else if ((op & 0x00ff) == 0x001a) state_.macl = r[n];
        else if ((op & 0x00ff) == 0x002a) state_.pr = r[n];
        else if ((op & 0x00ff) == 0x005a) state_.fpul = r[n];
        else if ((op & 0x00ff) == 0x006a) setFpscr(r[n]);
        else if ((op & 0x00ff) == 0x000e) setSr(r[n]);
        else if ((op & 0x00ff) == 0x001e) state_.gbr = r[n];
        else if ((op & 0x00ff) == 0x002e) state_.vbr = r[n];
        else if ((op & 0x00ff) == 0x003e) state_.ssr = r[n];
        else if ((op & 0x00ff) == 0x004e) state_.spc = r[n];
        else if ((op & 0x00ff) == 0x00fa) state_.dbr = r[n];
        else if ((op & 0xf08f) == 0x408e) state_.rBank[m & 7] = r[n];
        else if ((op & 0xf00f) == 0x4002 || (op & 0xf00f) == 0x4003) {
            const unsigned code = (op >> 4) & 15;
            const bool system = (op & 15) == 2;
            uint32_t value = 0;
            if (system) {
                switch (code) { case 0: value = state_.mach; break; case 1: value = state_.macl; break; case 2: value = state_.pr; break; case 3: value = state_.sgr; break; case 5: value = state_.fpul; break; case 6: value = state_.fpscr; break; case 15: value = state_.dbr; break; default: supported = false; }
            } else {
                switch (code) { case 0: value = state_.sr; break; case 1: value = state_.gbr; break; case 2: value = state_.vbr; break; case 3: value = state_.ssr; break; case 4: value = state_.spc; break; default: if (code >= 8) value = state_.rBank[code & 7]; else supported = false; }
            }
            if (supported) { const uint32_t address = r[n] - 4; store(address, value, 4); r[n] = address; }
        }
        else if ((op & 0xf00f) == 0x4006 || (op & 0xf00f) == 0x4007) {
            const unsigned code = (op >> 4) & 15;
            const bool system = (op & 15) == 6;
            if ((system && !(code <= 2 || code == 3 || code == 5 || code == 6 || code == 15)) || (!system && !(code <= 4 || code >= 8))) supported = false;
            else {
                const uint32_t value = load(r[n], 4); r[n] += 4;
                if (system) { switch (code) { case 0: state_.mach = value; break; case 1: state_.macl = value; break; case 2: state_.pr = value; break; case 3: state_.sgr = value; break; case 5: state_.fpul = value; break; case 6: setFpscr(value); break; case 15: state_.dbr = value; break; } }
                else { switch (code) { case 0: setSr(value); break; case 1: state_.gbr = value; break; case 2: state_.vbr = value; break; case 3: state_.ssr = value; break; case 4: state_.spc = value; break; default: state_.rBank[code & 7] = value; } }
            }
        }
        else supported = false;
        break;
    case 0x5: r[n] = load(r[m] + disp4 * 4, 4); break;
    case 0x6:
        switch (op & 15) {
        case 0: case 1: case 2: r[n] = load(r[m], 1u << (op & 15)); break;
        case 3: r[n] = r[m]; break;
        case 4: case 5: case 6: { const unsigned s = 1u << ((op & 15) - 4); const uint32_t v = load(r[m], s); if (n != m) r[m] += s; r[n] = v; break; }
        case 7: r[n] = ~r[m]; break;
        case 8: r[n] = (r[m] & 0xffff0000u) | ((r[m] & 255u) << 8) | ((r[m] >> 8) & 255u); break;
        case 9: r[n] = (r[m] << 16) | (r[m] >> 16); break;
        case 10: { const uint32_t value = r[m], carry = state_.sr & 1; r[n] = 0u - value - carry; setT(value != 0 || carry != 0); break; }
        case 11: r[n] = 0u - r[m]; break;
        case 12: r[n] = r[m] & 255u; break;
        case 13: r[n] = r[m] & 65535u; break;
        case 14: r[n] = signExtend(r[m] & 255u, 8); break;
        case 15: r[n] = signExtend(r[m] & 65535u, 16); break;
        default: supported = false; break;
        } break;
    case 0x7: r[n] += signExtend(imm, 8); break;
    case 0x8:
        switch ((op >> 8) & 15) {
        case 0: store(r[m] + disp4, r[0], 1); break;
        case 1: store(r[m] + disp4 * 2, r[0], 2); break;
        case 4: r[0] = load(r[m] + disp4, 1); break;
        case 5: r[0] = load(r[m] + disp4 * 2, 2); break;
        case 8: setT(r[0] == signExtend(imm, 8)); break;
        case 9: case 11: case 13: case 15: {
            control = true; const unsigned sub = (op >> 8) & 15;
            const bool taken = ((state_.sr & 1) != 0) == (sub == 9 || sub == 13);
            if (sub >= 13) { scheduleBranch = true; target = taken ? pc + 4 + signExtend(imm, 8) * 2 : pc + 4; }
            else if (taken) nextPc = pc + 4 + signExtend(imm, 8) * 2;
            break;
        }
        default: supported = false; break;
        } break;
    case 0x9: if (wasDelayed) supported = false; else r[n] = load(pc + 4 + imm * 2, 2); break;
    case 0xa: case 0xb:
        control = scheduleBranch = true; target = pc + 4 + signExtend(op & 0xfff, 12) * 2;
        link = (op >> 12) == 0xb;
        break;
    case 0xc:
        switch ((op >> 8) & 15) {
        case 0: case 1: case 2: { unsigned size = 1u << ((op >> 8) & 15); store(state_.gbr + imm * size, r[0], size); break; }
        case 4: case 5: case 6: { unsigned size = 1u << (((op >> 8) & 15) - 4); r[0] = load(state_.gbr + imm * size, size); break; }
        case 7: if (wasDelayed) supported = false; else r[0] = ((pc + 4) & ~3u) + imm * 4; break;
        case 8: setT((r[0] & imm) == 0); break;
        case 9: r[0] &= imm; break;
        case 10: r[0] ^= imm; break;
        case 11: r[0] |= imm; break;
        case 12: { const uint32_t value = bus_.read(state_.gbr + r[0], 1); setT((value & imm) == 0); break; }
        case 13: case 14: case 15: { const uint32_t address = state_.gbr + r[0], value = bus_.read(address, 1) & 255; const unsigned sub = (op >> 8) & 15; store(address, sub == 13 ? (value & imm) : sub == 14 ? (value ^ imm) : (value | imm), 1); break; }
        default: supported = false; break;
        } break;
    case 0xd: if (wasDelayed) supported = false; else r[n] = load(((pc + 4) & ~3u) + imm * 4, 4); break;
    case 0xe: r[n] = signExtend(imm, 8); break;
    case 0xf:
        if (state_.sr & 0x8000) { supported = false; break; } // FPU-disabled exception not modeled.
        if (op == 0xfbfd) setFpscr(state_.fpscr ^ 0x00200000u);
        else if (op == 0xf3fd) setFpscr(state_.fpscr ^ 0x00100000u);
        else if ((op & 0xf0ff) == 0xf00d) state_.fr[n] = state_.fpul;
        else if ((op & 0xf0ff) == 0xf01d) state_.fpul = state_.fr[n];
        else if ((op & 0xf0ff) == 0xf08d && !(state_.fpscr & 0x00080000u)) state_.fr[n] = 0;
        else if ((op & 0xf0ff) == 0xf09d && !(state_.fpscr & 0x00080000u)) state_.fr[n] = 0x3f800000;
        else if ((op & 0xf0ff) == 0xf04d && !(state_.fpscr & 0x00080000u)) state_.fr[n] ^= 0x80000000u;
        else if ((op & 0xf0ff) == 0xf05d && !(state_.fpscr & 0x00080000u)) state_.fr[n] &= 0x7fffffffu;
        else if (((op & 0xf0ff) == 0xf06d || (op & 0xf0ff) == 0xf02d) && !(state_.fpscr & 0x00080000u)) {
            const bool convert = (op & 0xff) == 0x2d;
            uint32_t result;
            if (floatOperation(convert ? state_.fpul : state_.fr[n], 0, 0, convert ? 6 : 4, state_.fpscr, result)) state_.fr[n] = result;
            else supported = false;
        }
        else if ((op & 0xf0ff) == 0xf03d && !(state_.fpscr & 0x00080000u)) {
            if ((state_.fpscr & 0xf80u) || (!(state_.fpscr & 0x40000u) && !(state_.fr[n] & 0x7f800000u) && (state_.fr[n] & 0x7fffffu))) supported = false;
            else {
                const uint32_t bits = state_.fr[n], exponent = bits & 0x7f800000u;
                const bool nan = exponent == 0x7f800000u && (bits & 0x7fffffu);
                const float value = ((state_.fpscr & 0x40000u) && exponent == 0) ? fromBits(bits & 0x80000000u) : fromBits(bits);
                uint32_t flags = 0;
                if (nan || value <= -2147483648.0f || value >= 2147483648.0f) {
                    state_.fpul = !nan && !(bits & 0x80000000u) ? 0x7fffffffu : 0x80000000u;
                    if (nan || value != -2147483648.0f) flags = 16;
                } else {
                    const float truncated = std::trunc(value);
                    state_.fpul = static_cast<uint32_t>(static_cast<int32_t>(truncated));
                    if (truncated != value) flags = 1;
                }
                state_.fpscr = (state_.fpscr & ~0x3f000u) | (flags << 12) | (flags << 2);
            }
        }
        else if (((op & 0xf0ff) == 0xf0ed || (op & 0xf3ff) == 0xf1fd) && !(state_.fpscr & 0x00080000u)) {
            const bool matrix = (op & 0xf3ff) == 0xf1fd;
            const unsigned vectorN = n & 12u, vectorM = (n & 3u) * 4u;
            std::array<uint32_t, 4> result{};
            if (!vectorOperation(state_.fr, state_.xf, vectorN, vectorM, matrix, state_.fpscr, result)) supported = false;
            else if (matrix) for (unsigned i = 0; i < 4; ++i) state_.fr[vectorN + i] = result[i];
            else state_.fr[vectorN + 3] = result[0];
        }
        else if ((op & 15) == 14 && !(state_.fpscr & 0x00080000u)) {
            uint32_t result;
            if (floatOperation(state_.fr[0], state_.fr[m], state_.fr[n], 5, state_.fpscr, result)) state_.fr[n] = result;
            else supported = false;
        }
        else if ((op & 15) <= 5) {
            if (state_.fpscr & 0x00080000u) supported = false; // double precision not yet implemented
            else if ((op & 15) <= 3) {
                uint32_t result;
                if (floatOperation(state_.fr[n], state_.fr[m], 0, op & 15, state_.fpscr, result)) state_.fr[n] = result;
                else supported = false;
            } else {
                uint32_t leftBits = state_.fr[n], rightBits = state_.fr[m];
                if (state_.fpscr & 0x40000u) {
                    if (!(leftBits & 0x7f800000u)) leftBits &= 0x80000000u;
                    if (!(rightBits & 0x7f800000u)) rightBits &= 0x80000000u;
                }
                if (!supportedFloat(leftBits) || !supportedFloat(rightBits)) supported = false;
                else { const float left = fromBits(leftBits), right = fromBits(rightBits); setT((op & 15) == 4 ? left == right : left > right); state_.fpscr &= ~0x3f000u; }
            }
        }
        else if ((state_.fpscr & 0x00100000u) && (op & 15) == 12) {
            auto& destination = (n & 1) ? state_.xf : state_.fr;
            const auto& source = (m & 1) ? state_.xf : state_.fr;
            const uint32_t first = source[m & ~1u], second = source[(m & ~1u) + 1];
            destination[n & ~1u] = first;
            destination[(n & ~1u) + 1] = second;
        }
        else if (!(state_.fpscr & 0x00100000u)) {
            switch (op & 15) {
            case 6: state_.fr[n] = load(r[m] + r[0], 4); break;
            case 7: store(r[n] + r[0], state_.fr[m], 4); break;
            case 8: state_.fr[n] = load(r[m], 4); break;
            case 9: state_.fr[n] = load(r[m], 4); r[m] += 4; break;
            case 10: store(r[n], state_.fr[m], 4); break;
            case 11: { const uint32_t address = r[n] - 4; store(address, state_.fr[m], 4); r[n] = address; break; }
            case 12: state_.fr[n] = state_.fr[m]; break;
            default: supported = false; break;
            }
        } else supported = false;
        break;
    default: supported = false; break;
    }
    if (wasDelayed && control) {
        if (restoreBank) setSr(oldSr);
        return {Status::UnsupportedDelaySlot, pc, op};
    }
    if (!supported) {
        if (restoreBank) setSr(oldSr);
        return {wasDelayed ? Status::UnsupportedDelaySlot : Status::UnsupportedOpcode, pc, op};
    }
    if (wasDelayed) {
        state_.pc = previousTarget; delayed_ = false; if (delayedLink_) state_.pr = linkValue_; delayedLink_ = delayedRestoreSr_ = false;
    }
    else { state_.pc = nextPc; delayed_ = scheduleBranch; if (scheduleBranch) { branchTarget_ = target; delayedLink_ = link; linkValue_ = pc + 4; delayedRestoreSr_ = restoreSr; restoreSr_ = state_.ssr; } }
    return {Status::Executed, pc, op};
}
} // namespace hikaru
