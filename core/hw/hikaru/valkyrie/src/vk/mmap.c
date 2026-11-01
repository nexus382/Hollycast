/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* 
 * Valkyrie
 * Copyright (C) 2011, Stefano Teso
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "vk/mmap.h"

typedef struct {
	uint32_t lo;
	uint32_t hi;
	uint32_t mask;
	uint32_t flags;
	union {
		vk_buffer_t *buf;
		vk_device_t *dev;
		void *ptr;
	};
	char *name;
} region_t;

static inline unsigned
get_size_flag_for_size (unsigned size)
{
	switch (size) {
	case 1:
		return VK_REGION_SIZE_8;
	case 2:
		return VK_REGION_SIZE_16;
	case 4:
		return VK_REGION_SIZE_32;
	case 8:
		return VK_REGION_SIZE_64;
	}
	return 0;
}

static int
add_region (vk_mmap_t *mmap, uint32_t lo, uint32_t hi, uint32_t mask,
            uint32_t flags, void *ptr, const char *name)
{
	region_t *region;

	VK_ASSERT (mmap);
	VK_ASSERT (mmap->regions);

	VK_ASSERT (flags & VK_REGION_RW);
	VK_ASSERT (flags & VK_REGION_SIZE_ALL);
	VK_ASSERT (hi > lo);
	VK_ASSERT (mask != 0);
	VK_ASSERT (ptr != NULL);
	VK_ASSERT (name != NULL);

	region = (region_t *) vk_vector_append_entry (mmap->regions);
	if (!region)
		return -1;

	region->lo = lo;
	region->hi = hi;
	region->mask = mask;
	region->flags = flags;
	region->ptr = ptr;
	region->name = strdup (name);
	mmap->fetch_span.data = NULL;
	for (unsigned i = 0; i < 4; ++i) {
		mmap->data_spans[i].data = NULL;
		mmap->write_spans[i].data = NULL;
	}

	VK_ASSERT (region->name);

	return 0;
}

int
vk_mmap_add_ram (vk_mmap_t *mmap, uint32_t lo, uint32_t hi, uint32_t mask,
                 uint32_t flags, vk_buffer_t *buf, const char *name)
{
	flags |= VK_REGION_DIRECT | VK_REGION_RW | VK_REGION_SIZE_ALL;
	return add_region (mmap, lo, hi, mask, flags, (void *) buf, name);
}

int
vk_mmap_add_rom (vk_mmap_t *mmap, uint32_t lo, uint32_t hi, uint32_t mask,
                 uint32_t flags, vk_buffer_t *buf, const char *name)
{
	VK_ASSERT (!(flags & VK_REGION_W));

	flags |= VK_REGION_DIRECT | VK_REGION_R | VK_REGION_SIZE_ALL;
	return add_region (mmap, lo, hi, mask, flags, (void *) buf, name);
}

int
vk_mmap_add_dev (vk_mmap_t *mmap, uint32_t lo, uint32_t hi, uint32_t mask,
                 uint32_t flags, vk_device_t *dev, const char *name)
{
	VK_ASSERT (!(flags & VK_REGION_DIRECT));

	return add_region (mmap, lo, hi, mask, flags, (void *) dev, name);
}

static region_t *
get_region (vk_mmap_t *mmap, uint32_t addr, uint32_t flags)
{
	uint32_t offs;

	VK_ASSERT (mmap);
	VK_ASSERT ((flags & ~VK_REGION_RW) == 0);

	VK_VECTOR_FOREACH (mmap->regions, offs) {
		region_t *region = (region_t *) &mmap->regions->data[offs];
		if (addr >= region->lo && addr <= region->hi &&
		    (region->flags & flags))
			return region;
	}

	return NULL;
}

int
vk_mmap_get_read_span(vk_mmap_t *mmap, unsigned size, uint32_t addr, vk_mmap_read_span_t *span)
{
    region_t *region = get_region(mmap, addr, VK_REGION_R);
    if (!span) return -1;
    span->data = NULL;
    if (!region || !(region->flags & VK_REGION_DIRECT) ||
        (region->flags & VK_REGION_LOG_R) ||
        !(region->flags & get_size_flag_for_size(size)) ||
        !vk_buffer_is_plain_le(region->buf) || !region->buf->ptr)
        return -1;
    /* Only consecutive low mask bits advance linearly. This also handles
     * non-power-of-two masks without caching across a masked-out bit. */
    const uint32_t linear_mask = region->mask == UINT32_MAX ? UINT32_MAX :
        (region->mask ^ (region->mask + 1)) >> 1;
    uint32_t first = MAX2(region->lo, addr & ~linear_mask);
    uint32_t last = MIN2(region->hi, addr | linear_mask);
    /* A higher-priority region may begin or end inside the broad direct
     * mapping. Clip on both sides, including device/unsupported-size overlays. */
    uint32_t offset;
    VK_VECTOR_FOREACH(mmap->regions, offset) {
        region_t *earlier = (region_t *)&mmap->regions->data[offset];
        if (earlier == region) break;
        if (!(earlier->flags & VK_REGION_R)) continue;
        if (addr < earlier->lo && earlier->lo <= last) last = earlier->lo - 1;
        if (addr > earlier->hi && earlier->hi >= first) first = earlier->hi + 1;
    }
    const uint32_t buffer_offset = first & region->mask;
    if (buffer_offset >= region->buf->size) return -1;
    const uint64_t available = region->buf->size - buffer_offset;
    if ((uint64_t)last - first + 1 > available) last = first + (uint32_t)available - 1;
    if (addr < first || (uint64_t)addr + size > (uint64_t)last + 1) return -1;
    span->first = first; span->last = last;
    span->data = region->buf->ptr + buffer_offset;
    return 0;
}

static int
make_write_span(vk_mmap_t *mmap, region_t *region, unsigned size,
                uint32_t addr, vk_mmap_write_span_t *span)
{
    if (!span) return -1;
    span->data = NULL;
    if (!region || !(region->flags & VK_REGION_DIRECT) ||
        (region->flags & VK_REGION_LOG_W) ||
        !(region->flags & get_size_flag_for_size(size)) ||
        !vk_buffer_is_plain_le_writer(region->buf) || !region->buf->ptr)
        return -1;
    /* A masked-out bit, region edge or buffer end terminates the linear view. */
    const uint32_t linear_mask = region->mask == UINT32_MAX ? UINT32_MAX :
        (region->mask ^ (region->mask + 1)) >> 1;
    uint32_t first = MAX2(region->lo, addr & ~linear_mask);
    uint32_t last = MIN2(region->hi, addr | linear_mask);
    uint32_t offset;
    VK_VECTOR_FOREACH(mmap->regions, offset) {
        region_t *earlier = (region_t *)&mmap->regions->data[offset];
        if (earlier == region) break;
        /* Read-only overlays do not intercept writes in the ordinary bus path. */
        if (!(earlier->flags & VK_REGION_W)) continue;
        if (addr < earlier->lo && earlier->lo <= last) last = earlier->lo - 1;
        if (addr > earlier->hi && earlier->hi >= first) first = earlier->hi + 1;
    }
    const uint32_t buffer_offset = first & region->mask;
    if (buffer_offset >= region->buf->size) return -1;
    const uint64_t available = region->buf->size - buffer_offset;
    if ((uint64_t)last - first + 1 > available) last = first + (uint32_t)available - 1;
    if (addr < first || (uint64_t)addr + size > (uint64_t)last + 1) return -1;
    span->first = first; span->last = last;
    span->data = region->buf->ptr + buffer_offset;
    return 0;
}

int
vk_mmap_get_write_span(vk_mmap_t *mmap, unsigned size, uint32_t addr, vk_mmap_write_span_t *span)
{
    return make_write_span(mmap, get_region(mmap, addr, VK_REGION_W), size, addr, span);
}

int
vk_mmap_fetch16_slow(vk_mmap_t *mmap, uint32_t addr, uint16_t *instruction)
{
    if (!vk_mmap_get_read_span(mmap, 2, addr, &mmap->fetch_span)) {
        memcpy(instruction, mmap->fetch_span.data + (addr - mmap->fetch_span.first), sizeof(*instruction));
        return 0;
    }
    return vk_mmap_get(mmap, 2, addr, instruction);
}

int
vk_mmap_get (vk_mmap_t *mmap, unsigned size, uint32_t addr, void *data)
{
	region_t *region;

	VK_ASSERT (mmap != NULL);
	VK_ASSERT (data != NULL);
	VK_ASSERT (is_size_valid (size));

	region = get_region (mmap, addr, VK_REGION_R);
	if (!region || !(region->flags & get_size_flag_for_size (size)))
		return -1;

	if (region->flags & VK_REGION_LOG_R)
		VK_MACH_LOG (mmap->mach, "%s R%u %08X", region->name, size * 8, addr);

	if (region->flags & VK_REGION_DIRECT) {
		uint32_t offs = addr & region->mask;
		uint64_t temp;
		temp = region->buf->get (region->buf, size, offs);
		set_ptr (data, size, temp);
		return 0;
	}

	return vk_device_get (region->dev, size, addr, data);
}

static int
put_region(vk_mmap_t *mmap, region_t *region, unsigned size, uint32_t addr, uint64_t data)
{
	if (!region)
		return -1;

	if (!(region->flags & get_size_flag_for_size (size)))
		return -1;

	if (region->flags & VK_REGION_LOG_W)
		VK_MACH_LOG (mmap->mach, "%s W%u %08X = %" PRIX64 "", region->name, size * 8, addr, data);

	if (region->flags & VK_REGION_DIRECT) {
		uint32_t offs = addr & region->mask;
		region->buf->put (region->buf, size, offs, data);
		return 0;
	}

	return vk_device_put (region->dev, size, addr, data);
}

int
vk_mmap_put(vk_mmap_t *mmap, unsigned size, uint32_t addr, uint64_t data)
{
    VK_ASSERT(mmap != NULL);
    VK_ASSERT(is_size_valid(size));
    return put_region(mmap, get_region(mmap, addr, VK_REGION_W), size, addr, data);
}

int
vk_mmap_put_data_slow(vk_mmap_t *mmap, unsigned size, uint32_t addr, uint64_t data,
                      vk_mmap_write_span_t *span)
{
    VK_ASSERT(mmap != NULL);
    VK_ASSERT(is_size_valid(size));
    VK_ASSERT(span != NULL);
    region_t *region = get_region(mmap, addr, VK_REGION_W);
    vk_mmap_write_span_t candidate;
    if (!make_write_span(mmap, region, size, addr, &candidate))
        *span = candidate;
    /* Resolve once even for uncached device/logged/custom writes. Retain the
     * previous RAM view on such misses; its bounds already exclude overlays. */
    return put_region(mmap, region, size, addr, data);
}

vk_mmap_t *
vk_mmap_new (vk_machine_t *mach)
{
	vk_mmap_t *mmap;

	VK_ASSERT (mach);

	mmap = ALLOC (vk_mmap_t);
	if (!mmap)
		goto fail;

	mmap->regions = vk_vector_new (8, sizeof (region_t));
	if (!mmap->regions)
		goto fail;

	mmap->mach = mach;

	return mmap;

fail:
	vk_mmap_destroy (&mmap);
	return NULL;
}

void
vk_mmap_destroy (vk_mmap_t **mmap_)
{
	if (mmap_) {
		vk_mmap_t *mmap = *mmap_;
		if (mmap) {
			uint32_t offs;

			if (mmap->regions) VK_VECTOR_FOREACH (mmap->regions, offs) {
				region_t *region = (region_t *) &mmap->regions->data[offs];
				free (region->name);
			}

			vk_vector_destroy (&mmap->regions);
			mmap->mach = NULL;
		}
		free (mmap);
		*mmap_ = NULL;
	}
}
