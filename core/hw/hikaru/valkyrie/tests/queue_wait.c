// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "cpu/sh/sh4.h"
#include "vk/machine.h"
#include "vk/mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_poll_skip(bool enabled);
uint64_t hikaru_vendor_test_poll_skipped(void);

static const uint32_t ram_base = 0x0c000000;
static const uint8_t *device_ram;
static unsigned device_reads, patch_calls;

enum Mode {
    PLAIN, FLAG_DEVICE, POINTER_DEVICE, HEAD_DEVICE, TAIL_DEVICE,
    PREFIX_DEVICE, CODE_TAIL_DEVICE, PATCH, MASKED_IRQ, ACTIVE_IRQ,
    CHANGED_PREFIX, CHANGED_TAIL, UNALIGNED_QUEUE, UNALIGNED_GBR,
    RAM_ALIAS, QUEUE_AT_END, SPLIT_CODE, SPLIT_QUEUE, MODE_COUNT
};

static int device_get(vk_device_t *device, unsigned size, uint32_t address, void *out)
{
    (void)device;
    ++device_reads;
    memcpy(out, device_ram + (address & 0xffff), size);
    return 0;
}

static uint32_t unchanged_patch(vk_cpu_t *cpu, uint32_t pc, uint32_t instruction)
{
    (void)cpu;
    (void)pc;
    ++patch_calls;
    return instruction;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size--)
        hash = (hash ^ *bytes++) * 1099511628211ull;
    return hash;
}

static void store32(vk_buffer_t *ram, uint32_t address, uint32_t value)
{
    memcpy(ram->ptr + (address & 0xffff), &value, sizeof(value));
}

typedef struct {
    uint64_t hash, skipped;
    unsigned reads, patches, checks;
} Result;

static Result run_case(enum Mode mode, bool enabled)
{
    vk_machine_t *machine = calloc(1, sizeof(*machine));
    if (!machine) abort();
    vk_mmap_t *map = vk_mmap_new(machine);
    vk_buffer_t *ram = vk_buffer_le32_new(65536, 0);
    if (!map || !ram) abort();
    vk_device_t device = {0};
    device.get = device_get;
    device_ram = ram->ptr;
    uint32_t overlay = 0;
    unsigned width = 0;
    switch (mode) {
    case FLAG_DEVICE: overlay = 0x200; width = 1; break;
    case POINTER_DEVICE: overlay = 0x274; width = 4; break;
    case HEAD_DEVICE: overlay = 0x400; width = 4; break;
    case TAIL_DEVICE: overlay = 0x408; width = 4; break;
    case PREFIX_DEVICE: overlay = 0x100; width = 2; break;
    case CODE_TAIL_DEVICE: overlay = 0x138; width = 2; break;
    default: break;
    }
    if (width && vk_mmap_add_dev(map, ram_base + overlay, ram_base + overlay + (width > 1 ? width - 1 : 1),
            0xffff, VK_REGION_R | VK_REGION_SIZE_ALL, &device, "read overlay")) abort();
    if (mode == SPLIT_CODE || mode == SPLIT_QUEUE) {
        uint32_t split = mode == SPLIT_CODE ? 0x130 : 0x404;
        if (vk_mmap_add_ram(map, ram_base, ram_base + split - 1, 0xffff, 0, ram, "lower RAM") ||
            vk_mmap_add_ram(map, ram_base + split, ram_base + 0xffff, 0xffff, 0, ram, "upper RAM")) abort();
    } else if (vk_mmap_add_ram(map, ram_base, ram_base + 0xffff, 0xffff, 0, ram, "RAM")) abort();
    vk_machine_register_buffer(machine, ram);
    vk_cpu_t *cpu = sh4_new(machine, map, true, true);
    if (!cpu) abort();
    sh4_t *sh = (sh4_t *)cpu;
    if (mode == PATCH) cpu->patch = unchanged_patch;

    /* Synthetic workload: the nonempty path writes a marker and exits.
     * Its body is deliberately unrelated to the game's work submission. */
    uint16_t code[32];
    for (unsigned i = 0; i < 32; ++i) code[i] = 0x0009;
    const uint16_t prefix[] = {0xc61d, 0x5100, 0x5202, 0x3210, 0x6303, 0x8914};
    memcpy(code, prefix, sizeof(prefix));
    code[6] = 0xe401; // MOV #1,R4
    code[7] = 0x2542; // MOV.L R4,@R5
    code[8] = 0xa014; // BRA exit
    code[27] = 0xc400;
    code[28] = mode == CHANGED_TAIL ? 0x4011 : 0x4015;
    code[29] = 0x89e1;
    code[30] = 0xaffe; // exit: BRA exit; NOP
    if (mode == CHANGED_PREFIX) code[4] = 0x0009;
    memcpy(ram->ptr + 0x100, code, sizeof(code));
    const uint16_t irq_loop[] = {0xaffe, 0x0009};
    memcpy(ram->ptr + 0x600, irq_loop, sizeof(irq_loop));
    const int flags[] = {-128, -1, 0, 1, 127};
    const unsigned phases[] = {0, 2, 4, 6, 8, 10, 54, 56, 58};
    const int budgets[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 16, 17, 18, 19, 20,
                          26, 27, 28, 63, 64, 65, 127, 128, 129, 600, 7000};
    const uint32_t alias = mode == RAM_ALIAS ? 0xa0000000 : 0;
    const uint32_t gbr = (ram_base + 0x200 + (mode == UNALIGNED_GBR)) | alias;
    const uint32_t queue = (ram_base + (mode == QUEUE_AT_END ? 0xfff4 : 0x400)
                            + (mode == UNALIGNED_QUEUE)) | alias;
    hikaru_vendor_test_poll_skip(enabled);
    device_reads = patch_calls = 0;
    Result result = {1469598103934665603ull, 0, 0, 0, 0};
    for (unsigned f = 0; f < sizeof(flags) / sizeof(flags[0]); ++f)
        for (unsigned equal = 0; equal < 2; ++equal)
            for (unsigned phase = 0; phase < 18; ++phase)
                for (unsigned b = 0; b < sizeof(budgets) / sizeof(budgets[0]); ++b) {
                    vk_device_reset((vk_device_t *)cpu, VK_RESET_TYPE_HARD);
                    ram->ptr[gbr & 0xffff] = (uint8_t)flags[f];
                    store32(ram, gbr + 0x74, queue);
                    store32(ram, queue, 0x12345678);
                    store32(ram, queue + 8, equal ? 0x12345678 : 0x87654321);
                    store32(ram, ram_base + 0x500, 0x87654321);
                    store32(ram, ram_base + 0x508, 0x87654321);
                    store32(ram, ram_base + 0x580, 0);
                    unsigned entry = phases[phase % 9];
                    sh->regs.pc = (ram_base + 0x100 + entry) | alias;
                    sh->regs.gbr = gbr;
                    for (unsigned r = 0; r < 16; ++r) sh->regs.r[r] = 0x13570000 + r;
                    sh->regs.r[0] = entry >= 2 && entry <= 10 ? queue : (uint32_t)flags[f];
                    sh->regs.r[1] = 0x12345678;
                    sh->regs.r[2] = equal ? 0x12345678 : 0x87654321;
                    sh->regs.r[5] = ram_base + 0x580;
                    sh->regs.sr.bit.t = phase / 9;
                    sh->tmu.is_running[0] = true;
                    sh->tmu.counter[0] = phase < 9 ? 7 : 0x100000;
                    if (mode == MASKED_IRQ || mode == ACTIVE_IRQ) {
                        sh->regs.sr.bit.bl = 0;
                        sh->regs.sr.bit.i = mode == MASKED_IRQ ? 15 : 0;
                        sh->regs.vbr = ram_base;
                        vk_cpu_set_irq_state(cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED);
                    }
                    for (unsigned slice = 0; slice < 2; ++slice) {
                        if (slice) {
                            /* External writes happen at slice boundaries. Test both
                             * exit predicates and a new equal queue that keeps waiting. */
                            if (b % 3 == 0) ram->ptr[gbr & 0xffff] = 0;
                            else if (b % 3 == 1) {
                                if (mode == PLAIN || mode == RAM_ALIAS || mode == QUEUE_AT_END)
                                    store32(ram, gbr + 0x74, (ram_base + 0x500) | alias);
                                else {
                                    store32(ram, queue, 0x87654321);
                                    store32(ram, queue + 8, 0x87654321);
                                }
                            } else store32(ram, queue + 8, 0x55555555);
                        }
                        int ran = vk_cpu_run(cpu, slice ? 37 : budgets[b]);
                        result.hash = hash_bytes(result.hash, &ran, sizeof(ran));
                        result.hash = hash_bytes(result.hash, &cpu->remaining, sizeof(cpu->remaining));
                        result.hash = hash_bytes(result.hash, &cpu->state, sizeof(cpu->state));
                        result.hash = hash_bytes(result.hash, &sh->regs, sizeof(sh->regs));
                        result.hash = hash_bytes(result.hash, &sh->intc, sizeof(sh->intc));
                        result.hash = hash_bytes(result.hash, &sh->tmu, sizeof(sh->tmu));
                        result.hash = hash_bytes(result.hash, &sh->sound_cycle_debt, sizeof(sh->sound_cycle_debt));
                        result.hash = hash_bytes(result.hash, ram->ptr + 0x100, 0x600);
                        result.hash = hash_bytes(result.hash, ram->ptr + 0xfff4, 12);
                        ++result.checks;
                    }
                }
    result.skipped = hikaru_vendor_test_poll_skipped();
    result.reads = device_reads;
    result.patches = patch_calls;
    vk_mmap_destroy(&map);
    vk_machine_destroy(&machine);
    return result;
}

int main(void)
{
    unsigned failures = 0, checks = 0;
    for (enum Mode mode = PLAIN; mode < MODE_COUNT; ++mode) {
        Result expected = run_case(mode, false), actual = run_case(mode, true);
        bool accelerates = mode == PLAIN || mode == RAM_ALIAS || mode == QUEUE_AT_END;
        checks += expected.checks;
        if (expected.hash != actual.hash || expected.reads != actual.reads ||
            expected.patches != actual.patches || (accelerates ? !actual.skipped : actual.skipped != 0)) {
            fprintf(stderr, "queue wait mode %u: hash %llx/%llx reads %u/%u patches %u/%u skipped %llu\n",
                    (unsigned)mode, (unsigned long long)expected.hash, (unsigned long long)actual.hash,
                    expected.reads, actual.reads, expected.patches, actual.patches,
                    (unsigned long long)actual.skipped);
            ++failures;
        }
    }
    printf("Hikaru queue wait: %u slice checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
