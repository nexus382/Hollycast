// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "vk/mmap.h"
#include <stdio.h>

static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #condition); ++failures; } } while (0)

typedef struct {
    vk_device_t base;
    uint64_t hash;
    unsigned calls;
} test_device_t;

typedef struct {
    vk_buffer_t base;
    uint64_t hash;
    unsigned calls;
} test_buffer_t;

typedef struct {
    vk_machine_t machine;
    vk_mmap_t *map;
    vk_buffer_t *ram, *be, *readonly, *custom_storage, *small;
    test_device_t device;
    test_buffer_t custom;
    vk_buffer_t custom_reader;
} fixture_t;

static int device_put(vk_device_t *base, unsigned size, uint32_t address, uint64_t value)
{
    test_device_t *device = (test_device_t *)base;
    ++device->calls;
    device->hash = device->hash * 33 + value + address + size;
    /* Side effects and errors must both survive the fallback path. */
    return (address & 4) ? -7 : 0;
}

static void custom_put(vk_buffer_t *base, unsigned size, uint32_t offset, uint64_t value)
{
    test_buffer_t *buffer = (test_buffer_t *)base;
    ++buffer->calls;
    buffer->hash = buffer->hash * 33 + value + offset + size;
    for (unsigned i = 0; i < size; ++i)
        base->ptr[offset + i] = (uint8_t)(value >> (8 * i)) ^ 0x5a;
}

static uint64_t custom_get(vk_buffer_t *buffer, unsigned size, uint32_t offset)
{
    (void)buffer; (void)size; (void)offset;
    return 0;
}

static void init(fixture_t *f)
{
    f->map = vk_mmap_new(&f->machine);
    f->ram = vk_buffer_le32_new(256, 0);
    f->be = vk_buffer_be32_new(256, 0);
    f->readonly = vk_buffer_le32_new(256, 0);
    f->custom_storage = vk_buffer_le32_new(256, 0);
    f->small = vk_buffer_le32_new(37, 0);
    if (!f->map || !f->ram || !f->be || !f->readonly || !f->custom_storage || !f->small)
        exit(2);
    vk_buffer_t *buffers[] = {f->ram, f->be, f->readonly, f->custom_storage, f->small};
    for (unsigned b = 0; b < 5; ++b)
        for (unsigned i = 0; i < buffers[b]->size; ++i)
            buffers[b]->ptr[i] = (uint8_t)(17 * i + b);
    f->custom.base = *f->custom_storage;
    f->custom.base.put = custom_put;
    f->custom_reader = *f->ram;
    f->custom_reader.get = custom_get;
    f->device.base.put = device_put;

    /* A byte-only writable overlay rejects wider writes, rather than falling
     * through to the broad RAM mapping. A read-only overlay does not intercept
     * writes to that underlying RAM in the original bus implementation. */
    CHECK(!vk_mmap_add_dev(f->map, 0x1040, 0x104f, 15,
        VK_REGION_W | VK_REGION_SIZE_8, &f->device.base, "byte overlay"));
    CHECK(!vk_mmap_add_dev(f->map, 0x1200, 0x121f, 31,
        VK_REGION_RW | VK_REGION_SIZE_ALL, &f->device.base, "device overlay"));
    CHECK(!vk_mmap_add_rom(f->map, 0x1080, 0x108f, 255, 0, f->readonly, "read-only overlay"));
    CHECK(!vk_mmap_add_ram(f->map, 0x1000, 0x1fff, 255, 0, f->ram, "mirrored RAM"));
    CHECK(!vk_mmap_add_ram(f->map, 0x2000, 0x20ff, 255, 0, f->be, "big-endian RAM"));
    CHECK(!vk_mmap_add_ram(f->map, 0x3000, 0x30ff, 255, 0, &f->custom.base, "custom writer"));
    CHECK(!vk_mmap_add_ram(f->map, 0x4000, 0x40ff, 5, 0, f->ram, "sparse mask"));
    CHECK(!vk_mmap_add_rom(f->map, 0x5000, 0x50ff, 255, 0, f->readonly, "ROM"));
    CHECK(!vk_mmap_add_ram(f->map, 0x6000, 0x60ff, 255, VK_REGION_LOG_W, f->ram, "logged RAM"));
    CHECK(!vk_mmap_add_ram(f->map, 0x7000, 0x70ff, 255, 0, f->small, "short buffer"));
    CHECK(!vk_mmap_add_ram(f->map, 0x8000, 0x80ff, 255, 0, &f->custom_reader, "custom reader only"));
    CHECK(!vk_mmap_add_ram(f->map, 0x9000, 0x900f, 255, 0, f->ram, "short region"));
    CHECK(!vk_mmap_add_ram(f->map, 0, 255, UINT32_MAX, 0, f->ram, "full mask"));
    CHECK(!vk_mmap_add_ram(f->map, 0xffffff00u, UINT32_MAX, 255, 0, f->ram, "top boundary"));
}

static void equal(const fixture_t *a, const fixture_t *b)
{
    CHECK(!memcmp(a->ram->ptr, b->ram->ptr, a->ram->size));
    CHECK(!memcmp(a->be->ptr, b->be->ptr, a->be->size));
    CHECK(!memcmp(a->readonly->ptr, b->readonly->ptr, a->readonly->size));
    CHECK(!memcmp(a->custom_storage->ptr, b->custom_storage->ptr, a->custom_storage->size));
    CHECK(!memcmp(a->small->ptr, b->small->ptr, a->small->size));
    CHECK(a->device.calls == b->device.calls && a->device.hash == b->device.hash);
    CHECK(a->custom.calls == b->custom.calls && a->custom.hash == b->custom.hash);
}

static void compare(fixture_t *a, fixture_t *b, unsigned size, uint32_t address)
{
    uint64_t value = 0xfedcba9876543210ull ^ ((uint64_t)address << 19) ^ checks;
    int expected = vk_mmap_put(a->map, size, address, value);
    int actual = vk_mmap_put_data_fast(b->map, size, address, value);
    CHECK(expected == actual);
    equal(a, b);
}

static void destroy(fixture_t *f)
{
    vk_mmap_destroy(&f->map);
    vk_buffer_destroy(&f->ram); vk_buffer_destroy(&f->be);
    vk_buffer_destroy(&f->readonly); vk_buffer_destroy(&f->custom_storage);
    vk_buffer_destroy(&f->small);
}

int main(void)
{
    fixture_t reference = {0}, fast = {0};
    init(&reference); init(&fast);
    CHECK(vk_buffer_is_plain_le(&fast.custom.base));
    CHECK(!vk_buffer_is_plain_le_writer(&fast.custom.base));
    CHECK(!vk_buffer_is_plain_le(&fast.custom_reader));
    CHECK(vk_buffer_is_plain_le_writer(&fast.custom_reader));

    vk_mmap_write_span_t span;
    CHECK(!vk_mmap_get_write_span(fast.map, 8, 0x1020, &span));
    CHECK(span.first == 0x1000 && span.last == 0x103f);
    CHECK(!vk_mmap_get_write_span(fast.map, 8, 0x1080, &span));
    CHECK(span.first == 0x1050 && span.last == 0x10ff);
    const uint32_t fallback[] = {0x1040, 0x1200, 0x2000, 0x3000, 0x5000, 0x6000};
    for (unsigned i = 0; i < sizeof(fallback) / sizeof(fallback[0]); ++i) {
        CHECK(vk_mmap_get_write_span(fast.map, 4, fallback[i], &span));
        CHECK(!span.data);
    }
    CHECK(vk_mmap_get_write_span(fast.map, 8, 0x7020, &span));
    CHECK(vk_mmap_get_write_span(fast.map, 8, 0x900c, &span));

    for (unsigned size = 1; size <= 8; size *= 2) {
        for (uint32_t address = 0x1000; address <= 0x1fff; ++address)
            if ((address & 255) <= 256 - size) compare(&reference, &fast, size, address);
        const uint32_t bases[] = {0, 0x2000, 0x3000, 0x4000, 0x5000, 0x6000, 0x8000};
        for (unsigned b = 0; b < sizeof(bases) / sizeof(bases[0]); ++b)
            for (unsigned offset = 0; offset <= 256 - size; ++offset)
                compare(&reference, &fast, size, bases[b] + offset);
        for (unsigned offset = 0; offset <= 37 - size; ++offset)
            compare(&reference, &fast, size, 0x7000 + offset);
        /* Cross-region writes retain the old start-address lookup semantics. */
        for (unsigned offset = 0; offset < 24; ++offset)
            compare(&reference, &fast, size, 0x9000 + offset);
        compare(&reference, &fast, size, (uint32_t)(0x100000000ull - size));
        compare(&reference, &fast, size, 0xdead0000);
    }
    CHECK(fast.device.calls && fast.custom.calls);
    for (unsigned i = 0; i < fast.readonly->size; ++i)
        CHECK(fast.readonly->ptr[i] == (uint8_t)(17 * i + 2));

    /* Read/fetch caches point at live storage and must observe direct stores. */
    uint16_t instruction = 0;
    uint64_t value = 0;
    CHECK(!vk_mmap_fetch16(fast.map, 0x1020, &instruction));
    CHECK(!vk_mmap_get_data_fast(fast.map, 4, 0x1020, &value));
    CHECK(!vk_mmap_put_data_fast(fast.map, 4, 0x1020, 0x1234a55a));
    CHECK(!vk_mmap_fetch16(fast.map, 0x1020, &instruction) && instruction == 0xa55a);
    CHECK(!vk_mmap_get_data_fast(fast.map, 4, 0x1020, &value) && value == 0x1234a55a);

    for (unsigned size = 1; size <= 8; size *= 2) {
        CHECK(!vk_mmap_put_data_fast(fast.map, size, 0x1020, 0));
        CHECK(!vk_mmap_get_data_fast(fast.map, size, 0x1020, &value));
    }
    for (unsigned i = 0; i < 4; ++i)
        CHECK(fast.map->write_spans[i].data && fast.map->data_spans[i].data);
    CHECK(!vk_mmap_add_dev(fast.map, 0x1300, 0x131f, 31,
        VK_REGION_W | VK_REGION_SIZE_ALL, &fast.device.base, "later overlay"));
    CHECK(!fast.map->fetch_span.data);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(!fast.map->write_spans[i].data && !fast.map->data_spans[i].data);
    unsigned calls = fast.device.calls;
    CHECK(!vk_mmap_put_data_fast(fast.map, 4, 0x1300, 0x76543210));
    CHECK(fast.device.calls == calls); /* The earlier RAM mapping still wins. */
    CHECK(vk_buffer_get(fast.ram, 4, 0) == 0x76543210);
    CHECK(vk_mmap_put_data_fast(fast.map, 3, 0x1020, 0) == -1);
    destroy(&reference); destroy(&fast);
    printf("Hikaru data writes: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
