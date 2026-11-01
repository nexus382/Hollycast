/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* 
 * Valkyrie
 * Copyright (C) 2011-2014, Stefano Teso
 * 
 * Valkyrie is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Valkyrie is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Valkyrie.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __HIKARU_RENDERER_PRIVATE_H__
#define __HIKARU_RENDERER_PRIVATE_H__

#include "mach/hikaru/hikaru-gpu-private.h"

enum {
	HR_DEBUG_LOG,
	HR_DEBUG_NO_LAYER1,
	HR_DEBUG_NO_LAYER2,
	HR_DEBUG_NO_3D,
	HR_DEBUG_SELECT_VIEWPORT,
	HR_DEBUG_NO_TEXTURES,
	HR_DEBUG_NO_MIPMAPS,
	HR_DEBUG_SELECT_POLYTYPE,
	HR_DEBUG_NO_INSTANCING,
	HR_DEBUG_SELECT_INSTANCE,
	HR_DEBUG_NO_LIGHTING,
	HR_DEBUG_NO_AMBIENT,
	HR_DEBUG_NO_DIFFUSE,
	HR_DEBUG_NO_SPECULAR,
	HR_DEBUG_FOG,

	HR_NUM_DEBUG_VARS
};

typedef union {
	struct {
		uint64_t has_texture		: 1;
		uint64_t has_lighting		: 1;
		uint64_t has_two_sided_lighting	: 1;
		uint64_t has_phong		: 1;

		uint64_t has_light0		: 1;
		uint64_t light0_type		: 2;
		uint64_t light0_att_type	: 3;
		uint64_t has_light0_specular	: 1;

		uint64_t has_light1		: 1;
		uint64_t light1_type		: 2;
		uint64_t light1_att_type	: 3;
		uint64_t has_light1_specular	: 1;

		uint64_t has_light2		: 1;
		uint64_t light2_type		: 2;
		uint64_t light2_att_type	: 3;
		uint64_t has_light2_specular	: 1;

		uint64_t has_light3		: 1;
		uint64_t light3_type		: 2;
		uint64_t light3_att_type	: 3;
		uint64_t has_light3_specular	: 1;

		uint64_t has_fog		: 1;
		uint64_t use_unlit_diffuse	: 1;
	};
	uint64_t full;
} hikaru_glsl_variant_t;

typedef struct {
	GLuint id;
	hikaru_texhead_t th;
} hikaru_texture_t;

typedef struct {
	GLuint			vbo;
	uint32_t		vertex_chunk, first_vertex;
	uint32_t		num_tris;
	uint32_t		addr[2];
	uint32_t		vp_index;
	uint32_t		mv_index;
	uint32_t		alternate_mv_index;
	uint32_t		num_instances;
	uint32_t		mat_index;
	uint32_t		tex_index;
	uint32_t		ls_index;
	float			alpha_thresh[2];
	float			depth_bias;
	bool			has_two_sided_lighting;
	uint32_t		num;
} hikaru_mesh_t;

typedef struct {
	GLuint u_projection, u_modelview, u_normal;
	GLuint u_modelview_alternate, u_normal_alternate;
	struct {
		GLuint position, direction, diffuse, specular, extents, spot, spot_table;
	} u_lights[4];
	GLuint u_ambient, u_texture, u_fog, u_fog_color;
} hikaru_uniform_locations_t;

typedef struct {
	vk_renderer_t base;

	hikaru_gpu_t *gpu;

	hikaru_viewport_t	*vp_list;
	uint32_t		 num_vps;
	uint32_t		 capacity_vps;

	hikaru_modelview_t	*mv_list;
	uint32_t		 num_mvs;
	uint32_t		 capacity_mvs;
	hikaru_modelview_t	*alternate_mv_list;
	uint32_t		 num_alternate_mvs;
	uint32_t		 capacity_alternate_mvs;
	uint32_t		 num_instances;

	hikaru_material_t	*mat_list;
	uint32_t		 num_mats;
	uint32_t		 capacity_mats;

	hikaru_texhead_t	*tex_list;
	uint32_t		 num_texs;
	uint32_t		 capacity_texs;

	hikaru_lightset_t	*ls_list;
	uint32_t		 num_lss;
	uint32_t		 capacity_lss;

	hikaru_mesh_t		*mesh_list[8][8];
	uint32_t		 num_meshes[8][8];
	uint32_t		 total_meshes;
	struct {
		uint8_t *data;
		size_t used;
		GLuint vbo, vao;
	} vertex_chunks[8];

	struct {
		unsigned		num_verts, num_tris;
		hikaru_vertex_t		tmp[4];
		hikaru_vertex_body_t	all[MAX_VERTICES_PER_MESH];
	} push;

	struct {
		hikaru_mesh_t		*current;

		hikaru_glsl_variant_t	variant;
		unsigned                cache_slot;
		GLuint			program;
		GLuint			vao;

		hikaru_uniform_locations_t locs;
	} meshes;

	struct {
		hikaru_texture_t cache[2][0x40][0x80];
		bool is_clear[2];
		bool cpu_dirty;
		uint64_t uploads, invalidations, wrap_updates;
	} textures;

	struct {
		GLuint program;
		GLuint fb[2], cb[2], db[2];
		unsigned width, height;
		GLuint vao, vbo;
		struct {
			GLuint u_front;
			GLuint u_back;
			GLuint u_layer1;
			GLuint u_layer2;
			GLuint u_factor;
			GLuint u_mult_f;
			GLuint u_mult_b;
			GLuint u_mult_1;
			GLuint u_mult_2;
			GLuint u_direct_1;
			GLuint u_direct_2;
			GLuint u_cover_1;
			GLuint u_cover_2;
			GLuint u_invert_f;
			GLuint u_invert_b;
		} locs;
	} framebuffer;

	struct {
		int32_t flags[HR_NUM_DEBUG_VARS];
	} debug;

} hikaru_renderer_t;

#define LOG(fmt_, args_...) \
	do { \
		if (hr->debug.flags[HR_DEBUG_LOG]) \
			fprintf (stdout, "\tHR: " fmt_"\n", ##args_); \
	} while (0)

uint32_t	rgba1111_to_rgba4444 (uint8_t pixel);
uint16_t	abgr1555_to_rgba5551 (uint16_t pixel);
uint16_t	abgr4444_to_rgba4444 (uint16_t pixel);
uint32_t	a8_to_rgba8888 (uint32_t a);

#endif /* __HIKARU_RENDERER_PRIVATE_H__ */

