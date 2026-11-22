// Portions Copyright 2026 The Hollycast Authors
#include "assembly_debugger.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/flashrom/nvmem.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/dyna/ngen.h"
#include "hw/sh4/modules/mmu.h"
#include <charconv>
#include <cfenv>
#include <cstring>

namespace assembly_debugger {

std::atomic<bool> enabled{false};
static std::atomic<bool> pending{false};
static std::set<u32> points;
static u32 stoppedPc;
static bool skipOnce;

bool available()
{
    // Two debugger controllers must not race over CPU state. Network sessions
    // and achievement hardcore mode also require uninterrupted execution.
#ifdef GDB_SERVER
    if (config::GDB)
        return false;
#endif
    return EventManager::isGameRunning() && !settings.network.online
            && !settings.naomi.multiboard && !settings.raHardcoreMode
            && !config::GGPOEnable;
}

bool open()
{
    if (!available())
        return false;
    emu.stop();
    enabled.store(true, std::memory_order_relaxed);
    return true;
}

void close()
{
    if (!active())
        return;
    emu.stop();
    reset();
    // Interpreter execution may have changed RAM/code since the last dynarec
    // block was compiled. Invalidate it before normal execution resumes.
#if FEAT_SHREC != DYNAREC_NONE
    Sh4Recompiler::Instance->ResetCache();
#endif
}

void reset()
{
    enabled.store(false, std::memory_order_relaxed);
    points.clear();
    pending.store(false, std::memory_order_relaxed);
    skipOnce = false;
}

void resume()
{
    if (!active() || emu.running())
        return;
    // Execute past the current stop once, including a breakpoint in the
    // branch's delay slot, then allow loops to hit the same breakpoint again.
    stoppedPc = Sh4cntx.pc;
    skipOnce = true;
    pending.store(false, std::memory_order_relaxed);
    emu.start();
}

void step()
{
    if (!active() || emu.running())
        return;
    pending.store(false, std::memory_order_relaxed);
    skipOnce = false;
    SetMemoryHandlers();
    // emu.step() schedules work on the rendering thread in shared-thread mode.
    // We are already on that thread and paused, so execute synchronously.
    // Preserve the UI's floating-point environment across guest instructions.
    fenv_t environment;
    const bool savedEnvironment = std::fegetenv(&environment) == 0;
    try {
        emu.getSh4Executor()->Step();
    } catch (...) {
        if (savedEnvironment)
            std::fesetenv(&environment);
        throw;
    }
    if (savedEnvironment)
        std::fesetenv(&environment);
}

bool breakPending() { return pending.load(std::memory_order_acquire); }
void acknowledgeBreak() { pending.store(false, std::memory_order_relaxed); }
const std::set<u32>& breakpoints() { return points; }

bool parseAddress(std::string_view text, u32& address)
{
    if (text.substr(0, 2) == "0x" || text.substr(0, 2) == "0X")
        text.remove_prefix(2);
    if (text.empty() || text.size() > 8)
        return false;
    u32 value;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || (value & 1))
        return false;
    address = value;
    return true;
}

bool readInstruction(u32 address, u16& opcode)
{
    if (!active() || emu.running() || (address & 1) || address >= 0xe0000000)
        return false;
    u32 physical = address;
    if (mmu_enabled() && mmu_is_translated(address, 2))
    {
        // Debugger reads must not refill the TLB, advance its replacement
        // counters, raise guest exceptions, or read memory-mapped devices.
        const TLB_Entry* match = nullptr;
        for (const TLB_Entry& entry : ITLB)
            if (entry.Data.V && mmu_match(address, entry.Address, entry.Data))
            {
                if (match != nullptr)
                    return false;
                match = &entry;
            }
        if (match == nullptr)
            for (const TLB_Entry& entry : UTLB)
                if (entry.Data.V && mmu_match(address, entry.Address, entry.Data))
                {
                    if (match != nullptr)
                        return false;
                    match = &entry;
                }
        if (match == nullptr || (!Sh4cntx.sr.MD && !(match->Data.PR & 2)))
            return false;
        const u32 mask = mmu_mask[match->Data.SZ1 * 2 + match->Data.SZ0];
        physical = ((match->Data.PPN << 10) & mask) | (address & ~mask);
    }
    bool isMemory;
    const void* data = addrspace::readConst(physical, isMemory, 2);
    if (!isMemory)
    {
        const u32 offset = physical & 0x1fffffff;
        if (physical >= 0xe0000000 || offset >= settings.platform.bios_size)
            return false;
        data = nvmem::getBiosData() + offset;
    }
    std::memcpy(&opcode, data, sizeof(opcode));
    return true;
}

bool setBreakpoint(u32 address, bool set)
{
    if (!active() || emu.running() || (address & 1))
        return false;
    if (!set)
    {
        points.erase(address);
        return true;
    }
    u16 opcode;
    if (!readInstruction(address, opcode))
        return false;
    points.insert(address);
    return true;
}

bool checkBreakpoint(u32 pc, u16 opcode, bool t)
{
    if (skipOnce)
    {
        skipOnce = false;
        if (pc == stoppedPc)
            return false;
    }
    bool found = points.count(pc) != 0;
    if (!found && (OpDesc[opcode]->type & Delayslot))
    {
        // Taken BT/S and BF/S execute the slot inside the branch handler.
        // Otherwise the next top-level dispatch checks the slot's address.
        const bool slotExecuted = (opcode & 0xff00) == 0x8d00 ? t
                : (opcode & 0xff00) == 0x8f00 ? !t : true;
        found = slotExecuted && points.count(pc + 2) != 0;
    }
    if (!found)
        return false;
    pending.store(true, std::memory_order_release);
    return true;
}

}
