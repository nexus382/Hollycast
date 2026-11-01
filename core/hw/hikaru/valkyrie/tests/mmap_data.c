// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/mmap.h"
#include <stdio.h>

static unsigned checks, failures, device_reads, custom_reads;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #condition); ++failures; } } while (0)

static int device_get(vk_device_t *device, unsigned size, uint32_t address, void *out)
{
    (void)device;
    ++device_reads;
    return set_ptr(out, size, 0x12345678abcdef00ull + address);
}
static uint64_t custom_get(vk_buffer_t *buffer, unsigned size, uint32_t address)
{
    (void)buffer; (void)size;
    ++custom_reads;
    return 0x1122334455667700ull + address;
}
static void compare(vk_mmap_t *map, unsigned size, uint32_t address)
{
    uint64_t expected = 0, actual = 0;
    int a = vk_mmap_get(map, size, address, &expected);
    int b = vk_mmap_get_data_fast(map, size, address, &actual);
    CHECK(a == b);
    if (!a) CHECK(expected == actual);
}
int main(void)
{
    vk_machine_t machine = {0};
    vk_device_t device = {0}; device.get = device_get;
    vk_mmap_t *map = vk_mmap_new(&machine);
    vk_buffer_t *ram = vk_buffer_le32_new(256, 0);
    vk_buffer_t *be = vk_buffer_be32_new(256, 0);
    if (!map || !ram || !be) return 2;
    for (unsigned i = 0; i < 256; ++i) ram->ptr[i] = be->ptr[i] = (uint8_t)i;

    /* An earlier device takes priority over broad direct RAM. */
    CHECK(!vk_mmap_add_dev(map, 0x1040, 0x104f, 15, VK_REGION_R | VK_REGION_SIZE_ALL, &device, "overlay"));
    CHECK(!vk_mmap_add_ram(map, 0x1000, 0x1fff, 255, 0, ram, "RAM"));
    CHECK(!vk_mmap_add_rom(map, 0x2000, 0x20ff, 255, 0, be, "big endian"));
    vk_buffer_t custom = *ram; custom.get = custom_get;
    CHECK(!vk_mmap_add_rom(map, 0x3000, 0x30ff, 255, 0, &custom, "custom reader"));
    CHECK(!vk_mmap_add_rom(map, 0x4000, 0x40ff, 5, 0, ram, "sparse mask"));
    CHECK(!vk_mmap_add_rom(map, 0x6000, 0x60ff, 255, VK_REGION_LOG_R, ram, "logged"));
    CHECK(!vk_mmap_add_rom(map, 0xffffff00u, UINT32_MAX, 255, 0, ram, "top boundary"));

    for (unsigned size = 1; size <= 8; size *= 2) {
        for (uint32_t a = 0x1000; a < 0x1ff9; ++a)
            if ((a & 255) <= 256 - size) compare(map, size, a);
        for (uint32_t a = 0x2000; a <= 0x20f8; ++a) compare(map, size, a);
        for (uint32_t a = 0x3000; a <= 0x30f8; ++a) compare(map, size, a);
        for (uint32_t a = 0x4000; a <= 0x40f8; ++a) compare(map, size, a);
        for (uint32_t a = 0x6000; a <= 0x60f8; ++a) compare(map, size, a);
        compare(map, size, (uint32_t)(0x100000000ull - size));
    }
    CHECK(device_reads > 0 && custom_reads > 0);
    /* Writes are immediately visible through a cached view. */
    uint32_t before = 0, after = 0;
    CHECK(!vk_mmap_get_data_fast(map, 4, 0x1020, &before));
    vk_buffer_put(ram, 4, 0x20, 0xdeadbeef);
    CHECK(!vk_mmap_get_data_fast(map, 4, 0x1020, &after) && after == 0xdeadbeef);
    CHECK(!vk_mmap_add_rom(map, 0x5000, 0x50ff, 255, 0, ram, "later mapping"));
    for (unsigned i = 0; i < 4; ++i) CHECK(!map->data_spans[i].data);
    vk_mmap_destroy(&map);
    vk_buffer_destroy(&ram); vk_buffer_destroy(&be);
    printf("Hikaru data reads: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
