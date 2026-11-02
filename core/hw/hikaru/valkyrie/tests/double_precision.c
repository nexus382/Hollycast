/* Portions Copyright 2026 The Hollycast Authors */
/* Synthetic ISA programs only; no game instructions or ROM data. */
#include "vk/machine.h"
#include "vk/mmap.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_switch_dispatch(bool enabled);

static unsigned failures;
static vk_buffer_t *ram;
static sh4_t *cpu;

static void check(const char *name, uint32_t actual, uint32_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got %08x expected %08x\n", name, actual, expected);
        ++failures;
    }
}

static void reset(void)
{
    vk_device_reset((vk_device_t *)cpu, VK_RESET_TYPE_HARD);
    memset(&cpu->regs, 0, sizeof(cpu->regs));
    cpu->regs.sr.full = 0x40000000;
    cpu->regs.fpscr.full = 0;
    cpu->regs.fpscr.bit.pr = 1;
}

static void execute(const uint16_t *code, size_t count)
{
    const uint16_t stop[] = {0xaffe, 0x0009}; /* BRA to itself, NOP */
    memcpy(ram->ptr + 0x100, code, count * 2);
    memcpy(ram->ptr + 0x100 + count * 2, stop, sizeof(stop));
    cpu->regs.pc = 0x0c000100;
    vk_cpu_run((vk_cpu_t *)cpu, 128);
}

static void instruction(uint16_t code) { execute(&code, 1); }

static void arithmetic(void)
{
    static const struct { uint16_t code; uint32_t high, low; } cases[] = {
        {0xf020, 0x40130000, 0}, /* 3.25 + 1.5 = 4.75 */
        {0xf021, 0x3ffc0000, 0}, /* 3.25 - 1.5 = 1.75 */
        {0xf022, 0x40138000, 0}, /* 3.25 * 1.5 = 4.875 */
        {0xf023, 0x40015555, 0x55555555}, /* 3.25 / 1.5 */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        reset();
        cpu->regs.f.f[0].u = 0x400a0000;
        cpu->regs.f.f[2].u = 0x3ff80000;
        instruction(cases[i].code);
        check("arithmetic high word", cpu->regs.f.f[0].u, cases[i].high);
        check("arithmetic low word", cpu->regs.f.f[1].u, cases[i].low);
    }
    reset(); cpu->regs.fpul.u = (uint32_t)-7;
    instruction(0xf02d); /* FLOAT FPUL,DR0 */
    check("FLOAT high word", cpu->regs.f.f[0].u, 0xc01c0000);
    check("FLOAT low word", cpu->regs.f.f[1].u, 0);
    instruction(0xf03d); /* FTRC DR0,FPUL */
    check("FTRC", cpu->regs.fpul.u, (uint32_t)-7);
    instruction(0xf04d); /* FNEG DR0 */
    check("FNEG high word", cpu->regs.f.f[0].u, 0x401c0000);
    cpu->regs.f.f[0].u = 0xc01c0000;
    instruction(0xf05d); /* FABS DR0 */
    check("FABS high word", cpu->regs.f.f[0].u, 0x401c0000);
    cpu->regs.f.f[0].u = 0x40100000;
    instruction(0xf06d); /* FSQRT DR0 */
    check("FSQRT", cpu->regs.f.f[0].u, 0x40000000);
    cpu->regs.f.f[2].u = 0x3ff00000;
    instruction(0xf025); /* FCMP/GT DR2,DR0 */
    check("FCMP/GT", cpu->regs.sr.bit.t, 1);
    instruction(0xf024); /* FCMP/EQ DR2,DR0 */
    check("FCMP/EQ unequal", cpu->regs.sr.bit.t, 0);
    cpu->regs.f.f[2].u = 0x40000000;
    instruction(0xf024);
    check("FCMP/EQ equal", cpu->regs.sr.bit.t, 1);
    reset(); cpu->regs.fpscr.bit.pr = 0; cpu->regs.fpul.u = 0x40600000;
    instruction(0xf0ad); /* FCNVSD FPUL,DR0 */
    check("FCNVSD high word", cpu->regs.f.f[0].u, 0x400c0000);
    check("FCNVSD low word", cpu->regs.f.f[1].u, 0);
    instruction(0xf0bd); /* FCNVDS DR0,FPUL */
    check("FCNVDS", cpu->regs.fpul.u, 0x40600000);
}

static void moves_and_division(void)
{
    /* Load the two words of 2^31 separately with PR=1, SZ=0, then use
     * that bias to divide unsigned integers through double arithmetic.
     * This detects both reversed arithmetic words and PR-sized FMOV. */
    static const uint16_t divide[] = {
        0xf509, 0xf408, 0x445a, 0xf02d, 0xf040,
        0x455a, 0xf22d, 0xf240, 0xf023, 0xf03d
    };
    static const uint32_t numbers[] = {0, 1, 9, 10, 99, 12345, 99999, 0x80000000, 0xffffffff};
    static const uint32_t divisors[] = {2, 3, 10, 100, 1000, 10000};
    const uint32_t bias[] = {0, 0x41e00000};
    memcpy(ram->ptr + 0x2000, bias, sizeof(bias));
    for (unsigned n = 0; n < sizeof(numbers) / sizeof(numbers[0]); ++n)
        for (unsigned d = 0; d < sizeof(divisors) / sizeof(divisors[0]); ++d) {
            reset();
            cpu->regs.r[0] = 0x0c002000;
            cpu->regs.r[4] = numbers[n] ^ 0x80000000;
            cpu->regs.r[5] = divisors[d] ^ 0x80000000;
            execute(divide, sizeof(divide) / sizeof(divide[0]));
            check("unsigned division", cpu->regs.fpul.u, numbers[n] / divisors[d]);
            check("single-word FMOV increment with PR=1", cpu->regs.r[0], 0x0c002004);
        }
    reset(); cpu->regs.fpscr.bit.sz = 1; cpu->regs.r[0] = 0x0c002000;
    instruction(0xf109); /* FMOV @R0+,XD0 */
    check("paired FMOV increment", cpu->regs.r[0], 0x0c002008);
    check("paired FMOV first raw word", cpu->regs.x.f[0].u, bias[0]);
    check("paired FMOV second raw word", cpu->regs.x.f[1].u, bias[1]);
    instruction(0xf21c); /* FMOV XD0,DR2 */
    check("paired register first word", cpu->regs.f.f[2].u, bias[0]);
    check("paired register second word", cpu->regs.f.f[3].u, bias[1]);
    instruction(0xf02a); /* FMOV DR2,@R0 */
    check("paired store first word", vk_buffer_get(ram, 4, 0x2008), bias[0]);
    check("paired store second word", vk_buffer_get(ram, 4, 0x200c), bias[1]);
}

int main(void)
{
    vk_machine_t *machine = calloc(1, sizeof(*machine));
    if (!machine) return 1;
    vk_mmap_t *map = vk_mmap_new(machine);
    ram = vk_buffer_le32_new(65536, 0);
    if (!map || !ram || vk_mmap_add_ram(map, 0x0c000000, 0x0c00ffff, 65535, 0, ram, "RAM")) return 1;
    vk_machine_register_buffer(machine, ram);
    cpu = (sh4_t *)sh4_new(machine, map, true, true);
    if (!cpu) return 1;
    for (unsigned use_switch = 0; use_switch < 2; ++use_switch) {
        hikaru_vendor_test_switch_dispatch(use_switch != 0);
        arithmetic();
        moves_and_division();
    }
    vk_mmap_destroy(&map);
    vk_machine_destroy(&machine);
    if (!failures) puts("SH-4 double arithmetic and FMOV checks passed");
    return failures ? 1 : 0;
}
