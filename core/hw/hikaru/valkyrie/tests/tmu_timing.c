// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "cpu/sh/sh4.h"
#include "cpu/sh/sh4-ireg.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

void hikaru_vendor_test_tmu_advance(sh4_t *, unsigned, int);

static uint32_t register_offset(uint32_t address)
{
    return ((address >> 8) & 0xff00) | (address & 0xff);
}

static int check(vk_buffer_t *registers, unsigned channel, uint32_t initial,
                 uint32_t reload, uint16_t control, int cycles)
{
    sh4_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.iregs = registers;
    cpu.tmu.counter[channel] = initial;
    const unsigned sources[] = {SH4_IESOURCE_TUNI0, SH4_IESOURCE_TUNI1, SH4_IESOURCE_TUNI2};
    cpu.intc.irqs[sources[channel]].priority = 4;
    const uint32_t tcor = register_offset(TMU_TCOR0 + channel * 12);
    const uint32_t tcr = register_offset(TMU_TCR0 + channel * 12);
    vk_buffer_put(registers, 4, tcor, reload);
    vk_buffer_put(registers, 2, tcr, control);
    uint32_t expected = initial;
    uint16_t flags = control;
    bool raised = false;
    /* Independent, tick-at-a-time reference for the pre-optimization
     * semantics. Include exact-zero boundaries and sticky UNF/IRQ state. */
    for (int tick = 0; tick < cycles; ++tick) {
        if (!expected) {
            expected = reload;
            flags |= 0x100;
            if (control & 0x20) raised = true;
        }
        --expected;
    }
    hikaru_vendor_test_tmu_advance(&cpu, channel, cycles);
    const bool actual_irq = cpu.intc.irqs[sources[channel]].state == VK_IRQ_STATE_RAISED;
    if (cpu.tmu.counter[channel] != expected || vk_buffer_get(registers, 2, tcr) != flags ||
        actual_irq != raised || cpu.intc.pending != raised) {
        fprintf(stderr, "TMU ch%u initial=%08x reload=%08x control=%04x ticks=%d: counter=%08x expected=%08x irq=%d expected=%d\n",
                channel, initial, reload, control, cycles, cpu.tmu.counter[channel], expected, actual_irq, raised);
        return 1;
    }
    return 0;
}

int main(void)
{
    vk_buffer_t *registers = vk_buffer_le32_new(0x10000, 0);
    if (!registers) return 2;
    const uint32_t counters[] = {0, 1, 2, 2048, UINT32_MAX - 1, UINT32_MAX};
    const uint32_t reloads[] = {0, 1, 2, 3, 1000, UINT32_MAX};
    const uint16_t controls[] = {0, 0x20, 0x100, 0x120, 0xc123};
    const int advances[] = {-1, 0, 1, 2, 3, 4, 1024, 32768};
    unsigned failures = 0, cases = 0;
    vk_verbosity = 0;
    for (unsigned channel = 0; channel < 3; ++channel)
        for (unsigned c = 0; c < sizeof(counters) / sizeof(*counters); ++c)
            for (unsigned r = 0; r < sizeof(reloads) / sizeof(*reloads); ++r)
                for (unsigned f = 0; f < sizeof(controls) / sizeof(*controls); ++f)
                    for (unsigned t = 0; t < sizeof(advances) / sizeof(*advances); ++t) {
                        failures += check(registers, channel, counters[c], reloads[r], controls[f], advances[t]);
                        ++cases;
                    }
    /* An advance this large must remain bounded work, including zero reload. */
    sh4_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.iregs = registers;
    vk_buffer_put(registers, 4, register_offset(TMU_TCOR0), 0);
    vk_buffer_put(registers, 2, register_offset(TMU_TCR0), 0);
    hikaru_vendor_test_tmu_advance(&cpu, 0, INT_MAX);
    failures += cpu.tmu.counter[0] != UINT32_C(0x80000001);
    vk_buffer_destroy(&registers);
    printf("TMU differential cases: %u, failures: %u\n", cases + 1, failures);
    return failures ? 1 : 0;
}
