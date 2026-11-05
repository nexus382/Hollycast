// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "vk/renderer.h"
#include <math.h>
#include <stdio.h>
static int close_enough(float a, float b) { return fabsf(a-b) < 0.00001f; }
int hikaru_vendor_test_triangle_winding(unsigned winding, unsigned nocull);
int main(void)
{
    mtx4x4f_t transform = {{2,0,0,0},{0,3,0,0},{0,0,4,0},{5,6,7,1}};
    mtx3x3f_t normal;
    if (!vk_renderer_compute_normal_matrix(normal, transform) ||
        !close_enough(normal[0][0], 0.5f) || !close_enough(normal[1][1], 1.0f/3) ||
        !close_enough(normal[2][2], 0.25f) || normal[0][1] != 0) return 1;
    transform[2][2] = 0;
    for (unsigned i = 0; i < 3; ++i) for (unsigned j = 0; j < 3; ++j) normal[i][j] = 12345;
    if (vk_renderer_compute_normal_matrix(normal, transform) ||
        normal[0][0] != 0 || normal[1][1] != 0 || normal[2][2] != 6 || normal[0][1] != 0) return 2;
    mtx4x4f_t rotated = {{0,2,0,0},{-3,0,0,0},{0,0,4,0},{0,0,0,1}};
    if (!vk_renderer_compute_normal_matrix(normal, rotated) ||
        !close_enough(normal[0][1], 0.5f) || !close_enough(normal[1][0], -1.0f/3) ||
        !close_enough(normal[2][2], 0.25f)) return 3;
    for (unsigned winding = 0; winding < 2; ++winding)
        for (unsigned nocull = 0; nocull < 2; ++nocull)
            if (!hikaru_vendor_test_triangle_winding(winding, nocull)) {
                fprintf(stderr, "triangle normal disagrees with winding=%u nocull=%u\n", winding, nocull);
                return 4;
            }
    puts("Hikaru normal matrix and triangle winding checks passed");
    return 0;
}
