// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Generated programs only: no game instructions or ROM data.
#include "vk/machine.h"
#include "vk/mmap.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_switch_dispatch(bool enabled);
void hikaru_vendor_test_read_hit(bool enabled);
void hikaru_vendor_test_write_hit(bool enabled);
int hikaru_vendor_test_verify_dispatch(void);

static unsigned patch_count, device_reads, device_writes;
static uint64_t device_hash;
static uint32_t rng;
static uint32_t next(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static uint32_t patch(vk_cpu_t *cpu, uint32_t pc, uint32_t instruction)
{
    (void)cpu; (void)pc;
    ++patch_count;
    return instruction == 0x0009 ? 0xe52a : instruction; // NOP -> MOV #42,R5
}
static int device_get(vk_device_t *device, unsigned size, uint32_t addr, void *out)
{
    (void)device;
    ++device_reads;
    device_hash = device_hash * 31 + addr + size;
    return set_ptr(out, size, 0x12345678);
}
static int device_put(vk_device_t *device, unsigned size, uint32_t addr, uint64_t value)
{
    (void)device;
    ++device_writes;
    device_hash = device_hash * 31 + addr + size + value;
    return 0;
}

typedef struct {
    vk_machine_t *machine;
    vk_mmap_t *map;
    vk_buffer_t *ram;
    sh4_t *cpu;
    vk_device_t device;
} Fixture;
static Fixture *create(void)
{
    Fixture *f = calloc(1, sizeof(*f));
    if (!f) abort();
    f->machine = calloc(1, sizeof(*f->machine));
    if (!f->machine) abort();
    f->map = vk_mmap_new(f->machine);
    f->ram = vk_buffer_le32_new(65536, 0);
    if (!f->map || !f->ram) abort();
    f->device.get = device_get;
    f->device.put = device_put;
    if (vk_mmap_add_dev(f->map, 0x0d000000, 0x0d0000ff, 255,
                        VK_REGION_RW | VK_REGION_SIZE_ALL, &f->device, "MMIO") ||
        vk_mmap_add_ram(f->map, 0x0c000000, 0x0c00ffff, 65535, 0, f->ram, "RAM")) abort();
    vk_machine_register_buffer(f->machine, f->ram);
    f->cpu = (sh4_t *)sh4_new(f->machine, f->map, true, true);
    if (!f->cpu) abort();
    return f;
}
static void destroy(Fixture *f)
{
    vk_mmap_destroy(&f->map);
    vk_machine_destroy(&f->machine);
    free(f);
}
static uint64_t hash(const void *data, size_t size)
{
    const unsigned char *p = data;
    uint64_t h = 1469598103934665603ull;
    while (size--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}
typedef struct {
    uint64_t regs, ram, iregs, intc, tmu, io;
    int remaining, ran, state, slot;
    unsigned patches, reads, writes;
} Result;

static Result run(Fixture *f, bool use_switch, unsigned seed, unsigned kind)
{
    // Register arithmetic, comparisons, shifts, status operations.
    static const uint16_t alu[] = {
        0x312c, 0x3128, 0x2129, 0x212a, 0x212b, 0x6123, 0x6127, 0x6128,
        0x6129, 0x612c, 0x612d, 0x612e, 0x612f, 0x4110, 0x4100, 0x4101,
        0x4104, 0x4105, 0x4124, 0x4125, 0x4108, 0x4118, 0x4128, 0x2128, 0x3120, 0x3122,
        0x3123, 0x3126, 0x3127, 0x0008, 0x0018, 0x0028, 0x0129
    };
    // Includes FMAC, FIPR and FTRV: inlining must preserve compiled FP order.
    static const uint16_t fp[] = {
        0xf120, 0xf121, 0xf122, 0xf123, 0xf124, 0xf125, 0xf12c, 0xf12e,
        0xf14d, 0xf15d, 0xf16d, 0xf19d, 0xf17d, 0xf18d, 0xf1ed, 0xf1fd,
        0xf0fd, 0xf5fd, 0xf9fd, 0xfdfd
    };
    // Both signs, subnormals, infinities, and distinct quiet/signaling NaNs.
    static const uint32_t bits[] = {
        0, 0x80000000, 1, 0x80000001, 0x007fffff, 0x00800000,
        0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
        0x7fc01234, 0xffc05678, 0x7f801234, 0xff805678
    };
    sh4_t *s = f->cpu;
    vk_device_reset((vk_device_t *)s, VK_RESET_TYPE_HARD);
    rng = seed;
    for (unsigned i = 0; i < 65536; i += 4) {
        uint32_t value = next();
        memcpy(f->ram->ptr + i, &value, 4);
    }
    for (unsigned i = 0; i < 16; ++i) {
        s->regs.r[i] = next();
        s->regs.f.f[i].u = seed % 2 ? next() : bits[(seed + i) % 14];
        s->regs.x.f[i].u = seed % 2 ? next() : bits[(seed + 3 * i) % 14];
    }
    s->regs.fpscr.full = 0;
    s->regs.sr.bit.t = seed & 1;
    s->regs.pc = 0x0c000100;
    s->regs.gbr = 0x0c008000;
    s->regs.vbr = 0x0c000000;
    uint16_t code[80];
    for (unsigned i = 0; i < 80; ++i) code[i] = 0x0009;
    unsigned count = 32;
    if (kind < 2) {
        for (unsigned i = 0; i < count; ++i)
            code[i] = kind ? fp[next() % NUMELEM(fp)] : alu[next() % NUMELEM(alu)];
    } else if (kind == 2) {
        // BRA, BT/S, BF/S, and delay-slot register writes, both initial T states.
        const uint16_t branches[] = {0xa001, 0xe101, 0xe102, 0x8d01, 0xe203,
                                    0xe204, 0x8f01, 0xe305, 0xe306};
        memcpy(code, branches, sizeof(branches));
        count = NUMELEM(branches);
    } else {
        // Exercise both single and paired (SZ=1) FMOV memory transfers.
        s->regs.fpscr.bit.sz = seed & 1;
        s->regs.r[2] = kind == 3 ? 0x0c008040 : 0x0d000040;
        const uint16_t memory[] = {0x2210, 0x6120, 0x2211, 0x6121, 0x2212,
                                  0x6122, 0xf128, 0xf22a, 0xf129, 0xf22b};
        memcpy(code, memory, sizeof(memory));
        count = NUMELEM(memory);
    }
    code[count] = 0xaffe; code[count + 1] = 0x0009;
    memcpy(f->ram->ptr + 0x100, code, sizeof(code));
    const uint16_t irq[] = {0xaffe, 0x0009};
    memcpy(f->ram->ptr + 0x600, irq, sizeof(irq));
    if (seed % 13 == 0) {
        s->regs.sr.bit.bl = 0;
        s->regs.sr.bit.i = 0;
        vk_cpu_set_irq_state((vk_cpu_t *)s, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED);
    }
    s->base.patch = seed % 7 == 0 ? patch : NULL;
    patch_count = device_reads = device_writes = 0;
    device_hash = 0;
    hikaru_vendor_test_switch_dispatch(use_switch);
    hikaru_vendor_test_read_hit(use_switch);
    hikaru_vendor_test_write_hit(use_switch);
    int ran = vk_cpu_run((vk_cpu_t *)s, 1 + seed % 48);
    Result r = {
        hash(&s->regs, sizeof(s->regs)), hash(f->ram->ptr, 65536),
        hash(s->iregs->ptr, s->iregs->size), hash(&s->intc, sizeof(s->intc)),
        hash(&s->tmu, sizeof(s->tmu)), device_hash, s->base.remaining, ran,
        s->base.state, s->in_slot, patch_count, device_reads, device_writes
    };
    return r;
}
static bool equal(Result a, Result b)
{
    return a.regs == b.regs && a.ram == b.ram && a.iregs == b.iregs &&
           a.intc == b.intc && a.tmu == b.tmu && a.io == b.io &&
           a.remaining == b.remaining && a.ran == b.ran && a.state == b.state &&
           a.slot == b.slot && a.patches == b.patches &&
           a.reads == b.reads && a.writes == b.writes;
}
int main(void)
{
    Fixture *a = create(), *b = create();
    if (hikaru_vendor_test_verify_dispatch()) abort();
    unsigned checks = 0;
    for (unsigned kind = 0; kind < 5; ++kind) {
        for (unsigned seed = 1; seed <= 2048; ++seed) {
            Result expected = run(a, false, seed, kind), actual = run(b, true, seed, kind);
            if (!equal(expected, actual)) {
                fprintf(stderr, "dispatch mismatch: kind=%u seed=%u\n", kind, seed);
                destroy(a); destroy(b);
                return 1;
            }
            ++checks;
        }
    }
    destroy(a); destroy(b);
    printf("Hikaru dispatch: 65536 opcode mappings, %u differential programs passed\n", checks);
    return 0;
}
