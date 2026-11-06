// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "cpu/sh/sh4.h"
#include "vk/machine.h"
#include "vk/mmap.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_poll_skip(bool enabled);
uint64_t hikaru_vendor_test_idle_skipped(void);

enum { RamBase = 0x0c000000, CodeAddress = RamBase + 0x100 };
static unsigned deviceReads;
static unsigned patchCalls;

enum Mode {
    PLAIN,
    ALIAS,
    PATCH,
    CODE_OVERLAY,
    CHANGED_BRANCH,
    CHANGED_SLOT,
    SPLIT_CODE,
    WRAPPED_CODE,
    MASKED_IRQ,
    BLOCKED_IRQ,
    ACTIVE_IRQ,
    NMI,
    BLOCKED_NMI,
    IRQ_AND_NMI,
    MODE_COUNT
};

enum Wake {
    MASKED_IRQ_WAKE,
    BLOCKED_IRQ_WAKE,
    ACTIVE_IRQ_WAKE,
    NMI_WAKE,
    BLOCKED_NMI_WAKE,
    IRQ_AND_NMI_WAKE,
    WAKE_COUNT
};

typedef struct {
    vk_machine_t *machine;
    vk_mmap_t *map;
    vk_buffer_t *ram;
    vk_cpu_t *cpu;
    sh4_t *sh;
    vk_device_t codeDevice;
} Fixture;

typedef struct {
    uint64_t hash;
    uint64_t skipped;
    unsigned reads;
    unsigned patches;
    unsigned checks;
} Result;

typedef struct {
    uint64_t hash;
    uint64_t waitSkipped;
    uint64_t mutationSkipped;
    uint64_t resumeSkipped;
    uint64_t wakeSkipped;
    uint64_t skipped;
} SequenceResult;

static const uint16_t deviceCode[] = {0xaffe, 0x0009, 0x0009};

static int codeDeviceGet(vk_device_t *device, unsigned size, uint32_t address, void *out)
{
    (void)device;
    ++deviceReads;
    if (size == 2 && address >= CodeAddress) {
        unsigned index = (address - CodeAddress) / 2;
        if (index < sizeof(deviceCode) / sizeof(deviceCode[0]))
            return set_ptr(out, size, deviceCode[index]);
    }
    return set_ptr(out, size, 0x0009);
}

static uint32_t unchangedPatch(vk_cpu_t *cpu, uint32_t pc, uint32_t instruction)
{
    (void)cpu;
    (void)pc;
    ++patchCalls;
    return instruction;
}

static uint64_t hashBytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    while (size--)
        hash = (hash ^ *bytes++) * 1099511628211ull;
    return hash;
}

static uint64_t hashWord(uint64_t hash, uint32_t value)
{
    return hashBytes(hash, &value, sizeof(value));
}

static void recordState(Result *result, const Fixture *fixture, int ran)
{
    const sh4_t *sh = fixture->sh;
    result->hash = hashBytes(result->hash, &ran, sizeof(ran));
    result->hash = hashBytes(result->hash, &fixture->cpu->remaining, sizeof(fixture->cpu->remaining));
    result->hash = hashBytes(result->hash, &fixture->cpu->state, sizeof(fixture->cpu->state));

    result->hash = hashBytes(result->hash, sh->regs.r, sizeof(sh->regs.r));
    result->hash = hashWord(result->hash, sh->regs.pc);
    result->hash = hashWord(result->hash, sh->regs.sr.full);
    result->hash = hashWord(result->hash, sh->regs.pr);
    result->hash = hashWord(result->hash, sh->regs.gbr);
    result->hash = hashWord(result->hash, sh->regs.vbr);
    result->hash = hashWord(result->hash, sh->regs.dbr);
    result->hash = hashBytes(result->hash, &sh->regs.mac.full, sizeof(sh->regs.mac.full));
    result->hash = hashWord(result->hash, sh->regs.spc);
    result->hash = hashWord(result->hash, sh->regs.ssr.full);
    result->hash = hashWord(result->hash, sh->regs.sgr);
    result->hash = hashBytes(result->hash, sh->regs.rbank, sizeof(sh->regs.rbank));
    for (unsigned i = 0; i < 16; ++i) {
        result->hash = hashWord(result->hash, sh->regs.f.f[i].u);
        result->hash = hashWord(result->hash, sh->regs.x.f[i].u);
    }
    result->hash = hashWord(result->hash, sh->regs.fpul.u);
    result->hash = hashWord(result->hash, sh->regs.fpscr.full);

    result->hash = hashBytes(result->hash, &sh->in_slot, sizeof(sh->in_slot));
    result->hash = hashBytes(result->hash, &sh->intc.pending, sizeof(sh->intc.pending));
    result->hash = hashBytes(result->hash, &sh->intc.index, sizeof(sh->intc.index));
    for (unsigned i = 0; i < SH4_NUM_IESOURCES; ++i) {
        result->hash = hashBytes(result->hash, &sh->intc.irqs[i].state, sizeof(sh->intc.irqs[i].state));
        result->hash = hashBytes(result->hash, &sh->intc.irqs[i].priority, sizeof(sh->intc.irqs[i].priority));
        result->hash = hashWord(result->hash, sh->intc.irqs[i].offset);
        result->hash = hashWord(result->hash, sh->intc.irqs[i].code);
    }
    for (unsigned i = 0; i < 3; ++i) {
        result->hash = hashBytes(result->hash, &sh->tmu.is_running[i], sizeof(sh->tmu.is_running[i]));
        result->hash = hashWord(result->hash, sh->tmu.counter[i]);
    }
    result->hash = hashBytes(result->hash, &sh->sound_cycle_debt, sizeof(sh->sound_cycle_debt));
    result->hash = hashWord(result->hash, sh->sound_wait_fraction);
    result->hash = hashBytes(result->hash, &sh->sound_wait_this_slice, sizeof(sh->sound_wait_this_slice));
    result->hash = hashBytes(result->hash, fixture->sh->iregs->ptr, fixture->sh->iregs->size);
    result->hash = hashBytes(result->hash, fixture->ram->ptr, 65536);
    ++result->checks;
}

static void fillNops(vk_buffer_t *ram)
{
    const uint16_t nop = 0x0009;
    for (size_t offset = 0; offset < 65536; offset += sizeof(nop))
        memcpy(ram->ptr + offset, &nop, sizeof(nop));
}

static void createFixture(Fixture *fixture, enum Mode mode)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->machine = calloc(1, sizeof(*fixture->machine));
    if (!fixture->machine) abort();
    fixture->map = vk_mmap_new(fixture->machine);
    fixture->ram = vk_buffer_le32_new(65536, 0);
    if (!fixture->map || !fixture->ram) abort();
    fillNops(fixture->ram);

    fixture->codeDevice.get = codeDeviceGet;
    if (mode == CODE_OVERLAY &&
        vk_mmap_add_dev(fixture->map, CodeAddress, CodeAddress + 5, 7,
                        VK_REGION_R | VK_REGION_SIZE_16, &fixture->codeDevice, "code overlay"))
        abort();

    if (mode == SPLIT_CODE) {
        if (vk_mmap_add_ram(fixture->map, RamBase, RamBase + 0xffff, 0xffff, 0,
                            fixture->ram, "lower RAM") ||
            vk_mmap_add_ram(fixture->map, RamBase + 0x10000, RamBase + 0x1ffff, 0xffff, 0,
                            fixture->ram, "upper RAM"))
            abort();
    } else if (mode == WRAPPED_CODE) {
        if (vk_mmap_add_ram(fixture->map, 0, 0xffff, 0xffff, 0, fixture->ram, "low RAM") ||
            vk_mmap_add_ram(fixture->map, 0x1fff0000, 0x1fffffff, 0xffff, 0,
                            fixture->ram, "high RAM"))
            abort();
    } else if (vk_mmap_add_ram(fixture->map, RamBase, RamBase + 0xffff, 0xffff, 0,
                               fixture->ram, "RAM")) {
        abort();
    }

    vk_machine_register_buffer(fixture->machine, fixture->ram);
    fixture->cpu = sh4_new(fixture->machine, fixture->map, true, true);
    if (!fixture->cpu) abort();
    fixture->sh = (sh4_t *)fixture->cpu;
    if (mode == PATCH)
        fixture->cpu->patch = unchangedPatch;
}

static uint32_t writeLoop(Fixture *fixture, enum Mode mode)
{
    uint32_t pc = CodeAddress;
    size_t branchOffset = 0x100;
    size_t slotOffset = 0x102;
    uint16_t branch = mode == CHANGED_BRANCH ? 0xaffd : 0xaffe;
    uint16_t slot = mode == CHANGED_SLOT ? 0xe101 : 0x0009;

    if (mode == SPLIT_CODE) {
        pc = RamBase + 0xfffe;
        branchOffset = 0xfffe;
        slotOffset = 0;
    } else if (mode == WRAPPED_CODE) {
        pc = 0x1ffffffe;
        branchOffset = 0xfffe;
        slotOffset = 0;
    } else if (mode == ALIAS) {
        pc = 0xac000100;
    }

    memcpy(fixture->ram->ptr + branchOffset, &branch, sizeof(branch));
    memcpy(fixture->ram->ptr + slotOffset, &slot, sizeof(slot));
    return pc;
}

static void resetCpu(Fixture *fixture, enum Mode mode, uint32_t pc)
{
    vk_device_reset((vk_device_t *)fixture->cpu, VK_RESET_TYPE_HARD);
    fixture->sh->regs.pc = pc;
    fixture->sh->regs.vbr = RamBase;
    fixture->sh->regs.sr.bit.bl = 0;
    fixture->sh->regs.sr.bit.i = 0;
    fixture->sh->tmu.is_running[0] = true;
    fixture->sh->tmu.counter[0] = 0x100000;
    fixture->sh->sound_cycle_debt = 1;

    if (mode == MASKED_IRQ || mode == BLOCKED_IRQ || mode == ACTIVE_IRQ || mode == IRQ_AND_NMI) {
        fixture->sh->regs.sr.bit.bl = mode == BLOCKED_IRQ ? 1 : 0;
        fixture->sh->regs.sr.bit.i = mode == MASKED_IRQ ? 15 : 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED))
            abort();
    }
    if (mode == NMI || mode == BLOCKED_NMI || mode == IRQ_AND_NMI) {
        fixture->sh->regs.sr.bit.bl = mode == BLOCKED_NMI ? 1 : 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_NMI, VK_IRQ_STATE_RAISED))
            abort();
    }
}

static Result runMode(enum Mode mode, bool enabled)
{
    static const int longBudgets[] = {127, 128, 129, 600, 7000};
    Fixture fixture;
    createFixture(&fixture, mode);
    const uint32_t pc = writeLoop(&fixture, mode);
    hikaru_vendor_test_poll_skip(enabled);
    deviceReads = patchCalls = 0;

    Result result = {1469598103934665603ull, 0, 0, 0, 0};
    for (int cycles = 1; cycles <= 64; ++cycles) {
        resetCpu(&fixture, mode, pc);
        const int ran = vk_cpu_run(fixture.cpu, cycles);
        recordState(&result, &fixture, ran);
    }
    for (unsigned i = 0; i < sizeof(longBudgets) / sizeof(longBudgets[0]); ++i) {
        resetCpu(&fixture, mode, pc);
        const int ran = vk_cpu_run(fixture.cpu, longBudgets[i]);
        recordState(&result, &fixture, ran);
    }

    result.skipped = hikaru_vendor_test_idle_skipped();
    result.reads = deviceReads;
    result.patches = patchCalls;
    vk_mmap_destroy(&fixture.map);
    vk_machine_destroy(&fixture.machine);
    return result;
}

static void raiseWake(Fixture *fixture, enum Wake wake)
{
    switch (wake) {
    case MASKED_IRQ_WAKE:
        fixture->sh->regs.sr.bit.bl = 0;
        fixture->sh->regs.sr.bit.i = 15;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED)) abort();
        break;
    case BLOCKED_IRQ_WAKE:
        fixture->sh->regs.sr.bit.bl = 1;
        fixture->sh->regs.sr.bit.i = 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED)) abort();
        break;
    case ACTIVE_IRQ_WAKE:
        fixture->sh->regs.sr.bit.bl = 0;
        fixture->sh->regs.sr.bit.i = 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED)) abort();
        break;
    case NMI_WAKE:
        fixture->sh->regs.sr.bit.bl = 0;
        fixture->sh->regs.sr.bit.i = 15;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_NMI, VK_IRQ_STATE_RAISED)) abort();
        break;
    case BLOCKED_NMI_WAKE:
        fixture->sh->regs.sr.bit.bl = 1;
        fixture->sh->regs.sr.bit.i = 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_NMI, VK_IRQ_STATE_RAISED)) abort();
        break;
    case IRQ_AND_NMI_WAKE:
        fixture->sh->regs.sr.bit.bl = 0;
        fixture->sh->regs.sr.bit.i = 0;
        if (vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED) ||
            vk_cpu_set_irq_state(fixture->cpu, SH4_IESOURCE_NMI, VK_IRQ_STATE_RAISED))
            abort();
        break;
    default:
        abort();
    }
}

static SequenceResult runMultislice(enum Wake wake, bool enabled)
{
    Fixture fixture;
    createFixture(&fixture, PLAIN);
    const uint32_t pc = writeLoop(&fixture, PLAIN);
    hikaru_vendor_test_poll_skip(enabled);
    deviceReads = patchCalls = 0;
    resetCpu(&fixture, PLAIN, pc);

    Result state = {1469598103934665603ull, 0, 0, 0, 0};
    SequenceResult result = {0, 0, 0, 0, 0, 0};
    static const int slices[] = {5, 17, 4, 65, 130};
    for (unsigned i = 0; i < sizeof(slices) / sizeof(slices[0]); ++i) {
        const int ran = vk_cpu_run(fixture.cpu, slices[i]);
        recordState(&state, &fixture, ran);
    }
    result.waitSkipped = hikaru_vendor_test_idle_skipped();

    uint16_t changedSlot = 0xe101;
    memcpy(fixture.ram->ptr + 0x102, &changedSlot, sizeof(changedSlot));
    uint64_t before = hikaru_vendor_test_idle_skipped();
    int ran = vk_cpu_run(fixture.cpu, 71);
    result.mutationSkipped = hikaru_vendor_test_idle_skipped() - before;
    recordState(&state, &fixture, ran);

    const uint16_t nop = 0x0009;
    memcpy(fixture.ram->ptr + 0x102, &nop, sizeof(nop));
    before = hikaru_vendor_test_idle_skipped();
    ran = vk_cpu_run(fixture.cpu, 65);
    result.resumeSkipped = hikaru_vendor_test_idle_skipped() - before;
    recordState(&state, &fixture, ran);

    raiseWake(&fixture, wake);
    before = hikaru_vendor_test_idle_skipped();
    ran = vk_cpu_run(fixture.cpu, 29);
    result.wakeSkipped = hikaru_vendor_test_idle_skipped() - before;
    recordState(&state, &fixture, ran);

    result.hash = state.hash;
    result.skipped = hikaru_vendor_test_idle_skipped();
    vk_mmap_destroy(&fixture.map);
    vk_machine_destroy(&fixture.machine);
    return result;
}

static bool equalResults(enum Mode mode, Result expected, Result actual)
{
    const bool shouldAccelerate = mode == PLAIN || mode == ALIAS;
    if (expected.hash != actual.hash || expected.reads != actual.reads ||
        expected.patches != actual.patches || expected.skipped ||
        (shouldAccelerate ? !actual.skipped : actual.skipped)) {
        fprintf(stderr, "idle mode %u differs: hash %llx/%llx reads %u/%u patches %u/%u skipped %llu\n",
                (unsigned)mode, (unsigned long long)expected.hash, (unsigned long long)actual.hash,
                expected.reads, actual.reads, expected.patches, actual.patches,
                (unsigned long long)actual.skipped);
        return false;
    }
    return true;
}

int main(void)
{
    unsigned failures = 0;
    unsigned checks = 0;
    for (enum Mode mode = PLAIN; mode < MODE_COUNT; ++mode) {
        Result expected = runMode(mode, false);
        Result actual = runMode(mode, true);
        checks += expected.checks;
        failures += !equalResults(mode, expected, actual);
    }

    for (enum Wake wake = MASKED_IRQ_WAKE; wake < WAKE_COUNT; ++wake) {
        SequenceResult expected = runMultislice(wake, false);
        SequenceResult actual = runMultislice(wake, true);
        const bool differs = expected.hash != actual.hash || expected.skipped ||
            expected.waitSkipped || expected.mutationSkipped || expected.resumeSkipped || expected.wakeSkipped ||
            !actual.waitSkipped || actual.mutationSkipped || !actual.resumeSkipped || actual.wakeSkipped;
        if (differs) {
            fprintf(stderr, "idle wake %u differs: hash %llx/%llx skip %llu wait %llu mutation %llu resume %llu wake %llu\n",
                    (unsigned)wake, (unsigned long long)expected.hash, (unsigned long long)actual.hash,
                    (unsigned long long)actual.skipped, (unsigned long long)actual.waitSkipped,
                    (unsigned long long)actual.mutationSkipped, (unsigned long long)actual.resumeSkipped,
                    (unsigned long long)actual.wakeSkipped);
            ++failures;
        }
    }

    printf("Hikaru idle spin: %u state checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
