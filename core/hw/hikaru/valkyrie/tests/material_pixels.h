// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Inputs for a hidden-window probe through the production material copy,
 * shader cache, texture combination and per-mesh blend setup. */
typedef struct {
    const char *name;
    uint32_t flags, texture_head;
    uint8_t diffuse[3], color1[3], texel[4];
    uint16_t color691[3];
    unsigned polytype, blend_mode;
    uint8_t polygon_alpha, vertex_alpha;
    bool blend;
    float background[4];
    uint8_t expected[4];
    bool raw_material; /* inspect color/alpha before pass coverage */
    uint8_t pattern;
    uint8_t *coverage_pixels; /* optional 4 x 4 RGBA output from the real draw */
} hikaru_material_pixel_case_t;
int hikaru_vendor_test_material_case_pixels(const hikaru_material_pixel_case_t *, uint8_t[4]);
