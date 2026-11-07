/* Portions Copyright 2026 The Hollycast Authors */
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

#define HIKARU_MAX_PEEL_LAYERS HIKARU_MAX_TRANSPARENCY_LAYERS
#include "engine.h"

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
		uint64_t has_pattern		: 1;
		uint64_t shading_mode		: 2;
		uint64_t alpha_mode		: 1;
		uint64_t texture_mode		: 3;
	};
	uint64_t full;
} hikaru_glsl_variant_t;

typedef struct {
	GLuint id;
	hikaru_texhead_t th;
	bool sampling_valid;
} hikaru_texture_t;

typedef struct {
	GLuint id;
	unsigned format, width, height;
	bool storage_valid;
	/* Retain tightly packed source rows, excluding framebuffer padding. */
	uint8_t *source_snapshot;
	size_t source_snapshot_capacity;
	/* Source layout for the retained bytes. */
	uint32_t source_x, source_y;
	unsigned source_format;
	bool source_valid;
} hikaru_layer_texture_t;

/* Uniform values belong to a linked mesh program and survive shader/frame
 * switches. Relinking or direct uniform writes invalidate their cache. */
typedef struct {
	bool viewport_valid, modelview_valid[2], lightset_valid, ambient_valid;
	bool blend_valid, peel_valid, depth_valid;
	vec2f_t depth;
	hikaru_viewport_t viewport;
	float alpha_ref;
	mtx4x4f_t modelview[2];
	hikaru_lightset_t lightset;
	unsigned light_debug_flags;
	vec3f_t ambient;
	bool weigh_by_alpha, force_coverage;
	unsigned peel_mode, peel_width, peel_height;
} hikaru_mesh_uniform_cache_t;

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
	/* Retained guest vertices support CPU sorting without relying on buffer readback. */
	hikaru_vertex_body_t *cpu_vertices;
	bool			has_two_sided_lighting;
	uint8_t			pattern;
	uint32_t		num;
} hikaru_mesh_t;

typedef struct {
	GLuint u_projection, u_modelview, u_normal;
	GLuint u_modelview_alternate, u_normal_alternate;
	struct {
		GLuint position, direction, diffuse, ambient, specular, extents, spot, spot_table;
	} u_lights[4];
	GLuint u_ambient, u_texture, u_fog, u_fog_color, u_alpha_ref, u_weigh_by_alpha, u_force_coverage, u_pattern, u_lod_bias;
	GLuint u_peel_enabled, u_peel_first, u_peel_size, u_peel_depth, u_peel_opaque_depth;
	GLuint u_depth, u_sort_scale, u_family, u_family_other;
} hikaru_uniform_locations_t;

typedef struct {
	vk_renderer_t base;

	hikaru_gpu_t *gpu;
	/* Capture callbacks only touch CPU data. The live renderer's forwarding
	 * pointer and strip assembler are producer-owned; drawing never reads them. */
	bool capture_only, capture_started, capture_invalidate_all;
	void *capture_target;
	bool invalidate_all;
	unsigned num_invalidations;
	hikaru_texhead_t invalidations[256];
	bool packet_trace, packet_active;
	char trace_master[256], trace_slave[256];
	/* Deletion can fail through a native backend's abort boundary. Detach
	 * packet identifiers first and retain unfinished disposal on this thread. */
	GLuint *pending_buffers;
	size_t pending_buffer_capacity;
	unsigned pending_buffer_count;

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
	uint32_t		 capacity_meshes[8][8];
	uint32_t		 num_meshes[8][8];
	uint32_t		 total_meshes;
	/* Packet swaps move CPU data/usage; the GL objects stay on this owner. */
	struct {
		uint8_t *data;
		size_t used;
		GLuint vbo, vao;
		bool uploaded, configured;
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
        bool active, depth_pass;
        unsigned selected, width, height;
        GLuint fb[2], color[2], metadata[2], depth[2], second_depth;
        GLuint color_fb, program;
    } families;
	float pass_alpha_ref;
	HikaruTransparencySorting transparency_sorting;
	unsigned transparency_peel_capacity;
	uint64_t transparency_memory_budget;
	bool transparency_overflow_seen, transparency_overflow_warned;
	struct {
		bool enabled, first;
		GLuint previous_depth, opaque_depth;
	} peel;
	struct {
		bool enabled, configured;
		hikaru_mesh_t *mesh;
		unsigned first, count, instance;
		GLuint vao, vbo;
		/* Retained sort workspace: grown on demand, reused every frame. */
		void *refs;
		void *refs_tmp;
		size_t refs_capacity;
		hikaru_vertex_body_t *batch;
	} triangle_replay;
	struct {
		bool enabled;
		hikaru_mesh_t *mesh;
		unsigned instance;
	} strip_replay;

	struct {
		/* A RAM origin can have multiple size/format interpretations without a
		 * write. Keep two layouts to avoid reuploading alternating views, with
		 * least-recently-used replacement bounding host and graphics memory. */
		hikaru_texture_t cache[2][0x40][0x80][2];
		uint8_t most_recent[2][0x40][0x80];
		/* Graphics-owner index: one bit for each slot with any live layout. */
		uint32_t occupied[2][0x40][4];
		bool is_clear[2];
		bool cpu_dirty;
		uint64_t uploads, invalidations, wrap_updates;
	} textures;

	struct {
		GLuint program;
		GLuint fb[2], cb[2], db[2]; /* db are sampleable depth textures */
		GLuint peel_fb, peel_color[HIKARU_MAX_PEEL_LAYERS + 1], peel_depth[HIKARU_MAX_PEEL_LAYERS + 1];
		unsigned peel_width, peel_height;
		unsigned peel_layers_allocated;
		GLuint peel_query[2];
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
			GLuint u_keyed_1;
			GLuint u_keyed_2;
			GLuint u_invert_f;
			GLuint u_invert_b;
		} locs;
	} framebuffer;
	struct {
		hikaru_layer_texture_t textures[2];
		/* The two layers upload sequentially and share this 1555 staging image. */
		uint8_t *abgr1555_staging;
		/* ES conversion destination retained for recurring 1555 layer uploads. */
		size_t rgba8_staging_size;
		uint8_t *rgba8_staging;
	} layer_upload;

	struct {
		int32_t flags[HR_NUM_DEBUG_VARS];
	} debug;

} hikaru_renderer_t;

/* Host-only packet storage, deliberately outside serialized GPU/CPU layouts.
 * The shallow GPU copy is a draw view: all live pointers are cleared/rebound. */
struct HikaruFrame {
	hikaru_renderer_t *capture;
	hikaru_gpu_t gpu;
	vk_buffer_t *fb, *texram[2];
	bool ready;
	/* Each alternating slot tracks its own retained contents. A shared dirty
	 * bit cleared by the other slot would miss writes from two frames ago. */
	uint64_t texture_source, texture_generation[2];
};

#define LOG(fmt_, ...) \
	do { \
		if (hr->debug.flags[HR_DEBUG_LOG]) \
			fprintf (stdout, "\tHR: " fmt_"\n", ##__VA_ARGS__); \
	} while (0)

uint32_t	rgba1111_to_rgba4444 (uint8_t pixel);
uint16_t	abgr1555_to_rgba5551 (uint16_t pixel);
uint16_t	abgr4444_to_rgba4444 (uint16_t pixel);
uint32_t	a8_to_rgba8888 (uint32_t a);

#endif /* __HIKARU_RENDERER_PRIVATE_H__ */

