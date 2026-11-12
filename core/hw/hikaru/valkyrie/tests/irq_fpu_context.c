/* Portions Copyright 2026 The Hollycast Authors */
/* The ROM signatures and register setup reproduce only the captured PH IRQ boundary. */
#include "cpu/sh/sh4.h"
#include "vk/machine.h"
#include "vk/mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_switch_dispatch(bool enabled);

#define RAM_BASE 0x0c000000u
#define RAM_SIZE (128u * 1024u)
#define IRQ_JSR_PC 0x0c0010a4u
#define IRQ_TARGET 0x0c0102c0u
#define TEST_PC 0x0c002000u
#define IRQ_KERNEL_FPSCR 0x00100000u
#define IRQ_GUEST_FPSCR 0x000c0001u
#define IRQ_GUEST_FPSCR_FR1 0x002c0001u
#define IRQ_GUEST_FPSCR_DN0 0x00080001u
#define IRQ_DIVISION_SPC 0x0c10e770u
#define FPSCR_PR_BIT (1u << 19)
#define FPSCR_FR_BIT (1u << 21)
#define IRQ_ENTRY_FPSCR(value_) ((value_) & ~FPSCR_PR_BIT)
#define F_BANK_SENTINEL 0x11223344u
#define X_BANK_SENTINEL 0xaabbccddu

static const uint16_t irq_tail[] = {
    0x6997, 0xf0cb, 0x2399, 0xf0ab, 0xf08b, 0xf06b, 0xf04b,
    0xf02b, 0xf00b, 0xdf26, 0xc7ab, 0xf0fa, 0xf0db, 0xf0bb,
    0xf09b, 0xf07b, 0xf05b, 0xf03b, 0xf01b, 0x450b, 0x436a
};

static const uint16_t irq_entry_prefix[] = {
    0xc412, 0xe780, 0xd672, 0x4728, 0xd372, 0x7730, 0x6171, 0x4f22
};

static unsigned failures;
static vk_buffer_t *ram;
static vk_game_t *game;
static const char *current_case = "fixture setup";

#define CHECK(name_, condition_) \
    do { \
        if (!(condition_)) { \
            fprintf(stderr, "%s: %s failed at line %d\n", current_case, (name_), __LINE__); \
            ++failures; \
        } \
    } while (0)

static void write_words(uint32_t address, const uint16_t *words, size_t count)
{
    const uint32_t offset = address - RAM_BASE;
    for (size_t i = 0; i < count; ++i)
        vk_buffer_put(ram, 2, offset + (uint32_t)(i * 2), words[i]);
}

static void install_signatures(void)
{
    write_words(0x0c00107e, irq_tail, sizeof(irq_tail) / sizeof(irq_tail[0]));
    write_words(IRQ_TARGET, irq_entry_prefix, sizeof(irq_entry_prefix) / sizeof(irq_entry_prefix[0]));
}

static void set_game_name(const char *name)
{
    snprintf(game->name, sizeof(game->name), "%s", name);
}

static void reset_irq_boundary(sh4_t *cpu, uint32_t target, uint32_t spc,
                               uint32_t incoming_fpscr)
{
    vk_device_reset((vk_device_t *)cpu, VK_RESET_TYPE_HARD);
    memset(&cpu->regs, 0, sizeof(cpu->regs));
    cpu->in_slot = false;
    cpu->base.state = VK_CPU_STATE_RUN;
    cpu->base.remaining = 0;
    cpu->regs.pc = IRQ_JSR_PC;
    cpu->regs.sr.full = 0x70000100;
    cpu->regs.fpscr.full = IRQ_KERNEL_FPSCR;
    cpu->regs.f.f[7].u = F_BANK_SENTINEL;
    cpu->regs.x.f[7].u = X_BANK_SENTINEL;
    cpu->regs.r[3] = incoming_fpscr;
    cpu->regs.r[5] = target;
    cpu->regs.spc = spc;
}

static void run_irq_case(sh4_t *cpu, const char *label, const char *game_name,
                         uint32_t target, uint32_t spc, uint32_t incoming_fpscr,
                         bool mutate_tail, bool mutate_irq_entry, bool expect_pr_clear)
{
    current_case = label;
    install_signatures();
    if (mutate_tail)
        vk_buffer_put(ram, 2, 0x107e, 0x0009);
    if (mutate_irq_entry)
        vk_buffer_put(ram, 2, IRQ_TARGET - RAM_BASE, 0x0009);
    set_game_name(game_name);
    reset_irq_boundary(cpu, target, spc, incoming_fpscr);
    vk_buffer_put(ram, 4, 0x1304, incoming_fpscr);
    const uint32_t expected_fpscr = expect_pr_clear ? IRQ_ENTRY_FPSCR(incoming_fpscr) : incoming_fpscr;

    /* sh4_run returns the budget overrun, rather than elapsed cycles. */
    CHECK("JSR and delay slot cycle budget", vk_cpu_run(&cpu->base, 2) == 0);
    CHECK("IRQ handler target", cpu->regs.pc == target);
    CHECK("IRQ delay slot completed", !cpu->in_slot);
    CHECK("JSR return address", cpu->regs.pr == 0x0c0010a8);
    CHECK("IRQ handler FPSCR", cpu->regs.fpscr.full == expected_fpscr);
    CHECK("saved guest FPSCR register", cpu->regs.r[3] == incoming_fpscr);
    CHECK("saved stack FPSCR unchanged",
          vk_buffer_get(ram, 4, 0x1304) == incoming_fpscr);
    const bool fr_swapped = (incoming_fpscr & FPSCR_FR_BIT) != 0;
    CHECK("active FR bank identity",
          cpu->regs.f.f[7].u == (fr_swapped ? X_BANK_SENTINEL : F_BANK_SENTINEL));
    CHECK("inactive FR bank identity",
          cpu->regs.x.f[7].u == (fr_swapped ? F_BANK_SENTINEL : X_BANK_SENTINEL));
    if (expect_pr_clear)
        CHECK("handler-entry PR cleared", cpu->regs.fpscr.bit.pr == 0);
    else
        CHECK("nonmatching PR preserved", cpu->regs.fpscr.bit.pr == 1);
}

static void run_direct_non_slot(sh4_t *cpu)
{
    const uint32_t pc = 0x0c002100;
    current_case = "direct non-slot";
    set_game_name("pharrier");
    install_signatures();
    write_words(pc, (const uint16_t[]){0x436a}, 1);
    reset_irq_boundary(cpu, IRQ_TARGET, IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR);
    cpu->regs.pc = pc;
    cpu->regs.pr = 0x0c0010a8;
    CHECK("direct LDS instruction cycle budget", vk_cpu_run(&cpu->base, 1) == 0);
    CHECK("direct non-slot FPSCR unchanged", cpu->regs.fpscr.full == IRQ_GUEST_FPSCR);
}

static void run_post_irq_fpu_sequence(sh4_t *cpu, uint32_t original_fpscr)
{
    const uint16_t sequence[] = {0xf412, 0x4066, 0xf022};
    current_case = "post-IRQ FPU sequence";
    if (cpu->regs.fpscr.full != IRQ_ENTRY_FPSCR(original_fpscr)) {
        CHECK("single FMUL precision precondition", false);
        return;
    }
    const bool fr_swapped = (original_fpscr & FPSCR_FR_BIT) != 0;
    const uint32_t active_sentinel = fr_swapped ? X_BANK_SENTINEL : F_BANK_SENTINEL;
    const uint32_t inactive_sentinel = fr_swapped ? F_BANK_SENTINEL : X_BANK_SENTINEL;
    write_words(TEST_PC, sequence, sizeof(sequence) / sizeof(sequence[0]));

    cpu->regs.f.f[1].u = 0x40000000; /* 2.0 */
    cpu->regs.f.f[4].u = 0x3fc00000; /* 1.5 */
    cpu->regs.r[0] = 0x0c001304;
    cpu->regs.pc = TEST_PC;
    CHECK("single FMUL cycle budget", vk_cpu_run(&cpu->base, 1) == 0);
    CHECK("single FMUL after IRQ entry", cpu->regs.f.f[4].u == 0x40400000);
    CHECK("single FMUL keeps IRQ-entry precision", cpu->regs.fpscr.bit.pr == 0);
    CHECK("single FMUL keeps saved FPSCR", cpu->regs.r[3] == original_fpscr);
    CHECK("single FMUL preserves active bank", cpu->regs.f.f[7].u == active_sentinel);
    CHECK("single FMUL preserves inactive bank", cpu->regs.x.f[7].u == inactive_sentinel);

    cpu->regs.pc = TEST_PC + 2;
    CHECK("LDS.L FPSCR cycle budget", vk_cpu_run(&cpu->base, 1) == 0);
    CHECK("LDS.L restores saved FPSCR", cpu->regs.fpscr.full == original_fpscr);
    CHECK("LDS.L preserves FR bank", cpu->regs.fpscr.bit.fr == ((original_fpscr >> 21) & 1));
    CHECK("LDS.L advances original stack", cpu->regs.r[0] == 0x0c001308);
    CHECK("LDS.L preserves active bank", cpu->regs.f.f[7].u == active_sentinel);
    CHECK("LDS.L preserves inactive bank", cpu->regs.x.f[7].u == inactive_sentinel);

    cpu->regs.f.f[0].u = 0x400a0000; /* 3.25 */
    cpu->regs.f.f[1].u = 0;
    cpu->regs.f.f[2].u = 0x3ff80000; /* 1.5 */
    cpu->regs.f.f[3].u = 0;
    cpu->regs.pc = TEST_PC + 4;
    CHECK("double FMUL cycle budget", vk_cpu_run(&cpu->base, 1) == 0);
    CHECK("double FMUL after FPSCR restore", cpu->regs.f.f[0].u == 0x40138000 &&
          cpu->regs.f.f[1].u == 0);
}

int main(void)
{
    vk_machine_t *machine = calloc(1, sizeof(*machine));
    vk_mmap_t *map = NULL;
    sh4_t *master = NULL;
    sh4_t *slave = NULL;
    vk_game_t guest = {0};
    game = &guest;
    strcpy(game->name, "pharrier");
    if (!machine)
        return 1;

    map = vk_mmap_new(machine);
    ram = vk_buffer_le32_new(RAM_SIZE, 0);
    if (!map || !ram) {
        CHECK("machine RAM allocation", false);
        goto cleanup;
    }
    vk_machine_register_buffer(machine, ram);
    if (vk_mmap_add_ram(map, RAM_BASE, RAM_BASE + RAM_SIZE - 1,
                        RAM_SIZE - 1, 0, ram, "RAM")) {
        CHECK("machine RAM mapping", false);
        goto cleanup;
    }
    machine->game = game;
    master = (sh4_t *)sh4_new(machine, map, true, true);
    slave = (sh4_t *)sh4_new(machine, map, false, true);
    if (!master || !slave) {
        CHECK("master and slave CPU allocation", false);
        goto cleanup;
    }

    for (unsigned use_switch = 0; use_switch < 2; ++use_switch) {
        hikaru_vendor_test_switch_dispatch(use_switch != 0);

        run_irq_case(master, "captured Planet Harriers IRQ, FR0", "pharrier",
                     IRQ_TARGET, IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR,
                     false, false, true);
        run_post_irq_fpu_sequence(master, IRQ_GUEST_FPSCR);
        run_irq_case(master, "captured Planet Harriers IRQ, FR1", "pharrier",
                     IRQ_TARGET, IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR_FR1,
                     false, false, true);
        run_post_irq_fpu_sequence(master, IRQ_GUEST_FPSCR_FR1);

        run_irq_case(master, "other game", "other", IRQ_TARGET,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR, false, false, false);
        run_irq_case(slave, "slave CPU", "pharrier", IRQ_TARGET,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR, false, false, false);
        run_irq_case(master, "wrong IRQ target", "pharrier", IRQ_TARGET + 2,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR, false, false, false);
        run_irq_case(master, "unrelated interrupted PC", "pharrier", IRQ_TARGET,
                     0x0c10e700, IRQ_GUEST_FPSCR, false, false, false);
        run_irq_case(master, "unmatched DN mode", "pharrier", IRQ_TARGET,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR_DN0, false, false, false);
        run_irq_case(master, "modified IRQ tail", "pharrier", IRQ_TARGET,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR, true, false, false);
        run_irq_case(master, "modified IRQ entry prefix", "pharrier", IRQ_TARGET,
                     IRQ_DIVISION_SPC, IRQ_GUEST_FPSCR, false, true, false);
        run_direct_non_slot(master);
    }

cleanup:
    if (map)
        vk_mmap_destroy(&map);
    if (machine)
        vk_machine_destroy(&machine);
    if (!failures)
        puts("Planet Harriers IRQ FPSCR compatibility checks passed");
    return failures ? 1 : 0;
}
