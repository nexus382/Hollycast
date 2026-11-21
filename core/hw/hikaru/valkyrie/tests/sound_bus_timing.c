// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
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
int hikaru_vendor_test_memctl_dma(vk_device_t *, uint32_t, uint32_t, uint32_t, uint32_t, int);
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
typedef struct {
    int bulk;
    uint32_t source, destination, control, status;
    vk_irq_state_t irq;
    uint8_t output[64];
    size_t output_size;
    unsigned gets, puts;
} dma_result_t;

static unsigned dma_custom_gets, dma_custom_puts;
static unsigned dma_aica_gets;
static int (*dma_aica_original_get)(vk_device_t *, unsigned, uint32_t, void *);
static int dma_count_aica_get(vk_device_t *device, unsigned size,
                              uint32_t address, void *value) {
    ++dma_aica_gets;
    return dma_aica_original_get(device, size, address, value);
}
static uint64_t dma_custom_get(vk_buffer_t *buffer, unsigned size, uint32_t offset) {
    uint64_t value = 0;
    memcpy(&value, buffer->ptr + offset, size);
    ++dma_custom_gets;
    return value;
}
static void dma_custom_put(vk_buffer_t *buffer, unsigned size, uint32_t offset, uint64_t value) {
    memcpy(buffer->ptr + offset, &value, size);
    ++dma_custom_puts;
}
static bool dma_test_ram_span(hikaru_t *board, uint32_t address,
                              vk_buffer_t **buffer, uint32_t *offset,
                              unsigned *available) {
    const uint32_t bus_address = address & 0x7fffffff;
    uint32_t base, limit;
    vk_buffer_t *ram;
    if (bus_address >= 0x40000000 && bus_address <= 0x41ffffff) {
        ram = board->ram_s;
        base = 0x40000000;
        limit = 0x41ffffff;
    } else if (bus_address >= 0x70000000 && bus_address <= 0x71ffffff) {
        ram = board->ram_m;
        base = 0x70000000;
        limit = 0x71ffffff;
    } else {
        return false;
    }
    if (!ram || !ram->ptr) return false;
    uint32_t ram_offset = bus_address - base;
    unsigned size = vk_buffer_get_size(ram);
    if (ram_offset > size) return false;
    unsigned buffer_bytes = size - ram_offset;
    uint32_t window_bytes = limit - bus_address + 1;
    if (buffer_bytes > window_bytes) buffer_bytes = window_bytes;
    *buffer = ram;
    *offset = ram_offset;
    *available = buffer_bytes;
    return true;
}
static void dma_seed_range(hikaru_t *board, uint32_t address,
                           unsigned bytes, uint8_t seed) {
    vk_buffer_t *buffer;
    uint32_t offset;
    unsigned available;
    if (!dma_test_ram_span(board, address, &buffer, &offset, &available)) return;
    if (bytes > available) bytes = available;
    for (unsigned i = 0; i < bytes; ++i)
        buffer->ptr[offset + i] = (uint8_t)(seed + i * 29u);
}
static void dma_prepare(hikaru_t *board, uint32_t source, uint32_t destination,
                        unsigned words) {
    unsigned bytes = words * 4;
    dma_seed_range(board, source, bytes, 0x13);
    dma_seed_range(board, destination, bytes, 0xa7);
}
static void dma_run(hikaru_t *board, uint32_t source, uint32_t destination,
                    unsigned words, unsigned cycles, int force_reference,
                    dma_result_t *result) {
    memset(result, 0, sizeof(*result));
    vk_device_reset(board->memctl_m, VK_RESET_TYPE_HARD);
    vk_cpu_set_irq_state(board->sh_m, SH4_IESOURCE_IRL1, VK_IRQ_STATE_CLEAR);
    dma_custom_gets = dma_custom_puts = 0;
    result->bulk = hikaru_vendor_test_memctl_dma(board->memctl_m, source,
                                                  destination, words, cycles, force_reference);
    CHECK(result->bulk >= 0);
    if (result->bulk < 0) return;
    CHECK(!vk_device_get(board->memctl_m, 4, 0x04000030, &result->source));
    CHECK(!vk_device_get(board->memctl_m, 4, 0x04000034, &result->destination));
    CHECK(!vk_device_get(board->memctl_m, 4, 0x04000038, &result->control));
    CHECK(!vk_device_get(board->memctl_m, 4, 0x04000004, &result->status));
    result->irq = ((sh4_t *)board->sh_m)->intc.irqs[SH4_IESOURCE_IRL1].state;
    result->gets = dma_custom_gets;
    result->puts = dma_custom_puts;
    vk_buffer_t *buffer;
    uint32_t offset;
    unsigned available;
    if (dma_test_ram_span(board, destination, &buffer, &offset, &available)) {
        result->output_size = words * 4;
        if (result->output_size > available) result->output_size = available;
        if (result->output_size > sizeof(result->output))
            result->output_size = sizeof(result->output);
        memcpy(result->output, buffer->ptr + offset, result->output_size);
    }
}
static void compare_memctl_dma(hikaru_t *board, uint32_t source,
                              uint32_t destination, unsigned words, unsigned cycles,
                              int expected_bulk, unsigned expected_gets,
                              unsigned expected_puts) {
    dma_result_t fast, reference;
    dma_prepare(board, source, destination, words);
    dma_run(board, source, destination, words, cycles, 0, &fast);
    CHECK(fast.bulk == expected_bulk);
    dma_prepare(board, source, destination, words);
    dma_run(board, source, destination, words, cycles, 1, &reference);
    CHECK(reference.bulk == 0);
    CHECK(fast.output_size == reference.output_size);
    CHECK(!memcmp(fast.output, reference.output, fast.output_size));
    CHECK(fast.source == reference.source);
    CHECK(fast.destination == reference.destination);
    CHECK(fast.control == reference.control);
    CHECK(fast.status == reference.status);
    CHECK(fast.irq == reference.irq);
    CHECK(fast.gets == reference.gets && fast.puts == reference.puts);
    CHECK(fast.gets == expected_gets && fast.puts == expected_puts);
    unsigned transferred = cycles < words ? cycles : words;
    bool complete = transferred == words;
    CHECK(fast.source == source + transferred * 4);
    CHECK(fast.destination == destination + transferred * 4);
    CHECK(fast.control == (complete ? 0 : 0x01000000 | (words - transferred)));
    CHECK(fast.status == (complete ? 0x1000 : 0));
    CHECK(fast.irq == (complete ? VK_IRQ_STATE_RAISED : VK_IRQ_STATE_CLEAR));
}

int main(void) {
    hikaru_t *board=calloc(1,sizeof(*board));
    if(!board) return 2;
    vk_mmap_t *map=vk_mmap_new(&board->base);
    board->ram_m=vk_buffer_le32_new(0x2000000,0);
    board->ram_s=vk_buffer_le32_new(0x2000000,0);
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
    CHECK(!vk_mmap_add_dev(map,0x04000000,0x0400003f,0x3f,VK_REGION_RW|VK_REGION_SIZE_ALL,board->memctl_m,"MEMCTL"));
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
    /* VON4 refills the same staging RAM immediately after a DMA start.
     * Consuming it at the next device slice would mix two geometry blocks. */
    for(unsigned i=0;i<8;++i) vk_buffer_put(board->ram_m,4,0x400+4*i,0x11223300u+i);
    CHECK(!vk_device_put(board->memctl_m,4,0x04000030,0x70000400));
    CHECK(!vk_device_put(board->memctl_m,4,0x04000034,0x40000500));
    CHECK(!vk_device_put(board->memctl_m,4,0x04000038,0x01000008));
    uint32_t control=~0u;
    CHECK(!vk_device_get(board->memctl_m,4,0x04000038,&control) && control==0);
    for(unsigned i=0;i<8;++i) vk_buffer_put(board->ram_m,4,0x400+4*i,0xa5a5a5a5);
    CHECK(!vk_device_exec(board->memctl_m,1));
    for(unsigned i=0;i<8;++i) CHECK(vk_buffer_get(board->ram_s,4,0x500+4*i)==0x11223300u+i);
    /* A MEMCTL start executed by the CPU must not charge sound DMA reads
     * as CPU data loads or defer the copy until after the instruction. */
    reset_cpu(sh); set_instruction(board->ram_m,0x2102); /* mov.l r0,@r1 */
    sh->regs.r[0]=0x01000001; sh->regs.r[1]=0x04000038;
    CHECK(!vk_device_put(board->memctl_m,4,0x04000030,0x0c800010));
    CHECK(!vk_device_put(board->memctl_m,4,0x04000034,0x70000304));
    vk_cpu_run(cpu,1);
    CHECK(vk_buffer_get(board->ram_m,4,0x304)==0x12345678 && sh->sound_cycle_debt==0);
    /* Real SH4 DMA triggered by a CPU write: its sound source read is exempt. */
    reset_cpu(sh); set_instruction(board->ram_m,0x2102); /* mov.l r0,@r1 */
    sh->regs.r[0]=1; sh->regs.r[1]=0xffa00040;
    vk_buffer_put(sh->iregs,4,0xa000,0x02800010); /* SAR0 */
    vk_buffer_put(sh->iregs,4,0xa004,0x0c000300); /* DAR0 */
    vk_buffer_put(sh->iregs,4,0xa008,1);          /* TCR0 */
    vk_buffer_put(sh->iregs,4,0xa00c,0x5431);     /* CHCR0: long, auto, increment */
    vk_cpu_run(cpu,1);
    CHECK(vk_buffer_get(board->ram_m,4,0x300)==0x12345678 && sh->sound_cycle_debt==0);
    /* The reference loop and guarded RAM bulk path have identical bytes,
     * address progress, completion status, and MEMCTL IRQ state. */
    compare_memctl_dma(board,0x70000201,0x40000403,5,5,1,0,0); /* unaligned master to slave */
    compare_memctl_dma(board,0xc0000802,0xf0001003,3,3,1,0,0); /* 31-bit bus aliases */
    compare_memctl_dma(board,0x70000600,0x70000600,4,4,1,0,0); /* exact self-copy */
    compare_memctl_dma(board,0x41fffffc,0x70000800,2,2,0,0,0); /* source crosses RAM end */
    compare_memctl_dma(board,0x70000800,0x41fffffc,2,2,0,0,0); /* destination crosses RAM end */
    compare_memctl_dma(board,0x7ffffffc,0x70000a00,2,2,0,0,0); /* 31-bit wrap */
    compare_memctl_dma(board,0xfffffffc,0x70000b00,2,2,0,0,0); /* 32-bit wrap */
    dma_aica_original_get=board->aica_m->get;
    board->aica_m->get=dma_count_aica_get;
    dma_aica_gets=0;
    compare_memctl_dma(board,0x0c800010,0x70000c00,1,1,0,0,0); /* AICA callback path */
    CHECK(dma_aica_gets==2);
    board->aica_m->get=dma_aica_original_get;
    compare_memctl_dma(board,0x70000400,0x40000500,8,8,1,0,0); /* VON4 staging RAM */
    compare_memctl_dma(board,0x70000400,0x40000500,8,3,1,0,0); /* partial device slice */

    /* Aliased host storage must keep the original forward-copy behavior. */
    uint8_t *saved_slave_ram = board->ram_s->ptr;
    board->ram_s->ptr = board->ram_m->ptr + 0x1004;
    dma_prepare(board,0x70002000,0x40001000,8);
    uint32_t first_dma_word = (uint32_t)vk_buffer_get(board->ram_m,4,0x2000);
    compare_memctl_dma(board,0x70002000,0x40001000,8,8,0,0,0);
    for(unsigned i=0;i<8;++i)
        CHECK(vk_buffer_get(board->ram_s,4,0x1000+4*i)==first_dma_word);
    board->ram_s->ptr = saved_slave_ram;

    /* Custom accessors remain observable even when the host pointers overlap. */
    saved_slave_ram = board->ram_s->ptr;
    board->ram_s->ptr = board->ram_m->ptr + 0x1004;
    uint64_t (*overlap_master_get)(vk_buffer_t *,unsigned,uint32_t)=board->ram_m->get;
    void (*overlap_master_put)(vk_buffer_t *,unsigned,uint32_t,uint64_t)=board->ram_m->put;
    uint64_t (*overlap_slave_get)(vk_buffer_t *,unsigned,uint32_t)=board->ram_s->get;
    void (*overlap_slave_put)(vk_buffer_t *,unsigned,uint32_t,uint64_t)=board->ram_s->put;
    board->ram_m->get=dma_custom_get; board->ram_m->put=dma_custom_put;
    board->ram_s->get=dma_custom_get; board->ram_s->put=dma_custom_put;
    compare_memctl_dma(board,0x70002000,0x40001000,8,8,0,8,8);
    board->ram_m->get=overlap_master_get; board->ram_m->put=overlap_master_put;
    board->ram_s->get=overlap_slave_get; board->ram_s->put=overlap_slave_put;
    board->ram_s->ptr = saved_slave_ram;

    /* Custom RAM accessors must remain observable and use the bus loop. */
    uint64_t (*master_get)(vk_buffer_t *,unsigned,uint32_t)=board->ram_m->get;
    void (*master_put)(vk_buffer_t *,unsigned,uint32_t,uint64_t)=board->ram_m->put;
    uint64_t (*slave_get)(vk_buffer_t *,unsigned,uint32_t)=board->ram_s->get;
    void (*slave_put)(vk_buffer_t *,unsigned,uint32_t,uint64_t)=board->ram_s->put;
    board->ram_m->get=dma_custom_get; board->ram_m->put=dma_custom_put;
    board->ram_s->get=dma_custom_get; board->ram_s->put=dma_custom_put;
    compare_memctl_dma(board,0x70003001,0x40004003,5,5,0,5,5);
    board->ram_m->get=master_get; board->ram_m->put=master_put;
    board->ram_s->get=slave_get; board->ram_s->put=slave_put;
    vk_mmap_destroy(&map);
    vk_machine_t *machine=&board->base; vk_machine_destroy(&machine);
    if(failures) return 1;
    puts("Hikaru sound bus CPU timing passed"); return 0;
}
