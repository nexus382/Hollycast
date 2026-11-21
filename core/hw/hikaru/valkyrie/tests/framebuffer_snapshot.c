// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "mach/hikaru/hikaru-gpu-private.h"
#include "vk/buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_freeze_framebuffer(const hikaru_gpu_t *, vk_buffer_t *);
int hikaru_vendor_test_texture_snapshots(void);

typedef struct {
    const char *name;
    hikaru_layer_t layers[2];
    unsigned front, back, selected;
    bool fallback, unit1;
} Case;

static bool check_snapshot(const Case *test, const vk_buffer_t *source,
                           const vk_buffer_t *snapshot, const vk_buffer_t *previous)
{
    /* Inspect the union of readable byte intervals independently of the
     * production row copies, including every untouched byte and row gap. */
    for (uint32_t offset = 0; offset < snapshot->size; ++offset) {
        bool copied = test->fallback;
        for (unsigned slot = 0; slot < 2 && !copied; ++slot) {
            if (!(test->selected & (1u << slot))) continue;
            const hikaru_layer_t *layer = &test->layers[slot];
            const uint64_t start = (uint64_t)layer->y0 * 4096 + (uint64_t)layer->x0 * 4;
            const unsigned width = layer->format == HIKARU_FORMAT_ABGR1555 ? 1280 : 2560;
            if (offset >= start) {
                const uint64_t relative = offset - start;
                copied = relative / 4096 < 480 && relative % 4096 < width;
            }
        }
        const uint8_t expected = copied ? source->ptr[offset] : previous ? previous->ptr[offset] : 0xa5;
        if (snapshot->ptr[offset] != expected) {
            fprintf(stderr, "framebuffer snapshot %s: byte %u got %02x expected %02x\n",
                    test->name, offset, snapshot->ptr[offset], expected);
            return false;
        }
    }
    return true;
}

int main(void)
{
    static const Case cases[] = {
        {.name="none"},
        {.name="first 1555", .layers={{.format=HIKARU_FORMAT_ABGR1555,.enabled=1}}, .selected=1},
        {.name="second 2A10", .layers={{0},{.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=2},
        {.name="first 2A10 origin", .layers={{.x0=7,.y0=3,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=1},
        {.name="second 1555 origin", .layers={{0},{.x0=11,.y0=9,.format=HIKARU_FORMAT_ABGR1555,.enabled=1}}, .selected=2},
        {.name="overlapping formats", .layers={{.x0=7,.y0=3,.format=HIKARU_FORMAT_ABGR1555,.enabled=1},
            {.x0=19,.y0=7,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=3},
        {.name="same origin", .layers={{.format=HIKARU_FORMAT_ABGR1555,.enabled=1},
            {.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=3},
        {.name="front first disabled", .layers={{.format=HIKARU_FORMAT_ABGR1555}}, .front=2,.selected=1},
        {.name="back first disabled", .layers={{.format=HIKARU_FORMAT_A2BGR10}}, .back=2,.selected=1},
        {.name="front second disabled", .layers={{0},{.format=HIKARU_FORMAT_ABGR1555}}, .front=3,.selected=2},
        {.name="back second disabled", .layers={{0},{.format=HIKARU_FORMAT_A2BGR10}}, .back=3,.selected=2},
        {.name="unit one ignored", .unit1=true},
        {.name="1555 exact end", .layers={{.x0=704,.y0=1568,.format=HIKARU_FORMAT_ABGR1555,.enabled=1}}, .selected=1},
        {.name="2A10 exact end", .layers={{.x0=384,.y0=1568,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=1},
        {.name="past end", .layers={{.x0=385,.y0=1568,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .fallback=true},
        {.name="overflow origin", .layers={{.x0=UINT32_MAX,.y0=UINT32_MAX,.enabled=1}}, .fallback=true},
        {.name="unsupported selected", .layers={{.format=UINT32_MAX,.enabled=1}}, .fallback=true},
        {.name="invalid second", .layers={{.format=HIKARU_FORMAT_ABGR1555,.enabled=1},
            {.y0=2048,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .fallback=true},
        {.name="unsupported unselected", .layers={{.format=UINT32_MAX}}},
    };
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    vk_buffer_t *source = vk_buffer_le32_new(8u * 1024 * 1024, 0);
    vk_buffer_t *first = vk_buffer_le32_new(8u * 1024 * 1024, 0);
    vk_buffer_t *second = vk_buffer_le32_new(8u * 1024 * 1024, 0);
    if (!gpu || !source || !first || !second) abort();
    gpu->fb = source;
    unsigned failures = !hikaru_vendor_test_texture_snapshots();
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        const Case *test = &cases[n];
        for (uint32_t offset = 0; offset < source->size; ++offset)
            source->ptr[offset] = (uint8_t)(offset * 13u + (offset >> 12) + n);
        memset(first->ptr, 0xa5, first->size);
        memcpy(gpu->state.layers.layer[0], test->layers, sizeof(test->layers));
        gpu->state.layers.layer[1][0].enabled = test->unit1;
        gpu->fb_config.num_1 = test->front;
        gpu->fb_config.num_2 = test->back;
        hikaru_vendor_test_freeze_framebuffer(gpu, first);
        failures += !check_snapshot(test, source, first, NULL);
        /* A reused destination must refresh newly selected regions. Keep a
         * second frozen image while changing all live pixels and metadata. */
        memset(second->ptr, 0xa5, second->size);
        hikaru_vendor_test_freeze_framebuffer(gpu, second);
        memset(source->ptr, 0x3c, source->size);
        memset(gpu->state.layers.layer, 0, sizeof(gpu->state.layers.layer));
        gpu->fb_config.num_1 = gpu->fb_config.num_2 = 0;
        hikaru_vendor_test_freeze_framebuffer(gpu, first);
        failures += memcmp(first->ptr, second->ptr, first->size) != 0;
        const Case reused = {
            .name="reused second layer", .layers={{0},
                {.x0=23,.y0=511,.format=HIKARU_FORMAT_A2BGR10,.enabled=1}}, .selected=2
        };
        gpu->state.layers.layer[0][1] = reused.layers[1];
        hikaru_vendor_test_freeze_framebuffer(gpu, first);
        failures += !check_snapshot(&reused, source, first, second);
        /* The old snapshot remains independent after a later freeze. */
        for (uint32_t offset = 0; offset < source->size; ++offset)
            source->ptr[offset] = (uint8_t)(offset * 13u + (offset >> 12) + n);
        failures += !check_snapshot(test, source, second, NULL);
    }
    free(gpu);
    vk_buffer_destroy(&source);
    vk_buffer_destroy(&first);
    vk_buffer_destroy(&second);
    printf("Hikaru framebuffer snapshot: %zu cases, %u failures\n",
           sizeof(cases) / sizeof(cases[0]), failures);
    return failures ? 1 : 0;
}
