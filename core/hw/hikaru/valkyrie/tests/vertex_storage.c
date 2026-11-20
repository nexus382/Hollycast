// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include <stdio.h>

int hikaru_vendor_test_vertex_storage(void);

int main(void)
{
    const int passed = hikaru_vendor_test_vertex_storage();
    printf("Hikaru vertex storage: %s\n", passed ? "passed" : "failed");
    return passed ? 0 : 1;
}
