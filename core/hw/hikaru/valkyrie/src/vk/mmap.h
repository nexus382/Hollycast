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

#ifndef __VK_MMAP_H__
#define __VK_MMAP_H__

#include "vk/region.h"
#include "vk/vector.h"

#define VK_REGION_R		(1 << 0)
#define VK_REGION_W		(1 << 1)
#define VK_REGION_RW		(VK_REGION_R|VK_REGION_W)

#define VK_REGION_DIRECT	(1 << 2)

#define VK_REGION_LOG_R		(1 << 4)
#define VK_REGION_LOG_W		(1 << 5)
#define VK_REGION_LOG_RW	(VK_REGION_LOG_R|VK_REGION_LOG_W)

#define VK_REGION_SIZE_8	(1 << 6)
#define VK_REGION_SIZE_16	(1 << 7)
#define VK_REGION_SIZE_32	(1 << 8)
#define VK_REGION_SIZE_64	(1 << 9)
#define VK_REGION_SIZE_ALL	(VK_REGION_SIZE_8|VK_REGION_SIZE_16|VK_REGION_SIZE_32|VK_REGION_SIZE_64)

/* A contiguous view of live buffer bytes, valid until map/buffer destruction.
 * Mapping changes invalidate all internal views. Buffer backing storage and
 * callbacks must remain stable while mapped; writes to the bytes remain live. */
typedef struct {
    const uint8_t *data;
    uint32_t first, last; /* Inclusive byte addresses. */
} vk_mmap_read_span_t;

typedef struct {
    uint8_t *data;
    uint32_t first, last; /* Inclusive byte addresses. */
} vk_mmap_write_span_t;

typedef struct {
    vk_mmap_read_span_t fetch_span;
	/* Access widths have separate views: a region may allow only some sizes. */
	vk_mmap_read_span_t data_spans[4];
	vk_mmap_write_span_t write_spans[4];
	vk_vector_t *regions;
	vk_machine_t *mach;
} vk_mmap_t;

vk_mmap_t	*vk_mmap_new (vk_machine_t *mach);
void		 vk_mmap_destroy (vk_mmap_t **mmap_);
int		 vk_mmap_add_ram (vk_mmap_t *mmap, uint32_t lo, uint32_t hi,
		                  uint32_t mask, uint32_t flags,
		                  vk_buffer_t *buf, const char *name);
int		 vk_mmap_add_rom (vk_mmap_t *mmap, uint32_t lo, uint32_t hi,
		                  uint32_t mask, uint32_t flags,
		                  vk_buffer_t *buf, const char *name);
int		 vk_mmap_add_dev (vk_mmap_t *mmap, uint32_t lo, uint32_t hi,
		                  uint32_t mask, uint32_t flags,
		                  vk_device_t *dev, const char *name);
int		 vk_mmap_get (vk_mmap_t *mmap, unsigned size, uint32_t addr, void *data);
int		 vk_mmap_put (vk_mmap_t *mmap, unsigned size, uint32_t addr, uint64_t data);

/* Only direct, unlogged regions backed by the built-in LE reader qualify.
 * The range excludes mask wrapping, buffer ends and earlier readable overlays. */
int vk_mmap_get_read_span(vk_mmap_t *, unsigned size, uint32_t addr, vk_mmap_read_span_t *);
/* Write views require the built-in LE writer and exclude earlier writable
 * overlays, even when an overlay rejects this particular access width. */
int vk_mmap_get_write_span(vk_mmap_t *, unsigned size, uint32_t addr, vk_mmap_write_span_t *);
int vk_mmap_put_data_slow(vk_mmap_t *, unsigned size, uint32_t addr, uint64_t value,
                        vk_mmap_write_span_t *);
int vk_mmap_fetch16_slow(vk_mmap_t *, uint32_t addr, uint16_t *instruction);

/* Unlogged plain little-endian storage can be read directly; device, custom,
 * logged and boundary accesses retain the ordinary bus callbacks. */
static inline int
vk_mmap_get_data_fast(vk_mmap_t *mmap, unsigned size, uint32_t addr, void *out)
{
	unsigned index;
	switch (size) {
	case 1: index = 0; break;
	case 2: index = 1; break;
	case 4: index = 2; break;
	case 8: index = 3; break;
	default: return -1;
	}
	vk_mmap_read_span_t *span = &mmap->data_spans[index];
	if (!span->data || addr < span->first || (uint64_t)addr + size - 1 > span->last) {
		if (vk_mmap_get_read_span(mmap, size, addr, span))
			return vk_mmap_get(mmap, size, addr, out);
	}
	const uint8_t *source = span->data + (addr - span->first);
	/* Constant widths let the compiler emit unaligned-safe scalar accesses
	 * even when the caller's access width is not known at compile time. */
	switch (size) {
	case 1: memcpy(out, source, 1); break;
	case 2: memcpy(out, source, 2); break;
	case 4: memcpy(out, source, 4); break;
	case 8: memcpy(out, source, 8); break;
	}
	return 0;
}

/* Device and custom writers, write logging, and boundary accesses keep the
 * ordinary bus path. Direct stores remain visible to existing read/fetch views. */
static inline int
vk_mmap_put_data_fast(vk_mmap_t *mmap, unsigned size, uint32_t addr, uint64_t value)
{
	unsigned index;
	switch (size) {
	case 1: index = 0; break;
	case 2: index = 1; break;
	case 4: index = 2; break;
	case 8: index = 3; break;
	default: return -1;
	}
	vk_mmap_write_span_t *span = &mmap->write_spans[index];
	if (!span->data || addr < span->first || (uint64_t)addr + size - 1 > span->last)
		return vk_mmap_put_data_slow(mmap, size, addr, value, span);
	uint8_t *destination = span->data + (addr - span->first);
	switch (size) {
	case 1: *destination = (uint8_t)value; break;
	case 2: { uint16_t v = (uint16_t)value; memcpy(destination, &v, sizeof(v)); break; }
	case 4: { uint32_t v = (uint32_t)value; memcpy(destination, &v, sizeof(v)); break; }
	case 8: memcpy(destination, &value, sizeof(value)); break;
	}
	return 0;
}

static inline int vk_mmap_fetch16(vk_mmap_t *mmap, uint32_t addr, uint16_t *instruction)
{
    const vk_mmap_read_span_t *span = &mmap->fetch_span;
    if (span->data && addr >= span->first && addr < span->last) {
        memcpy(instruction, span->data + (addr - span->first), sizeof(*instruction));
        return 0;
    }
    return vk_mmap_fetch16_slow(mmap, addr, instruction);
}

#endif /* __VK_MMAP_H__ */
