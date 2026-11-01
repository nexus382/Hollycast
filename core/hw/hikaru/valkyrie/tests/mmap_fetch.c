// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/mmap.h"
#include <stdio.h>

static unsigned checks, failures, device_reads, custom_reads;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #condition); ++failures; } } while (0)

static int device_get(vk_device_t *device, unsigned size, uint32_t address, void *value)
{
    (void)device;
    ++device_reads;
    return set_ptr(value, size, 0xa000 | (address & 0xff));
}
static uint64_t custom_get(vk_buffer_t *buffer, unsigned size, uint32_t address)
{
    (void)buffer; (void)size; (void)address;
    ++custom_reads;
    return 0xbabe;
}
static void compare(vk_mmap_t *map, uint32_t address)
{
    uint16_t generic = 0, fast = 0;
    int expected = vk_mmap_get(map, 2, address, &generic);
    int actual = vk_mmap_fetch16(map, address, &fast);
    CHECK(actual == expected);
    if (!expected) CHECK(fast == generic);
}

int main(void)
{
    vk_machine_t machine = {0};
    vk_device_t device = {0}; device.get = device_get;
    vk_buffer_t *ram = vk_buffer_le32_new(256, 0);
    vk_buffer_t *rom = vk_buffer_le32_new(256, 0);
    vk_buffer_t *be = vk_buffer_be32_new(256, 0);
    vk_mmap_t *map = vk_mmap_new(&machine);
    if (!ram || !rom || !be || !map) return 2;
    for (unsigned i = 0; i < 256; ++i) {
        ram->ptr[i] = (uint8_t)i; rom->ptr[i] = 0x5a; be->ptr[i] = (uint8_t)i;
    }
    /* Earlier readable devices and ROM overlays take priority over broad RAM.
     * A write-only region must not shadow reads. */
    CHECK(!vk_mmap_add_dev(map, 0x1040, 0x104f, 15, VK_REGION_R | VK_REGION_SIZE_16, &device, "device"));
    CHECK(!vk_mmap_add_dev(map, 0x1060, 0x106f, 15, VK_REGION_W | VK_REGION_SIZE_16, &device, "write only"));
    CHECK(!vk_mmap_add_rom(map, 0x1080, 0x108f, 255, 0, rom, "overlay ROM"));
    CHECK(!vk_mmap_add_dev(map, 0x10c0, 0x10cf, 15, VK_REGION_R | VK_REGION_SIZE_8, &device, "8-bit-only overlay"));
    CHECK(!vk_mmap_add_ram(map, 0x1000, 0x10ff, 255, 0, ram, "RAM"));
    vk_mmap_read_span_t span;
    CHECK(!vk_mmap_get_read_span(map, 2, 0x1020, &span));
    CHECK(span.first == 0x1000 && span.last == 0x103f);
    CHECK(!vk_mmap_get_read_span(map, 2, 0x1050, &span));
    CHECK(span.first == 0x1050 && span.last == 0x107f);
    CHECK(!vk_mmap_get_read_span(map, 2, 0x10b0, &span));
    CHECK(span.first == 0x1090 && span.last == 0x10bf);
    for (uint32_t address = 0x1000; address < 0x10ff; ++address) compare(map, address);
    CHECK(device_reads == 32); /* Two reads for every 16-bit device start. */
    CHECK(vk_mmap_get_read_span(map, 2, 0x1040, &span) == -1 && !span.data);
    CHECK(vk_mmap_get_read_span(map, 2, 0x10c0, &span) == -1 && !span.data);
    CHECK(vk_mmap_get_read_span(map, 2, 0x103f, &span) == -1); /* Crosses overlay edge. */
    CHECK(vk_mmap_get_read_span(map, 2, 0x10ff, &span) == -1); /* Crosses buffer end. */
    uint16_t instruction;
    CHECK(!vk_mmap_fetch16(map, 0x1020, &instruction));
    CHECK(instruction == 0x2120 && map->fetch_span.data);
    CHECK(!vk_mmap_put(map, 2, 0x1020, 0xe007));
    CHECK(!vk_mmap_fetch16(map, 0x1020, &instruction) && instruction == 0xe007);
    ram->ptr[0x20] = 0x09; ram->ptr[0x21] = 0x00;
    CHECK(!vk_mmap_fetch16(map, 0x1020, &instruction) && instruction == 0x0009);
    CHECK(!vk_mmap_add_rom(map, 0x2000, 0x20ff, 255, VK_REGION_LOG_R, rom, "logged ROM"));
    CHECK(!map->fetch_span.data); /* Every supported map mutation drops the view. */
    CHECK(vk_mmap_get_read_span(map, 2, 0x2000, &span) == -1 && !span.data);
    compare(map, 0x2000);
    CHECK(!vk_mmap_add_rom(map, 0x3000, 0x30ff, 255, 0, be, "big endian"));
    CHECK(vk_mmap_get_read_span(map, 2, 0x3000, &span) == -1);
    CHECK(!vk_mmap_fetch16(map, 0x3000, &instruction) && instruction == 1);
    vk_buffer_t custom = *ram; custom.get = custom_get;
    CHECK(!vk_mmap_add_rom(map, 0x4000, 0x40ff, 255, 0, &custom, "custom reader"));
    CHECK(vk_mmap_get_read_span(map, 2, 0x4000, &span) == -1);
    CHECK(!vk_mmap_fetch16(map, 0x4000, &instruction) && instruction == 0xbabe && custom_reads == 1);
    CHECK(vk_mmap_fetch16(map, 0x5000, &instruction) == -1);
    vk_mmap_destroy(&map);

    map = vk_mmap_new(&machine);
    if (!map) return 2;
    /* Mirroring and non-contiguous masks: never reuse a span across a wrap. */
    CHECK(!vk_mmap_add_rom(map, 0x6003, 0x60ff, 15, 0, ram, "mirrored, clipped start"));
    CHECK(!vk_mmap_add_rom(map, 0x7000, 0x70ff, 5, 0, ram, "non-contiguous mask"));
    for (uint32_t address = 0x6003; address < 0x60ff; ++address) compare(map, address);
    for (uint32_t address = 0x7000; address < 0x70ff; ++address) compare(map, address);
    CHECK(!vk_mmap_get_read_span(map, 2, 0x6004, &span));
    CHECK(span.first == 0x6003 && span.last == 0x600f);
    CHECK(!vk_mmap_get_read_span(map, 2, 0x6010, &span));
    CHECK(span.first == 0x6010 && span.last == 0x601f);
    CHECK(!vk_mmap_get_read_span(map, 2, 0x7004, &span));
    CHECK(span.first == 0x7004 && span.last == 0x7005);
    /* The mask allows more bytes than the backing buffer: clip the pointer. */
    CHECK(!vk_mmap_add_rom(map, 0x8000, 0x8fff, 4095, 0, ram, "short buffer"));
    CHECK(!vk_mmap_get_read_span(map, 2, 0x80fe, &span) && span.last == 0x80ff);
    CHECK(vk_mmap_get_read_span(map, 2, 0x80ff, &span) == -1);
    CHECK(vk_mmap_get_read_span(map, 2, 0x8100, &span) == -1);
    /* UINT32_MAX is a real end address, not a wrapping exclusive sentinel. */
    CHECK(!vk_mmap_add_rom(map, 0xffffff00u, UINT32_MAX, 255, 0, ram, "top of address space"));
    CHECK(!vk_mmap_get_read_span(map, 2, 0xfffffffeu, &span) && span.last == UINT32_MAX);
    compare(map, 0xfffffffeu);
    CHECK(vk_mmap_get_read_span(map, 2, UINT32_MAX, &span) == -1);
    CHECK(!vk_mmap_add_rom(map, 0, 255, UINT32_MAX, 0, ram, "full address mask"));
    CHECK(!vk_mmap_get_read_span(map, 2, 254, &span) && span.first == 0 && span.last == 255);
    compare(map, 254);
    CHECK(!vk_mmap_add_rom(map, 0x9000, 0x90ff, 6, 0, ram, "mask with no low bit"));
    CHECK(vk_mmap_get_read_span(map, 2, 0x9000, &span) == -1);
    for (uint32_t address = 0x9000; address < 0x90ff; ++address) compare(map, address);
    CHECK(vk_mmap_get_read_span(map, 0, 0, &span) == -1);
    vk_mmap_destroy(&map);
    vk_buffer_destroy(&ram); vk_buffer_destroy(&rom); vk_buffer_destroy(&be);
    printf("Hikaru instruction fetch: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
