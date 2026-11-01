// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic timing checks: no game ROM or sound firmware required.
#include "cpu/sh/sh4.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-aica.h"
#include "mach/hikaru/hikaru-memctl.h"
#include "vk/mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t hikaru_vendor_test_read_value(sh4_t *, unsigned, uint32_t);
static unsigned failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"sound timing line %u: %s\n",__LINE__,#x); ++failures; } } while (0)
static void set_instruction(vk_buffer_t *ram, uint16_t op) {
    vk_buffer_put(ram,2,0x100,op); vk_buffer_put(ram,2,0x102,0x0009);
}
static void reset_cpu(sh4_t *cpu) {
    vk_device_reset((vk_device_t *)cpu,VK_RESET_TYPE_HARD);
    cpu->base.remaining=0;
    cpu->regs.pc=0x0c000100;
    cpu->regs.r[1]=0x02800010;
}
int main(void) {
    hikaru_t *board=calloc(1,sizeof(*board));
    if(!board) return 2;
    vk_mmap_t *map=vk_mmap_new(&board->base);
    board->ram_m=vk_buffer_le32_new(65536,0);
    board->ram_s=vk_buffer_le32_new(0x1000000,0);
    board->aica_ram_m=vk_buffer_le32_new(0x800000,0);
    board->aica_ram_s=vk_buffer_le32_new(0x800000,0);
    if(!map || !board->ram_m || !board->ram_s || !board->aica_ram_m || !board->aica_ram_s) return 2;
    vk_machine_register_buffer(&board->base,board->ram_m);
    vk_machine_register_buffer(&board->base,board->ram_s);
    vk_machine_register_buffer(&board->base,board->aica_ram_m);
    vk_machine_register_buffer(&board->base,board->aica_ram_s);
    board->aica_m=hikaru_aica_new(&board->base,board->aica_ram_m,true);
    board->aica_s=hikaru_aica_new(&board->base,board->aica_ram_s,false);
    board->memctl_m=hikaru_memctl_new(&board->base,true);
    if(!board->aica_m || !board->aica_s || !board->memctl_m) return 2;
    CHECK(!vk_mmap_add_dev(map,0x02000000,0x02ffffff,0x00ffffff,VK_REGION_RW|VK_REGION_SIZE_ALL,board->memctl_m,"aperture"));
    CHECK(!vk_mmap_add_ram(map,0x0c000000,0x0c00ffff,0xffff,0,board->ram_m,"master RAM"));
    vk_cpu_t *cpu=sh4_new(&board->base,map,true,true);
    if(!cpu) return 2;
    board->sh_m=board->sh_current=cpu;
    sh4_t *sh=(sh4_t *)cpu;
    vk_buffer_put(board->aica_ram_m,4,0x10,0x12345678);
    vk_buffer_put(board->aica_ram_s,4,0x10,0x87654321);
    vk_buffer_put(board->ram_s,4,0x800010,0xaabbccdd);
    const unsigned rates[]={100,200,250,300};
    const unsigned waits[]={32,64,80,96};
    for(unsigned k=0;k<4;++k) for(unsigned bank=0;bank<2;++bank) {
        board->cpu_clock_mhz=rates[k];
        CHECK(!vk_device_put(board->memctl_m,1,0x0400001a,0x0c+bank));
        reset_cpu(sh); set_instruction(board->ram_m,0x6012); /* mov.l @r1,r0 */
        sh->tmu.is_running[0]=true; sh->tmu.counter[0]=1000;
        vk_cpu_run(cpu,1);
        CHECK(sh->regs.r[0]==(bank?0x87654321u:0x12345678u));
        CHECK(sh->sound_cycle_debt==(int)waits[k]);
        CHECK(sh->tmu.counter[0]==999);
        const uint32_t pc=sh->regs.pc;
        for(unsigned i=0;i<waits[k];++i) vk_cpu_run(cpu,1);
        CHECK(sh->sound_cycle_debt==0 && sh->regs.pc==pc);
        CHECK(sh->tmu.counter[0]==999-waits[k]);
        vk_cpu_run(cpu,1); CHECK(sh->regs.pc==pc+2);
    }
    board->cpu_clock_mhz=200;
    CHECK(!vk_device_put(board->memctl_m,1,0x0400001a,0x0c));
    /* The direct-memory fast path for ordinary RAM remains free of wait. */
    reset_cpu(sh); set_instruction(board->ram_m,0x6012); sh->regs.r[1]=0x0c000200;
    vk_buffer_put(board->ram_m,4,0x200,0x11223344); vk_cpu_run(cpu,1);
    CHECK(sh->regs.r[0]==0x11223344 && sh->sound_cycle_debt==0);
    /* The same aperture address remapped to slave RAM is not sound RAM. */
    CHECK(!vk_device_put(board->memctl_m,1,0x0400001a,0x40));
    reset_cpu(sh); vk_cpu_run(cpu,1);
    CHECK(sh->regs.r[0]==0xaabbccdd && sh->sound_cycle_debt==0);
    /* Direct device/debug reads outside CPU execution do not charge. */
    CHECK(!vk_device_put(board->memctl_m,1,0x0400001a,0x0c));
    reset_cpu(sh); uint32_t value=0;
    CHECK(!vk_device_get(board->memctl_m,4,0x02800010,&value));
    CHECK(value==0x12345678 && sh->sound_cycle_debt==0 && cpu->remaining==0);
    /* Exercise each width at the actual instruction reader, including R64. */
    for(unsigned size=1;size<=8;size*=2) {
        reset_cpu(sh); cpu->remaining=1000; sh->cpu_active=true;
        (void)hikaru_vendor_test_read_value(sh,size,0x02800010);
        sh->cpu_active=false;
        CHECK(cpu->remaining==936);
    }
    /* At 201 MHz, 25 reads carry 25 * 64.32 cycles without drift. */
    board->cpu_clock_mhz=201; reset_cpu(sh); cpu->remaining=100000; sh->cpu_active=true;
    for(unsigned i=0;i<25;++i) (void)hikaru_vendor_test_read_value(sh,4,0x02800010);
    sh->cpu_active=false;
    CHECK(cpu->remaining==98392 && sh->sound_wait_fraction==0);
    /* Saving and loading preserve debt/fraction but never transient contexts. */
    FILE *file=tmpfile(); CHECK(file!=NULL);
    if(file) {
        vk_state_t state={VK_STATE_SAVE,file};
        sh->sound_cycle_debt=17; sh->sound_wait_fraction=320;
        CHECK(!((vk_device_t *)cpu)->save_state((vk_device_t *)cpu,&state));
        rewind(file); state.mode=VK_STATE_LOAD;
        sh->sound_cycle_debt=99; sh->sound_wait_fraction=0;
        sh->cpu_active=sh->data_read_active=sh->dma_active=sh->sound_wait_this_slice=true;
        CHECK(!((vk_device_t *)cpu)->load_state((vk_device_t *)cpu,&state));
        CHECK(sh->sound_cycle_debt==17 && sh->sound_wait_fraction==320);
        CHECK(!sh->cpu_active && !sh->data_read_active && !sh->dma_active && !sh->sound_wait_this_slice);
        fclose(file);
    }
    reset_cpu(sh); CHECK(sh->sound_cycle_debt==0 && sh->sound_wait_fraction==0);
    board->cpu_clock_mhz=200;
    /* A delayed-slot load retains both the branch and the read's overshoot. */
    reset_cpu(sh); vk_buffer_put(board->ram_m,2,0x100,0xa000); /* bra +0 */
    vk_buffer_put(board->ram_m,2,0x102,0x6012); vk_cpu_run(cpu,1);
    CHECK(sh->regs.r[0]==0x12345678 && sh->sound_cycle_debt==65);
    /* MEMCTL DMA runs outside CPU execution and does not add CPU debt. */
    reset_cpu(sh);
    CHECK(!vk_device_put(board->memctl_m,4,0x04000030,0x0c800010));
    CHECK(!vk_device_put(board->memctl_m,4,0x04000034,0x70000300));
    CHECK(!vk_device_put(board->memctl_m,4,0x04000038,0x01000001));
    CHECK(!vk_device_exec(board->memctl_m,1));
    CHECK(vk_buffer_get(board->ram_m,4,0x300)==0x12345678 && sh->sound_cycle_debt==0);
    /* Real SH4 DMA triggered by a CPU write: its sound source read is exempt. */
    reset_cpu(sh); set_instruction(board->ram_m,0x2102); /* mov.l r0,@r1 */
    sh->regs.r[0]=1; sh->regs.r[1]=0xffa00040;
    vk_buffer_put(sh->iregs,4,0xa000,0x02800010); /* SAR0 */
    vk_buffer_put(sh->iregs,4,0xa004,0x0c000300); /* DAR0 */
    vk_buffer_put(sh->iregs,4,0xa008,1);          /* TCR0 */
    vk_buffer_put(sh->iregs,4,0xa00c,0x5431);     /* CHCR0: long, auto, increment */
    vk_cpu_run(cpu,1);
    CHECK(vk_buffer_get(board->ram_m,4,0x300)==0x12345678 && sh->sound_cycle_debt==0);
    vk_mmap_destroy(&map);
    vk_machine_t *machine=&board->base; vk_machine_destroy(&machine);
    if(failures) return 1;
    puts("Hikaru sound bus CPU timing passed"); return 0;
}
