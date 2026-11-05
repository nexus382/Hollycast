// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "engine_internal.h"
#include "mach/hikaru/hikaru.h"
#include "vk/cpu.h"
#include "vk/device.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int hikaru_vendor_test_run_cycles(vk_machine_t *, int);
void hikaru_vendor_test_reset(vk_machine_t *);
static uint64_t master, slave, memctl, mie, gpu, sound;
static unsigned calls;
static vk_cpu_t cpu_master, cpu_slave;
static vk_device_t dev_memctl, dev_mie, dev_gpu;
static int run_cpu(vk_cpu_t *cpu, int cycles) {
    if (cpu == &cpu_master) master += cycles; else slave += cycles;
    ++calls; return cycles;
}
static void set_state(vk_cpu_t *cpu, vk_cpu_state_t state) { cpu->state = state; }
static int set_irq(vk_cpu_t *cpu, unsigned number, vk_irq_state_t state) {
    (void)cpu; (void)number; (void)state; return 0;
}
static int run_device(vk_device_t *dev, int cycles) {
    if (dev == &dev_memctl) memctl += cycles;
    else if (dev == &dev_mie) mie += cycles;
    else if (dev == &dev_gpu) gpu += cycles;
    return cycles;
}
static int read_mie(void *opaque, unsigned size, uint32_t address, uint64_t *value) {
    (void)opaque; if (size != 1 || address != 0x00800006) return -1; *value = 0x20; return 0;
}
static int set_sound_irq(void *opaque, int level) { (void)opaque; return level; }
static int advance_sound(void *opaque, uint32_t cycles) { (void)opaque; sound += cycles; return 0; }
static int get_sound_irq(void *opaque, unsigned board) { (void)opaque; (void)board; return 0; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"clock check failed at line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(void) {
    hikaru_t board; HikaruEngine engine;
    memset(&board, 0, sizeof(board)); memset(&engine, 0, sizeof(engine));
    cpu_master.run=cpu_slave.run=run_cpu; cpu_master.set_irq_state=set_irq; cpu_slave.set_state=set_state;
    dev_memctl.exec=dev_mie.exec=dev_gpu.exec=run_device;
    board.sh_m=&cpu_master; board.sh_s=&cpu_slave; board.memctl_m=&dev_memctl; board.mie=&dev_mie; board.gpu=&dev_gpu;
    board.base.host=&engine; board.porta_m=0xffff; board.cpu_clock_mhz=200;
    engine.machine=&board.base; engine.sound_attached=true; engine.mie.read=read_mie; engine.mie.set_sound_irq=set_sound_irq;
    engine.sound.advance=advance_sound; engine.sound.irq=get_sound_irq;
    const unsigned rates[]={100,150,200,250,300};
    for (unsigned k=0;k<sizeof(rates)/sizeof(rates[0]);++k) {
        const unsigned rate=rates[k]; CHECK(!hikaru_engine_set_cpu_clock_mhz(&engine,rate));
        master=slave=memctl=mie=gpu=sound=0; calls=0;
        for (unsigned i=0;i<544;++i) {
            CHECK(!hikaru_vendor_test_run_cycles(&board.base,6127));
            CHECK(master==slave);
            CHECK(memctl==mie && mie==gpu && gpu==sound && sound==(uint64_t)(i+1)*6127);
            CHECK(master==((uint64_t)(i+1)*6127*rate)/200);
            if (rate==200) CHECK(master==(uint64_t)(i+1)*6127);
        }
        CHECK(calls==1088);
    }
    CHECK(!hikaru_engine_set_cpu_clock_mhz(&engine,150));
    CHECK(!hikaru_vendor_test_run_cycles(&board.base,6127));
    CHECK(board.cpu_cycle_remainder==50);
    CHECK(!hikaru_engine_set_cpu_clock_mhz(&engine,150)); CHECK(board.cpu_cycle_remainder==50);
    CHECK(hikaru_engine_set_cpu_clock_mhz(&engine,99)==-1); CHECK(board.cpu_clock_mhz==150 && board.cpu_cycle_remainder==50);
    CHECK(hikaru_engine_set_cpu_clock_mhz(&engine,301)==-1); CHECK(board.cpu_clock_mhz==150 && board.cpu_cycle_remainder==50);
    CHECK(hikaru_engine_set_cpu_clock_mhz(NULL,200)==-1);
    CHECK(!hikaru_engine_set_cpu_clock_mhz(&engine,250)); CHECK(board.cpu_cycle_remainder==0);
    CHECK(!hikaru_vendor_test_run_cycles(&board.base,6127)); CHECK(board.cpu_cycle_remainder==150);
    hikaru_vendor_test_reset(&board.base); CHECK(board.cpu_clock_mhz==250 && board.cpu_cycle_remainder==0);
    puts("dual CPU clock and fixed device budgets passed"); return 0;
}
