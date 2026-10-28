# Portions Copyright 2026 The Hollycast Authors
"""Generate checks of the real BSD sampling branches with deterministic OS adapters."""
from pathlib import Path
import argparse

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
output_directory = parser.parse_args().output_directory.resolve()
output_directory.mkdir(parents=True, exist_ok=True)

cpu = (root / 'core/oslib/cpu_usage.cpp').read_text()
cpu = cpu[cpu.index('namespace cpu_usage'):]
clock_start = cpu.index('std::uint64_t getWallMicroseconds()')
clock_end = cpu.index('\n}', clock_start) + 2
cpu = cpu[:clock_start] + 'std::uint64_t getWallMicroseconds() { return wall; }' + cpu[clock_end:]
memory = (root / 'core/oslib/process_memory.cpp').read_text()
memory = memory[memory.index('namespace process_memory'):]

shim = r'''
// Portions Copyright 2026 The Hollycast Authors
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sys/resource.h>
#include <unistd.h>
#include "oslib/cpu_usage.h"
#include "oslib/process_memory.h"
// Select only the production BSD branches after the Linux standard headers.
// These adapters check syscall contracts, not native BSD header compatibility.
#undef __linux__
#define PLATFORM
#if defined(__OpenBSD__)
constexpr int CPUSTATES = 6, CP_IDLE = 5;
#else
constexpr int CPUSTATES = 5, CP_IDLE = 4;
#endif
#if defined(__NetBSD__)
using Tick = std::uint64_t;
#else
using Tick = long;
#endif
constexpr int CTL_KERN = 1, KERN_PROC = 2, KERN_PROC2 = 3, KERN_PROC_PID = 4;
constexpr int KERN_CP_TIME = 5, KERN_CPTIME = 6;
struct kinfo_proc { long ki_rssize; std::int32_t p_vm_rssize; };
struct kinfo_proc2 { std::int32_t p_vm_rssize; };
static std::uint64_t wall = 1000000;
static Tick ticks[CPUSTATES] {};
static long processors = 4, pageSize = 4096, rss = 20;
static rusage usage {};
static bool failSystem = false, failProcess = false, failMemory = false, shortRead = false;
long testSysconf(int key) {
    assert(key == _SC_NPROCESSORS_ONLN || key == _SC_PAGESIZE);
    return key == _SC_NPROCESSORS_ONLN ? processors : pageSize;
}
int testGetrusage(int who, rusage* value) {
    assert(who == RUSAGE_SELF);
    if (failProcess) return -1;
    *value = usage; return 0;
}
int readTicks(void* data, size_t* size) {
    assert(*size == sizeof(ticks));
    if (failSystem) return -1;
    std::memcpy(data, ticks, sizeof(ticks));
    if (shortRead) --*size;
    return 0;
}
int sysctlbyname(const char* name, void* data, size_t* size, void* input, size_t length) {
    assert(std::strcmp(name, "kern.cp_time") == 0 && input == nullptr && length == 0);
    return readTicks(data, size);
}
int sysctl(const int* mib, unsigned count, void* data, size_t* size, void* input, size_t length) {
    assert(mib[0] == CTL_KERN && input == nullptr && length == 0);
    if (count == 2) {
#if defined(__NetBSD__)
        assert(mib[1] == KERN_CP_TIME);
#else
        assert(mib[1] == KERN_CPTIME);
#endif
        return readTicks(data, size);
    }
    assert(mib[2] == KERN_PROC_PID && mib[3] == getpid());
#if defined(__FreeBSD__)
    assert(mib[1] == KERN_PROC && count == 4 && *size == sizeof(kinfo_proc));
    static_cast<kinfo_proc*>(data)->ki_rssize = rss;
#elif defined(__NetBSD__)
    assert(mib[1] == KERN_PROC2 && count == 6 && *size == sizeof(kinfo_proc2));
    assert(mib[4] == sizeof(kinfo_proc2) && mib[5] == 1);
    static_cast<kinfo_proc2*>(data)->p_vm_rssize = rss;
#else
    assert(mib[1] == KERN_PROC && count == 6 && *size == sizeof(kinfo_proc));
    assert(mib[4] == sizeof(kinfo_proc) && mib[5] == 1);
    static_cast<kinfo_proc*>(data)->p_vm_rssize = rss;
#endif
    if (shortRead) --*size;
    return failMemory ? -1 : 0;
}
#define sysconf testSysconf
#define getrusage testGetrusage
'''

tests = r'''
int main() {
    auto first = cpu_usage::queryCurrentProcessAndSystem();
    assert(!first.processValid && !first.systemValid && first.logicalProcessorCount == 4);
    // Include every non-idle state (in particular OpenBSD's CP_SPIN).
    for (int state = 0; state < CP_IDLE; ++state) ticks[state] = 10;
    ticks[CP_IDLE] = 10 * CP_IDLE;
    usage.ru_utime.tv_usec = 250000; usage.ru_stime.tv_usec = 750000;
    wall += 1000000;
    auto current = cpu_usage::queryCurrentProcessAndSystem();
    assert(current.processValid && current.systemValid);
    assert(current.processPercent == 25.f && current.systemPercent == 50.f);
    shortRead = true;
    assert(!cpu_usage::readSnapshot().systemValid);
    assert(!process_memory::queryCurrentProcess().valid);
    shortRead = false; failSystem = true;
    auto snapshot = cpu_usage::readSnapshot();
    assert(!snapshot.systemValid && snapshot.processValid);
    failSystem = false; failProcess = true;
    snapshot = cpu_usage::readSnapshot();
    assert(snapshot.systemValid && !snapshot.processValid);
    failProcess = false;
    // A reset/hotplug must not underflow a decreasing busy counter.
    ticks[0] = 0; ticks[CP_IDLE] += 100;
    wall += 1000000;
    assert(!cpu_usage::queryCurrentProcessAndSystem().systemValid);
    processors = -1; wall += 1000000;
    assert(!cpu_usage::queryCurrentProcessAndSystem().processValid);
    processors = 4; wall += 1000000;
    assert(cpu_usage::queryCurrentProcessAndSystem().processValid);
    auto memory = process_memory::queryCurrentProcess();
    assert(memory.valid && memory.residentBytes == 20 * 4096);
    rss = 10; memory = process_memory::queryCurrentProcess();
    assert(memory.valid && memory.residentBytes == 10 * 4096);
    rss = 0; memory = process_memory::queryCurrentProcess();
    assert(memory.valid && memory.residentBytes == 0);
    rss = -1; assert(!process_memory::queryCurrentProcess().valid);
    rss = 20; pageSize = -1; assert(!process_memory::queryCurrentProcess().valid);
    pageSize = 4096; failMemory = true; assert(!process_memory::queryCurrentProcess().valid);
    failMemory = false; assert(process_memory::queryCurrentProcess().valid);
    std::cout << "PASS PLATFORM CPU normalization, syscall failures, reset counters, and current RSS\n";
}
'''

for platform in ('FreeBSD', 'NetBSD', 'OpenBSD'):
    output = output_directory / f'{platform.lower()}_sampling.cpp'
    output.write_text(shim.replace('PLATFORM', f'__{platform}__') + cpu + memory
                      + tests.replace('PLATFORM', platform))
    print('Generated:', output)
