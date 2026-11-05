// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/machine.h"
#include "vk/mmap.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_poll_skip(bool enabled);
uint64_t hikaru_vendor_test_poll_skipped(void);
static unsigned device_reads, patch_calls;
static int device_get(vk_device_t *dev, unsigned size, uint32_t addr, void *out)
{
    (void)dev;
    ++device_reads;
    if (size == 2) {
        static const uint16_t instructions[] = {0xc400, 0x4015, 0x89fc};
        return set_ptr(out, size, instructions[(addr - 0x0c000100) / 2]);
    }
    return set_ptr(out, size, 1);
}
static uint32_t unchanged_patch(vk_cpu_t *cpu, uint32_t pc, uint32_t instruction)
{ (void)cpu; (void)pc; ++patch_calls; return instruction; }
static uint64_t hash_word(uint64_t h, uint32_t value) { return (h ^ value) * 1099511628211ull; }

typedef struct { uint64_t hash, skipped; unsigned device, patches, checks; } Result;
static Result run_case(unsigned mode, bool enabled)
{
    vk_machine_t *machine = calloc(1, sizeof(*machine));
    vk_mmap_t *map = vk_mmap_new(machine);
    vk_buffer_t *ram = vk_buffer_le32_new(65536, 0);
    vk_device_t device = {0}; device.get = device_get;
    if (!machine || !map || !ram) abort();
    if (mode == 1 && vk_mmap_add_dev(map, 0x0c000200, 0x0c000201, 1,
                                     VK_REGION_R | VK_REGION_SIZE_8, &device, "data overlay")) abort();
    if (mode == 2 && vk_mmap_add_dev(map, 0x0c000100, 0x0c000105, 7,
                                     VK_REGION_R | VK_REGION_SIZE_16, &device, "code overlay")) abort();
    if (vk_mmap_add_ram(map, 0x0c000000, 0x0c00ffff, 0xffff, 0, ram, "RAM")) abort();
    vk_machine_register_buffer(machine, ram);
    vk_cpu_t *cpu = sh4_new(machine, map, true, true);
    if (!cpu) abort();
    if (mode == 3) cpu->patch = unchanged_patch;
    sh4_t *sh = (sh4_t *)cpu;
    const uint16_t code[] = {0xc400, 0x4015, 0x89fc, 0xaffe, 0x0009};
    const uint16_t irq_loop[] = {0xaffe, 0x0009};
    memcpy(ram->ptr + 0x100, code, sizeof(code));
    memcpy(ram->ptr + 0x600, irq_loop, sizeof(irq_loop));
    hikaru_vendor_test_poll_skip(enabled); device_reads = patch_calls = 0;
    Result result = {1469598103934665603ull, 0, 0, 0, 0};
    const int values[] = {-1, 0, 1, 127};
    for (unsigned v = 0; v < sizeof(values) / sizeof(values[0]); ++v)
        for (unsigned phase = 0; phase < 3; ++phase)
            for (int cycles = 1; cycles <= 600; ++cycles) {
                vk_device_reset((vk_device_t *)cpu, VK_RESET_TYPE_HARD);
                ram->ptr[0x200] = (uint8_t)values[v];
                sh->regs.pc = 0x0c000100 + 2 * phase;
                sh->regs.gbr = 0x0c000200;
                sh->regs.r[0] = (uint32_t)values[v];
                sh->regs.sr.bit.t = values[v] > 0;
                if (mode >= 4) {
                    sh->regs.sr.bit.bl = 0;
                    sh->regs.sr.bit.i = mode == 4 ? 15 : 0;
                    sh->regs.vbr = 0x0c000000;
                    vk_cpu_set_irq_state(cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED);
                }
                int ran = vk_cpu_run(cpu, cycles);
                result.hash = hash_word(result.hash, (uint32_t)ran);
                result.hash = hash_word(result.hash, sh->regs.pc);
                result.hash = hash_word(result.hash, sh->regs.r[0]);
                result.hash = hash_word(result.hash, sh->regs.sr.full);
                result.hash = hash_word(result.hash, sh->regs.spc);
                result.hash = hash_word(result.hash, sh->regs.ssr.full);
                result.hash = hash_word(result.hash, sh->intc.pending);
                result.hash = hash_word(result.hash, (uint32_t)sh->intc.index);
                result.hash = hash_word(result.hash, sh->intc.irqs[SH4_IESOURCE_IRL2].state);
                result.hash = hash_word(result.hash, sh->tmu.counter[0]);
                ++result.checks;
            }
    result.skipped = hikaru_vendor_test_poll_skipped();
    result.device = device_reads; result.patches = patch_calls;
    vk_mmap_destroy(&map);
    vk_machine_destroy(&machine);
    return result;
}
int main(void)
{
    unsigned failures = 0, checks = 0;
    for (unsigned mode = 0; mode < 6; ++mode) {
        Result expected = run_case(mode, false), actual = run_case(mode, true);
        checks += expected.checks;
        if (expected.hash != actual.hash || expected.device != actual.device || expected.patches != actual.patches ||
            (mode == 0 ? !actual.skipped : actual.skipped != 0)) {
            fprintf(stderr, "poll mode %u differs: hash %llx/%llx device %u/%u patches %u/%u skipped %llu\n",
                    mode, (unsigned long long)expected.hash, (unsigned long long)actual.hash,
                    expected.device, actual.device, expected.patches, actual.patches, (unsigned long long)actual.skipped);
            ++failures;
        }
    }
    printf("Hikaru polling loop: %u cases, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
