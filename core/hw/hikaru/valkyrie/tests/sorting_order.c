// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include <stdio.h>

int hikaru_vendor_test_triangle_sort_order(void);
int hikaru_vendor_test_triangle_batch_state(void);

int main(void)
{
    int failure = hikaru_vendor_test_triangle_sort_order();
    if (failure) {
        fprintf(stderr, "Hikaru triangle sort ordering failed at renderer line %d\n", failure);
        return 1;
    }
    failure = hikaru_vendor_test_triangle_batch_state();
    if (failure) {
        fprintf(stderr, "Hikaru triangle batch state failed at renderer line %d\n", failure);
        return 1;
    }
    puts("Hikaru stable triangle ordering, batch state and retained workspace checks passed");
    return 0;
}
