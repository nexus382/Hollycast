// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "cpu/sh/sh4.h"
#include "vk/machine.h"
#include "vk/mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void hikaru_vendor_test_poll_skip(bool);
uint64_t hikaru_vendor_test_poll_skipped(void);
static unsigned devreads, patches, port_reads, port_writes;
static int port_get(sh4_t *s, uint16_t *v) {
  (void)s;
  ++port_reads;
  *v = 0;
  return 0;
}
static int port_put(sh4_t *s, uint16_t v) {
  (void)s;
  (void)v;
  ++port_writes;
  return 0;
}
static const uint16_t code[] = {0xc400, 0xe202, 0x4218, 0x372e, 0xe180, 0x8f06,
                                0x4128, 0x7130, 0x6211, 0xe702, 0x227a, 0x2121,
                                0xe700, 0x2008, 0xe000, 0x89ef, 0xaffe, 0x0009};
static int devget(vk_device_t *d, unsigned size, uint32_t a, void *out) {
  (void)d;
  devreads++;
  if (size == 2)
    return set_ptr(out, size, code[(a - 0x0c000100) / 2]);
  return set_ptr(out, size, 0);
}
static uint32_t patch(vk_cpu_t *c, uint32_t p, uint32_t i) {
  (void)c;
  (void)p;
  patches++;
  return i;
}
static uint64_t hash(uint64_t h, const void *p, size_t n) {
  const unsigned char *b = p;
  while (n--)
    h = (h ^ *b++) * 1099511628211ull;
  return h;
}
typedef struct {
  uint64_t h, skipped;
  unsigned dr, p, r, w, cases;
} Result;
static Result run(unsigned mode, bool enabled) {
  vk_machine_t *mach = calloc(1, sizeof(*mach));
  vk_mmap_t *map = vk_mmap_new(mach);
  vk_buffer_t *ram = vk_buffer_le32_new(65536, 0);
  vk_device_t d = {0};
  d.get = devget;
  if (mode == 1 && vk_mmap_add_dev(map, 0x0c000200, 0x0c000201, 1,
                                   VK_REGION_R | VK_REGION_SIZE_8, &d, "data"))
    abort();
  if (mode == 2 && vk_mmap_add_dev(map, 0x0c000100, 0x0c000123, 0x3f,
                                   VK_REGION_R | VK_REGION_SIZE_16, &d, "code"))
    abort();
  if (vk_mmap_add_ram(map, 0x0c000000, 0x0c00ffff, 0xffff, 0, ram, "ram"))
    abort();
  vk_machine_register_buffer(mach, ram);
  vk_cpu_t *cpu = sh4_new(mach, map, true, true);
  sh4_t *sh = (sh4_t *)cpu;
  sh4_set_porta_handlers(cpu, port_get, port_put);
  if (mode == 3)
    cpu->patch = patch;
  memcpy(ram->ptr + 0x100, code, sizeof(code));
  uint16_t il[] = {0xaffe, 9};
  memcpy(ram->ptr + 0x600, il, sizeof(il));
  const uint32_t r7[] = {0,          1,          0xfffff000, 0xfffffbfd,
                         0xfffffdfd, 0xfffffdfe, 0xfffffdff, 0xffffffff};
  hikaru_vendor_test_poll_skip(enabled);
  port_reads = port_writes = devreads = patches = 0;
  Result z = {.h = 1469598103934665603ull};
  for (unsigned v = 0; v < 2; v++)
    for (unsigned initT = 0; initT < 2; initT++)
      for (unsigned p = 0; p < 16; p++)
        for (unsigned k = 0; k < 8; k++)
          for (int cyc = 1; cyc <= 70; cyc++) {
            vk_device_reset((vk_device_t *)cpu, VK_RESET_TYPE_HARD);
            ram->ptr[0x200] = v;
            sh->regs.pc = 0x0c000100 + 2 * p;
            sh->regs.gbr = 0x0c000200;
            sh->regs.r[0] = v;
            sh->regs.r[1] = p == 5 || p == 6 ? 0xffffff80u
                            : p == 7         ? 0xff800000u
                                             : 0xff800030u;
            sh->regs.r[2] = 512;
            sh->regs.r[7] = r7[k];
            sh->regs.sr.bit.t = initT;
            if (mode >= 4) {
              sh->regs.sr.bit.bl = 0;
              sh->regs.sr.bit.i = mode == 4 ? 15 : 0;
              sh->regs.vbr = 0x0c000000;
              vk_cpu_set_irq_state(cpu, SH4_IESOURCE_IRL2, VK_IRQ_STATE_RAISED);
            }
            int n = vk_cpu_run(cpu, cyc);
            z.h = hash(z.h, &n, sizeof(n));
            z.h = hash(z.h, &sh->regs, sizeof(sh->regs));
            z.h = hash(z.h, sh->iregs->ptr, 256);
            z.h = hash(z.h, &sh->intc.pending, sizeof(sh->intc.pending));
            z.h = hash(z.h, &sh->tmu.counter[0], sizeof(sh->tmu.counter[0]));
            z.cases++;
          }
  z.skipped = hikaru_vendor_test_poll_skipped();
  z.dr = devreads;
  z.p = patches;
  z.r = port_reads;
  z.w = port_writes;
  vk_mmap_destroy(&map);
  vk_machine_destroy(&mach);
  return z;
}
int main(void) {
  unsigned failures = 0;
  for (unsigned m = 0; m < 6; m++) {
    Result a = run(m, false), b = run(m, true);
    if (a.h != b.h || a.dr != b.dr || a.p != b.p || a.r != b.r || a.w != b.w ||
        (m == 0 ? !b.skipped : b.skipped)) {
      fprintf(stderr,
              "mode %u hash %llx/%llx mmio %u:%u/%u:%u dev %u/%u patch %u/%u "
              "skipped %llu\n",
              m, (unsigned long long)a.h, (unsigned long long)b.h, a.r, a.w,
              b.r, b.w, a.dr, b.dr, a.p, b.p, (unsigned long long)b.skipped);
      failures++;
    }
    printf("mode %u cases %u skipped %llu mmio %u/%u\n", m, b.cases,
           (unsigned long long)b.skipped, b.r, b.w);
  }
  return failures ? 1 : 0;
}
