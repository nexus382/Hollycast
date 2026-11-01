/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* SPDX-License-Identifier: GPL-3.0-or-later
 * Embedded MIE bridge, Copyright 2026 The Hollycast Authors.
 * The host owns the actual device and its JVS firmware/interactions.
 */
#include "engine_internal.h"
#include "vk/device.h"
#include "vk/buffer.h"
#include "mach/hikaru/hikaru-mie.h"

typedef struct { vk_device_t base; } hosted_mie;
static void reset_device(vk_device_t *dev, vk_reset_type_t type)
{
    HikaruEngine *engine = dev->mach->host;
    (void)type;
    engine->mie.reset(engine->mie.opaque);
}
static int read_device(vk_device_t *dev, unsigned size, uint32_t address, void *value)
{
    HikaruEngine *engine = dev->mach->host;
    uint64_t data = 0;
    if (engine->mie.read(engine->mie.opaque, size, address, &data)) {
        snprintf(engine->error, sizeof(engine->error), "MIE read failed at %08x", address);
        return -1;
    }
    return set_ptr(value, size, data);
}
static int write_device(vk_device_t *dev, unsigned size, uint32_t address, uint64_t value)
{
    HikaruEngine *engine = dev->mach->host;
    if (engine->mie.write(engine->mie.opaque, size, address, value)) {
        snprintf(engine->error, sizeof(engine->error), "MIE write failed at %08x", address);
        return -1;
    }
    /* A pulse can begin and end within one SH4 slice. Observe host writes to
     * GPIO D or the direction register immediately, not only at slice ends. */
    if (engine->sound_attached &&
        ((address <= 0x00800006 && (uint64_t)address + size > 0x00800006) ||
         (address <= 0x00800010 && (uint64_t)address + size > 0x00800010)) &&
        hikaru_engine_sync_sound_reset(engine)) return -1;
    return 0;
}
static int advance_device(vk_device_t *dev, int cycles)
{
    HikaruEngine *engine = dev->mach->host;
    if (engine->mie.advance(engine->mie.opaque, (uint32_t)cycles)) {
        snprintf(engine->error, sizeof(engine->error), "MIE execution failed");
        return -1;
    }
    return cycles;
}
vk_device_t *hikaru_mie_new(vk_machine_t *mach)
{
    hosted_mie *mie;
    VK_DEVICE_ALLOC(mie, mach);
    if (!mie) return NULL;
    mie->base.reset = reset_device;
    mie->base.exec = advance_device;
    mie->base.get = read_device;
    mie->base.put = write_device;
    return &mie->base;
}
