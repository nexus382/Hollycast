// Portions Copyright 2026 The Hollycast Authors
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

#ifndef __VK_HIKARU_RENDERER_H__
#define __VK_HIKARU_RENDERER_H__

#include "vk/core.h"
#include "vk/buffer.h"
#include "vk/renderer.h"
#include "vk/machine.h"
#include "engine.h"

vk_renderer_t	*hikaru_renderer_new (vk_machine_t *mach, vk_buffer_t *fb, vk_buffer_t *texram[2]);
void		 hikaru_renderer_set_gpu (vk_renderer_t *, void *);
void		 hikaru_renderer_note_cpu_texram_write (vk_renderer_t *);
void             hikaru_renderer_set_transparency_sorting (vk_renderer_t *, HikaruTransparencySorting);
void             hikaru_renderer_set_transparency_peel_capacity(vk_renderer_t *, unsigned layers);
void             hikaru_renderer_set_transparency_memory_budget(vk_renderer_t *, uint64_t bytes);
vk_renderer_t *hikaru_renderer_begin_capture(vk_renderer_t *, void *gpu, HikaruFrame *);
void hikaru_renderer_seal_capture(vk_renderer_t *, void *gpu, HikaruFrame *);
void hikaru_renderer_end_capture(vk_renderer_t *, void *gpu);
void hikaru_renderer_draw_packet(vk_renderer_t *, HikaruFrame *);
void hikaru_renderer_finish_packet(vk_renderer_t *, HikaruFrame *);
#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_packet_ownership(vk_renderer_t *, void *gpu);
void hikaru_vendor_test_fail_next_packet_draw(void);
int hikaru_vendor_test_seed_packet_3d(vk_renderer_t *, void *gpu, HikaruFrame *, unsigned style);
#endif

#endif /* __VK_HIKARU_RENDERER_H__ */
