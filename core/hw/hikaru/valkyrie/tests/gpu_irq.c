// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "engine_internal.h"
#include "mach/hikaru/hikaru-gpu.h"
#include "mach/hikaru/hikaru-gpu-private.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>
#include <stdlib.h>

static vk_irq_state_t irq;
static bool sound_enabled;
static int set_irq(vk_cpu_t *cpu, unsigned source, vk_irq_state_t state)
{
    (void)cpu;
    if (source != SH4_IESOURCE_IRL2) return -1;
    irq = state;
    return 0;
}
static int set_sound_irq(void *opaque, int pending)
{
    (void)opaque;
    return pending && sound_enabled;
}
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "GPU IRQ check failed at line %d: %s\n", __LINE__, #condition); result = 1; goto cleanup; } } while (0)
#define PUT(address, value) CHECK(vk_device_put(device, 4, address, value) == 0)
#define GET(address, expected) do { uint32_t value = 0; CHECK(vk_device_get(device, 4, address, &value) == 0); CHECK(value == (expected)); } while (0)
int main(void)
{
    hikaru_t board = {0};
    HikaruEngine engine = {0};
    vk_cpu_t cpu = {0};
    vk_buffer_t *texram[2] = {0};
    uint8_t *payload = NULL;
    int result = 0;
    board.base.host = &engine;
    board.sh_m = board.sh_current = &cpu;
    board.porta_m = 0xffff;
    cpu.set_irq_state = set_irq;
    engine.machine = &board.base;
    engine.sound_attached = true;
    engine.mie.set_sound_irq = set_sound_irq;
    vk_device_t *device = hikaru_gpu_new(&board.base, NULL, NULL, texram, NULL);
    CHECK(device != NULL);
    board.gpu = device;
    hikaru_gpu_t *gpu = (hikaru_gpu_t *)device;

    // The system library masks errors with 0x72 and enables completion IRQs.
    PUT(0x15000084, 0x72);
    hikaru_gpu_raise_irq(gpu, GPU15_IRQ_CMD_ANALYSIS_END, 0);
    GET(0x15000088, GPU15_IRQ_CMD_ANALYSIS_END);
    CHECK(irq == VK_IRQ_STATE_RAISED && !(board.porta_m & 0x40));
    PUT(0x15000084, 0x76);
    CHECK(irq == VK_IRQ_STATE_CLEAR && (board.porta_m & 0x40));
    GET(0x15000088, GPU15_IRQ_CMD_ANALYSIS_END); // Masking retains the cause.
    PUT(0x15000084, 0x72);
    CHECK(irq == VK_IRQ_STATE_RAISED);
    PUT(0x15000088, 0x72); // Status uses AND-on-write acknowledgement.
    GET(0x15000088, 0);
    CHECK(irq == VK_IRQ_STATE_CLEAR);

    // The display summary follows all four causes, including the last ACK.
    hikaru_gpu_raise_irq(gpu, 0, GPU1A_IRQ_VBLANK | GPU1A_IRQ_PLOT_END);
    GET(0x15000088, GPU15_IRQ_GPU1A);
    CHECK(irq == VK_IRQ_STATE_RAISED);
    PUT(0x15000088, 0x7f); // A live display cause cannot lose its summary.
    GET(0x15000088, GPU15_IRQ_GPU1A);
    PUT(0x1a00000c, 1);
    CHECK(irq == VK_IRQ_STATE_RAISED);
    PUT(0x1a000010, 1);
    GET(0x15000088, 0);
    CHECK(irq == VK_IRQ_STATE_CLEAR);

    // Frame boundaries must not manufacture the error PHARRIER recovers from.
    for (unsigned frame = 0; frame < 120; ++frame) hikaru_gpu_vblank_in(device);
    GET(0x15000088, 0);
    CHECK(irq == VK_IRQ_STATE_CLEAR);

    // GPU acknowledgement preserves an enabled AICA source on shared IRL2.
    board.porta_m &= ~0x80;
    sound_enabled = true;
    hikaru_gpu_raise_irq(gpu, GPU15_IRQ_IDMA_END, 0);
    PUT(0x15000088, 0);
    CHECK(irq == VK_IRQ_STATE_RAISED && (board.porta_m & 0x40));
    sound_enabled = false;
    PUT(0x15000084, 0x72);
    CHECK(irq == VK_IRQ_STATE_CLEAR); // A masked raw AICA GPIO is insufficient.
    board.porta_m |= 0x80;

    // Keep the native payload layout while removing the old synthetic error.
    PUT(0x15000084, 0xff);
    hikaru_gpu_raise_irq(gpu, GPU15_IRQ_ERROR, 0);
    vk_state_t count = {0};
    count.capacity = SIZE_MAX;
    CHECK(device->save_state(device, &count) == 0 && count.offset > 0);
    payload = malloc(count.offset);
    CHECK(payload != NULL);
    vk_state_t state = {0};
    state.data = payload;
    state.capacity = count.offset;
    CHECK(device->save_state(device, &state) == 0 && state.offset == count.offset);
    state.mode = VK_STATE_LOAD;
    state.offset = 0;
    CHECK(device->load_state(device, &state) == 0 && state.offset == count.offset);
    GET(0x15000088, 0);
    CHECK(irq == VK_IRQ_STATE_CLEAR);
cleanup:
    free(payload);
    vk_device_destroy(&device);
    vk_vector_destroy(&board.base.devices);
    if (!result) puts("GPU mask, completion, display summary, shared AICA and legacy state checks passed");
    return result;
}
