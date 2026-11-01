// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/mmap.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>

bool hikaru_vendor_test_cached_read(sh4_t *, unsigned, uint32_t, void *);
uint64_t hikaru_vendor_test_read_value(sh4_t *, unsigned, uint32_t);
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
static void compare(sh4_t *cpu, unsigned size, uint32_t address)
{
    uint64_t expected = 0;
    unsigned before_device = device_reads, before_custom = custom_reads;
    CHECK(!vk_mmap_get(cpu->base.mmap, size, address & 0x1fffffff, &expected));
    unsigned expected_device = device_reads - before_device;
    unsigned expected_custom = custom_reads - before_custom;
    before_device = device_reads; before_custom = custom_reads;
    uint64_t actual = hikaru_vendor_test_read_value(cpu, size, address);
    CHECK(expected == actual);
    CHECK(device_reads - before_device == expected_device);
    CHECK(custom_reads - before_custom == expected_custom);
}
int main(void)
{
    vk_machine_t machine = {0};
    vk_device_t device = {0}; device.get = device_get;
    vk_mmap_t *map = vk_mmap_new(&machine);
    vk_buffer_t *ram = vk_buffer_le32_new(256, 0);
    vk_buffer_t *be = vk_buffer_be32_new(256, 0);
    if (!map || !ram || !be) return 2;
    sh4_t cpu = {0}; cpu.base.mmap = map; cpu.base.base.mach = &machine;
    for (unsigned i = 0; i < 256; ++i) ram->ptr[i] = be->ptr[i] = (uint8_t)i;

    CHECK(!vk_mmap_add_dev(map, 0x1040, 0x104f, 15, VK_REGION_R | VK_REGION_SIZE_ALL, &device, "overlay"));
    CHECK(!vk_mmap_add_ram(map, 0x1000, 0x1fff, 255, 0, ram, "RAM"));
    CHECK(!vk_mmap_add_rom(map, 0x2000, 0x20ff, 255, 0, be, "big endian"));
    vk_buffer_t custom = *ram; custom.get = custom_get;
    CHECK(!vk_mmap_add_rom(map, 0x3000, 0x30ff, 255, 0, &custom, "custom reader"));
    CHECK(!vk_mmap_add_rom(map, 0x4000, 0x40ff, 5, 0, ram, "sparse mask"));
    CHECK(!vk_mmap_add_rom(map, 0x6000, 0x60ff, 255, VK_REGION_LOG_R, ram, "logged"));

    for (unsigned size = 1; size <= 8; size *= 2) {
        for (uint32_t addr = 0x1000; addr < 0x1ff9; ++addr)
            if ((addr & 255) <= 256 - size) compare(&cpu, size, addr);
        for (uint32_t addr = 0x2000; addr <= 0x20f8; ++addr) compare(&cpu, size, addr);
        for (uint32_t addr = 0x3000; addr <= 0x30f8; ++addr) compare(&cpu, size, addr);
        for (uint32_t addr = 0x4000; addr <= 0x40f8; ++addr) compare(&cpu, size, addr);
        for (uint32_t addr = 0x6000; addr <= 0x60f8; ++addr) compare(&cpu, size, addr);
        // Ordinary cached/uncached virtual aliases preserve the same byte value.
        compare(&cpu, size, 0xa0001020);
        compare(&cpu, size, 0x80001020);
        uint64_t ignored = 0;
        CHECK(!hikaru_vendor_test_cached_read(&cpu, size, 0x1040, &ignored));
        CHECK(!hikaru_vendor_test_cached_read(&cpu, size, 0x2000, &ignored));
        CHECK(!hikaru_vendor_test_cached_read(&cpu, size, 0x3000, &ignored));
        CHECK(!hikaru_vendor_test_cached_read(&cpu, size, 0x6000, &ignored));
    }
    CHECK(device_reads > 0 && custom_reads > 0);
    // A cached span must remain bounded by the full width, including unaligned reads.
    uint64_t out = 0;
    compare(&cpu, 8, 0x1030);
    CHECK(hikaru_vendor_test_cached_read(&cpu, 8, 0x1038, &out));
    CHECK(!hikaru_vendor_test_cached_read(&cpu, 8, 0x1039, &out));
    CHECK(!hikaru_vendor_test_cached_read(&cpu, 8, 0x0fff, &out));
    compare(&cpu, 4, 0x1020);
    vk_buffer_put(ram, 4, 0x20, 0xdeadbeef);
    CHECK(hikaru_vendor_test_read_value(&cpu, 4, 0x1020) == 0xdeadbeef);
    CHECK(!vk_mmap_add_rom(map, 0x5000, 0x50ff, 255, 0, ram, "later mapping"));
    for (unsigned i = 0; i < 4; ++i) CHECK(!map->data_spans[i].data);
    compare(&cpu, 4, 0x1020);

    // Even a numerically matching span cannot bypass special SH4 address spaces.
    const uint32_t special[] = {0x1f000000, 0xe0000000, 0xe3ffffff, 0xec001000,
                                0xf0000000, 0xf7ffffff, 0xff000000, 0xffffffff};
    for (unsigned index = 0; index < 4; ++index) {
        for (unsigned i = 0; i < NUMELEM(special); ++i) {
            uint32_t physical = special[i] & 0x1fffffff;
            map->data_spans[index] = (vk_mmap_read_span_t){ram->ptr, physical, physical + 7};
            out = 0x1122334455667788ull;
            CHECK(!hikaru_vendor_test_cached_read(&cpu, 1u << index, special[i], &out));
            CHECK(out == 0x1122334455667788ull);
        }
    }
    CHECK(!hikaru_vendor_test_cached_read(&cpu, 0, 0x1000, &out));
    CHECK(!hikaru_vendor_test_cached_read(&cpu, 3, 0x1000, &out));
    CHECK(!hikaru_vendor_test_cached_read(&cpu, 16, 0x1000, &out));
    vk_mmap_destroy(&map);
    vk_buffer_destroy(&ram); vk_buffer_destroy(&be);
    printf("Hikaru inline cached reads: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
