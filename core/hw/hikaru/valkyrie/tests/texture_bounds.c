// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/buffer.h"
#include "engine.h"
#include "engine_internal.h"
#include "vk/cpu.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
void *hikaru_vendor_test_decode_abgr1111(vk_buffer_t*, uint32_t, uint32_t, uint32_t, uint32_t);
int hikaru_vendor_test_texture_span(const vk_buffer_t*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
int hikaru_vendor_test_layer_span(const vk_buffer_t*, uint32_t, uint32_t, unsigned);
void *hikaru_vendor_test_decode_layer_abgr1555(const vk_buffer_t*, uint32_t, uint32_t);
int hikaru_vendor_test_fb_dma(vk_buffer_t*, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned);
uint32_t hikaru_vendor_test_texture_wrap(int, int);
int hikaru_vendor_test_same_texture_state(uint32_t, uint32_t);
int hikaru_vendor_test_texture_wrap_changed(uint32_t, uint32_t);
int hikaru_vendor_test_texture_overlap(uint32_t, uint32_t, uint32_t, uint32_t);
int hikaru_vendor_test_viewport_origin(void);
int hikaru_vendor_test_gpu_capability_read(void);
int hikaru_vendor_test_cpu_texram_dirty(void);
int hikaru_vendor_test_modelview_pair(void);
int hikaru_vendor_test_vertex_matrix_selector(void);
int hikaru_vendor_test_uv_packets(uint32_t, uint32_t, uint32_t, const uint32_t[8], float[3][2], uint16_t[2]);
uint64_t hikaru_vendor_test_rombd_read(vk_buffer_t*, uint32_t, unsigned);
int hikaru_vendor_test_protection_bus(const HikaruEngineProtection*, int, unsigned, uint32_t, uint64_t*);
static uint32_t address_seen; static unsigned size_seen; static uint64_t written;
static uint64_t custom_get(vk_buffer_t *buf, unsigned size, uint32_t offset) { (void)buf; (void)size; (void)offset; return 0; }
static void protect_reset(void *p) { (void)p; }
static int protect_read(void *p, unsigned size, uint32_t address, uint64_t *value) {
    (void)p; address_seen = address; size_seen = size; *value = 0x12345678; return address == 0x0a000004 ? 0 : -1;
}
static int protect_write(void *p, unsigned size, uint32_t address, uint64_t value) {
    (void)p; address_seen = address; size_seen = size; written = value; return address == 0x0a010000 ? 0 : -1;
}

static uint16_t dma_source_pixel(unsigned x, unsigned y)
{
    return (uint16_t)(0x8000 | (y << 8) | x);
}

static unsigned test_fb_dma_lanes(vk_buffer_t *fb)
{
    unsigned errors = 0;
    /* Logical ABGR1555 pixels occupy the opposite halfword lane of their
     * byte address. Exercise the real register-triggered DMA, then inspect
     * the renderer's decoded layer, including every adjacent sentinel. */
    for (unsigned src_parity = 0; src_parity < 2; ++src_parity)
        for (unsigned dst_parity = 0; dst_parity < 2; ++dst_parity)
            for (unsigned width = 1; width <= 7; ++width) {
                const unsigned src_x = 16 + src_parity, src_y = 600;
                const unsigned dst_x = 32 + dst_parity, dst_y = 16, height = 3;
                memset(fb->ptr, 0x5a, fb->size);
                for (unsigned row = 0; row < height; ++row)
                    for (unsigned col = 0; col < width + 4; ++col) {
                        unsigned x = src_x - 2 + col;
                        vk_buffer_put(fb, 2, ((src_y + row) * 4096 + x * 2) ^ 2,
                                      dma_source_pixel(x, row));
                    }
                if (!hikaru_vendor_test_fb_dma(fb, src_x, src_y, dst_x, dst_y, width, height)) {
                    ++errors;
                    continue;
                }
                uint16_t *layer = hikaru_vendor_test_decode_layer_abgr1555(fb, 0, 0);
                if (!layer) { ++errors; continue; }
                int failed = 0;
                for (unsigned row = dst_y - 1; row <= dst_y + height; ++row)
                    for (unsigned col = dst_x - 2; col < dst_x + width + 2; ++col) {
                        uint16_t expected = row >= dst_y && row < dst_y + height &&
                                            col >= dst_x && col < dst_x + width
                            ? dma_source_pixel(src_x + col - dst_x, row - dst_y) : 0x5a5a;
                        uint16_t actual = layer[row * 640 + col];
                        if (actual != expected && !failed) {
                            fprintf(stderr, "FB DMA src parity %u dst parity %u width %u: pixel (%u,%u) got %04x expected %04x\n",
                                    src_parity, dst_parity, width, col, row, actual, expected);
                            failed = 1;
                        }
                    }
                errors += failed;
                free(layer);
            }
    return errors;
}

static unsigned test_fb_dma_overlap(vk_buffer_t *fb)
{
    static const struct {
        const char *name;
        unsigned src_x, src_y, dst_x, dst_y;
    } cases[] = {
        {"horizontal forward", 20, 20, 21, 20},
        {"horizontal backward", 21, 20, 20, 20},
        {"vertical forward", 21, 20, 21, 21},
        {"vertical backward", 21, 21, 21, 20},
    };
    unsigned errors = 0;
    /* Hardware overlap behavior is unverified. Preserve the existing model:
     * each pixel is read then written, in ascending row/column order. */
    for (unsigned n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
        uint16_t expected[12][12];
        memset(fb->ptr, 0x5a, fb->size);
        for (unsigned row = 0; row < 12; ++row)
            for (unsigned col = 0; col < 12; ++col) {
                expected[row][col] = dma_source_pixel(col, row);
                vk_buffer_put(fb, 2, ((16 + row) * 4096 + (16 + col) * 2) ^ 2,
                              expected[row][col]);
            }
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned col = 0; col < 5; ++col)
                expected[cases[n].dst_y - 16 + row][cases[n].dst_x - 16 + col] =
                    expected[cases[n].src_y - 16 + row][cases[n].src_x - 16 + col];
        if (!hikaru_vendor_test_fb_dma(fb, cases[n].src_x, cases[n].src_y,
                                      cases[n].dst_x, cases[n].dst_y, 5, 3)) {
            ++errors;
            continue;
        }
        uint16_t *layer = hikaru_vendor_test_decode_layer_abgr1555(fb, 0, 0);
        if (!layer) { ++errors; continue; }
        int failed = 0;
        for (unsigned row = 0; row < 12; ++row)
            for (unsigned col = 0; col < 12; ++col) {
                uint16_t actual = layer[(16 + row) * 640 + 16 + col];
                if (actual != expected[row][col] && !failed) {
                    fprintf(stderr, "FB DMA %s overlap: pixel (%u,%u) got %04x expected %04x\n",
                            cases[n].name, 16 + col, 16 + row, actual, expected[row][col]);
                    failed = 1;
                }
            }
        errors += failed;
        free(layer);
    }
    return errors;
}

static unsigned test_uv_packets(void)
{
    /* Brave's captured 000206D1/00880440 preceded this 1BC packet. Its raw
     * (0,32) UV must normalize to (0,1), rather than the old (2.125,3.125).
     * Other real UV packet forms cover signed coordinates and 1111 scaling. */
    static const struct {
        const char *name;
        uint32_t packet[8];
        unsigned count;
        float pixels[3][2];
    } cases[] = {
        {"captured 1BC", {0xff00c1bc,0,0,0xbf800000,0x02000000,0,0,0x3f800000},
         1, {{0,32}}},
        {"signed 158", {0x00000158,0x80007fff}, 1, {{2047.9375f,-2048}}},
        {"signed 0E8", {0x000000e8,0x0100fe00,0x80007fff,0x0000ffff},
         3, {{-32,16},{2047.9375f,-2048},{-0.0625f,0}}},
    };
    static const unsigned formats[] = {0,1,2,4};
    static const uint32_t offsets[] = {0x00880440,0x7fff4000};
    unsigned errors = 0, checks = 0;
    for (unsigned f = 0; f < sizeof(formats)/sizeof(formats[0]); ++f)
        for (unsigned mode = 0; mode < 4; ++mode)
            for (unsigned o = 0; o < sizeof(offsets)/sizeof(offsets[0]); ++o)
                for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); ++c) {
                    float actual[3][2] = {{0}};
                    uint16_t decoded[2] = {0};
                    uint32_t texhead = (0x44cc02c1u & ~(7u << 26)) | (formats[f] << 26);
                    int failed = !hikaru_vendor_test_uv_packets((mode << 16) | 0x6d1,
                        offsets[o], texhead, cases[c].packet, actual, decoded);
                    const float width = formats[f] == 2 ? 512.0f : 256.0f;
                    const float height = formats[f] == 2 ? 64.0f : 32.0f;
                    for (unsigned i = 0; i < cases[c].count; ++i) {
                        failed |= fabsf(actual[i][0] - cases[c].pixels[i][0] / width) > 0.000001f;
                        failed |= fabsf(actual[i][1] - cases[c].pixels[i][1] / height) > 0.000001f;
                    }
                    const uint16_t dx = mode == 1 || mode == 2 ? offsets[o] & 0xffff : 0;
                    const uint16_t dy = mode == 1 || mode == 2 ? offsets[o] >> 16 : 0;
                    failed |= decoded[0] != dx || decoded[1] != dy;
                    if (failed && errors < 8)
                        fprintf(stderr, "UV %s format%u mode%u offset%08x: got %.6f,%.6f\n",
                                cases[c].name, formats[f], mode, offsets[o], actual[0][0], actual[0][1]);
                    errors += failed;
                    ++checks;
                }
    fprintf(stderr, "CP packet UV normalization: %u cases, %u failures\n", checks, errors);
    return errors;
}

int main(void)
{
    unsigned errors = 0;
    errors += test_uv_packets();
    if (!hikaru_vendor_test_modelview_pair()) ++errors;
    if (!hikaru_vendor_test_vertex_matrix_selector()) ++errors;
    if (!hikaru_vendor_test_cpu_texram_dirty()) ++errors;
    unsigned evaluated = 0;
    vk_verbosity = 0;
    VK_MACH_LOG((++evaluated, (vk_machine_t *)NULL), "ignored %u", ++evaluated);
    VK_CPU_LOG((++evaluated, (vk_cpu_t *)NULL), "ignored %u", ++evaluated);
    if (evaluated) ++errors;
    HikaruEngine *diagnostics = calloc(1, sizeof(*diagnostics));
    if (!diagnostics) return 1;
    if (vk_host_diagnostic(diagnostics, 1, 0xff000084) != 1 ||
        vk_host_diagnostic(diagnostics, 1, 0xff000084) != 2 ||
        vk_host_diagnostic(diagnostics, 1, 0xff000084) != 0 ||
        vk_host_diagnostic(diagnostics, 1, 0xff000084) != 4 ||
        vk_host_diagnostic(diagnostics, 2, 0xff000084) != 1 ||
        vk_host_diagnostic(diagnostics, 1, 0xff100008) != 1) ++errors;
    free(diagnostics);
    vk_buffer_t *tex = vk_buffer_le32_new(4u*1024*1024, 0);
    vk_buffer_t *fb = vk_buffer_le32_new(8u*1024*1024, 0);
    if (!tex || !fb) return 1;
    if (!vk_buffer_is_plain_le(tex) || vk_buffer_is_plain_le(NULL)) ++errors;
    vk_buffer_t *big_endian = vk_buffer_be32_new(16, 0);
    if (!big_endian || vk_buffer_is_plain_le(big_endian)) ++errors;
    vk_buffer_destroy(&big_endian);
    uint64_t (*original_get)(vk_buffer_t *, unsigned, uint32_t) = tex->get;
    tex->get = custom_get;
    if (vk_buffer_is_plain_le(tex)) ++errors;
    tex->get = original_get;
    for (unsigned width = 1; width <= 8; width *= 2) {
        const uint64_t mask = width == 8 ? UINT64_MAX : (UINT64_C(1) << (width * 8)) - 1;
        for (unsigned offset = 1; offset < 17; ++offset) {
            const uint64_t input = UINT64_C(0x0123456789abcdef) & mask;
            vk_buffer_put(tex, width, offset, input);
            if (vk_buffer_get(tex, width, offset) != input) ++errors;
            for (unsigned byte = 0; byte < width; ++byte)
                if (tex->ptr[offset + byte] != (uint8_t)(input >> (byte * 8))) ++errors;
        }
    }
    tex->ptr[0] = 0x10; tex->ptr[1] = 0x32; tex->ptr[2] = 0x54; tex->ptr[3] = 0x76;
    tex->ptr[4u*1024*1024-1] = 0xab;
    if (hikaru_vendor_test_rombd_read(tex, 0, 4) != 0x76543210 ||
        hikaru_vendor_test_rombd_read(tex, 4u*1024*1024-1, 4) != 0xab ||
        hikaru_vendor_test_rombd_read(tex, 4u*1024*1024, 8) != 0 ||
        hikaru_vendor_test_rombd_read(tex, UINT32_MAX, 8) != 0) ++errors;
    HikaruEngineProtection protection = {NULL, protect_reset, protect_read, protect_write};
    uint64_t value = 0;
    if (hikaru_vendor_test_protection_bus(&protection, 0, 4, 0x0a000004, &value) ||
        value != 0x12345678 || address_seen != 0x0a000004 || size_seen != 4) ++errors;
    value = 0xabcd;
    if (hikaru_vendor_test_protection_bus(&protection, 1, 2, 0x0a010000, &value) ||
        written != 0xabcd || address_seen != 0x0a010000 || size_seen != 2) ++errors;
    if (hikaru_vendor_test_protection_bus(&protection, 0, 4, 0x0a020000, &value) == 0) ++errors;
    if (!hikaru_vendor_test_viewport_origin() || !hikaru_vendor_test_gpu_capability_read()) ++errors;
    uint16_t *pixels = hikaru_vendor_test_decode_abgr1111(tex, 2, 2, 0, 0);
    if (!pixels || pixels[0] != 0xff00 || pixels[1] != 0x0f00 || pixels[2] != 0xfff0 || pixels[3] != 0x0ff0 ||
        pixels[4] != 0xf000 || pixels[5] != 0 || pixels[6] != 0xf0f0 || pixels[7] != 0x00f0) ++errors;
    free(pixels);
    /* Nonzero slot uses 16-bit storage coordinates, and expands both axes. */
    tex->ptr[32] = 0x10; tex->ptr[33] = 0x32; tex->ptr[34] = 0x54; tex->ptr[35] = 0x76;
    pixels = hikaru_vendor_test_decode_abgr1111(tex, 2, 1, 16, 0);
    if (!pixels || pixels[0] != 0xff00 || pixels[1] != 0x0f00 || pixels[2] != 0xfff0 || pixels[3] != 0x0ff0) ++errors;
    free(pixels);
    pixels = hikaru_vendor_test_decode_abgr1111(tex, 1, 1, 2046, 1023);
    if (!pixels) ++errors;
    else { volatile uint16_t last = pixels[3]; (void)last; }
    free(pixels);
    if (!hikaru_vendor_test_texture_span(tex, 0, 16, 16, 2032, 1008)) ++errors;
    if (hikaru_vendor_test_texture_span(tex, 0, 16, 16, 2033, 1008)) ++errors;
    if (hikaru_vendor_test_texture_span(tex, 0, 16, 16, 2032, 1009)) ++errors;
    if (!hikaru_vendor_test_texture_span(tex, 2, 2, 2, 2046, 1022)) ++errors;
    if (hikaru_vendor_test_texture_span(tex, 2, 2, 2, 2047, 1022)) ++errors;
    if (!hikaru_vendor_test_layer_span(fb, 0, 1568, 640*4)) ++errors;
    if (hikaru_vendor_test_layer_span(fb, 1023, 2047, 640*4)) ++errors;
    if (hikaru_vendor_test_layer_span(fb, UINT32_MAX, UINT32_MAX, 640*4)) ++errors;
    /* Last valid 480-row 16-bit layer, with distinct pixels in both rows. */
    const size_t layer_start = (size_t)1568 * 4096;
    const size_t layer_last = (size_t)2047 * 4096 + 1276;
    fb->ptr[layer_start] = 0x11; fb->ptr[layer_start+1] = 0x22;
    fb->ptr[layer_start+2] = 0x33; fb->ptr[layer_start+3] = 0x44;
    fb->ptr[layer_last] = 0x55; fb->ptr[layer_last+1] = 0x66;
    fb->ptr[layer_last+2] = 0x77; fb->ptr[layer_last+3] = 0x88;
    uint8_t *layer = hikaru_vendor_test_decode_layer_abgr1555(fb, 0, 1568);
    const size_t out_last = (size_t)640 * 480 * 2 - 4;
    if (!layer || layer[0] != 0x33 || layer[1] != 0x44 || layer[2] != 0x11 || layer[3] != 0x22 ||
        layer[out_last] != 0x77 || layer[out_last+1] != 0x88 || layer[out_last+2] != 0x55 || layer[out_last+3] != 0x66) ++errors;
    free(layer);
    if (hikaru_vendor_test_decode_layer_abgr1555(fb, 0, 1569)) ++errors;
    errors += test_fb_dma_lanes(fb);
    errors += test_fb_dma_overlap(fb);
    if (hikaru_vendor_test_texture_wrap(0, 0) != GL_CLAMP_TO_EDGE ||
        hikaru_vendor_test_texture_wrap(0, 1) != GL_CLAMP_TO_EDGE ||
        hikaru_vendor_test_texture_wrap(1, 0) != GL_REPEAT ||
        hikaru_vendor_test_texture_wrap(1, 1) != GL_MIRRORED_REPEAT) ++errors;
    if (!hikaru_vendor_test_same_texture_state(0, 0) ||
        !hikaru_vendor_test_same_texture_state(0, 1u << 22) ||
        !hikaru_vendor_test_same_texture_state(0, 1u << 23) ||
        !hikaru_vendor_test_same_texture_state(0, 1u << 24) ||
        !hikaru_vendor_test_same_texture_state(0, 1u << 25) ||
        hikaru_vendor_test_texture_wrap_changed(0, 0) ||
        !hikaru_vendor_test_texture_wrap_changed(0, 1u << 22) ||
        !hikaru_vendor_test_texture_wrap_changed(0, 1u << 23) ||
        !hikaru_vendor_test_texture_wrap_changed(0, 1u << 24) ||
        !hikaru_vendor_test_texture_wrap_changed(0, 1u << 25)) ++errors;
    /* Same-bank base overlap, disjoint transfer, and an alternating-bank
     * write which touches only a cached texture's first mip level. */
    const uint32_t slot0 = 0xc0800000u;
    const uint32_t far_slot = 0xc8900000u;
    const uint32_t mip_slot_other_bank = 0xe0c01000u;
    if (!hikaru_vendor_test_texture_overlap(0, slot0, 0, slot0) ||
        hikaru_vendor_test_texture_overlap(0, slot0, 0, far_slot) ||
        !hikaru_vendor_test_texture_overlap(0, slot0, 0, mip_slot_other_bank) ||
        !hikaru_vendor_test_texture_overlap(1u << 16, slot0, 0, 0xc0810000u) ||
        !hikaru_vendor_test_texture_overlap((2u << 26) | (1u << 16), slot0, 0, 0xc0810000u)) ++errors;
    vk_buffer_destroy(&tex); vk_buffer_destroy(&fb);
    printf("Hikaru renderer bounds: %u failures\n", errors);
    return errors ? 1 : 0;
}
