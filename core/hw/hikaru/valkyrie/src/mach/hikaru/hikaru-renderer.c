/* Modified for Hollycast on 2026-11-01; see valkyrie/PROVENANCE.md.
 * Portions Copyright 2026 The Hollycast Authors */
/* 
 * Valkyrie
 * Copyright (C) 2011-2013, Stefano Teso
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

#include "vk/input.h"
#include "vk/cpu.h"

#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-renderer.h"
#include "mach/hikaru/hikaru-renderer-private.h"
#include "graphics_dispatch.h"

#ifdef HIKARU_VENDOR_TESTING
static bool fail_renderer_init_once;
void hikaru_vendor_test_fail_renderer_init(void) { fail_renderer_init_once = true; }
#endif

#define VP0	VP.scratch
#define MAT0	MAT.scratch
#define TEX0	TEX.scratch
#define LS0	LIT.scratchset

#define INITIAL_STATE_CAPACITY 4096u
#define MAX_MESHES 16384u
/* Existing mesh lists bound the maximum possible snapshots in one frame. */
#define MAX_FRAME_STATES (8u * 8u * MAX_MESHES)

#define INV255	(1.0f / 255.0f)

#define isnonnegative(x_) \
	(isfinite(x_) && (x_) >= 0.0)

/****************************************************************************
 Debug
****************************************************************************/

static const struct {
	int32_t min, max;
	uint32_t key;
} debug_controls[] = {
	[HR_DEBUG_LOG]			= {  0, 1,     ~0 },
	[HR_DEBUG_NO_LAYER1]		= {  0, 1, '1' },
	[HR_DEBUG_NO_LAYER2]		= {  0, 1, '2' },
	[HR_DEBUG_NO_3D]		= {  0, 1, '3' },
	[HR_DEBUG_SELECT_VIEWPORT]	= { -1, 7, 'v' },
	[HR_DEBUG_NO_TEXTURES]		= {  0, 1, 't' },
	[HR_DEBUG_NO_MIPMAPS]		= {  0, 1, 'u' },
	[HR_DEBUG_SELECT_POLYTYPE]	= { -1, 7, 'p' },
	[HR_DEBUG_NO_INSTANCING]	= {  0, 1, 'i' },
	[HR_DEBUG_SELECT_INSTANCE]	= {  0, 3, 'j' }, 
	[HR_DEBUG_NO_LIGHTING]		= {  0, 1, 'l' },
	[HR_DEBUG_NO_AMBIENT]		= {  0, 1, 'a' },
	[HR_DEBUG_NO_DIFFUSE]		= {  0, 1, 'd' },
	[HR_DEBUG_NO_SPECULAR]		= {  0, 1, 's' },
	[HR_DEBUG_FOG]			= {  0, 1, 'g' },
};

static void
init_debug_flags (hikaru_renderer_t *hr)
{
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++)
		hr->debug.flags[i] = debug_controls[i].min;

	hr->debug.flags[HR_DEBUG_LOG] =
		vk_util_get_bool_option ("HR_LOG", false) ? 1 : 0;
	/* Guest fog is normal rendering state; the debug key can disable it. */
	hr->debug.flags[HR_DEBUG_FOG] = 1;
}

static void
update_debug_flags (hikaru_renderer_t *hr)
{
	char *msg = hr->base.message;
	unsigned i;

	for (i = 0; i < NUMELEM (debug_controls); i++) {
		uint32_t key = debug_controls[i].key;
		if (key != ~0 && vk_input_get_key (key)) {
			hr->debug.flags[i] += 1;
			if (i == HR_DEBUG_NO_MIPMAPS)
				hikaru_renderer_invalidate_texcache (&hr->base, NULL);
			if (hr->debug.flags[i] > debug_controls[i].max)
				hr->debug.flags[i] = debug_controls[i].min;
		}
		msg += sprintf (msg, "%d|", hr->debug.flags[i]);
	}
}

/****************************************************************************
 Utils
****************************************************************************/

static GLuint
get_uniform_loc (GLuint prog, const char *id)
{
	GLuint loc;

	loc = glGetUniformLocation (prog, id);
	VK_ASSERT_NO_GL_ERROR ();
	if (loc == (GLuint) -1)
		VK_ABORT ("uniform '%s' has no location!", id);

	return loc;
}

/****************************************************************************
 Textures
****************************************************************************/

static bool
is_texhead_eq (hikaru_renderer_t *hr,
               hikaru_texhead_t *a, hikaru_texhead_t *b)
{
	return (a->format == b->format) &&
	       (a->logw == b->logw) &&
	       (a->logh == b->logh) &&
	       (a->bank == b->bank) &&
	       (a->slotx == b->slotx) &&
	       (a->sloty == b->sloty);
}

static void
destroy_texture (hikaru_texture_t *tex)
{
	if (tex->id) {
		glDeleteTextures (1, &tex->id);
		VK_ASSERT_NO_GL_ERROR ();
	}
	memset ((void *) tex, 0, sizeof (hikaru_texture_t));
}

static uint16_t
abgr1111_to_rgba4444 (uint32_t texel)
{
	/* Format 2 stores one 4-bit intensity. The same nibble fills all
	 * four RGBA4444 channels; treating the texel as four 1-bit
	 * channels garbled the loading screens and 1P art. */
	const uint16_t nibble = (uint16_t) (texel & 0xFu);
	return (uint16_t) (nibble | (nibble << 4) | (nibble << 8) | (nibble << 12));
}

/* Command 2C1 wrap selection, per axis: a set mirror bit selects
 * mirrored repeat, else a set repeat bit selects repeat, else the
 * edge clamps. In hikaru_texhead_t, wrapu/wrapv hold the mirror
 * bits and repeatu/repeatv the repeat bits. */
static GLenum
texture_wrap (bool repeat, bool mirror)
{
	if (mirror) return GL_MIRRORED_REPEAT;
	if (repeat) return GL_REPEAT;
	return GL_CLAMP_TO_EDGE;
}

static bool
texture_wrap_changed (const hikaru_texhead_t *a, const hikaru_texhead_t *b)
{
	return a->wrapu != b->wrapu || a->wrapv != b->wrapv ||
	       a->repeatu != b->repeatu || a->repeatv != b->repeatv;
}

#define PUT16(x, y, t) \
	*(uint16_t *) &(data[(y)*w*2 + (x)*2]) = (t)

static bool
valid_texture_span (const vk_buffer_t *texram, uint32_t format, uint32_t w, uint32_t h,
                    uint32_t basex, uint32_t basey)
{
    if (!texram || !texram->ptr || !w || !h) return false;
    uint64_t end;
    if (format == HIKARU_FORMAT_ABGR1111) {
        const uint64_t row_bytes = ((uint64_t)w * 2 + 3) & ~3ULL;
        end = ((uint64_t)basey + h - 1) * 4096 + (uint64_t)basex * 2 + row_bytes;
        return (uint64_t)basex * 2 + row_bytes <= 4096 && (uint64_t)basey + h <= 1024 && end <= texram->size;
    }
    end = ((uint64_t)basey + h - 1) * 4096 + ((uint64_t)basex + w) * 2;
    return (uint64_t)basex + w <= 2048 && (uint64_t)basey + h <= 1024 && end <= texram->size;
}

static bool
valid_layer_span (const vk_buffer_t *fb, uint32_t x, uint32_t y, unsigned row_bytes)
{
    if (!fb || !fb->ptr) return false;
    const uint64_t start = (uint64_t)y * 4096 + (uint64_t)x * 4;
    return start + 479ULL * 4096 + row_bytes <= fb->size;
}

/* The 16-bit framebuffer stores adjacent pixels in reversed halfword lanes.
 * Copy into a tightly packed image; the 32-bit layer format is unaffected. */
static uint8_t *
decode_layer_abgr1555 (const vk_buffer_t *fb, uint32_t x, uint32_t y)
{
	if (!valid_layer_span (fb, x, y, 640 * 2)) return NULL;
	uint8_t *out = malloc (640 * 480 * 2);
	if (!out) return NULL;
	const uint64_t start = (uint64_t) y * 4096 + (uint64_t) x * 4;
	for (unsigned row = 0; row < 480; ++row) {
		const uint8_t *src = fb->ptr + start + (uint64_t) row * 4096;
		uint8_t *dst = out + row * 640 * 2;
		for (unsigned byte = 0; byte < 640 * 2; byte += 4) {
			dst[byte] = src[byte + 2]; dst[byte + 1] = src[byte + 3];
			dst[byte + 2] = src[byte]; dst[byte + 3] = src[byte + 1];
		}
	}
	return out;
}

static void *
decode_texture_abgr1111 (vk_buffer_t *texram,
                         uint32_t w, uint32_t h,
                         uint32_t basex, uint32_t basey)
{
	uint32_t x, y;
	uint8_t *data;

	/* Each 16-bit storage texel contains a 2x2 group of 4-bit pixels. */
	w *= 2;
	basex *= 2;
	data = (uint8_t *) malloc (w * (h * 2) * 2);
	if (!data)
		return NULL;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x += 4) {
			uint32_t offs = (basey + y) * 4096 + (basex + x);
			uint32_t texels = vk_buffer_get (texram, 4, offs);
			if (x + 2 < w) {
				PUT16 (x + 2, y*2 + 0, abgr1111_to_rgba4444 (texels >> 28));
				PUT16 (x + 2, y*2 + 1, abgr1111_to_rgba4444 (texels >> 20));
			}
			if (x + 3 < w) {
				PUT16 (x + 3, y*2 + 0, abgr1111_to_rgba4444 (texels >> 24));
				PUT16 (x + 3, y*2 + 1, abgr1111_to_rgba4444 (texels >> 16));
			}
	      		PUT16 (x + 0, y*2 + 0, abgr1111_to_rgba4444 (texels >> 12));
	      		PUT16 (x + 0, y*2 + 1, abgr1111_to_rgba4444 (texels >> 4));
			if (x + 1 < w) {
				PUT16 (x + 1, y*2 + 0, abgr1111_to_rgba4444 (texels >> 8));
				PUT16 (x + 1, y*2 + 1, abgr1111_to_rgba4444 (texels >> 0));
			}
		}
	}
	return (void *) data;
}

#ifdef HIKARU_VENDOR_TESTING
void *hikaru_vendor_test_decode_abgr1111(vk_buffer_t *buffer, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{ return valid_texture_span(buffer, HIKARU_FORMAT_ABGR1111, w, h, x, y) ? decode_texture_abgr1111(buffer, w, h, x, y) : NULL; }
int hikaru_vendor_test_texture_span(const vk_buffer_t *buffer, uint32_t format, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{ return valid_texture_span(buffer, format, w, h, x, y); }
int hikaru_vendor_test_layer_span(const vk_buffer_t *buffer, uint32_t x, uint32_t y, unsigned row_bytes)
{ return valid_layer_span(buffer, x, y, row_bytes); }
void *hikaru_vendor_test_decode_layer_abgr1555(const vk_buffer_t *buffer, uint32_t x, uint32_t y)
{ return decode_layer_abgr1555(buffer, x, y); }
uint32_t hikaru_vendor_test_texture_wrap(int wrap, int mirror)
{ return texture_wrap(wrap != 0, mirror != 0); }
int hikaru_vendor_test_same_texture_state(uint32_t left, uint32_t right)
{
    hikaru_texhead_t a = {0}, b = {0};
    a._2C1 = left; b._2C1 = right;
    return is_texhead_eq(NULL, &a, &b);
}
int hikaru_vendor_test_texture_wrap_changed(uint32_t left, uint32_t right)
{
    hikaru_texhead_t a = {0}, b = {0};
    a._2C1 = left; b._2C1 = right;
    return texture_wrap_changed(&a, &b);
}
#endif

#undef PUT16

/* ES 3 has no reversed 16-bit pixel types. Convert cached texture uploads
 * to RGBA8 there, respecting the guest row/skip layout already validated by
 * the caller. Desktop GL keeps its direct packed upload. */
static int
upload_abgr16(GLint level, GLenum internal_format, unsigned width, unsigned height,
              GLenum type, const void *data)
{
	if (!vk_renderer_is_gles()) {
		glTexImage2D(GL_TEXTURE_2D, level, internal_format, width, height, 0, GL_RGBA, type, data);
		return 0;
	}
	if (!width || !height || width > SIZE_MAX / height / 4) return -1;
	GLint row_length, skip_rows, skip_pixels;
	glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
	glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
	glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
	const size_t stride = (row_length ? (unsigned)row_length : width) * 2u;
	uint8_t *rgba = malloc((size_t)width * height * 4);
	if (!rgba) return -1;
	for (unsigned y = 0; y < height; ++y) {
		const uint8_t *src = (const uint8_t *)data + ((size_t)skip_rows + y) * stride + (size_t)skip_pixels * 2;
		for (unsigned x = 0; x < width; ++x) {
			uint16_t pixel;
			memcpy(&pixel, src + x * 2, sizeof(pixel));
			uint8_t *dst = rgba + ((size_t)y * width + x) * 4;
			if (type == GL_UNSIGNED_SHORT_1_5_5_5_REV) {
				for (unsigned c = 0; c < 3; ++c) {
					const unsigned value = (pixel >> (c * 5)) & 31;
					dst[c] = (uint8_t)((value << 3) | (value >> 2));
				}
				dst[3] = pixel & 0x8000 ? 255 : 0;
			} else {
				for (unsigned c = 0; c < 4; ++c)
					dst[c] = (uint8_t)(((pixel >> (c * 4)) & 15) * 17);
			}
		}
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
	free(rgba);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
	return 0;
}

#ifdef HIKARU_VENDOR_TESTING
/* Exercise the production packed-upload path, including nonzero source
 * row/skip state, on both desktop GL and ES contexts. */
int hikaru_vendor_test_abgr_upload(void)
{
    const GLenum types[2] = {GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_UNSIGNED_SHORT_4_4_4_4_REV};
    const GLint formats[2] = {GL_RGB5_A1, GL_RGBA4};
    const uint16_t samples[2][12] = {
        {0,0,0,0, 0,0x801f,0x83e0,0, 0,0x7c00,0xffff,0},
        {0,0,0,0, 0,0xf00f,0xf0f0,0, 0,0x0f00,0xffff,0}
    };
    const uint8_t expected[16] = {255,0,0,255, 0,255,0,255, 0,0,255,0, 255,255,255,255};
    GLuint texture, fbo;
    int ok = 1;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 4);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 1);
    for (unsigned i = 0; i < 2; ++i) {
        uint8_t actual[16] = {0};
        GLint row, skip_rows, skip_pixels;
        if (upload_abgr16(0, formats[i], 2, 2, types[i], samples[i])) { ok = 0; break; }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = 0; break; }
        glReadPixels(0, 0, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, actual);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
        ok &= !memcmp(actual, expected, sizeof(expected)) && row == 4 && skip_rows == 1 && skip_pixels == 1;
        ok &= glGetError() == GL_NO_ERROR;
    }
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    return ok;
}
#endif

static GLuint
upload_texture (hikaru_renderer_t *hr, hikaru_texhead_t *th)
{
	uint32_t w, h, num_levels, level, basex, basey, bank;
	GLuint id;

	w = 16 << th->logw;
	h = 16 << th->logh;
	num_levels = hr->debug.flags[HR_DEBUG_NO_MIPMAPS] ? 1 :
	             MIN2 (th->logw, th->logh) + 5;

	get_texhead_coords (&basex, &basey, th);
	bank = th->bank;

	glGenTextures (1, &id);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 0);
	VK_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, id);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, texture_wrap(th->wrapu, th->repeatu));
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, texture_wrap(th->wrapv, th->repeatv));
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels - 1);
	VK_ASSERT_NO_GL_ERROR ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	VK_ASSERT_NO_GL_ERROR ();

	for (level = 0; level < num_levels; level++) {
		void *data;
		if (!w || !h || bank >= 2 || !hr->gpu->texram[bank] || !hr->gpu->texram[bank]->ptr) goto fail;
		data = (void *) hr->gpu->texram[bank]->ptr;
		if (!valid_texture_span(hr->gpu->texram[bank], th->format, w, h, basex, basey)) goto fail;

		switch (th->format) {
		case HIKARU_FORMAT_ABGR1555:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			if (upload_abgr16(level, GL_RGB5_A1, w, h, GL_UNSIGNED_SHORT_1_5_5_5_REV, data)) goto fail;
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR4444:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			if (upload_abgr16(level, GL_RGBA4, w, h, GL_UNSIGNED_SHORT_4_4_4_4_REV, data)) goto fail;
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_LA8:
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 2048);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, basey);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, basex);
			VK_ASSERT_NO_GL_ERROR ();
	
			glTexImage2D (GL_TEXTURE_2D, level,
			              GL_RG8,
			              w, h, 0,
			              GL_RG, GL_UNSIGNED_BYTE,
			              data);
			VK_ASSERT_NO_GL_ERROR ();

			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
			glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_GREEN);
			VK_ASSERT_NO_GL_ERROR ();
			break;
		case HIKARU_FORMAT_ABGR1111:
			data = decode_texture_abgr1111 (hr->gpu->texram[bank],
			                                w, h, basex, basey);
			if (!data)
				goto fail;
			glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
			glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
			glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);

			const int uploaded = upload_abgr16(level, GL_RGBA4, w * 2, h * 2, GL_UNSIGNED_SHORT_4_4_4_4_REV, data);
			VK_ASSERT_NO_GL_ERROR ();

			free (data);
			if (uploaded) goto fail;
			break;
		default:
			goto fail;
		}

		w = MAX2 (w >> 1, 1);
		h = MAX2 (h >> 1, 1);

		basex += (2048 - basex) / 2;
		basey += (1024 - basey) / 2;
		bank ^= 1;
	}

	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	VK_ASSERT_NO_GL_ERROR ();

	return id;

fail:
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	VK_ASSERT_NO_GL_ERROR ();

	glDeleteTextures (1, &id);
	VK_ASSERT_NO_GL_ERROR ();
	/* A bad texhead must not terminate the game. The draw selects its
	 * untextured variant when this upload cannot be represented safely. */
	LOG ("invalid or unsupported Hikaru texture upload");
	return 0;
}

hikaru_texture_t *
get_texture (hikaru_renderer_t *hr, hikaru_texhead_t *th)
{
	hikaru_texture_t *cached;
	uint32_t bank, slotx, sloty;
	GLuint id;
	if (hr->textures.cpu_dirty) {
		hikaru_renderer_invalidate_texcache (&hr->base, NULL);
		hr->textures.cpu_dirty = false;
	}

	bank  = th->bank;
	slotx = th->slotx;
	sloty = th->sloty;

	if (slotx < 0x80 || sloty < 0xC0)
		return NULL;

	slotx -= 0x80;
	sloty -= 0xC0;

	cached = &hr->textures.cache[bank][sloty][slotx];
	if (is_texhead_eq (hr, th, &cached->th))
		return cached;

	destroy_texture (cached);

	id = upload_texture (hr, th);
	if (!id) {
		destroy_texture (cached);
		return NULL;
	}

	cached->th = *th;
	cached->id = id;
	hr->textures.uploads++;

	hr->textures.is_clear[bank] = false;
	return cached;
}

void
hikaru_renderer_note_cpu_texram_write (vk_renderer_t *renderer)
{
	if (renderer) ((hikaru_renderer_t *)renderer)->textures.cpu_dirty = true;
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_cpu_texram_dirty(void)
{
	hikaru_renderer_t *hr = calloc (1, sizeof(*hr));
	if (!hr) return 0;
	hr->textures.is_clear[0] = hr->textures.is_clear[1] = true;
	hikaru_renderer_note_cpu_texram_write (&hr->base);
	bool marked = hr->textures.cpu_dirty;
	hikaru_texhead_t invalid = {0};
	get_texture (hr, &invalid); /* Flushes before rejecting the invalid slot. */
	bool flushed = !hr->textures.cpu_dirty;
	free (hr);
	return marked && flushed;
}
#endif

static void
clear_texcache_bank (hikaru_renderer_t *hr, unsigned bank)
{
	unsigned x, y;

	if (hr->textures.is_clear[bank])
		return;
	hr->textures.is_clear[bank] = true;

	/* Free all allocated surfaces. */
	for (y = 0; y < 0x40; y++)
		for (x = 0; x < 0x80; x++)
			destroy_texture (&hr->textures.cache[bank][y][x]);

	/* Zero out the cache itself, to avoid spurious hits. Note that
	 * texture RAM origin is (80,C0), so (slotx, sloty) will never match
	 * a zeroed out cache entries. */
	memset ((void *) &hr->textures.cache[bank], 0, sizeof (hr->textures.cache[bank]));
}

/* IDMA writes 16-bit texels with halfwords exchanged in each 32-bit word.
 * Texture uploads read all mip levels, alternating texture RAM banks. Use
 * conservative byte rectangles so a write to any sampled mip invalidates
 * the cached texture, regardless of the bank containing its base level. */
static bool
texture_overlaps_transfer (const hikaru_texhead_t *cached, const hikaru_texhead_t *transfer)
{
	uint32_t cx, cy, tx0, ty0;
	get_texhead_coords (&cx, &cy, (hikaru_texhead_t *) cached);
	get_texhead_coords (&tx0, &ty0, (hikaru_texhead_t *) transfer);
	uint32_t cw = 16u << cached->logw, ch = 16u << cached->logh;
	unsigned cb = cached->bank;
	unsigned levels = MIN2 (cached->logw, cached->logh) + 5;
	for (unsigned level = 0; level < levels; ++level) {
		uint32_t cstart = cx * 2;
		uint32_t cend = cstart + (cached->format == HIKARU_FORMAT_ABGR1111 ? ((cw * 2 + 3) & ~3u) : cw * 2);
		uint32_t tx = tx0, ty = ty0;
		uint32_t tw = 16u << transfer->logw, th = 16u << transfer->logh;
		unsigned tb = transfer->bank;
		const unsigned tlevels = MIN2 (transfer->logw, transfer->logh) + 5;
		for (unsigned tlevel = 0; tlevel < tlevels; ++tlevel) {
			uint32_t tstart = (tx * 2) & ~3u;
			uint32_t tend = (tx * 2 + tw * 2 + 3) & ~3u;
			if (cb == tb && cstart < tend && tstart < cend && cy < ty + th && ty < cy + ch)
				return true;
			tx += (2048 - tx) / 2; ty += (1024 - ty) / 2;
			tw = MAX2 (tw >> 1, 1); th = MAX2 (th >> 1, 1); tb ^= 1;
		}
		cx += (2048 - cx) / 2; cy += (1024 - cy) / 2;
		cw = MAX2 (cw >> 1, 1); ch = MAX2 (ch >> 1, 1); cb ^= 1;
	}
	return false;
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_texture_overlap(uint32_t c2, uint32_t c4, uint32_t t2, uint32_t t4)
{
	hikaru_texhead_t cached = {0}, transfer = {0};
	cached._2C1 = c2; cached._4C1 = c4;
	transfer._2C1 = t2; transfer._4C1 = t4;
	return texture_overlaps_transfer(&cached, &transfer);
}
#endif

void
hikaru_renderer_invalidate_texcache (vk_renderer_t *rend, hikaru_texhead_t *th)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;

	VK_ASSERT (hr);

	if (th == NULL) {
		clear_texcache_bank (hr, 0);
		clear_texcache_bank (hr, 1);
	} else {
		for (unsigned bank = 0; bank < 2; ++bank) {
			if (hr->textures.is_clear[bank]) continue;
			bool any_left = false;
			for (unsigned y = 0; y < 0x40; ++y)
				for (unsigned x = 0; x < 0x80; ++x) {
					hikaru_texture_t *cached = &hr->textures.cache[bank][y][x];
					if (cached->id) {
						if (texture_overlaps_transfer (&cached->th, th)) { destroy_texture (cached); hr->textures.invalidations++; }
						else any_left = true;
					}
				}
			hr->textures.is_clear[bank] = !any_left;
		}
	}
}

/****************************************************************************
 State
****************************************************************************/

static const char *mesh_vs_source =
"#version 150									\n \
										\n \
										\n \
%s										\n \
										\n \
uniform mat4 u_projection;							\n \
uniform mat4 u_modelview; uniform mat4 u_modelview_alternate;							\n \
uniform mat3 u_normal; uniform mat3 u_normal_alternate;								\n \
										\n \
in vec3 i_position;					\n \
in vec3 i_normal;						\n \
in vec3 i_diffuse;						\n \
in vec3 i_ambient;						\n \
in vec4 i_specular;					\n \
in vec3 i_unknown;						\n \
in vec2 i_texcoords;					\n \
in float i_alpha; in float i_matrix_select;						\n \
										\n \
out vec4 p_position;								\n \
out vec3 p_normal;								\n \
out vec3 p_diffuse;								\n \
out vec4 p_specular;								\n \
out vec3 p_ambient;								\n \
out vec3 p_unknown;								\n \
out vec2 p_texcoords;								\n \
out float p_alpha;								\n \
										\n \
void main (void) {								\n \
	int sel = int (i_matrix_select + 0.5);\n \
	if (sel == 1) p_position = u_modelview_alternate * vec4 (i_position, 1.0);\n \
	else if (sel == 0) p_position = u_modelview * vec4 (i_position, 1.0);\n \
	else if ((sel & 1) != 0) {\n \
		vec3 transformed = ((sel & 64) == 0) ? (mat3 (u_modelview_alternate) * i_position) : i_position;\n \
		if ((sel & 128) == 0) transformed += u_modelview_alternate[3].xyz;\n \
		p_position = vec4 (transformed, 1.0);\n \
	} else {\n \
		vec3 transformed = ((sel & 64) == 0) ? (mat3 (u_modelview) * i_position) : i_position;\n \
		if ((sel & 128) == 0) transformed += u_modelview[3].xyz;\n \
		p_position = vec4 (transformed, 1.0);\n \
	}			\n \
	gl_Position = u_projection * p_position;				\n \
										\n \
	if (sel == 1) p_normal = u_normal_alternate * i_normal;\n \
	else if (sel == 0) p_normal = u_normal * i_normal;\n \
	else if ((sel & 64) != 0) p_normal = i_normal;\n \
	else if ((sel & 1) != 0) p_normal = u_normal_alternate * i_normal;\n \
	else p_normal = u_normal * i_normal;						\n \
										\n \
	p_diffuse = i_diffuse;							\n \
	p_ambient = i_ambient;							\n \
	p_specular = i_specular;						\n \
	p_unknown = i_unknown;							\n \
	p_texcoords = i_texcoords;						\n \
	p_alpha = i_alpha;							\n \
}";

/* A spotlight's direction points out from the light. light_direction below
 * points from the shaded surface toward the light, so cone testing negates it. */
static const char *mesh_fs_source =
"#version 150									\n \
										\n \
%s										\n \
										\n \
struct light_t {								\n \
	vec3 position;								\n \
	vec3 direction;								\n \
	vec3 diffuse;								\n \
	vec3 specular;								\n \
	vec2 extents;								\n \
	vec3 spot;\n \
	float spot_table[16];\n \
};										\n \
										\n \
uniform light_t		u_lights[4];						\n \
uniform vec3		u_ambient;						\n \
uniform sampler2D	u_texture;						\n \
uniform vec2		u_fog;							\n \
uniform float		u_alpha_ref;							\n \
uniform float		u_weigh_by_alpha; uniform float u_force_coverage; uniform float u_pattern; uniform float u_lod_bias;	\n \
uniform vec3		u_fog_color;						\n \
uniform int u_peel_enabled; uniform int u_peel_first; uniform vec2 u_peel_size;\n \
uniform sampler2D u_peel_depth; uniform sampler2D u_peel_opaque_depth;\n \
										\n \
in vec4 p_position;								\n \
in vec3 p_normal;								\n \
in vec3 p_diffuse;								\n \
in vec4 p_specular;								\n \
in vec3 p_ambient;								\n \
in vec3 p_unknown;								\n \
in vec2 p_texcoords;								\n \
in float p_alpha;								\n \
										\n \
void										\n \
apply_light (inout vec3 diffuse,						\n \
             inout vec3 specular,						\n \
             in light_t light,							\n \
             in int type,							\n \
             in int att_type,							\n \
             in int has_specular)						\n \
{										\n \
	vec3 light_direction;							\n \
	float distance, attenuation, intensity;					\n \
										\n \
	if (type == 0) {							\n \
		light_direction = normalize (light.direction);			\n \
		distance = 0.001;						\n \
	} else {								\n \
		vec3 delta = light.position - p_position.xyz;			\n \
		distance = length (delta);					\n \
		light_direction = normalize (delta);				\n \
	}									\n \
										\n \
	if (att_type == 1)							\n \
		distance = distance*distance;					\n \
	else if (att_type == 2)							\n \
		distance = 1.0 / distance;					\n \
	else if (att_type == 3)							\n \
		distance = 1.0 / (distance*distance);				\n \
	attenuation = light.extents.x * (light.extents.y + distance);		\n \
	attenuation = clamp (attenuation, 0.0, 1.0);				\n \
										\n \
	vec3 normal = normalize (p_normal);					\n \
										\n \
#if HAS_TWO_SIDED_LIGHTING							\n \
	intensity = abs (dot (normal, light_direction));			\n \
#else										\n \
	intensity = max (dot (normal, light_direction), 0.0);			\n \
#endif										\n \
	if (type == 2 && light.spot.z != 0.0) {\n \
		vec3 spot_direction = normalize (light.direction);\n \
		float u = clamp ((dot (spot_direction, -light_direction) - light.spot.x) * light.spot.y, 0.0, 1.0);\n \
		float t = u * 16.0;\n \
		int index = clamp (int (floor (t)), 0, 15);\n \
		float lo = index == 0 ? 0.0 : light.spot_table[max (index - 1, 0)];\n \
		float hi = light.spot_table[index];\n \
		intensity *= mix (lo, hi, t - float (index));\n \
	}\n \
										\n \
	diffuse += attenuation * intensity * light.diffuse;		\n \
										\n \
	if (has_specular != 0) {								\n \
		vec3 view_direction = normalize (-p_position.xyz);				\n \
		vec3 reflect_direction = reflect (-light_direction, normal);			\n \
		float angle = max (dot (view_direction, reflect_direction), 0.0);		\n \
		specular += p_specular.rgb * light.specular * pow (angle, 255.0 * p_specular.a);	\n \
	}											\n \
}										\n \
										\n \
void										\n \
main (void)									\n \
{										\n \
#if HAS_PATTERN\n \
	/* 501 is a screen-space mask, independent of material alpha. */\n \
	ivec2 dot = ivec2(gl_FragCoord.xy) & 3;\n \
	if (((uint(u_pattern) >> uint((3 - dot.y) * 4 + dot.x)) & 1u) == 0u) discard;\n \
#endif\n \
	if (u_peel_enabled != 0) {\n \
		vec2 peel_uv = gl_FragCoord.xy / u_peel_size;\n \
		float opaque_z = texture(u_peel_opaque_depth, peel_uv).r;\n \
		if (gl_FragCoord.z > opaque_z + 0.000001) discard;\n \
		if (u_peel_first == 0 && gl_FragCoord.z <= texture(u_peel_depth, peel_uv).r + 0.000001) discard;\n \
	}\n \
	vec4 texel = vec4(1.0), color;\n \
	vec3 base;\n \
	float alpha;\n \
#if HAS_TEXTURE\n \
#ifdef GL_ES\n \
	texel = texture(u_texture, p_texcoords, u_lod_bias);\n \
#else\n \
	texel = texture(u_texture, p_texcoords);\n \
#endif\n \
#if ALPHA_MODE\n \
	base = mix(p_diffuse, texel.rgb, p_alpha);\n \
	alpha = texel.a;\n \
#elif TEXTURE_MODE == 1\n \
	base = mix(p_diffuse, texel.rgb, texel.a);\n \
	alpha = p_alpha;\n \
#elif TEXTURE_MODE == 2\n \
	base = texel.rgb * p_ambient;\n \
	alpha = texel.a * p_alpha;\n \
#elif TEXTURE_MODE == 3\n \
	base = p_diffuse;\n \
	alpha = texel.a * p_alpha;\n \
#elif TEXTURE_MODE == 4\n \
	base = mix(p_diffuse, p_ambient, texel.rgb);\n \
	alpha = texel.a * p_alpha;\n \
#else\n \
	base = texel.rgb;\n \
	alpha = texel.a * p_alpha;\n \
#endif\n \
#elif ALPHA_MODE\n \
	base = mix(p_diffuse, p_ambient, p_alpha);\n \
	alpha = 1.0;\n \
#else\n \
	base = p_diffuse;\n \
	alpha = p_alpha;\n \
#endif\n \
#if SHADING_MODE != 0\n \
	vec3 diffuse = vec3(0.0), specular = vec3(0.0);\n \
	/* 691 is ambient/emission in units of 1/255, carried as UNORM16. */\n \
	vec3 ambient = SHADING_MODE == 1 ? u_ambient * p_unknown * 257.0 : vec3(0.0);\n \
	vec3 emission = SHADING_MODE == 2 ? p_unknown * 257.0 : vec3(0.0);\n \
#if HAS_LIGHT0									\n \
	apply_light (diffuse, specular, u_lights[0], LIGHT0_TYPE, LIGHT0_ATT_TYPE, HAS_LIGHT0_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT1									\n \
	apply_light (diffuse, specular, u_lights[1], LIGHT1_TYPE, LIGHT1_ATT_TYPE, HAS_LIGHT1_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT2									\n \
	apply_light (diffuse, specular, u_lights[2], LIGHT2_TYPE, LIGHT2_ATT_TYPE, HAS_LIGHT2_SPECULAR);		\n \
#endif										\n \
#if HAS_LIGHT3									\n \
	apply_light (diffuse, specular, u_lights[3], LIGHT3_TYPE, LIGHT3_ATT_TYPE, HAS_LIGHT3_SPECULAR);		\n \
#endif										\n \
										\n \
	color = vec4((ambient + diffuse + emission) * base, alpha);\n \
#if SHADING_MODE != 3\n \
	color.rgb += specular;\n \
#endif\n \
#else\n \
	color = vec4(base, alpha);\n \
#endif\n \
#if HAS_FOG									\n \
	float z = -p_position.z;				\n \
	float a = clamp (u_fog.x * (z - u_fog.y), 0.0, 1.0);			\n \
	vk_frag_color = mix (color, vec4 (u_fog_color, color.a), a);			\n \
#else										\n \
	vk_frag_color = color;							\n \
#endif										\n \
	if (u_weigh_by_alpha != 0.0) vk_frag_color.rgb *= vk_frag_color.a;	\n \
	if (vk_frag_color.a <= u_alpha_ref) discard;\n \
	if (u_force_coverage != 0.0) vk_frag_color.a = 1.0;\n \
}";

static hikaru_light_att_t
get_light_attenuation_type (hikaru_light_t *lit)
{
	if (lit->att_type == 0 &&
	    lit->attenuation[0] == 1.0f &&
	    lit->attenuation[1] == 1.0f)
		return HIKARU_LIGHT_ATT_INF;
	return lit->att_type;
}

static hikaru_light_type_t
get_light_type (hikaru_light_t *lit)
{
	/* Violated by BRAVEFF, in the intro sequence; it recalls a lightset
	 * at the wrong base, and poof. */
	if (!lit->has_direction && !lit->has_position)
		VK_ERROR ("recalling light with no direction nor position!");
	if (get_light_attenuation_type (lit) == HIKARU_LIGHT_ATT_INF)
		return HIKARU_LIGHT_TYPE_DIRECTIONAL;
	else if (lit->has_direction && lit->has_position)
		return HIKARU_LIGHT_TYPE_SPOT;
	else if (lit->has_position)
		return HIKARU_LIGHT_TYPE_POSITIONAL;
	return HIKARU_LIGHT_TYPE_DIRECTIONAL;
}

static hikaru_glsl_variant_t
get_glsl_variant (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, bool texture_available)
{
	hikaru_material_t *mat;
	hikaru_lightset_t *ls;
	hikaru_glsl_variant_t variant;

	VK_ASSERT (mesh->vp_index != ~0);

	variant.full = 0;
	variant.has_pattern = (mesh->pattern & 15) != 0;

	if (mesh->mat_index == ~0) {
		VK_ERROR ("no material.");
		return variant;
	}
	if (mesh->tex_index == ~0) {
		VK_ERROR ("no texhead.");
		return variant;
	}
	if (mesh->ls_index == ~0) {
		VK_ERROR ("no lightset.");
		return variant;
	}
	VK_ASSERT(mesh->mat_index < hr->num_mats && mesh->ls_index < hr->num_lss);
	mat = &hr->mat_list[mesh->mat_index];
	ls = &hr->ls_list[mesh->ls_index];

	variant.has_texture	= mat->has_texture && texture_available &&
	                          !hr->debug.flags[HR_DEBUG_NO_TEXTURES];

	variant.shading_mode = hr->debug.flags[HR_DEBUG_NO_LIGHTING] ? 0 : mat->shading_mode;
	variant.alpha_mode = mat->has_alpha;
	variant.has_lighting = ls->mask != 0xF && mat->shading_mode != 0 &&
	                       !hr->debug.flags[HR_DEBUG_NO_LIGHTING];
	/* 2C1 selects the texture/material combination independently of the
	 * polygon's blend mode. These modes also apply to unlit geometry. */
	if (variant.has_texture && mesh->tex_index < hr->num_texs) {
		static const unsigned modes[8] = {0, 1, 2, 0, 3, 4, 0, 0};
		variant.texture_mode = modes[hr->tex_list[mesh->tex_index]._2C1 >> 29];
	}

	/* Preserve the existing native shader inputs until its material port;
	 * the OpenGL shader below uses the explicit material modes instead. */
	variant.use_unlit_diffuse = mat->shading_mode == 0 &&
	    !mat->has_texture && !mat->has_alpha;
	variant.use_prelit_color = variant.has_lighting && variant.has_texture && mat->has_691 &&
	    mat->unknown[0] <= 255 && mat->unknown[1] <= 255 && mat->unknown[2] <= 255 &&
	    !(mat->diffuse[0] | mat->diffuse[1] | mat->diffuse[2] |
	      mat->ambient[0] | mat->ambient[1] | mat->ambient[2]);

	/* 881 bit 18 requests fog; 621 bit 17 disables its viewport colour.
	 * The guest supplies a start depth and inverse range in eye space. */
	variant.has_fog = mat->depth_blend && hr->debug.flags[HR_DEBUG_FOG] &&
	                  hr->vp_list[mesh->vp_index].depth.q_enabled;

	if (!variant.has_lighting)
		return variant;

	variant.has_phong		= mat->shading_mode == 2;

	variant.has_light0		= !(ls->mask & (1 << 0));
	variant.light0_type		= get_light_type (&ls->lights[0]);
	variant.light0_att_type		= get_light_attenuation_type (&ls->lights[0]);
	variant.has_light0_specular	= ls->lights[0].has_specular;

	variant.has_light1		= !(ls->mask & (1 << 1));
	variant.light1_type		= get_light_type (&ls->lights[1]);
	variant.light1_att_type		= get_light_attenuation_type (&ls->lights[1]);
	variant.has_light1_specular	= ls->lights[1].has_specular;

	variant.has_light2		= !(ls->mask & (1 << 2));
	variant.light2_type		= get_light_type (&ls->lights[2]);
	variant.light2_att_type		= get_light_attenuation_type (&ls->lights[2]);
	variant.has_light2_specular	= ls->lights[2].has_specular;

	variant.has_light3		= !(ls->mask & (1 << 3));
	variant.light3_type		= get_light_type (&ls->lights[3]);
	variant.light3_att_type		= get_light_attenuation_type (&ls->lights[3]);
	variant.has_light3_specular	= ls->lights[3].has_specular;

	if (mesh->has_two_sided_lighting)
		variant.has_two_sided_lighting = 1;

	return variant;
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_invalid_texture_fallback(void)
{
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    if (!hr || !gpu) { free(hr); free(gpu); return 0; }
    hikaru_material_t material = {0};
    hikaru_lightset_t lights = {0};
    hikaru_mesh_t mesh = {0};
    hikaru_texhead_t head = {0};
    material.has_texture = 1;
    lights.mask = 15;
    head.slotx = 0x80; head.sloty = 0xc0; head.format = 7;
    hr->gpu = gpu;
    hr->mat_list = &material; hr->num_mats = 1;
    hr->ls_list = &lights; hr->num_lss = 1;
    hr->tex_list = &head; hr->num_texs = 1;
    const bool available = get_texture(hr, &head) != NULL;
    const hikaru_glsl_variant_t variant = get_glsl_variant(hr, &mesh, available);
    const int ok = !available && !variant.has_texture && glGetError() == GL_NO_ERROR;
    free(hr); free(gpu);
    return ok;
}
#endif

#define MAX_PROGRAMS	256

typedef struct {
	bool params_valid, enabled, table_valid;
	uint8_t raw[4];
	uint32_t table[8];
} hikaru_spot_uniform_cache_t;

static struct {
	hikaru_glsl_variant_t variant;
	GLuint program;
	hikaru_uniform_locations_t locs;
	hikaru_spot_uniform_cache_t spot[4];
} program_cache[MAX_PROGRAMS];
unsigned num_programs;

#ifdef HIKARU_VENDOR_TESTING
static unsigned uniform_location_queries;
static unsigned state_uploads[3]; /* viewport, model matrices, lightset */
static unsigned spot_param_uploads, spot_table_uploads;
void hikaru_vendor_test_spot_upload_counts(unsigned *params, unsigned *tables) { *params = spot_param_uploads; *tables = spot_table_uploads; }
void hikaru_vendor_test_spot_upload_counts_reset(void) { spot_param_uploads = spot_table_uploads = 0; }
/* Force the production full-cache replacement path for the active variant. */
void hikaru_vendor_test_spot_force_replacement(void) {
    for (unsigned i=0; i<MAX_PROGRAMS; ++i) program_cache[i].variant.full = UINT64_MAX - i;
    num_programs = MAX_PROGRAMS;
}
static bool uniform_cache_bypass;
void hikaru_vendor_test_uniform_cache_bypass(bool enabled) { uniform_cache_bypass = enabled; }
#define GET_MESH_UNIFORM(program, name) \
	(++uniform_location_queries, glGetUniformLocation ((program), (name)))
#else
#define GET_MESH_UNIFORM(program, name) glGetUniformLocation ((program), (name))
#endif

static void
upload_glsl_program (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, bool texture_available)
{
	static const char *definitions_template =
	"#define HAS_TEXTURE %d\n"
	"#define HAS_LIGHTING %d\n"
	"#define HAS_TWO_SIDED_LIGHTING %d\n"
	"#define HAS_PHONG %d\n"
	"#define HAS_LIGHT0 %d\n"
	"#define LIGHT0_TYPE %d\n"
	"#define LIGHT0_ATT_TYPE %d\n"
	"#define HAS_LIGHT0_SPECULAR %d\n"
	"#define HAS_LIGHT1 %d\n"
	"#define LIGHT1_TYPE %d\n"
	"#define LIGHT1_ATT_TYPE %d\n"
	"#define HAS_LIGHT1_SPECULAR %d\n"
	"#define HAS_LIGHT2 %d\n"
	"#define LIGHT2_TYPE %d\n"
	"#define LIGHT2_ATT_TYPE %d\n"
	"#define HAS_LIGHT2_SPECULAR %d\n"
	"#define HAS_LIGHT3 %d\n"
	"#define LIGHT3_TYPE %d\n"
	"#define LIGHT3_ATT_TYPE %d\n"
	"#define HAS_LIGHT3_SPECULAR %d\n"
	"#define HAS_FOG %d\n"
	"#define HAS_PATTERN %d\n"
	"#define SHADING_MODE %d\n"
	"#define ALPHA_MODE %d\n"
	"#define TEXTURE_MODE %d\n"
	"#define USE_UNLIT_DIFFUSE %d\n"
	"#define USE_PRELIT_COLOR %d\n";

	hikaru_glsl_variant_t variant;
	char *definitions, *vs_source, *fs_source;
	int ret, i;
	unsigned slot;

	variant = get_glsl_variant (hr, mesh, texture_available);
	if (hr->meshes.variant.full == variant.full) {
		/* Layer resolve can bind the bitmap program between mesh passes. */
		glUseProgram(hr->meshes.program);
		return;
	}

	/* Layer programs never write these uniforms. A shader/frame transition
	 * invalidates the active-program values before the next mesh uses them. */
	memset(&hr->meshes.uniforms, 0, sizeof(hr->meshes.uniforms));
	hr->meshes.variant.full = variant.full;

	for (i = 0; i < num_programs; i++) {
		if (program_cache[i].variant.full == variant.full) {
			hr->meshes.cache_slot = (unsigned)i;
			hr->meshes.program = program_cache[i].program;
#ifdef HIKARU_VENDOR_TESTING
			if (uniform_cache_bypass) { slot = (unsigned)i; goto update_locations; }
#endif
			hr->meshes.locs = program_cache[i].locs;
			glUseProgram (hr->meshes.program);
			VK_ASSERT_NO_GL_ERROR ();
			return;
		}
	}

	VK_LOG ("compiling shader for variant %" PRIX64 "", variant.full);

	ret = vk_asprintf (&definitions, definitions_template,
	                variant.has_texture,
	                variant.has_lighting,
	                variant.has_two_sided_lighting,
	                variant.has_phong,
	                variant.has_light0,
	                variant.light0_type,
	                variant.light0_att_type,
	                variant.has_light0_specular,
	                variant.has_light1,
	                variant.light1_type,
	                variant.light1_att_type,
	                variant.has_light1_specular,
	                variant.has_light2,
	                variant.light2_type,
	                variant.light2_att_type,
	                variant.has_light2_specular,
	                variant.has_light3,
	                variant.light3_type,
	                variant.light3_att_type,
	                variant.has_light3_specular,
	                variant.has_fog,
	                variant.has_pattern,
	                variant.shading_mode,
	                variant.alpha_mode,
	                variant.texture_mode,
	                variant.use_unlit_diffuse,
	                variant.use_prelit_color);
	if (ret < 0) VK_ABORT("out of memory building shader definitions");

	ret = vk_asprintf (&vs_source, mesh_vs_source, definitions);
	if (ret < 0) { free(definitions); VK_ABORT("out of memory building vertex shader"); }

	ret = vk_asprintf (&fs_source, mesh_fs_source, definitions);
	if (ret < 0) { free(vs_source); free(definitions); VK_ABORT("out of memory building fragment shader"); }

	hr->meshes.variant = variant;
	hr->meshes.program = vk_renderer_compile_program (vs_source, fs_source);

	free (definitions);
	free (vs_source);
	free (fs_source);
	if (!hr->meshes.program) VK_ABORT("could not compile Hikaru shader program");

	if (0) {
		vk_renderer_print_uniforms (hr->meshes.program);
		VK_ASSERT_NO_GL_ERROR ();
	}

	/* Bound the cache even when a game generates more shader variants. */
	slot = num_programs < MAX_PROGRAMS ? num_programs++ : 0;
	if (program_cache[slot].program)
		vk_renderer_destroy_program(program_cache[slot].program);
	memset (program_cache[slot].spot, 0, sizeof(program_cache[slot].spot));
	hr->meshes.cache_slot = slot;
	program_cache[slot].variant.full = variant.full;
	program_cache[slot].program = hr->meshes.program;

#ifdef HIKARU_VENDOR_TESTING
update_locations:
#endif
	glUseProgram (hr->meshes.program);
	VK_ASSERT_NO_GL_ERROR ();

	hr->meshes.locs.u_projection =
		GET_MESH_UNIFORM (hr->meshes.program, "u_projection");
	hr->meshes.locs.u_modelview =
		GET_MESH_UNIFORM (hr->meshes.program, "u_modelview");
	hr->meshes.locs.u_normal =
		GET_MESH_UNIFORM (hr->meshes.program, "u_normal");
	hr->meshes.locs.u_modelview_alternate = GET_MESH_UNIFORM (hr->meshes.program, "u_modelview_alternate");
	hr->meshes.locs.u_normal_alternate = GET_MESH_UNIFORM (hr->meshes.program, "u_normal_alternate");
	for (i = 0; i < 4; i++) {
		char temp[64];

		sprintf (temp, "u_lights[%d].position", i);
		hr->meshes.locs.u_lights[i].position =
			GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].direction", i);
		hr->meshes.locs.u_lights[i].direction =
			GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].diffuse", i);
		hr->meshes.locs.u_lights[i].diffuse =
			GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].specular", i);
		hr->meshes.locs.u_lights[i].specular =
			GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].spot", i);
		hr->meshes.locs.u_lights[i].spot = GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].spot_table[0]", i);
		hr->meshes.locs.u_lights[i].spot_table = GET_MESH_UNIFORM (hr->meshes.program, temp);
		sprintf (temp, "u_lights[%d].extents", i);
		hr->meshes.locs.u_lights[i].extents =
			GET_MESH_UNIFORM (hr->meshes.program, temp);
	}
	hr->meshes.locs.u_ambient =
		GET_MESH_UNIFORM (hr->meshes.program, "u_ambient");
	hr->meshes.locs.u_texture =
		GET_MESH_UNIFORM (hr->meshes.program, "u_texture");
	hr->meshes.locs.u_fog =
		GET_MESH_UNIFORM (hr->meshes.program, "u_fog");
	hr->meshes.locs.u_fog_color =
		GET_MESH_UNIFORM (hr->meshes.program, "u_fog_color");
	hr->meshes.locs.u_alpha_ref =
		GET_MESH_UNIFORM (hr->meshes.program, "u_alpha_ref");
	hr->meshes.locs.u_weigh_by_alpha =
		GET_MESH_UNIFORM (hr->meshes.program, "u_weigh_by_alpha");
	hr->meshes.locs.u_force_coverage =
		GET_MESH_UNIFORM (hr->meshes.program, "u_force_coverage");
	hr->meshes.locs.u_pattern = GET_MESH_UNIFORM (hr->meshes.program, "u_pattern");
	hr->meshes.locs.u_lod_bias = GET_MESH_UNIFORM (hr->meshes.program, "u_lod_bias");
	hr->meshes.locs.u_peel_enabled = GET_MESH_UNIFORM(hr->meshes.program, "u_peel_enabled");
	hr->meshes.locs.u_peel_first = GET_MESH_UNIFORM(hr->meshes.program, "u_peel_first");
	hr->meshes.locs.u_peel_size = GET_MESH_UNIFORM(hr->meshes.program, "u_peel_size");
	hr->meshes.locs.u_peel_depth = GET_MESH_UNIFORM(hr->meshes.program, "u_peel_depth");
	hr->meshes.locs.u_peel_opaque_depth = GET_MESH_UNIFORM(hr->meshes.program, "u_peel_opaque_depth");
	VK_ASSERT_NO_GL_ERROR ();
	program_cache[slot].locs = hr->meshes.locs;
	/* Sampler units are fixed for the lifetime of this linked program. */
	glUniform1i(hr->meshes.locs.u_texture, 0);
	glUniform1i(hr->meshes.locs.u_peel_depth, 1);
	glUniform1i(hr->meshes.locs.u_peel_opaque_depth, 2);
}

#undef GET_MESH_UNIFORM

#ifdef HIKARU_VENDOR_TESTING
/* Exercise two linked variants, including an optimized-out uniform. */
int hikaru_vendor_test_shader_uniform_cache(void)
{
	if (num_programs) return 0;
	hikaru_renderer_t *hr = calloc (1, sizeof(*hr));
	if (!hr) return 0;
	hikaru_material_t material = {0};
	hikaru_lightset_t lightset = {0};
	hikaru_mesh_t mesh = {0};
	hikaru_viewport_t viewport = {0};
	viewport.depth.q_enabled = 1;
	hr->vp_list = &viewport; hr->num_vps = 1;
	material.depth_blend = 1;
	lightset.mask = 0xf;
	hr->mat_list = &material; hr->num_mats = 1;
	hr->ls_list = &lightset; hr->num_lss = 1;
	hr->meshes.variant.full = ~0ULL;
	GLint previous_program, current_program;
	glGetIntegerv (GL_CURRENT_PROGRAM, &previous_program);
	uniform_location_queries = 0;
	upload_glsl_program (hr, &mesh, true);
	const GLuint first_program = hr->meshes.program;
	const hikaru_uniform_locations_t first = hr->meshes.locs;
	int ok = uniform_location_queries == 47 && first.u_texture == (GLuint)-1;
	material.has_texture = 1;
	upload_glsl_program (hr, &mesh, true);
	const GLuint second_program = hr->meshes.program;
	const hikaru_uniform_locations_t second = hr->meshes.locs;
	ok &= second_program != first_program && uniform_location_queries == 94;
	ok &= second.u_texture == (GLuint)glGetUniformLocation (second_program, "u_texture");
	material.has_texture = 0;
	upload_glsl_program (hr, &mesh, true);
	glGetIntegerv (GL_CURRENT_PROGRAM, &current_program);
	ok &= hr->meshes.program == first_program && current_program == (GLint)first_program;
	ok &= !memcmp (&hr->meshes.locs, &first, sizeof(first));
	ok &= uniform_location_queries == 94;
	material.has_texture = 1;
	upload_glsl_program (hr, &mesh, true);
	ok &= hr->meshes.program == second_program && !memcmp (&hr->meshes.locs, &second, sizeof(second));
	ok &= uniform_location_queries == 94;
	/* Fill the unused entries without compiling hundreds of shaders, then
	 * exercise the real full-cache replacement and subsequent cache hit. */
	for (unsigned i = 2; i < MAX_PROGRAMS; ++i)
		program_cache[i].variant.full = UINT64_MAX - i;
	num_programs = MAX_PROGRAMS;
	hr->debug.flags[HR_DEBUG_FOG] = 1;
	upload_glsl_program (hr, &mesh, true);
	const GLuint third_program = hr->meshes.program;
	ok &= uniform_location_queries == 141 && !glIsProgram (first_program);
	hr->debug.flags[HR_DEBUG_FOG] = 0;
	upload_glsl_program (hr, &mesh, true);
	ok &= hr->meshes.program == second_program && uniform_location_queries == 141;
	material.has_texture = 0;
	upload_glsl_program (hr, &mesh, true);
	ok &= uniform_location_queries == 188 && !glIsProgram (third_program);
	ok &= hr->meshes.locs.u_texture == (GLuint)-1 && glGetError() == GL_NO_ERROR;
	glUseProgram ((GLuint)previous_program);
	for (unsigned i = 0; i < num_programs; ++i)
		vk_renderer_destroy_program (program_cache[i].program);
	memset (program_cache, 0, sizeof(program_cache));
	num_programs = 0;
	free (hr);
	return ok;
}
#endif

static void
viewport_projection (mtx4x4f_t projection, const hikaru_viewport_t *vp)
{
	/* Offset relative to the clip origin for nonzero clip origins is not yet
	 * verified; the BIOS and game cases tested use a (0,0) clip origin. */
	const float w = vp->clip.r - vp->clip.l, h = vp->clip.t - vp->clip.b;
	const float scale = vp->clip.n / vp->clip.f;
	const float hh = (h / 2.0f) * scale;
	const float hw = hh * (w / h);
	const float dx = (vp->offset.x - w / 2.0f) * scale;
	const float dy = (vp->offset.y - h / 2.0f) * scale;
	vk_renderer_frustum (projection,
	                     -hw - dx, hw - dx, -hh + dy, hh + dy,
	                     vp->clip.n, 1e5);
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_viewport_origin(void)
{
	hikaru_viewport_t vp = {0};
	mtx4x4f_t p;
	vp.clip.r = 640; vp.clip.t = 480; vp.clip.f = 1280; vp.clip.n = 0.1f;
	viewport_projection (p, &vp);
	/* A guest point on the optical axis maps to the top-left origin. */
	float x = (1.0f - p[2][0]) * 320.0f;
	float y = (1.0f + p[2][1]) * 240.0f;
	if (fabsf(x) > 0.01f || fabsf(y) > 0.01f) return 0;
	/* A centred origin preserves the existing centred gameplay projection. */
	vp.offset.x = 320; vp.offset.y = 240;
	viewport_projection (p, &vp);
	return fabsf(p[2][0]) < 0.0001f && fabsf(p[2][1]) < 0.0001f;
}
#endif

static void
upload_viewport (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	VK_ASSERT (mesh->vp_index < hr->num_vps);
	hikaru_viewport_t *vp = &hr->vp_list[mesh->vp_index];
	const float sx = hr->base.internal_width / 640.0f;
	const float sy = hr->base.internal_height / 480.0f;
	const GLint left = (GLint) lroundf(vp->clip.l * sx);
	const GLint bottom = (GLint) lroundf(vp->clip.b * sy);
	const GLint right = (GLint) lroundf(vp->clip.r * sx);
	const GLint top = (GLint) lroundf(vp->clip.t * sy);
	glViewport (left, bottom, right - left, top - bottom);
	/* The viewport rectangle is context state and may have been changed by
	 * a layer pass. Only the program-local projection/fog values are cached. */
	hikaru_mesh_uniform_cache_t *cache = &hr->meshes.uniforms;
	if (cache->viewport_valid && cache->alpha_ref == hr->pass_alpha_ref &&
	    !memcmp(&cache->viewport, vp, sizeof(*vp))) return;
	cache->viewport = *vp;
	cache->alpha_ref = hr->pass_alpha_ref;
	cache->viewport_valid = true;
#ifdef HIKARU_VENDOR_TESTING
	++state_uploads[0];
#endif
	const float h = vp->clip.t - vp->clip.b;
	const float w = vp->clip.r - vp->clip.l;
	const float dcx = (vp->offset.x - (w / 2.0f));
	const float dcy = (vp->offset.y - (h / 2.0f));
	mtx4x4f_t projection;

	VK_ASSERT (mesh->vp_index != ~0);

	if (!isnonnegative (vp->clip.l) || !isnonnegative (vp->clip.r) ||
	    !isnonnegative (vp->clip.b) || !isnonnegative (vp->clip.t) ||
	    !isnonnegative (vp->clip.f) || !isnonnegative (vp->clip.n)) {
		VK_ERROR ("non-positive viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if ((vp->clip.l >= vp->clip.r) ||
	    (vp->clip.b >= vp->clip.t) ||
	    (vp->clip.n >= vp->clip.f)) {
		VK_ERROR ("inverted viewport clipping planes: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	if (!isnonnegative (vp->offset.x) || (vp->offset.x >= 640.0f) ||
	    !isnonnegative (vp->offset.y) || (vp->offset.y >= 480.0f)) {
		VK_ERROR ("invalid viewport offset: %s",
		          get_viewport_str (vp));
		/* continue anyway */
	}

	LOG ("vp  = %s : [w=%f h=%f dcx=%f dcy=%f]",
	     get_viewport_str (vp), w, h, dcx, dcy);

	/* Guest offsets specify the projection centre in top-left coordinates. */
	viewport_projection (projection, vp);

	glUniformMatrix4fv (hr->meshes.locs.u_projection, 1, GL_FALSE,
	                    (const GLfloat *) projection);


	if (hr->meshes.variant.has_fog) {
		vec2f_t fog;
		vec3f_t fog_color;

		/* 621 stores P = 1/(end-start), Q = near/end, not a start
		 * distance. Decode it as Odin's view_fog before shading. */
		fog[0] = fog[1] = 0.0f;
		if (vp->depth.density > 0.0f && vp->depth.bias > 0.0f) {
			fog[0] = vp->depth.density;
			fog[1] = vp->clip.n / vp->depth.bias - 1.0f / vp->depth.density;
		}
		glUniform2fv (hr->meshes.locs.u_fog, 1, fog);

		fog_color[0] = vp->depth.mask[0] * INV255;
		fog_color[1] = vp->depth.mask[1] * INV255;
		fog_color[2] = vp->depth.mask[2] * INV255;
		glUniform3fv (hr->meshes.locs.u_fog_color, 1, fog_color);
	}
	glUniform1f (hr->meshes.locs.u_alpha_ref, hr->pass_alpha_ref);
}

static void
upload_modelview (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, unsigned i)
{
	hikaru_modelview_t *mv;
	mtx3x3f_t normal;

	if (mesh->mv_index == ~0) {
		static const hikaru_modelview_t identity_mv = {
			.mtx = {
				{ 1.0f, 0.0f, 0.0f, 0.0f },
				{ 0.0f, 1.0f, 0.0f, 0.0f },
				{ 0.0f, 0.0f, 1.0f, 0.0f },
				{ 0.0f, 0.0f, 0.0f, 1.0f }
			}
		};

		VK_ERROR ("attempting to draw with no modelview!");

		/* Attempt to render something anyway. */
		mv = (hikaru_modelview_t *) &identity_mv;
	}
	else {
		VK_ASSERT (mesh->mv_index < hr->num_mvs && i < hr->num_mvs - mesh->mv_index);
		mv = &hr->mv_list[mesh->mv_index + i];
	}

	hikaru_mesh_uniform_cache_t *cache = &hr->meshes.uniforms;
	if (!cache->modelview_valid[0] || memcmp(cache->modelview[0], mv->mtx, sizeof(mv->mtx))) {
		vk_renderer_compute_normal_matrix(normal, mv->mtx);
		glUniformMatrix4fv(hr->meshes.locs.u_modelview, 1, GL_FALSE, (const GLfloat *)mv->mtx);
		glUniformMatrix3fv(hr->meshes.locs.u_normal, 1, GL_FALSE, (const GLfloat *)normal);
		memcpy(cache->modelview[0], mv->mtx, sizeof(mv->mtx));
		cache->modelview_valid[0] = true;
#ifdef HIKARU_VENDOR_TESTING
		++state_uploads[1];
#endif
	}
	/* Bit 12 selects the alternate matrix; without one it uses the primary.
	 * Cache both values independently, including that fallback transition. */
	hikaru_modelview_t *alternate = mv;
	if (mesh->alternate_mv_index != ~0u) {
		VK_ASSERT(mesh->alternate_mv_index < hr->num_alternate_mvs);
		alternate = &hr->alternate_mv_list[mesh->alternate_mv_index];
	}
	if (!cache->modelview_valid[1] || memcmp(cache->modelview[1], alternate->mtx, sizeof(alternate->mtx))) {
		vk_renderer_compute_normal_matrix(normal, alternate->mtx);
		glUniformMatrix4fv(hr->meshes.locs.u_modelview_alternate, 1, GL_FALSE, (const GLfloat *)alternate->mtx);
		glUniformMatrix3fv(hr->meshes.locs.u_normal_alternate, 1, GL_FALSE, (const GLfloat *)normal);
		memcpy(cache->modelview[1], alternate->mtx, sizeof(alternate->mtx));
		cache->modelview_valid[1] = true;
#ifdef HIKARU_VENDOR_TESTING
		++state_uploads[1];
#endif
	}
}

#ifdef HIKARU_VENDOR_TESTING
static unsigned sampling_updates;
#endif

static void
apply_texture_sampling(hikaru_texture_t *tex, const hikaru_texhead_t *wanted, GLuint lod_bias_location)
{
	const uint32_t sampling_mask = (3u << 16) | (0xffu << 20);
	const float bias = -0.5f + (float)(int8_t)((wanted->_0C1 >> 20) & 0xff) * 0.125f;
	const bool gles = vk_renderer_is_gles();
	/* Filter and desktop LOD bias belong to the texture object, so repeated
	 * sorted draws need no driver updates until this texhead changes them. */
	if (!tex->sampling_valid || ((tex->th._0C1 ^ wanted->_0C1) & sampling_mask)) {
		const unsigned filter = (wanted->_0C1 >> 16) & 3;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter == 0 ? GL_NEAREST : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		                filter == 0 ? GL_NEAREST : filter == 2 ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR);
		if (!gles) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, bias);
		tex->th._0C1 = wanted->_0C1;
		tex->sampling_valid = true;
#ifdef HIKARU_VENDOR_TESTING
		++sampling_updates;
#endif
	}
	/* ES LOD bias is a program uniform, not texture state. It must still be
	 * restored when switching between programs or texture objects. */
	if (gles && lod_bias_location != (GLuint)-1)
		glUniform1f(lod_bias_location, bias);
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_texture_sampling_cache(void)
{
	GLuint ids[2];
	hikaru_texture_t textures[2] = {0};
	hikaru_texhead_t wanted = {0};
	GLint filter;
	GLfloat bias;
	const unsigned before = sampling_updates;
	glGenTextures(2, ids);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ids[0]);
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
	int ok = sampling_updates == before + 1 && filter == GL_NEAREST;
	glBindTexture(GL_TEXTURE_2D, ids[1]);
	wanted._0C1 = (1u << 16) | (20u << 20);
	apply_texture_sampling(&textures[1], &wanted, (GLuint)-1);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
	ok &= sampling_updates == before + 2 && filter == GL_LINEAR_MIPMAP_LINEAR;
	glBindTexture(GL_TEXTURE_2D, ids[0]);
	wanted._0C1 = 0;
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
	ok &= sampling_updates == before + 2 && filter == GL_NEAREST;
	wanted._0C1 = (2u << 16) | (255u << 20);
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
	ok &= sampling_updates == before + 3 && filter == GL_LINEAR;
	if (!vk_renderer_is_gles()) {
		glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &bias);
		ok &= fabsf(bias + 0.625f) < 0.0001f;
	}
	/* Bias can change without changing the minification filter. */
	wanted._0C1 = (2u << 16) | (20u << 20);
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	ok &= sampling_updates == before + 4;
	if (!vk_renderer_is_gles()) {
		glGetTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, &bias);
		ok &= fabsf(bias - 2.0f) < 0.0001f;
	}
	/* Destroying/recreating a texture clears this per-object cache. */
	memset(&textures[0], 0, sizeof(textures[0]));
	apply_texture_sampling(&textures[0], &wanted, (GLuint)-1);
	ok &= sampling_updates == before + 5 && glGetError() == GL_NO_ERROR;
	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(2, ids);
	return ok;
}
#endif

static void
upload_material_texhead (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_texture_t *tex;

	if (!hr->meshes.variant.has_texture)
		return;

	tex = get_texture (hr, &hr->tex_list[mesh->tex_index]);

	glActiveTexture (GL_TEXTURE0 + 0);
	VK_ASSERT_NO_GL_ERROR ();

	glBindTexture (GL_TEXTURE_2D, tex ? tex->id : 0);
	VK_ASSERT_NO_GL_ERROR ();
	if (tex && texture_wrap_changed (&tex->th, &hr->tex_list[mesh->tex_index])) {
		hikaru_texhead_t *wanted = &hr->tex_list[mesh->tex_index];
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, texture_wrap(wanted->wrapu, wanted->repeatu));
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, texture_wrap(wanted->wrapv, wanted->repeatv));
		tex->th.wrapu = wanted->wrapu; tex->th.wrapv = wanted->wrapv;
		tex->th.repeatu = wanted->repeatu; tex->th.repeatv = wanted->repeatv;
		hr->textures.wrap_updates++;
		VK_ASSERT_NO_GL_ERROR ();
	}

	if (tex) {
		apply_texture_sampling(tex, &hr->tex_list[mesh->tex_index], hr->meshes.locs.u_lod_bias);
		VK_ASSERT_NO_GL_ERROR ();
	}

}

static void
get_light_ambient (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, float *out)
{
	hikaru_viewport_t *vp =
		(mesh->vp_index == ~0) ? NULL : &hr->vp_list[mesh->vp_index];

	if (hr->debug.flags[HR_DEBUG_NO_AMBIENT] || !vp)
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = vp->color.ambient[0] * INV255;
		out[1] = vp->color.ambient[1] * INV255;
		out[2] = vp->color.ambient[2] * INV255;
	}
}

static void
get_light_diffuse (hikaru_renderer_t *hr, hikaru_light_t *lit, float *out)
{
	if (hr->debug.flags[HR_DEBUG_NO_DIFFUSE])
		out[0] = out[1] = out[2] = 0.0f;
	else {
		out[0] = lit->diffuse[0] * INV255;
		out[1] = lit->diffuse[1] * INV255;
		out[2] = lit->diffuse[2] * INV255;
	}
}

static void
get_light_specular (hikaru_renderer_t *hr, hikaru_light_t *lit, float *out)
{
	if (!lit->has_specular || hr->debug.flags[HR_DEBUG_NO_SPECULAR])
		out[0] = out[1] = out[2] = 0.0f;
	else {
		/* Make sure that specular enabled matches non-zero specular
		 * colors. */
		if (lit->specular[0] == 0.0f &&
		    lit->specular[1] == 0.0f &&
		    lit->specular[2] == 0.0f) {
			/* This state is common in Planet Harriers; warn once instead
			 * of flooding stderr on every affected draw. */
			static bool warned = false;
			if (!warned) { warned = true; VK_ERROR ("light with specular enabled but no specular color!"); }
		}

		out[0] = lit->specular[0] * INV255;
		out[1] = lit->specular[1] * INV255;
		out[2] = lit->specular[2] * INV255;
	}
}

static void
upload_lightset (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_lightset_t *ls;
	float tmp[4];
	unsigned i;

	if (!hr->meshes.variant.has_lighting)
		return;
	VK_ASSERT (mesh->ls_index < hr->num_lss);
	ls = &hr->ls_list[mesh->ls_index];

	LOG ("lightset = %s", get_lightset_str (ls));

	hikaru_mesh_uniform_cache_t *cache = &hr->meshes.uniforms;
	get_light_ambient (hr, mesh, tmp);
	if (!cache->ambient_valid || memcmp(cache->ambient, tmp, sizeof(cache->ambient))) {
		glUniform3fv(hr->meshes.locs.u_ambient, 1, tmp);
		memcpy(cache->ambient, tmp, sizeof(cache->ambient));
		cache->ambient_valid = true;
	}
	const unsigned debug_flags = (hr->debug.flags[HR_DEBUG_NO_DIFFUSE] ? 1u : 0u) |
	                             (hr->debug.flags[HR_DEBUG_NO_SPECULAR] ? 2u : 0u);
	const bool changed = !cache->lightset_valid || cache->light_debug_flags != debug_flags ||
	                     memcmp(&cache->lightset, ls, sizeof(*ls));
	cache->lightset = *ls;
	cache->light_debug_flags = debug_flags;
	cache->lightset_valid = true;
#ifdef HIKARU_VENDOR_TESTING
	if (changed) ++state_uploads[2];
#endif
	for (i = 0; i < 4; i++) {
		hikaru_light_t *lt = &ls->lights[i];

		if (ls->mask & (1 << i))
			continue;

		if (changed) {
			glUniform3fv (hr->meshes.locs.u_lights[i].position, 1,
			              lt->position);
			VK_ASSERT_NO_GL_ERROR ();

			glUniform3fv (hr->meshes.locs.u_lights[i].direction, 1,
			              lt->direction);
			VK_ASSERT_NO_GL_ERROR ();

			get_light_diffuse (hr, lt, tmp);
			glUniform3fv (hr->meshes.locs.u_lights[i].diffuse, 1, tmp);
			VK_ASSERT_NO_GL_ERROR ();

			get_light_specular (hr, lt, tmp);
			glUniform3fv (hr->meshes.locs.u_lights[i].specular, 1, tmp);
			VK_ASSERT_NO_GL_ERROR ();

			glUniform2fv (hr->meshes.locs.u_lights[i].extents, 1,
			              lt->attenuation);

		}

		/* Spotlight LUTs are draw-time state, independent of the light snapshot.
		 * Air Trix's 451 builder stores a Q1.15 cosine and the high eight
		 * bits of (float_bits(1.0) - float_bits(cos(inner)-cos(outer))).
		 * Interpret that reciprocal approximation as four exponent and
		 * four mantissa bits. The legacy specular alias is unchanged. */
		if (hr->meshes.locs.u_lights[i].spot != (GLuint)-1) {
			hikaru_spot_uniform_cache_t *cache = &program_cache[hr->meshes.cache_slot].spot[i];
			bool enabled = lt->has_451 && lt->has_specular &&
			               get_light_type (lt) == HIKARU_LIGHT_TYPE_SPOT;
			if (!cache->params_valid || cache->enabled != enabled ||
			    (enabled && memcmp (cache->raw, lt->spot_raw, sizeof(cache->raw)))) {
				float spot[3] = {0};
				if (enabled) {
					unsigned cutoff = lt->spot_raw[0] | ((unsigned)lt->spot_raw[1] << 8);
					unsigned code = lt->spot_raw[2];
					spot[0] = cutoff / 32768.0f;
					spot[1] = ldexpf (1.0f + (code & 15) / 16.0f, code >> 4);
					spot[2] = 1.0f;
					memcpy (cache->raw, lt->spot_raw, sizeof(cache->raw));
				}
				glUniform3fv (hr->meshes.locs.u_lights[i].spot, 1, spot);
#ifdef HIKARU_VENDOR_TESTING
				++spot_param_uploads;
#endif
				cache->enabled = enabled;
				cache->params_valid = true;
			}
			/* A disabled cone does not read its LUT. Retaining its prior value
			 * avoids an upload when the same table is enabled again. */
			if (enabled && hr->meshes.locs.u_lights[i].spot_table != (GLuint)-1) {
				uint32_t words[8];
				/* Bank = light slot and the Q10 sample interpretation are provisional;
				 * tables reflect draw-time state, not mid-frame snapshots. */
				unsigned curve = (lt->spot_raw[3] >> 1) & 3;
				for (unsigned j = 0; j < 8; ++j)
					words[j] = hr->gpu->state.light_table[i][curve * 8 + j].full;
				if (!cache->table_valid || memcmp (cache->table, words, sizeof(words))) {
					float table[16];
					for (unsigned j = 0; j < 16; ++j)
						table[j] = ((words[j / 2] >> ((j & 1) ? 0 : 16)) & 0xffff) / 1024.0f;
					glUniform1fv (hr->meshes.locs.u_lights[i].spot_table, 16, table);
#ifdef HIKARU_VENDOR_TESTING
					++spot_table_uploads;
#endif
					memcpy (cache->table, words, sizeof(words));
					cache->table_valid = true;
				}
			}
		}
	}
}

#ifdef HIKARU_VENDOR_TESTING
/* Query real linked-program values after cache hits, state changes and shader
 * switches. Upload counts also ensure identical draws actually skip the work. */
int hikaru_vendor_test_mesh_uniform_values(void)
{
	if (num_programs) return 0;
	hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
	if (!hr) return 0;
	hikaru_viewport_t vp = {0};
	hikaru_modelview_t mv = {0}, alternate = {0};
	hikaru_material_t material = {0};
	hikaru_lightset_t lights = {0};
	hikaru_mesh_t mesh = {0};
	GLint previous_program, viewport[4];
	GLfloat matrix[16], value[4];
	glGetIntegerv(GL_CURRENT_PROGRAM, &previous_program);
	hr->vp_list = &vp; hr->num_vps = 1;
	hr->mv_list = &mv; hr->num_mvs = 1;
	hr->alternate_mv_list = &alternate; hr->num_alternate_mvs = 1;
	hr->mat_list = &material; hr->num_mats = 1;
	hr->ls_list = &lights; hr->num_lss = 1;
	hr->base.internal_width = 640; hr->base.internal_height = 480;
	hr->meshes.variant.full = ~UINT64_C(0);
	hr->debug.flags[HR_DEBUG_FOG] = 1;
	vp.clip.r = 640; vp.clip.t = 480; vp.clip.n = 1; vp.clip.f = 2;
	vp.offset.x = 320; vp.offset.y = 240;
	vp.depth.q_enabled = 1; vp.depth.density = 0.01f; vp.depth.bias = 0.1f;
	material.shading_mode = 2; material.depth_blend = 1; material.has_alpha = 1;
	lights.mask = 14; lights.lights[0].has_direction = 1;
	lights.lights[0].direction[2] = -1; lights.lights[0].diffuse[0] = 128;
	for (unsigned i = 0; i < 4; ++i) mv.mtx[i][i] = alternate.mtx[i][i] = 1.0f;
	memset(state_uploads, 0, sizeof(state_uploads));
	upload_glsl_program(hr, &mesh, false);
	for (unsigned repeat = 0; repeat < 3; ++repeat) {
		upload_viewport(hr, &mesh); upload_modelview(hr, &mesh, 0); upload_lightset(hr, &mesh);
	}
	int ok = state_uploads[0] == 1 && state_uploads[1] == 2 && state_uploads[2] == 1;
	/* Program cache hits must still restore context-global viewport state. */
	glViewport(1, 2, 3, 4);
	hr->base.internal_width = 1280; hr->base.internal_height = 960;
	upload_viewport(hr, &mesh);
	glGetIntegerv(GL_VIEWPORT, viewport);
	ok &= state_uploads[0] == 1 && viewport[0] == 0 && viewport[1] == 0 &&
	      viewport[2] == 1280 && viewport[3] == 960;
	vp.offset.x = 240; vp.color.ambient[0] = 32;
	mv.mtx[3][0] = 5; alternate.mtx[3][1] = 7;
	lights.lights[0].diffuse[0] = 64; hr->pass_alpha_ref = 0.5f;
	for (unsigned repeat = 0; repeat < 2; ++repeat) {
		upload_viewport(hr, &mesh); upload_modelview(hr, &mesh, 0); upload_lightset(hr, &mesh);
	}
	ok &= state_uploads[0] == 2 && state_uploads[1] == 4 && state_uploads[2] == 2;
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_projection, matrix);
	ok &= fabsf(matrix[8] - 0.25f) < 0.0001f;
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_modelview, matrix);
	ok &= matrix[12] == 5.0f;
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_modelview_alternate, matrix);
	ok &= matrix[13] == 7.0f;
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_alpha_ref, value);
	ok &= value[0] == 0.5f;
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_ambient, value);
	ok &= fabsf(value[0] - 32.0f / 255.0f) < 0.0001f;
	hr->debug.flags[HR_DEBUG_NO_DIFFUSE] = 1;
	upload_lightset(hr, &mesh);
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_lights[0].diffuse, value);
	ok &= value[0] == 0.0f && state_uploads[2] == 3;
	hr->debug.flags[HR_DEBUG_NO_DIFFUSE] = 0;
	upload_lightset(hr, &mesh);
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_lights[0].diffuse, value);
	ok &= fabsf(value[0] - 64.0f / 255.0f) < 0.0001f;
	mesh.alternate_mv_index = ~0u;
	upload_modelview(hr, &mesh, 0);
	glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_modelview_alternate, matrix);
	ok &= matrix[12] == 5.0f && matrix[13] == 0.0f && state_uploads[1] == 5;
	/* A different program starts with no valid values, including cache hits
	 * when switching back to a shader used earlier in the same frame. */
	for (unsigned fog = 0; fog < 2; ++fog) {
		material.depth_blend = fog;
		upload_glsl_program(hr, &mesh, false);
		upload_viewport(hr, &mesh); upload_modelview(hr, &mesh, 0); upload_lightset(hr, &mesh);
		glGetUniformfv(hr->meshes.program, hr->meshes.locs.u_modelview, matrix);
		ok &= matrix[12] == 5.0f;
	}
	ok &= glGetError() == GL_NO_ERROR;
	glUseProgram((GLuint)previous_program);
	for (unsigned i = 0; i < num_programs; ++i)
		vk_renderer_destroy_program(program_cache[i].program);
	memset(program_cache, 0, sizeof(program_cache));
	num_programs = 0;
	free(hr);
	return ok;
}
#endif

/****************************************************************************
 Meshes
****************************************************************************/

#define VK_COPY_VEC2F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
	} while (0)

#define VK_COPY_VEC3F(dst_, src_) \
	do { \
		dst_[0] = src_[0]; \
		dst_[1] = src_[1]; \
		dst_[2] = src_[2]; \
	} while (0)

static float
clampf (float x, float min_, float max_)
{
	return (min_ > x) ? min_ :
	       (max_ < x) ? max_ : x;
}

static void
copy_colors (hikaru_renderer_t *hr, hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	hikaru_gpu_t *gpu = hr->gpu;
	hikaru_material_t *mat = &MAT.scratch;
	float alpha;
	unsigned i;

	for (i = 0; i < 3; i++)
		dst->body.diffuse[i] = mat->diffuse[i];

	for (i = 0; i < 3; i++)
		dst->body.ambient[i] = mat->ambient[i];

	for (i = 0; i < 4; i++)
		dst->body.specular[i] = mat->specular[i];

	for (i = 0; i < 3; i++)
		dst->body.unknown[i] = mat->unknown[i];

	/* Patch diffuse alpha depending on poly type. NOTE: transparent
	 * polygons also have an alpha, with unknown meaning (it seems to have
	 * opposite sign w.r.t. translucent alpha though). */
	alpha = 1.0f;
	if (POLY.type == HIKARU_POLYTYPE_TRANSLUCENT ||
	    POLY.type == HIKARU_POLYTYPE_BACKGROUND || mat->has_alpha) {
		float p_alpha = POLY.alpha;
		float v_alpha = src->info.alpha * INV255;
		alpha = clampf (p_alpha * v_alpha, 0.0f, 1.0f);
	}
	dst->body.alpha = (uint8_t) (alpha * 255.0f);
}

static void
copy_texcoords (hikaru_renderer_t *hr,
                hikaru_vertex_t *dst, hikaru_vertex_t *src)
{
	hikaru_gpu_t *gpu = hr->gpu;
	hikaru_texhead_t *th = &TEX.scratch;
	float w = 16 << th->logw;
	float h = 16 << th->logh;

	if (th->format == HIKARU_FORMAT_ABGR1111) {
		w *= 2;
		h *= 2;
	}

	/* Vertex UVs are 12.4 unless the texhead says whole texels (2C1
	 * bit 14); the 0D1 offset the vertex selects (header bits 18-19) is
	 * added in 1/16 of a texel. */
	const float k = ((th->_2C1 >> 14) & 1) ? 16.0f : 1.0f;
	float u = src->body.texcoords[0] * k;
	float v = src->body.texcoords[1] * k;
	const unsigned uvsel = (src->info.full >> 18) & 3;
	if (uvsel) {
		u += gpu->texoffset[uvsel][0] * (1.0f / 16.0f);
		v += gpu->texoffset[uvsel][1] * (1.0f / 16.0f);
	}
	dst->body.texcoords[0] = u / w;
	dst->body.texcoords[1] = v / h;
}

static void
add_triangle (hikaru_renderer_t *hr)
{
	if (hr->push.num_verts >= 3) {
		uint32_t index = hr->push.num_tris * 3;
		unsigned needed = hr->push.tmp[2].info.nocull ? 6 : 3;
		VK_ASSERT (index <= MAX_VERTICES_PER_MESH && needed <= MAX_VERTICES_PER_MESH - index);
		hikaru_vertex_body_t *dst = &hr->push.all[index];

		if (hr->push.tmp[2].info.twosided &&
		    !hr->push.tmp[2].info.nocull)
			VK_ERROR ("got a vertex with culling and two-sided lighting!");

		if (hr->push.tmp[2].info.twosided)
			hr->meshes.current->has_two_sided_lighting = 1;

		if (hr->push.tmp[2].info.winding) {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[2].body;
			dst[2] = hr->push.tmp[1].body;
		} else {
			dst[0] = hr->push.tmp[0].body;
			dst[1] = hr->push.tmp[1].body;
			dst[2] = hr->push.tmp[2].body;
		}
		if (hr->push.tmp[2].info.nocull) {
			/* Wind the front before duplicating it. Ignoring the winding
			 * bit here assigns reversed normals to alternate strip faces,
			 * leaving triangular dark patches in lit smoke and characters. */
			dst[3] = dst[0];
			dst[4] = dst[2];
			dst[5] = dst[1];
			for (unsigned back = 3; back < 6; ++back)
				for (unsigned axis = 0; axis < 3; ++axis)
					dst[back].normal[axis] = -dst[back].normal[axis];
			hr->push.num_tris += 1;
		}
		hr->push.num_tris += 1;
	}
}

void
hikaru_renderer_push_vertices (vk_renderer_t *rend,
                               hikaru_vertex_t *v,
                               uint32_t flags,
                               unsigned num)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned i, vp_index = VP0.depth.func;

	VK_ASSERT (hr);
	VK_ASSERT (v);
	VK_ASSERT (num == 1 || num == 3);
	VK_ASSERT (v->info.tricap == 0 || v->info.tricap == 7);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] != vp_index)
		return;

	switch (num) {

	case 1:
		/* Note that VTX(2) always points to the last pushed vertex,
		 * which for instructions 12x, 1Ax and 1Bx means the vertex
		 * pushed by the instruction itself, and for instructions 1Ex
		 * and 15x the vertex pushed by the previous "push"
		 * instruction.
		 */

		/* If the incoming vertex includes the position, push it
		 * in the temporary buffer, updating it according to the
		 * p(osition)pivot bit. */
		if (flags & HR_PUSH_POS) {

			/* Do not change the pivot if it is not required */
			if (!v->info.ppivot)
				hr->push.tmp[0] = hr->push.tmp[1];
			hr->push.tmp[1] = hr->push.tmp[2];
			memset ((void *) &hr->push.tmp[2], 0, sizeof (hikaru_vertex_t));

			/* Set the position, colors and alpha. */
			VK_COPY_VEC3F (hr->push.tmp[2].body.position, v->body.position);
			copy_colors (hr, &hr->push.tmp[2], v);
			/* 1: second modelview, +64 without its 3x3, +128 without
			 * its last row (header bits 12, 13, 14). */
			hr->push.tmp[2].body.matrix_select =
			    (uint8_t) ((v->info.unknown1 ? 1 : 0) +
			               (((v->info.full >> 13) & 1) ? 0 : 64) +
			               (((v->info.full >> 14) & 1) ? 0 : 128));

			/* Account for the added vertex. */
			hr->push.num_verts += 1;
			VK_ASSERT (hr->push.num_verts < MAX_VERTICES_PER_MESH);
		}

		/* Set the normal. */
		if (flags & HR_PUSH_NRM)
			VK_COPY_VEC3F (hr->push.tmp[2].body.normal, v->body.normal);

		/* Set the texcoords. */
		if (flags & HR_PUSH_TXC)
			copy_texcoords (hr, &hr->push.tmp[2], v);
		break;

	case 3:
		VK_ASSERT (flags == HR_PUSH_TXC);

		if (hr->push.num_verts < 3)
			return;

		for (i = 0; i < 3; i++)
			copy_texcoords (hr, &hr->push.tmp[2 - i], &v[i]);
		break;

	default:
		VK_ASSERT (!"num is neither 1 nor 3");
		break;
	}

	/* Finish the previous triangle. */
	if (v[0].info.tricap == 7) {
		hr->push.tmp[2].info.full = v[0].info.full;
		add_triangle (hr);
	}
}

#ifdef HIKARU_VENDOR_TESTING
/* The submitted normal must agree with the face selected by the winding
 * bit. Check the production baker, including the reversed back-face copy. */
int hikaru_vendor_test_triangle_winding(unsigned winding, unsigned nocull)
{
	hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
	if (!hr) return 0;
	hr->push.num_verts = 3;
	hr->push.tmp[1].body.position[winding ? 1 : 0] = 1.0f;
	hr->push.tmp[2].body.position[winding ? 0 : 1] = 1.0f;
	for (unsigned i = 0; i < 3; ++i)
		hr->push.tmp[i].body.normal[2] = 1.0f;
	hr->push.tmp[2].info.winding = !!winding;
	hr->push.tmp[2].info.nocull = !!nocull;
	add_triangle(hr);
	int ok = hr->push.num_tris == (nocull ? 2u : 1u);
	for (unsigned face = 0; face < hr->push.num_tris; ++face) {
		const hikaru_vertex_body_t *v = &hr->push.all[face * 3];
		float ax = v[1].position[0] - v[0].position[0];
		float ay = v[1].position[1] - v[0].position[1];
		float bx = v[2].position[0] - v[0].position[0];
		float by = v[2].position[1] - v[0].position[1];
		for (unsigned i = 0; i < 3; ++i)
			ok &= (ax * by - ay * bx) * v[i].normal[2] > 0.0f;
	}
	free(hr);
	return ok;
}

int hikaru_vendor_test_uv_dispatch(hikaru_gpu_t *, const uint32_t[8]);
int hikaru_vendor_test_uv_packets(uint32_t offset_opcode, uint32_t offset_payload,
                                  uint32_t texhead, const uint32_t packet[8],
                                  float uv[3][2], uint16_t decoded[2])
{
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    int ok = 0;
    if (!hr || !gpu || (packet[0] & 0xe00)) goto cleanup;
    hr->gpu = gpu;
    gpu->renderer = &hr->base;
    hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] = -1;
    /* 0E8 updates the previous three positions; no triangle is capped. */
    hr->push.num_verts = 3;
    TEX.scratch._2C1 = texhead;
    TEX.table[0]._2C1 = texhead;
    const uint32_t offset[8] = {offset_opcode, offset_payload};
    if (!hikaru_vendor_test_uv_dispatch(gpu, offset) ||
        !hikaru_vendor_test_uv_dispatch(gpu, packet)) goto cleanup;
    decoded[0] = (uint16_t) gpu->texoffset[(offset[0] >> 16) & 3][0];
    decoded[1] = (uint16_t) gpu->texoffset[(offset[0] >> 16) & 3][1];
    const unsigned count = (packet[0] & 0x1ff) == 0x0e8 ? 3 : 1;
    for (unsigned i = 0; i < count; ++i)
        memcpy(uv[i], hr->push.tmp[2-i].body.texcoords, sizeof(uv[i]));
    ok = 1;
cleanup:
    free(gpu);
    free(hr);
    return ok;
}

/* A 0E8 packet names a texhead (top bits, from the base): the three
 * vertices go on under that texhead in a new mesh. */
int hikaru_vendor_test_texhead3_switch(uint32_t texhead_a, uint32_t texhead_b,
                                       const uint32_t packet[8], float uv[3][2],
                                       uint32_t *scratch_after)
{
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    int ok = 0;
    if (!hr || !gpu) goto cleanup;
    hr->gpu = gpu;
    gpu->renderer = &hr->base;
    hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] = -1;
    hr->push.num_verts = 3;
    TEX.base = 0;
    TEX.scratch._2C1 = texhead_a;
    TEX.table[0]._2C1 = texhead_a;
    TEX.table[1]._2C1 = texhead_b;
    if (!hikaru_vendor_test_uv_dispatch(gpu, packet)) goto cleanup;
    for (unsigned i = 0; i < 3; ++i)
        memcpy(uv[i], hr->push.tmp[2-i].body.texcoords, sizeof(uv[i]));
    *scratch_after = TEX.scratch._2C1;
    ok = 1;
cleanup:
    free(gpu);
    free(hr);
    return ok;
}

int hikaru_vendor_test_vertex_matrix_selector(void)
{
	hikaru_renderer_t *hr = calloc (1, sizeof(*hr));
	hikaru_gpu_t *gpu = calloc (1, sizeof(*gpu));
	if (!hr || !gpu) { free (hr); free (gpu); return 0; }
	hr->gpu = gpu;
	hr->debug.flags[HR_DEBUG_SELECT_VIEWPORT] = -1;
	hikaru_vertex_t vertex = {0};
	vertex.info.unknown1 = 1;
	vertex.info.unknown2 = 3;
	hikaru_renderer_push_vertices (&hr->base, &vertex, HR_PUSH_POS, 1);
	bool selected = hr->push.tmp[2].body.matrix_select == 1;
	vertex.info.unknown1 = 0;
	hikaru_renderer_push_vertices (&hr->base, &vertex, HR_PUSH_TXC, 1);
	bool retained = hr->push.tmp[2].body.matrix_select == 1;
	/* No 3x3 and no last row: eye-space vertices keep both flags. */
	vertex.info.unknown2 = 0;
	hikaru_renderer_push_vertices (&hr->base, &vertex, HR_PUSH_POS, 1);
	bool eye_space = hr->push.tmp[2].body.matrix_select == 192;
	free (gpu); free (hr);
	return selected && retained && eye_space;
}
#endif

static void
print_rendstate (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, char *prefix)
{
	LOG ("RENDSTATE %s @%p #instances = %u", prefix, mesh, mesh->num_instances);
	if (mesh->vp_index < hr->num_vps)
		LOG ("RENDSTATE %s %u vp:  %s", prefix, mesh->num,
		     get_viewport_str (&hr->vp_list[mesh->vp_index]));
	if (mesh->mv_index < hr->num_mvs)
		LOG ("RENDSTATE %s %u mv:  %s", prefix, mesh->num,
		     get_modelview_str (&hr->mv_list[mesh->mv_index]));
	if (mesh->mat_index < hr->num_mats)
		LOG ("RENDSTATE %s %u mat: %s", prefix, mesh->num,
		     get_material_str (&hr->mat_list[mesh->mat_index]));
	if (mesh->tex_index < hr->num_texs)
		LOG ("RENDSTATE %s %u tex: %s", prefix, mesh->num,
		     get_texhead_str (&hr->tex_list[mesh->tex_index]));
	if (mesh->ls_index < hr->num_lss)
		LOG ("RENDSTATE %s %u ls:  %s", prefix, mesh->num,
		     get_lightset_str (&hr->ls_list[mesh->ls_index]));
}

/* Meshes retain indices, not pointers, so arrays may move while a frame is
 * built. Keep the limit tied to the number of mesh slots, and fail through
 * the engine's normal error path for runaway command streams or OOM. */
static void *
reserve_frame_states (void *array, uint32_t *capacity, uint32_t required, size_t item_size)
{
    if (required <= *capacity) return array;
    if (required > MAX_FRAME_STATES || item_size > SIZE_MAX / MAX_FRAME_STATES)
        VK_ABORT("Hikaru frame state limit exceeded");
    uint32_t next = *capacity ? *capacity : INITIAL_STATE_CAPACITY;
    while (next < required) next *= 2;
    void *grown = realloc(array, (size_t)next * item_size);
    if (!grown) VK_ABORT("Hikaru frame state allocation failed");
    *capacity = next;
    return grown;
}

/* TODO check boundary conditions when nothing is uploaded in a frame. */
/* TODO alpha threshold */
static void
update_and_set_rendstate (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned i;

	memset ((void *) mesh, 0, sizeof (hikaru_mesh_t));

	hr->vp_list = reserve_frame_states(hr->vp_list, &hr->capacity_vps, hr->num_vps + 1, sizeof(*hr->vp_list));
	hr->mat_list = reserve_frame_states(hr->mat_list, &hr->capacity_mats, hr->num_mats + 1, sizeof(*hr->mat_list));
	hr->tex_list = reserve_frame_states(hr->tex_list, &hr->capacity_texs, hr->num_texs + 1, sizeof(*hr->tex_list));
	hr->ls_list = reserve_frame_states(hr->ls_list, &hr->capacity_lss, hr->num_lss + 1, sizeof(*hr->ls_list));
	hr->vp_list[hr->num_vps++] = VP0;
	hr->mat_list[hr->num_mats++] = MAT0;
	hr->tex_list[hr->num_texs++] = TEX0;
	hr->ls_list[hr->num_lss++] = LS0;

	/* Copy the per-instance modelviews from last to first. */
	/* TODO optimize by setting MV.total to 0 (and fix the fallback). */
	if (MV.total == ~0) {
		LOG ("RENDSTATE adding no mvs %u/%u [#instances=%u]",
		     hr->num_mvs, hr->capacity_mvs, hr->num_instances);

		mesh->mv_index = hr->num_mvs - 1;
		mesh->num_instances = hr->num_instances;
	} else {
		MV.total = 1;

		mesh->mv_index = hr->num_mvs;
		mesh->num_instances = MV.total;
		hr->num_instances = MV.total;

		for (i = 0; i < MV.total; i++, hr->num_mvs++) {
			/* Geometry goes through the local matrix first, where one
			 * was completed (see the 161 handler in hikaru-gpu-cp.c). */
			LOG ("RENDSTATE adding mv %u/%u [#instances=%u]",
			     hr->num_mvs, hr->capacity_mvs, MV.total);
	
			hr->mv_list = reserve_frame_states(hr->mv_list, &hr->capacity_mvs, hr->num_mvs + 1, sizeof(*hr->mv_list));
			hr->mv_list[hr->num_mvs] = MV.table[i];
			if (MV.has_local) {
				hikaru_modelview_t composed;
				const hikaru_modelview_t *m = &MV.table[i], *l = &MV.local;
				unsigned ci, cj;
				for (ci = 0; ci < 4; ci++) {
					for (cj = 0; cj < 3; cj++)
						composed.mtx[ci][cj] = m->mtx[0][cj] * l->mtx[ci][0] +
						                       m->mtx[1][cj] * l->mtx[ci][1] +
						                       m->mtx[2][cj] * l->mtx[ci][2] +
						                       ((ci == 3) ? m->mtx[3][cj] : 0.0f);
					composed.mtx[ci][3] = (ci == 3) ? 1.0f : 0.0f;
				}
				hr->mv_list[hr->num_mvs] = composed;
			}
		}

		MV.total = 0;
		MV.depth = 0;
	}

	mesh->depth_bias = gpu->depth_bias;
	mesh->alternate_mv_index = ~0u;
	/* A second modelview completed after the primary also applies. */
	if (MV.alternate_pending) {
		MV.alternate = MV.pending_alternate;
		MV.has_alternate = true;
		MV.alternate_pending = false;
	}
	if (MV.has_alternate) {
		hr->alternate_mv_list = reserve_frame_states(hr->alternate_mv_list, &hr->capacity_alternate_mvs, hr->num_alternate_mvs + 1, sizeof(*hr->alternate_mv_list));
		mesh->alternate_mv_index = hr->num_alternate_mvs;
		hr->alternate_mv_list[hr->num_alternate_mvs++] = MV.alternate;
	}

	mesh->vp_index = hr->num_vps - 1;
	mesh->mat_index = hr->num_mats - 1;
	mesh->tex_index = hr->num_texs - 1;
	mesh->ls_index = hr->num_lss - 1;

	mesh->num = hr->total_meshes++;
	print_rendstate (hr, mesh, "U");
}

#define OFFSET(member_) \
	((const GLvoid *) offsetof (hikaru_vertex_body_t, member_))

#define VAP(loc_, num_, type_, member_, normalize_) \
	if (loc_ != (GLuint) -1) { \
		glVertexAttribPointer (loc_, num_, type_, normalize_, \
		                       sizeof (hikaru_vertex_body_t), \
		                       OFFSET (member_)); \
		VK_ASSERT_NO_GL_ERROR (); \
		\
		glEnableVertexAttribArray (loc_); \
		VK_ASSERT_NO_GL_ERROR (); \
	}

typedef struct {
	hikaru_mesh_t *mesh;
	unsigned triangle, instance;
	float depth;
} sorted_triangle_ref_t;

typedef struct {
	hikaru_mesh_t *mesh;
	unsigned instance;
	float depth;
} sorted_strip_ref_t;

static int
compare_sorted_triangle_ref(const void *a, const void *b)
{
	const sorted_triangle_ref_t *ta = (const sorted_triangle_ref_t *)a;
	const sorted_triangle_ref_t *tb = (const sorted_triangle_ref_t *)b;
	if (ta->depth < tb->depth) return -1;
	if (ta->depth > tb->depth) return 1;
	if (ta->mesh->num < tb->mesh->num) return -1;
	if (ta->mesh->num > tb->mesh->num) return 1;
	return ta->triangle < tb->triangle ? -1 : ta->triangle > tb->triangle;
}

static int
compare_sorted_strip_ref(const void *a, const void *b)
{
	const sorted_strip_ref_t *sa = (const sorted_strip_ref_t *)a;
	const sorted_strip_ref_t *sb = (const sorted_strip_ref_t *)b;
	if (sa->depth < sb->depth) return -1;
	if (sa->depth > sb->depth) return 1;
	if (sa->mesh->num < sb->mesh->num) return -1;
	if (sa->mesh->num > sb->mesh->num) return 1;
	return sa->instance < sb->instance ? -1 : sa->instance > sb->instance;
}

static float
triangle_view_depth(hikaru_renderer_t *hr, hikaru_mesh_t *mesh,
	                unsigned instance, unsigned triangle)
{
	hikaru_modelview_t identity = { .mtx = {
		{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0,0,1}
	} };
	hikaru_modelview_t *primary = mesh->mv_index == ~0u ? &identity :
		&hr->mv_list[mesh->mv_index + instance];
	hikaru_modelview_t *alternate = mesh->alternate_mv_index == ~0u ? primary :
		&hr->alternate_mv_list[mesh->alternate_mv_index];
	float sum = 0.0f;
	for (unsigned v = 0; v < 3; ++v) {
		const hikaru_vertex_body_t *vertex = &mesh->cpu_vertices[triangle * 3 + v];
		unsigned selector = vertex->matrix_select;
		hikaru_modelview_t *matrix = (selector == 1 || (selector != 0 && (selector & 1))) ? alternate : primary;
		const float *p = vertex->position;
		float z = ((selector & 64) == 0)
			? matrix->mtx[0][2] * p[0] + matrix->mtx[1][2] * p[1] + matrix->mtx[2][2] * p[2]
			: p[2];
		if ((selector & 128) == 0) z += matrix->mtx[3][2];
		sum += z;
	}
	return sum / 3.0f;
}

static void
draw_mesh_geometry(hikaru_renderer_t *hr, hikaru_mesh_t *mesh,
	               int polytype, unsigned instance)
{
	if (hr->triangle_replay.enabled) {
		if (hr->triangle_replay.mesh == mesh && hr->triangle_replay.instance == instance)
			glDrawArrays(GL_TRIANGLES, hr->triangle_replay.first * 3, hr->triangle_replay.count * 3);
		return;
	}
	(void)polytype;
	glDrawArrays(GL_TRIANGLES, mesh->first_vertex, mesh->num_tris * 3);
}

#define VERTEX_CHUNK_SIZE (4u * 1024u * 1024u)
#define VERTEX_CHUNK_COUNT 8u

/* Material RGB rules follow Odin (aa89944, GPL-3.0-or-later,
 * Copyright 2026 Reda Cherif-Touil). Our scene alpha is coverage, whereas
 * Odin stores transmittance: zeroing destination coverage here makes an
 * opaque puff disappear when the bitmap planes are composed. */
static void
apply_material_blend(hikaru_renderer_t *hr, int polytype, const hikaru_material_t *mat)
{
	bool weigh = false;
	const bool blended = polytype == HIKARU_POLYTYPE_TRANSLUCENT ||
	                     polytype == HIKARU_POLYTYPE_BACKGROUND;
	if ((polytype == HIKARU_POLYTYPE_TRANSLUCENT ||
	     polytype == HIKARU_POLYTYPE_BACKGROUND) && !hr->peel.enabled) {
		const unsigned mode = mat ? mat->blending_mode : 0;
		weigh = mode == 0 || mode == 2;
		switch (mode) {
		case 1:
			/* Odin writes zero transmittance for product: full coverage here. */
			glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
			break;
		case 2:
			glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			break;
		default:
			glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
			                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			break;
		}
	}
	/* Depth-writing geometry covers the background after its cutout test. */
	const bool force_coverage = !blended || (!hr->peel.enabled && mat && mat->blending_mode == 1);
	hikaru_mesh_uniform_cache_t *cache = &hr->meshes.uniforms;
	if (!cache->blend_valid || cache->force_coverage != force_coverage || cache->weigh_by_alpha != weigh) {
		glUniform1f(hr->meshes.locs.u_force_coverage, force_coverage ? 1.0f : 0.0f);
		glUniform1f(hr->meshes.locs.u_weigh_by_alpha, weigh ? 1.0f : 0.0f);
		cache->force_coverage = force_coverage;
		cache->weigh_by_alpha = weigh;
		cache->blend_valid = true;
	}
}

/* Odin aa89944, hikaru_draw.hpp: 501 patterns, GPL-3.0-or-later,
 * Copyright 2026 Reda Cherif-Touil. 0/16 cover all pixels; 8 covers none. */
static uint16_t
pattern_bits(unsigned pattern)
{
    static const uint16_t patterns[32] = {
		0xFFFF, 0xFBFE, 0xFAFA, 0x7ADA, 0x5A5A, 0x5258, 0x5050, 0x1040,
		0x0000, 0x0401, 0x0505, 0x8525, 0xA5A5, 0xADA7, 0xAFAF, 0xEFBF,
		0xFFFF, 0xFDF7, 0xF5F5, 0xE5B5, 0xA5A5, 0xA4A1, 0xA0A0, 0x8020,
		0x0000, 0x0208, 0x0A0A, 0x1A4A, 0x5A5A, 0x5B5E, 0x5F5F, 0x7FDF,
	};
    return patterns[pattern & 31];
}

static void
draw_mesh (hikaru_renderer_t *hr, hikaru_mesh_t *mesh, int polytype, GLuint *bound_vao,
           float *applied_depth_bias)
{
	unsigned i;
	bool configure = true;

	VK_ASSERT (mesh);
	if (!mesh->num_tris || !mesh->num_instances) return;
	VK_ASSERT (mesh->vbo || mesh->vertex_chunk);

	LOG ("==== DRAWING MESH @%p (#vertices=%u #instances=%u) ====",
	     mesh, mesh->num_tris * 3, mesh->num_instances);

	print_rendstate (hr, mesh, "D");

	if (hr->triangle_replay.enabled) {
		if (*bound_vao != hr->triangle_replay.vao) {
			glBindVertexArray(hr->triangle_replay.vao);
			*bound_vao = hr->triangle_replay.vao;
		}
		configure = !hr->triangle_replay.configured;
	} else if (mesh->vertex_chunk) {
		unsigned ci = mesh->vertex_chunk - 1;
		VK_ASSERT (ci < VERTEX_CHUNK_COUNT);
		VK_ASSERT ((size_t)mesh->first_vertex * sizeof(hikaru_vertex_body_t) <= hr->vertex_chunks[ci].used);
		VK_ASSERT ((size_t)mesh->num_tris * 3 * sizeof(hikaru_vertex_body_t) <=
		           hr->vertex_chunks[ci].used - (size_t)mesh->first_vertex * sizeof(hikaru_vertex_body_t));
		if (!hr->vertex_chunks[ci].vbo) {
			glGenVertexArrays (1, &hr->vertex_chunks[ci].vao);
			glGenBuffers (1, &hr->vertex_chunks[ci].vbo);
			glBindVertexArray (hr->vertex_chunks[ci].vao);
			*bound_vao = hr->vertex_chunks[ci].vao;
			glBindBuffer (GL_ARRAY_BUFFER, hr->vertex_chunks[ci].vbo);
			glBufferData (GL_ARRAY_BUFFER, hr->vertex_chunks[ci].used,
			              hr->vertex_chunks[ci].data, GL_DYNAMIC_DRAW);
		} else {
			if (*bound_vao != hr->vertex_chunks[ci].vao) {
				glBindVertexArray (hr->vertex_chunks[ci].vao);
				*bound_vao = hr->vertex_chunks[ci].vao;
			}
			configure = false;
		}
	} else {
		if (*bound_vao != hr->meshes.vao) {
			glBindVertexArray (hr->meshes.vao);
			*bound_vao = hr->meshes.vao;
		}
		glBindBuffer (GL_ARRAY_BUFFER, mesh->vbo);
	}
	VK_ASSERT_NO_GL_ERROR ();

	const hikaru_material_t *material = mesh->mat_index < hr->num_mats
		? &hr->mat_list[mesh->mat_index] : NULL;
	const bool texture_available = material && material->has_texture &&
		!hr->debug.flags[HR_DEBUG_NO_TEXTURES] && mesh->tex_index < hr->num_texs &&
		get_texture(hr, &hr->tex_list[mesh->tex_index]) != NULL;
	upload_glsl_program (hr, mesh, texture_available);
	VK_ASSERT_NO_GL_ERROR ();

	apply_material_blend(hr, polytype,
	    mesh->mat_index < hr->num_mats ? &hr->mat_list[mesh->mat_index] : NULL);

	upload_viewport (hr, mesh);
	if (hr->meshes.variant.has_pattern)
		glUniform1f(hr->meshes.locs.u_pattern, pattern_bits(mesh->pattern));
	hikaru_mesh_uniform_cache_t *cache = &hr->meshes.uniforms;
	const unsigned peel_mode = (hr->peel.enabled ? 1u : 0u) | (hr->peel.first ? 2u : 0u);
	if (!cache->peel_valid || cache->peel_mode != peel_mode ||
	    cache->peel_width != hr->base.internal_width || cache->peel_height != hr->base.internal_height) {
		glUniform1i(hr->meshes.locs.u_peel_enabled, hr->peel.enabled ? 1 : 0);
		glUniform1i(hr->meshes.locs.u_peel_first, hr->peel.first ? 1 : 0);
		glUniform2f(hr->meshes.locs.u_peel_size, (float)hr->base.internal_width, (float)hr->base.internal_height);
		cache->peel_mode = peel_mode;
		cache->peel_width = hr->base.internal_width;
		cache->peel_height = hr->base.internal_height;
		cache->peel_valid = true;
	}
	if (hr->peel.enabled) {
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, hr->peel.previous_depth);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, hr->peel.opaque_depth);
		glActiveTexture(GL_TEXTURE0);
	}
	upload_material_texhead (hr, mesh);
	upload_lightset (hr, mesh);
	VK_ASSERT_NO_GL_ERROR ();

	/* We must do it here since locs are computed in upload_glsl_program. */
	if (configure) {
		VAP (0, 3, GL_FLOAT,          position,  GL_FALSE);
		VAP (1, 3, GL_FLOAT,          normal,    GL_FALSE);
		VAP (2, 3, GL_UNSIGNED_BYTE,  diffuse,   GL_TRUE);
		VAP (3, 3, GL_UNSIGNED_BYTE,  ambient,   GL_TRUE);
		VAP (4, 4, GL_UNSIGNED_BYTE,  specular,  GL_TRUE);
		VAP (5, 3, GL_UNSIGNED_SHORT, unknown,   GL_TRUE);
		VAP (6, 2, GL_FLOAT,          texcoords, GL_FALSE);
		VAP (7, 1, GL_UNSIGNED_BYTE,  alpha,     GL_TRUE);
		VAP (8, 1, GL_UNSIGNED_BYTE,  matrix_select, GL_FALSE);
		if (hr->triangle_replay.enabled)
			hr->triangle_replay.configured = true;
	}

	/* Opcode 101 supplies bias for all polygon types, including opaque
	 * projected geometry. A zero-bias mesh must not inherit the prior mesh's
	 * offset state. */
	if (mesh->depth_bias != *applied_depth_bias) {
		if (mesh->depth_bias != 0.0f)
			glEnable (GL_POLYGON_OFFSET_FILL);
		else
			glDisable (GL_POLYGON_OFFSET_FILL);
		glPolygonOffset (0.0f, -mesh->depth_bias);
		*applied_depth_bias = mesh->depth_bias;
	}

	if (hr->debug.flags[HR_DEBUG_NO_INSTANCING]) {
		unsigned i = MIN2 (hr->debug.flags[HR_DEBUG_SELECT_INSTANCE],
		                   mesh->num_instances - 1);
		if ((!hr->triangle_replay.enabled ||
	     (hr->triangle_replay.mesh == mesh && hr->triangle_replay.instance == i)) &&
	    (!hr->strip_replay.enabled ||
	     (hr->strip_replay.mesh == mesh && hr->strip_replay.instance == i))) {
			upload_modelview (hr, mesh, i);
			draw_mesh_geometry(hr, mesh, polytype, i);
		}
	} else {
		for (i = 0; i < mesh->num_instances; i++) {
			if ((hr->triangle_replay.enabled &&
			     (hr->triangle_replay.mesh != mesh || hr->triangle_replay.instance != i)) ||
			    (hr->strip_replay.enabled &&
			     (hr->strip_replay.mesh != mesh || hr->strip_replay.instance != i)))
				continue;
			upload_modelview (hr, mesh, i);
			draw_mesh_geometry(hr, mesh, polytype, i);
		}
	}
}

static void
stable_sort_triangle_refs (sorted_triangle_ref_t *refs, sorted_triangle_ref_t *tmp, size_t count)
{
	/* Stable merge sort: qsort is unstable, so equal-depth translucent
	 * triangles (smoke fans, layered sprites) reshuffled every frame
	 * and their seams showed. Ties keep submission order here. */
	for (size_t width = 1; width < count; width *= 2) {
		for (size_t base = 0; base < count; base += 2 * width) {
			const size_t mid = base + width < count ? base + width : count;
			const size_t end = base + 2 * width < count ? base + 2 * width : count;
			size_t left = base, right = mid, out = base;
			while (left < mid && right < end)
				tmp[out++] = compare_sorted_triangle_ref (&refs[left], &refs[right]) <= 0
					? refs[left++] : refs[right++];
			while (left < mid) tmp[out++] = refs[left++];
			while (right < end) tmp[out++] = refs[right++];
		}
		memcpy (refs, tmp, count * sizeof(*refs));
	}
}

static bool
draw_sorted_translucent_triangles(hikaru_renderer_t *hr, unsigned vpi,
	                             hikaru_mesh_t *meshes, unsigned num)
{
	size_t total = 0;
	for (unsigned m = 0; m < num; ++m) {
		if (!meshes[m].num_tris || !meshes[m].num_instances) continue;
		if (!meshes[m].cpu_vertices) return false;
		unsigned instances = hr->debug.flags[HR_DEBUG_NO_INSTANCING]
			? 1u : meshes[m].num_instances;
		if (meshes[m].num_tris && instances > (SIZE_MAX - total) / meshes[m].num_tris)
			return false;
		total += (size_t)meshes[m].num_tris * instances;
	}
	if (!total) return true;
	/* Both the GL draw offsets and the retained upload must represent the
	 * complete sorted stream without narrowing or allocation overflow. */
	const size_t max_triangles = MIN2((size_t)INT_MAX / 3,
	    (size_t)PTRDIFF_MAX / (3 * sizeof(*hr->triangle_replay.batch)));
	if (total > max_triangles)
		return false;
	if (hr->triangle_replay.refs_capacity < total) {
		size_t grown = hr->triangle_replay.refs_capacity ? hr->triangle_replay.refs_capacity : 1024;
		while (grown < total) grown = grown > max_triangles / 2 ? total : grown * 2;
		void *resized = realloc(hr->triangle_replay.refs, grown * sizeof(sorted_triangle_ref_t));
		if (!resized) return false;
		hr->triangle_replay.refs = resized;
		resized = realloc(hr->triangle_replay.refs_tmp, grown * sizeof(sorted_triangle_ref_t));
		if (!resized) return false;
		hr->triangle_replay.refs_tmp = resized;
		resized = realloc(hr->triangle_replay.batch, grown * 3 * sizeof(*hr->triangle_replay.batch));
		if (!resized) return false;
		hr->triangle_replay.batch = resized;
		hr->triangle_replay.refs_capacity = grown;
	}
	sorted_triangle_ref_t *refs = (sorted_triangle_ref_t *)hr->triangle_replay.refs;
	size_t at = 0;
	for (unsigned m = 0; m < num; ++m) {
		hikaru_mesh_t *mesh = &meshes[m];
		if (!mesh->num_tris || !mesh->num_instances) continue;
		unsigned begin = 0, end = mesh->num_instances;
		if (hr->debug.flags[HR_DEBUG_NO_INSTANCING]) {
			begin = end = MIN2(hr->debug.flags[HR_DEBUG_SELECT_INSTANCE], mesh->num_instances - 1);
			++end;
		}
		for (unsigned instance = begin; instance < end; ++instance)
			for (unsigned triangle = 0; triangle < mesh->num_tris; ++triangle) {
				refs[at].mesh = mesh;
				refs[at].triangle = triangle;
				refs[at].instance = instance;
				refs[at].depth = triangle_view_depth(hr, mesh, instance, triangle);
				++at;
			}
	}
	stable_sort_triangle_refs(refs, (sorted_triangle_ref_t *)hr->triangle_replay.refs_tmp, at);
	/* Preserve the stable triangle sequence in one upload. Replacing the VBO
	 * for every material run serializes driver allocation and submission. */
	for (size_t n = 0; n < at; ++n)
		memcpy(&hr->triangle_replay.batch[n * 3],
		       &refs[n].mesh->cpu_vertices[refs[n].triangle * 3],
		       3 * sizeof(*hr->triangle_replay.batch));
	if (!hr->triangle_replay.vao) glGenVertexArrays(1, &hr->triangle_replay.vao);
	if (!hr->triangle_replay.vbo) glGenBuffers(1, &hr->triangle_replay.vbo);
	GLuint scratch_vao = hr->triangle_replay.vao, scratch_vbo = hr->triangle_replay.vbo;
	if (!scratch_vao || !scratch_vbo) return false;
	glBindVertexArray(scratch_vao);
	glBindBuffer(GL_ARRAY_BUFFER, scratch_vbo);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(at * 3 * sizeof(*hr->triangle_replay.batch)),
	             hr->triangle_replay.batch, GL_DYNAMIC_DRAW);
	GLuint bound_vao = scratch_vao;
	float applied_depth_bias = -1.0f;
	hr->triangle_replay.enabled = true;
	hr->triangle_replay.vao = scratch_vao;
	hr->triangle_replay.vbo = scratch_vbo;
	hr->triangle_replay.configured = false;
	for (size_t i = 0; i < at;) {
		const sorted_triangle_ref_t *first = &refs[i];
		size_t end = i + 1;
		while (end < at &&
		       refs[end].mesh == first->mesh && refs[end].instance == first->instance)
			++end;
		const unsigned count = (unsigned)(end - i);
		hr->triangle_replay.mesh = first->mesh;
		hr->triangle_replay.instance = first->instance;
		hr->triangle_replay.first = (unsigned)i;
		hr->triangle_replay.count = count;
		draw_mesh(hr, first->mesh, HIKARU_POLYTYPE_TRANSLUCENT,
		          &bound_vao, &applied_depth_bias);
		i = end;
	}
	hr->triangle_replay.enabled = false;
	hr->triangle_replay.mesh = NULL;
	hr->triangle_replay.count = 0;
	hr->triangle_replay.first = 0;
	hr->triangle_replay.configured = false;
	glBindVertexArray(0);
	/* Scratch objects and sort buffers stay in triangle_replay. */
	(void)vpi;
	return true;
}

static bool
draw_sorted_translucent_strips(hikaru_renderer_t *hr, hikaru_mesh_t *meshes, unsigned num)
{
	size_t total = 0;
	for (unsigned m = 0; m < num; ++m) {
		if (!meshes[m].num_tris || !meshes[m].num_instances) continue;
		if (!meshes[m].cpu_vertices) return false;
		unsigned instances = hr->debug.flags[HR_DEBUG_NO_INSTANCING] ? 1u : meshes[m].num_instances;
		if (instances > SIZE_MAX - total) return false;
		total += instances;
	}
	if (!total) return true;
	sorted_strip_ref_t *refs = malloc(total * sizeof(*refs));
	if (!refs) return false;
	size_t at = 0;
	for (unsigned m = 0; m < num; ++m) {
		hikaru_mesh_t *mesh = &meshes[m];
		if (!mesh->num_tris || !mesh->num_instances) continue;
		unsigned begin = 0, end = mesh->num_instances;
		if (hr->debug.flags[HR_DEBUG_NO_INSTANCING]) {
			begin = MIN2(hr->debug.flags[HR_DEBUG_SELECT_INSTANCE], mesh->num_instances - 1);
			end = begin + 1;
		}
		for (unsigned instance = begin; instance < end; ++instance) {
			float depth = 0.0f;
			for (unsigned t = 0; t < mesh->num_tris; ++t)
				depth += triangle_view_depth(hr, mesh, instance, t);
			refs[at++] = (sorted_strip_ref_t){mesh, instance,
				mesh->num_tris ? depth / mesh->num_tris : 0.0f};
		}
	}
	qsort(refs, at, sizeof(*refs), compare_sorted_strip_ref);
	GLuint bound_vao = 0;
	float applied_depth_bias = -1.0f;
	hr->strip_replay.enabled = true;
	for (size_t i = 0; i < at; ++i) {
		hr->strip_replay.mesh = refs[i].mesh;
		hr->strip_replay.instance = refs[i].instance;
		draw_mesh(hr, refs[i].mesh, HIKARU_POLYTYPE_TRANSLUCENT,
		          &bound_vao, &applied_depth_bias);
	}
	hr->strip_replay.enabled = false;
	hr->strip_replay.mesh = NULL;
	glBindVertexArray(0);
	free(refs);
	return true;
}

#ifdef HIKARU_VENDOR_TESTING
#include "tests/material_pixels.h"
/* Render a flat region through the real material copy, vertex layout and
 * shader variant cache. The caller owns expected colors from captured cases. */
static int test_material_pixels(uint32_t flags,
                                             const uint8_t diffuse[3],
                                             const uint8_t ambient[3],
                                             const uint8_t texel[4], uint8_t pixel[4], float *depth_pixel, unsigned options,
                                             const uint16_t prelit[3], bool has_prelit, uint32_t texture_head_raw, const hikaru_material_pixel_case_t *test)
{
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    GLuint fbo = 0, target = 0, texture = 0, depth_target = 0, vao = 0, vbo = 0;
    int ok = 0;
    if (!hr || !gpu) goto cleanup;
    hr->gpu = gpu;
    hr->debug.flags[HR_DEBUG_FOG] = !!(options & 1u);
    hikaru_viewport_t viewport = {0};
    viewport.depth.q_enabled = !(options & 4u);
    viewport.clip.r = 640; viewport.clip.t = 480;
    viewport.clip.f = 1280; viewport.clip.n = 0.1f;
    viewport.offset.x = 320; viewport.offset.y = 240;
    viewport.depth.density = (options & 16u) ? 0.0f : 1.0f;
    viewport.depth.bias = (options & 8u) ? 0.2f : 0.1f;
    viewport.depth.mask[0] = 64; viewport.depth.mask[1] = 128; viewport.depth.mask[2] = 192;
    hr->base.internal_width = hr->base.internal_height = 8;
    hr->vp_list = &viewport; hr->num_vps = 1;
    hikaru_material_t *material = &MAT.scratch;
    material->_881 = flags;
    memcpy(material->diffuse, diffuse, 3);
    memcpy(material->ambient, ambient, 3);
    if (!prelit) for (unsigned c = 0; c < 3; ++c) material->unknown[c] = 255;
    if (prelit && has_prelit) {
        memcpy(material->unknown, prelit, sizeof(material->unknown));
        material->has_691 = has_prelit;
    }
    POLY.type = test ? test->polytype : HIKARU_POLYTYPE_OPAQUE;
    POLY.alpha = test ? test->polygon_alpha / 255.0f : 1.0f;
    material->blending_mode = test ? test->blend_mode : 0;
    hikaru_texhead_t texture_head = {0};
    texture_head._2C1 = test ? test->texture_head : texture_head_raw;
    hr->tex_list = &texture_head; hr->num_texs = 1;
    hikaru_lightset_t lights = {0};
    lights.mask = 0xf;
    if (options & 2u) {
        lights.mask = 0xe;
        for (unsigned i = 0; i < 4; ++i) lights.lights[i].has_direction = 1;
        lights.lights[0].attenuation[0] = lights.lights[0].attenuation[1] = 1;
        if (!prelit) {
            lights.lights[0].has_specular = 1;
            material->specular[0] = material->specular[1] = material->specular[2] = 255;
            material->specular[3] = 1;
        }
    }
    hikaru_mesh_t mesh = {0};
    mesh.pattern = test ? test->pattern : 0;
    hr->mat_list = material; hr->num_mats = 1;
    hr->ls_list = &lights; hr->num_lss = 1;
    hr->meshes.variant.full = ~0ULL;

    glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (depth_pixel) {
        glGenTextures(1, &depth_target); glBindTexture(GL_TEXTURE_2D, depth_target);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 8, 8, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_target, 0);
    }
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto cleanup;
    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    hikaru_vertex_body_t vertices[3] = {{0}};
    const float positions[3][2] = {{-1, -1}, {3, -1}, {-1, 3}};
    for (unsigned i = 0; i < 3; ++i) {
        hikaru_vertex_t src = {0}, dst = {0};
        src.info.alpha = test ? test->vertex_alpha : 255;
        copy_colors(hr, &dst, &src);
        vertices[i] = dst.body;
        vertices[i].position[0] = positions[i][0];
        vertices[i].position[1] = positions[i][1];
        if (options & 3u) vertices[i].position[2] = -0.5f;
        vertices[i].normal[2] = 1;
        vertices[i].texcoords[0] = vertices[i].texcoords[1] = 0.5f;
    }
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    VAP(0, 3, GL_FLOAT, position, GL_FALSE);
    VAP(1, 3, GL_FLOAT, normal, GL_FALSE);
    VAP(2, 3, GL_UNSIGNED_BYTE, diffuse, GL_TRUE);
    VAP(3, 3, GL_UNSIGNED_BYTE, ambient, GL_TRUE);
    VAP(4, 4, GL_UNSIGNED_BYTE, specular, GL_TRUE);
    VAP(5, 3, GL_UNSIGNED_SHORT, unknown, GL_TRUE);
    VAP(6, 2, GL_FLOAT, texcoords, GL_FALSE);
    VAP(7, 1, GL_UNSIGNED_BYTE, alpha, GL_TRUE);
    VAP(8, 1, GL_UNSIGNED_BYTE, matrix_select, GL_FALSE);
    upload_glsl_program(hr, &mesh, true);
    upload_viewport(hr, &mesh);
    if (hr->meshes.variant.has_pattern) glUniform1f(hr->meshes.locs.u_pattern, pattern_bits(mesh.pattern));
    GLfloat projection[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    /* Fog at eye depths 0.5 and 1.0, with window depths 0.5 and 0.75.
     * Their independent mappings catch fog based on projected depth. */
    if (options & 1u) { projection[10] = -1; projection[14] = -0.5f; }
    const GLfloat identity4[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const GLfloat identity3[9] = {1,0,0, 0,1,0, 0,0,1};
    glUniformMatrix4fv(hr->meshes.locs.u_projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(hr->meshes.locs.u_modelview, 1, GL_FALSE, identity4);
    glUniformMatrix4fv(hr->meshes.locs.u_modelview_alternate, 1, GL_FALSE, identity4);
    glUniformMatrix3fv(hr->meshes.locs.u_normal, 1, GL_FALSE, identity3);
    glUniformMatrix3fv(hr->meshes.locs.u_normal_alternate, 1, GL_FALSE, identity3);
    glUniform1i(hr->meshes.locs.u_texture, 0);
    if (options & 2u) {
        glUniform3f(hr->meshes.locs.u_ambient, 1, 1, 1);
        glUniform3f(hr->meshes.locs.u_lights[0].direction, 0, 0, 1);
        glUniform3f(hr->meshes.locs.u_lights[0].diffuse, 0, 0, 0);
        glUniform3f(hr->meshes.locs.u_lights[0].specular, 0.25f, 0.25f, 0.25f);
        glUniform2f(hr->meshes.locs.u_lights[0].extents, 1, 1);
    }
    glViewport(0, 0, 8, 8);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);
    if (test && test->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    apply_material_blend(hr, POLY.type, material);
    /* Unblended probes inspect the material output before RGB weighting. */
    if (test && !test->blend) glUniform1f(hr->meshes.locs.u_weigh_by_alpha, 0);
    if ((!test && !depth_pixel) || (test && test->raw_material))
        glUniform1f(hr->meshes.locs.u_force_coverage, 0);
    glUniform1f(hr->meshes.locs.u_alpha_ref, 0.0f);
    glUniform3f(hr->meshes.locs.u_ambient, 1, 1, 1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (test) glClearColor(test->background[0], test->background[1], test->background[2], test->background[3]);
    else glClearColor(1, 0, 1, 1);
    if (depth_pixel) {
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
        vk_renderer_depth_range(0, 1); vk_renderer_clear_depth(1);
    }
    glClear(GL_COLOR_BUFFER_BIT | (depth_pixel ? GL_DEPTH_BUFFER_BIT : 0));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (depth_pixel) {
        /* An opaque object farther away must be visible through alpha-zero
         * cutouts. Reuse the same production variant and vertex copy path. */
        const uint8_t background[3] = {17, 34, 51};
        const uint8_t white[4] = {255, 255, 255, 255};
        memcpy(material->diffuse, background, 3);
        memcpy(material->ambient, background, 3);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white);
        for (unsigned i = 0; i < 3; ++i) {
            hikaru_vertex_t src = {0}, dst = {0};
            src.info.alpha = 255;
            copy_colors(hr, &dst, &src);
            vertices[i].position[2] = (options & 1u) ? -1.0f : 0.5f;
            memcpy(vertices[i].diffuse, dst.body.diffuse, 3);
            memcpy(vertices[i].ambient, dst.body.ambient, 3);
            if (options & 2u) memset(vertices[i].specular, 0, 3);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(4, 4, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth_pixel);
    }
    if (test && test->coverage_pixels)
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, test->coverage_pixels);
    glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    ok = glGetError() == GL_NO_ERROR;
cleanup:
    glDeleteBuffers(1, &vbo); glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &target); glDeleteTextures(1, &texture);
    glDeleteTextures(1, &depth_target);
    free(gpu); free(hr);
    return ok;
}
int hikaru_vendor_test_material_case_pixels(const hikaru_material_pixel_case_t *test, uint8_t pixel[4])
{
    return test_material_pixels(test->flags, test->diffuse, test->color1, test->texel,
                               pixel, NULL, 0, test->color691, true, 0, test);
}
int hikaru_vendor_test_prelit_material_pixels(uint32_t flags, const uint16_t prelit[3],
                                             bool has_prelit, const uint8_t diffuse[3],
                                             const uint8_t ambient[3], const uint8_t texel[4], uint8_t pixel[4], uint32_t texture_head)
{
    return test_material_pixels(flags, diffuse, ambient, texel, pixel, NULL, 2u, prelit, has_prelit, texture_head, NULL);
}
int hikaru_vendor_test_unlit_material_pixels(uint32_t flags,
                                             const uint8_t diffuse[3],
                                             const uint8_t ambient[3],
                                             const uint8_t texel[4], uint8_t pixel[4], uint32_t texture_head)
{
    return test_material_pixels(flags, diffuse, ambient, texel, pixel, NULL, 0, NULL, false, texture_head, NULL);
}
int hikaru_vendor_test_material_depth_pixels(uint32_t flags,
                                             const uint8_t diffuse[3],
                                             const uint8_t ambient[3],
                                             const uint8_t texel[4], uint8_t pixel[4], float *depth, uint32_t texture_head)
{
    return test_material_pixels(flags, diffuse, ambient, texel, pixel, depth, 0, NULL, false, texture_head, NULL);
}
int hikaru_vendor_test_material_fog_depth_pixels(uint32_t flags,
                                             const uint8_t diffuse[3],
                                             const uint8_t ambient[3],
                                             const uint8_t texel[4], uint8_t pixel[4], float *depth,
                                             unsigned options, uint32_t texture_head)
{
    return test_material_pixels(flags, diffuse, ambient, texel, pixel, depth, options, NULL, false, texture_head, NULL);
}
#endif

#undef OFFSET

static void
upload_vertex_data (hikaru_renderer_t *hr, hikaru_mesh_t *mesh)
{
	VK_ASSERT (mesh);

	mesh->num_tris = hr->push.num_tris;
	size_t bytes = sizeof(hikaru_vertex_body_t) * (size_t)mesh->num_tris * 3;
	free(mesh->cpu_vertices);
	mesh->cpu_vertices = bytes ? malloc(bytes) : NULL;
	if (bytes && mesh->cpu_vertices)
		memcpy(mesh->cpu_vertices, hr->push.all, bytes);
	if (bytes && bytes <= VERTEX_CHUNK_SIZE) {
		for (unsigned i = 0; i < VERTEX_CHUNK_COUNT; ++i) {
			if (hr->vertex_chunks[i].used > VERTEX_CHUNK_SIZE - bytes)
				continue;
			if (!hr->vertex_chunks[i].data)
				hr->vertex_chunks[i].data = malloc (VERTEX_CHUNK_SIZE);
			if (!hr->vertex_chunks[i].data)
				break;
			memcpy (hr->vertex_chunks[i].data + hr->vertex_chunks[i].used, hr->push.all, bytes);
			mesh->vertex_chunk = i + 1;
			mesh->first_vertex = hr->vertex_chunks[i].used / sizeof(hikaru_vertex_body_t);
			hr->vertex_chunks[i].used += bytes;
			return;
		}
	}

	/* Generate the VAO if required. */
	if (!hr->meshes.vao) {
		glGenVertexArrays (1, &hr->meshes.vao);
		VK_ASSERT_NO_GL_ERROR ();
	}

	/* Bind the VAO. */
	glBindVertexArray (hr->meshes.vao);
	VK_ASSERT_NO_GL_ERROR ();

	/* Generate the mesh VBO. */
	glGenBuffers (1, &mesh->vbo);

	/* Upload the vertex data to the VBO. */
	glBindBuffer (GL_ARRAY_BUFFER, mesh->vbo);
	glBufferData (GL_ARRAY_BUFFER,
	              sizeof (hikaru_vertex_body_t) * mesh->num_tris * 3,
	              (const GLvoid *) hr->push.all, GL_DYNAMIC_DRAW);
	VK_ASSERT_NO_GL_ERROR ();
}

void
hikaru_renderer_begin_mesh (vk_renderer_t *rend, uint32_t addr,
                            bool is_static)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned vp_index = VP0.depth.func;
	unsigned polytype = POLY.type;
	unsigned mesh_index;
	hikaru_mesh_t *mesh;

	VK_ASSERT (hr);
	VK_ASSERT (!hr->meshes.current);
	VK_ASSERT (vp_index < 8 && polytype < 8);
	mesh_index = hr->num_meshes[vp_index][polytype];

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	/* Create a new mesh. */
	VK_ASSERT (mesh_index < MAX_MESHES);
	mesh = &hr->mesh_list[vp_index][polytype][mesh_index++];

	hr->num_meshes[vp_index][polytype] = mesh_index;

	/* Make the mesh current and set the rendering state. */
	hr->meshes.current = mesh;
	update_and_set_rendstate (hr, mesh);
	mesh->addr[0] = addr;
	mesh->pattern = gpu->state.pattern;

	/* The last vertices stay and take the new state: a strip goes on
	 * through a recall (seen in the BOOTROM colour bars). Dropping
	 * them tore segmented character strips. */
	if (hr->push.num_verts > 3)
		hr->push.num_verts = 3;
	for (unsigned kept = 0; kept < hr->push.num_verts; ++kept)
		copy_colors (hr, &hr->push.tmp[kept], &hr->push.tmp[kept]);
	hr->push.num_tris = 0;
}

void
hikaru_renderer_end_mesh (vk_renderer_t *rend, uint32_t addr)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) rend;

	VK_ASSERT (hr);

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;

	VK_ASSERT (hr->meshes.current);

	/* Upload the pushed vertex data. */
	upload_vertex_data (hr, hr->meshes.current);
	hr->meshes.current->addr[1] = addr;

	/* Make sure there is no current mesh bound. */
	hr->meshes.current = NULL;
}

static void
draw_meshes_for_polytype (hikaru_renderer_t *hr, unsigned vpi, int polytype)
{
	hikaru_mesh_t *meshes = hr->mesh_list[vpi][polytype];
	unsigned num = hr->num_meshes[vpi][polytype];
	GLuint bound_vao = 0;
	float applied_depth_bias = -1.0f; /* Guest opcode 101 is nonnegative. */
	int j;

	if (num == 0)
		return;

	if (hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] >= 0 &&
	    hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] != polytype)
		goto destroy_meshes;

	LOG (" ==== DRAWING VP %u, POLYTYPE %d ====", vpi, polytype);

	/* Per-class state: punch-through writes depth under an 0.5 alpha
	 * test with blending off; translucent and background blend
	 * without writing depth. */
	switch (polytype) {
	case HIKARU_POLYTYPE_TRANSPARENT:
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
		glEnable (GL_POLYGON_OFFSET_FILL);
		hr->pass_alpha_ref = 0.5f;
		break;
	case HIKARU_POLYTYPE_TRANSLUCENT:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDepthMask (GL_FALSE);
		glEnable (GL_POLYGON_OFFSET_FILL);
		hr->pass_alpha_ref = 1.0f / 255.0f;
		break;
	case HIKARU_POLYTYPE_BACKGROUND:
		glEnable (GL_BLEND);
		glBlendFunc (GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR);
		glDepthMask (GL_FALSE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		hr->pass_alpha_ref = 1.0f / 255.0f;
		break;
	default:
		glDisable (GL_BLEND);
		glDepthMask (GL_TRUE);
		glDisable (GL_POLYGON_OFFSET_FILL);
		hr->pass_alpha_ref = 0.0f;
		break;
	}
	if (polytype == HIKARU_POLYTYPE_TRANSLUCENT && hr->peel.enabled) {
		glDisable(GL_BLEND);
		glDepthMask(GL_TRUE);
	}
	if (polytype == HIKARU_POLYTYPE_TRANSLUCENT && !hr->peel.enabled &&
	    hr->transparency_sorting == HIKARU_TRANSPARENCY_PER_TRIANGLE &&
	    draw_sorted_translucent_triangles(hr, vpi, meshes, num))
		goto destroy_meshes;
	if (polytype == HIKARU_POLYTYPE_TRANSLUCENT && !hr->peel.enabled &&
	    hr->transparency_sorting == HIKARU_TRANSPARENCY_PER_STRIP &&
	    draw_sorted_translucent_strips(hr, meshes, num))
		goto destroy_meshes;
	
	for (j = 0; j < num; j++) {
		hikaru_mesh_t *mesh = &meshes[j];
		draw_mesh (hr, mesh, polytype, &bound_vao, &applied_depth_bias);
	}
	glBindVertexArray (0);

destroy_meshes:
	for (j = 0; j < num; j++) {
		hikaru_mesh_t *mesh = &meshes[j];
		if (mesh->vbo && !hr->peel.enabled)
			glDeleteBuffers (1, &mesh->vbo);
		if (!hr->peel.enabled) mesh->vbo = 0;
	}
}

static void
setup_scene_scissor (unsigned width, unsigned height)
{
	/* The internal 3D target is 640x480; the host UI may have left an
	 * unrelated scissor rectangle in drawable coordinates. */
	glScissor (0, 0, width, height);
	glEnable (GL_SCISSOR_TEST);
}

/* RGBA8 plus a conservatively four-byte depth texel per target. Reserve
 * one target for the overflow query instead of silently exceeding the budget. */
static unsigned
transparency_layer_capacity(unsigned width, unsigned height, uint64_t budget, unsigned requested)
{
	const uint64_t pixels = (uint64_t)width * height;
	if (!pixels || pixels > UINT64_MAX / 8) return 0;
	const uint64_t targets = budget / (pixels * 8);
	if (targets < 2) return 0;
	return (unsigned)MIN2(MIN2(targets - 1, requested), HIKARU_MAX_PEEL_LAYERS);
}

#ifdef HIKARU_VENDOR_TESTING
unsigned hikaru_vendor_test_transparency_budget(unsigned width, unsigned height, uint64_t budget, unsigned requested)
{ return transparency_layer_capacity(width, height, budget, requested); }
#endif

static void
allocate_peel_targets(hikaru_renderer_t *hr, unsigned width, unsigned height,
                      unsigned required, unsigned maximum_targets)
{
	static const GLenum targets[] = { GL_COLOR_ATTACHMENT0 };
	if (!required || required > maximum_targets || maximum_targets > HIKARU_MAX_PEEL_LAYERS + 1)
		VK_ABORT("Hikaru peel target count is out of range");
	const bool resize = hr->framebuffer.peel_width != width || hr->framebuffer.peel_height != height;
	if (hr->framebuffer.peel_layers_allocated &&
	    (resize || hr->framebuffer.peel_layers_allocated > maximum_targets)) {
		/* Drop all old storage on resize/budget reduction. Keeping the old
		 * allocation count would incorrectly reuse images of the old size. */
		glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.peel_fb);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
		glDeleteTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_color);
		glDeleteTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_depth);
		glGenTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_color);
		glGenTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_depth);
		hr->framebuffer.peel_layers_allocated = 0;
	}
	const unsigned old_count = hr->framebuffer.peel_layers_allocated;
	/* The caller has bound peel_fb; retained, correctly sized targets need
	 * no attachment checks or allocation calls before selecting the layer. */
	if (required <= old_count) return;
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.peel_fb);
	for (unsigned i = 0; i < required; ++i) {
		if (i < old_count) continue;
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.peel_color[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.peel_depth[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
		             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       hr->framebuffer.peel_color[0], 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
	                       hr->framebuffer.peel_depth[0], 0);
	glDrawBuffers(1, targets);
	if (glGetError() != GL_NO_ERROR || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		VK_ABORT("Cannot allocate Hikaru transparency peel targets at %ux%u", width, height);
	hr->framebuffer.peel_width = width;
	hr->framebuffer.peel_height = height;
	hr->framebuffer.peel_layers_allocated = MAX2(hr->framebuffer.peel_layers_allocated, required);
}

static void
composite_peel_layers(hikaru_renderer_t *hr, GLuint scene_fb, unsigned count)
{
	glBindFramebuffer(GL_FRAMEBUFFER, scene_fb);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	                    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(hr->framebuffer.program);
	glBindVertexArray(hr->framebuffer.vao);
	glViewport(0, 0, hr->base.internal_width, hr->base.internal_height);
	glUniform1i(hr->framebuffer.locs.u_front, 0);
	glUniform1i(hr->framebuffer.locs.u_back, 1);
	glUniform1i(hr->framebuffer.locs.u_layer1, 2);
	glUniform1i(hr->framebuffer.locs.u_layer2, 3);
	glUniform1f(hr->framebuffer.locs.u_factor, 0.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_f, 1.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_b, 1.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_1, 0.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_2, 0.0f);
	glUniform1i(hr->framebuffer.locs.u_direct_1, 0);
	glUniform1i(hr->framebuffer.locs.u_direct_2, 0);
	glUniform1i(hr->framebuffer.locs.u_keyed_1, 0);
	glUniform1i(hr->framebuffer.locs.u_keyed_2, 0);
	glUniform1i(hr->framebuffer.locs.u_invert_f, 0);
	glUniform1i(hr->framebuffer.locs.u_invert_b, 0);
	for (unsigned i = count; i > 0; --i) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.peel_color[i - 1]);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.peel_color[i - 1]);
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE3);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
}

static void
draw_transparency_peels(hikaru_renderer_t *hr, unsigned vpi, GLuint scene_fb,
	                    unsigned scene_index)
{
	if (hr->transparency_sorting != HIKARU_TRANSPARENCY_PER_PIXEL ||
	    hr->num_meshes[vpi][HIKARU_POLYTYPE_TRANSLUCENT] == 0)
		return;
	const unsigned width = hr->base.internal_width, height = hr->base.internal_height;
	const unsigned requested = hr->transparency_peel_capacity ? hr->transparency_peel_capacity : HIKARU_DEFAULT_TRANSPARENCY_LAYERS;
	const uint64_t budget = hr->transparency_memory_budget ? hr->transparency_memory_budget : HIKARU_DEFAULT_TRANSPARENCY_BUDGET;
	const unsigned layers = transparency_layer_capacity(width, height, budget, requested);
	if (!layers)
		VK_ABORT("Hikaru per-pixel memory budget is too small at %ux%u; increase Pixel Buffer Size or lower the resolution", width, height);
	bool overflow = false;
	unsigned captured = 0;
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.peel_fb);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	hr->peel.enabled = true;
	hr->peel.opaque_depth = hr->framebuffer.db[scene_index];
	for (unsigned layer = 0; layer <= layers; ++layer) {
		allocate_peel_targets(hr, width, height, layer + 1, layers + 1);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		                       hr->framebuffer.peel_color[layer], 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
		                       hr->framebuffer.peel_depth[layer], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			VK_ABORT("Hikaru transparency peel framebuffer is incomplete");
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		hr->peel.first = layer == 0;
		hr->peel.previous_depth = layer ? hr->framebuffer.peel_depth[layer - 1] : hr->framebuffer.db[scene_index];
		/* Non-textured material variants still share a stable, color-compatible
		 * unit 0 binding across drivers while units 1 and 2 hold depth samplers. */
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.cb[scene_index]);
		hikaru_graphics_begin_occlusion(hr->framebuffer.peel_query);
		draw_meshes_for_polytype(hr, vpi, HIKARU_POLYTYPE_TRANSLUCENT);
		const bool occupied = hikaru_graphics_end_occlusion(hr->framebuffer.peel_query);
		if (layer == layers) { overflow = occupied; break; }
		if (!occupied) break;
		++captured;
	}
	hr->peel.enabled = false;
	if (overflow) hr->transparency_overflow_seen = true;
	if (overflow && !hr->transparency_overflow_warned) {
		fprintf(stderr, "Hikaru: per-pixel transparency capacity (%u layers, %llu-byte budget) exceeded; deeper fragments were discarded\n",
		        layers, (unsigned long long)budget);
		hr->transparency_overflow_warned = true;
	}
	composite_peel_layers(hr, scene_fb, captured);
}

#ifdef HIKARU_VENDOR_TESTING
void hikaru_vendor_test_setup_scene_scissor(void) { setup_scene_scissor(640, 480); }
#endif

static void
draw_scene (hikaru_renderer_t *hr)
{
	static const int sorted_polytypes[] = {
		HIKARU_POLYTYPE_BACKGROUND,
		HIKARU_POLYTYPE_TRANSPARENT,
		HIKARU_POLYTYPE_OPAQUE,
		HIKARU_POLYTYPE_SHADOW_A,
		HIKARU_POLYTYPE_SHADOW_B,
		HIKARU_POLYTYPE_TRANSLUCENT,
	};
	hikaru_gpu_t *gpu = hr->gpu;
	unsigned vpi, i;
	GLint scene_binding = 0;
	unsigned scene_index = 0;

	if (hr->debug.flags[HR_DEBUG_NO_3D])
		return;
	hr->transparency_overflow_seen = false;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &scene_binding);
	if ((GLuint)scene_binding == hr->framebuffer.fb[1]) scene_index = 1;

	/* Note that "the pixel ownership test, the scissor test, dithering,
	 * and the buffer writemasks affect the operation of glClear". */
	glDepthMask (GL_TRUE);
	glDisable (GL_SCISSOR_TEST);
	/* Scene RGB is premultiplied, including the hardware clear color. */
	const float coverage = gpu->fb_config.clear[3] * INV255;
	glClearColor (gpu->fb_config.clear[0] * INV255 * coverage,
	              gpu->fb_config.clear[1] * INV255 * coverage,
	              gpu->fb_config.clear[2] * INV255 * coverage, coverage);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	setup_scene_scissor (hr->base.internal_width, hr->base.internal_height);

	glEnable (GL_DEPTH_TEST);
	glDepthFunc (GL_LEQUAL);

	glEnable (GL_CULL_FACE);
	glCullFace (GL_BACK);

	for (vpi = 0; vpi < 8; vpi++) {
		glDepthMask (GL_TRUE);
		glClear (GL_DEPTH_BUFFER_BIT);
		for (i = 0; i < NUMELEM (sorted_polytypes); i++) {
			if (hr->transparency_sorting == HIKARU_TRANSPARENCY_PER_PIXEL &&
			    sorted_polytypes[i] == HIKARU_POLYTYPE_TRANSLUCENT)
				continue;
			draw_meshes_for_polytype (hr, vpi, sorted_polytypes[i]);
		}
		draw_transparency_peels(hr, vpi, (GLuint)scene_binding, scene_index);
	}

	glDepthMask (GL_TRUE);

	glDisable (GL_SCISSOR_TEST);
	glViewport (0, 0, hr->base.internal_width, hr->base.internal_height);
}

#ifdef HIKARU_VENDOR_TESTING
static int build_fb_state(hikaru_renderer_t *hr);
static void destroy_fb_state(hikaru_renderer_t *hr);
static int build_3d_state(hikaru_renderer_t *hr);
static void destroy_3d_state(hikaru_renderer_t *hr);
/* Two translucent triangles exchange depth across the viewport. Their average
 * depths are equal, so a triangle/strip ordering cannot match both samples. */
int hikaru_vendor_test_transparency_sorting_pixels(void)
{
	hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
	hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
	int ok = 0;
	uint8_t left[4] = {0}, right[4] = {0};
	if (!hr || !gpu) goto cleanup;
	hr->gpu = gpu;
	hr->base.internal_width = 640;
	hr->base.internal_height = 480;
	hr->transparency_peel_capacity = 1; /* Second surface exercises overflow detection. */
	hr->transparency_sorting = HIKARU_TRANSPARENCY_PER_PIXEL;
	hr->meshes.variant.full = ~UINT64_C(0);
	hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] = -1;
	if (build_fb_state(hr) || build_3d_state(hr)) goto cleanup;
	if (!hr->vp_list || !hr->mv_list || !hr->mat_list || !hr->tex_list || !hr->ls_list) goto cleanup;
	for (unsigned vp = 0; vp < 8; ++vp)
		for (unsigned type = 0; type < 8; ++type)
			memset(hr->mesh_list[vp][type], 0, sizeof(hikaru_mesh_t) * MAX_MESHES);
	memset(hr->vp_list, 0, sizeof(*hr->vp_list));
	memset(hr->mv_list, 0, sizeof(*hr->mv_list));
	memset(hr->mat_list, 0, sizeof(*hr->mat_list));
	memset(hr->tex_list, 0, sizeof(*hr->tex_list));
	memset(hr->ls_list, 0, sizeof(*hr->ls_list));
	hr->num_vps = hr->num_mvs = hr->num_mats = hr->num_texs = hr->num_lss = 1;
	hr->vp_list[0].clip.r = 640; hr->vp_list[0].clip.b = 0; hr->vp_list[0].clip.t = 480;
	hr->vp_list[0].clip.n = 1; hr->vp_list[0].clip.f = 2;
	hr->vp_list[0].offset.x = 320; hr->vp_list[0].offset.y = 240;
	for (unsigned i = 0; i < 4; ++i) hr->mv_list[0].mtx[i][i] = 1.0f;
	hr->mat_list[0].shading_mode = 0;
	hr->ls_list[0].mask = 15;
	gpu->fb_config.clear[0] = gpu->fb_config.clear[1] = gpu->fb_config.clear[2] = 0;
	gpu->fb_config.clear[3] = 255;
	hikaru_mesh_t *meshes = hr->mesh_list[0][HIKARU_POLYTYPE_TRANSLUCENT];
	const unsigned deferred_triangles = 256u;
	const unsigned mesh0_triangles = deferred_triangles + 1;
	const unsigned total_vertices = (mesh0_triangles + 1) * 3;
	hikaru_vertex_body_t *vertices = calloc(total_vertices, sizeof(*vertices));
	if (!vertices) goto cleanup;
	static const float xy[3][2] = {{-1200,-1000},{1200,-1000},{0,1000}};
	/* More than 256 triangles exercise the retained stream beyond the old
	 * upload limit. Blue follows this red run at a nonzero VBO offset; its
	 * pixels verify both that offset and the stable equal-depth ordering. */
	for (unsigned triangle = 0; triangle < mesh0_triangles; ++triangle)
		for (unsigned v = 0; v < 3; ++v) {
			hikaru_vertex_body_t *dst = &vertices[triangle * 3 + v];
			if (triangle < deferred_triangles) {
				dst->position[0] = 2000.0f + v * 100.0f;
				dst->position[1] = 2000.0f + (v == 2 ? 100.0f : 0.0f);
				dst->position[2] = -6.0f;
			} else {
				dst->position[0] = xy[v][0]; dst->position[1] = xy[v][1];
				dst->position[2] = v == 0 ? -4.0f : v == 1 ? -8.0f : -6.0f;
			}
			dst->diffuse[0] = 255;
			dst->alpha = 128;
		}
	for (unsigned v = 0; v < 3; ++v) {
		hikaru_vertex_body_t *dst = &vertices[mesh0_triangles * 3 + v];
		dst->position[0] = xy[v][0]; dst->position[1] = xy[v][1];
		dst->position[2] = v == 0 ? -8.0f : v == 1 ? -4.0f : -6.0f;
		dst->diffuse[2] = 255;
		dst->alpha = 128;
	}
	/* Empty command meshes must not disable sorting of the real geometry. */
	hr->num_meshes[0][HIKARU_POLYTYPE_TRANSLUCENT] = 4;
	meshes[2].num_instances = 1;
	meshes[3].num_tris = 1;
	for (unsigned side = 0; side < 2; ++side) {
		hikaru_mesh_t *mesh = &meshes[side];
		const unsigned triangle_count = side == 0 ? mesh0_triangles : 1;
		const unsigned vertex_offset = side == 0 ? 0 : mesh0_triangles * 3;
		mesh->cpu_vertices = malloc((size_t)triangle_count * 3 * sizeof(*mesh->cpu_vertices));
		if (!mesh->cpu_vertices) { free(vertices); goto cleanup; }
		memcpy(mesh->cpu_vertices, &vertices[vertex_offset],
		       (size_t)triangle_count * 3 * sizeof(*vertices));
		mesh->num_tris = triangle_count; mesh->num_instances = 1; mesh->num = side;
		mesh->vertex_chunk = 1; mesh->first_vertex = vertex_offset;
		mesh->vp_index = mesh->mv_index = mesh->mat_index = mesh->tex_index = mesh->ls_index = 0;
		mesh->alternate_mv_index = ~0u;
	}
	hr->vertex_chunks[0].data = (uint8_t *)vertices;
	hr->vertex_chunks[0].used = (size_t)total_vertices * sizeof(*vertices);
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.fb[0]);
	const HikaruTransparencySorting modes[] = {HIKARU_TRANSPARENCY_PER_STRIP,
		HIKARU_TRANSPARENCY_PER_TRIANGLE, HIKARU_TRANSPARENCY_PER_PIXEL,
		HIKARU_TRANSPARENCY_PER_PIXEL};
	const unsigned capacities[] = {32, 32, 32, 1};
	uint8_t pixels[4][2][4] = {{{0}}};
	bool overflowed[4] = {false};
	for (unsigned pass = 0; pass < 4; ++pass) {
		hr->transparency_sorting = modes[pass];
		hr->transparency_peel_capacity = capacities[pass];
		draw_scene(hr);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, hr->framebuffer.fb[0]);
		glReadPixels(220, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels[pass][0]);
		glReadPixels(420, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels[pass][1]);
		overflowed[pass] = hr->transparency_overflow_seen;
	}
	left[0] = pixels[2][0][0]; left[2] = pixels[2][0][2];
	right[0] = pixels[2][1][0]; right[2] = pixels[2][1][2];
	ok = glGetError() == GL_NO_ERROR &&
		/* Strip and triangle order use one global back-to-front sequence. */
		pixels[0][0][2] > pixels[0][0][0] && pixels[0][1][2] > pixels[0][1][0] &&
		pixels[1][0][2] > pixels[1][0][0] && pixels[1][1][2] > pixels[1][1][0] &&
		/* Peeling composes both layers and picks a different nearest surface per pixel. */
		left[0] > left[2] && left[2] > 0 && right[2] > right[0] && right[0] > 0 &&
		!overflowed[2];
	hr->transparency_sorting = HIKARU_TRANSPARENCY_PER_PIXEL;
	hr->transparency_peel_capacity = 1;
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.fb[0]);
	draw_scene(hr);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, hr->framebuffer.fb[0]);
	glReadPixels(220, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
	glReadPixels(420, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right);
	ok = ok && overflowed[3] && pixels[3][0][0] > pixels[3][0][2] &&
		pixels[3][1][2] > pixels[3][1][0] && glGetError() == GL_NO_ERROR;
	/* Forty overlapping surfaces verify that the renderer actually retains
	 * more than 32 layers. The deepest blue surface must survive the faint
	 * red foreground; truncating the stream loses that blue contribution. */
	const unsigned deep_layers = 40;
	for (unsigned triangle = 0; triangle < deep_layers; ++triangle)
		for (unsigned v = 0; v < 3; ++v) {
			hikaru_vertex_body_t *dst = &vertices[triangle * 3 + v];
			memset(dst, 0, sizeof(*dst));
			dst->position[0] = xy[v][0]; dst->position[1] = xy[v][1];
			dst->position[2] = -4.0f - triangle;
			dst->diffuse[triangle + 1 == deep_layers ? 2 : 0] = 255;
			dst->alpha = triangle + 1 == deep_layers ? 255 : 2;
		}
	memcpy(meshes[0].cpu_vertices, vertices, deep_layers * 3 * sizeof(*vertices));
	meshes[0].num_tris = deep_layers; meshes[1].num_tris = 0;
	glBindVertexArray(hr->vertex_chunks[0].vao);
	glBindBuffer(GL_ARRAY_BUFFER, hr->vertex_chunks[0].vbo);
	glBufferData(GL_ARRAY_BUFFER, total_vertices * sizeof(*vertices), vertices, GL_DYNAMIC_DRAW);
	hr->transparency_peel_capacity = 128;
	hr->transparency_memory_budget = HIKARU_DEFAULT_TRANSPARENCY_BUDGET;
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.fb[0]);
	draw_scene(hr);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, hr->framebuffer.fb[0]);
	glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
	ok &= !hr->transparency_overflow_seen && left[2] > 100 && left[0] > 10 &&
	      hr->framebuffer.peel_layers_allocated > 32;
	/* Budget reduction must release old targets and reserve the probe. */
	hr->transparency_memory_budget = UINT64_C(16) * 1024 * 1024;
	const unsigned limited = transparency_layer_capacity(640, 480, hr->transparency_memory_budget, 128);
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.fb[0]);
	draw_scene(hr);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, hr->framebuffer.fb[0]);
	glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, left);
	ok &= hr->transparency_overflow_seen && left[2] == 0 &&
	      hr->framebuffer.peel_layers_allocated <= limited + 1;
	/* Reallocating just the first target must reset the retained count, so
	 * the following target is also created at the new dimensions. */
	glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.peel_fb);
	allocate_peel_targets(hr, 320, 240, 1, 3);
	ok &= hr->framebuffer.peel_layers_allocated == 1;
	allocate_peel_targets(hr, 320, 240, 2, 3);
	ok &= hr->framebuffer.peel_layers_allocated == 2 && glGetError() == GL_NO_ERROR;
cleanup:
	if (hr) {
		if (hr->mesh_list[0][0]) destroy_3d_state(hr);
		if (hr->framebuffer.program || hr->framebuffer.fb[0]) destroy_fb_state(hr);
		free(hr);
	}
	free(gpu);
	return ok;
}
#endif

static void
release_vertex_chunks (hikaru_renderer_t *hr)
{
	for (unsigned i = 0; i < VERTEX_CHUNK_COUNT; ++i) {
		if (hr->vertex_chunks[i].vbo) glDeleteBuffers (1, &hr->vertex_chunks[i].vbo);
		if (hr->vertex_chunks[i].vao) glDeleteVertexArrays (1, &hr->vertex_chunks[i].vao);
		hr->vertex_chunks[i].vbo = hr->vertex_chunks[i].vao = 0;
		hr->vertex_chunks[i].used = 0;
	}
}

static void
clear_frame_meshes (hikaru_renderer_t *hr)
{
	release_vertex_chunks(hr);
	hr->meshes.current = NULL;
	for (unsigned vpi = 0; vpi < 8; ++vpi)
		for (unsigned i = 0; i < 8; ++i) {
			if (hr->mesh_list[vpi][i])
				for (unsigned j = 0; j < hr->num_meshes[vpi][i]; ++j) {
					hikaru_mesh_t *mesh = &hr->mesh_list[vpi][i][j];
					if (mesh->vbo) glDeleteBuffers(1, &mesh->vbo);
					mesh->vbo = 0;
					free(mesh->cpu_vertices);
					mesh->cpu_vertices = NULL;
				}
			hr->num_meshes[vpi][i] = 0;
		}
}

static void
destroy_3d_state (hikaru_renderer_t *hr)
{
	unsigned vpi, i;

	clear_frame_meshes (hr);
	for (unsigned i = 0; i < VERTEX_CHUNK_COUNT; ++i)
		free(hr->vertex_chunks[i].data);
	free (hr->vp_list);
	free (hr->mv_list);
	free (hr->alternate_mv_list);
	free (hr->mat_list);
	free (hr->tex_list);
	free (hr->ls_list);

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			free (hr->mesh_list[vpi][i]);

	for (i = 0; i < num_programs; ++i)
		vk_renderer_destroy_program (program_cache[i].program);
	memset(program_cache, 0, sizeof(program_cache));
	num_programs = 0;
	VK_ASSERT_NO_GL_ERROR ();

	if (hr->meshes.vao) {
		glBindVertexArray (0);
		glDeleteVertexArrays (1, &hr->meshes.vao);
	}
	if (hr->triangle_replay.vbo) glDeleteBuffers (1, &hr->triangle_replay.vbo);
	if (hr->triangle_replay.vao) glDeleteVertexArrays (1, &hr->triangle_replay.vao);
	hr->triangle_replay.vbo = hr->triangle_replay.vao = 0;
	free (hr->triangle_replay.refs);
	free (hr->triangle_replay.refs_tmp);
	free (hr->triangle_replay.batch);
	hr->triangle_replay.refs = hr->triangle_replay.refs_tmp = NULL;
	hr->triangle_replay.batch = NULL;
	hr->triangle_replay.refs_capacity = 0;
}

static int
build_3d_state (hikaru_renderer_t *hr)
{
	unsigned vpi, i;

	hr->vp_list = (hikaru_viewport_t *)
			malloc (sizeof (hikaru_viewport_t) * INITIAL_STATE_CAPACITY);
	hr->mv_list = (hikaru_modelview_t *)
			malloc (sizeof (hikaru_modelview_t) * INITIAL_STATE_CAPACITY);
	hr->alternate_mv_list = (hikaru_modelview_t *)
			malloc (sizeof (hikaru_modelview_t) * INITIAL_STATE_CAPACITY);
	hr->mat_list = (hikaru_material_t *)
			malloc (sizeof (hikaru_material_t) * INITIAL_STATE_CAPACITY);
	hr->tex_list = (hikaru_texhead_t *)
			malloc (sizeof (hikaru_texhead_t) * INITIAL_STATE_CAPACITY);
	hr->ls_list = (hikaru_lightset_t *)
			malloc (sizeof (hikaru_lightset_t) * INITIAL_STATE_CAPACITY);

	if (!hr->vp_list || !hr->mv_list || !hr->alternate_mv_list || !hr->mat_list || !hr->tex_list || !hr->ls_list)
		return -1;

	hr->capacity_vps = hr->capacity_mvs = hr->capacity_alternate_mvs = INITIAL_STATE_CAPACITY;
	hr->capacity_mats = hr->capacity_texs = hr->capacity_lss = INITIAL_STATE_CAPACITY;

	for (vpi = 0; vpi < 8; vpi++) {
		for (i = 0; i < 8; i++) {
			hr->mesh_list[vpi][i] =
				(hikaru_mesh_t *) malloc (sizeof (hikaru_mesh_t) * MAX_MESHES);
			if (!hr->mesh_list[vpi][i])
				return -1;
		}
	}

	return 0;
}

/****************************************************************************
 Framebuffers
****************************************************************************/

typedef struct {
       vec3f_t position;
       vec2f_t texcoords;
} framebuffer_vertex_t;

static const framebuffer_vertex_t fb_vbo_data[] = {
       { { 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f } },
       { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f } },
       { { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },
       { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } },
};

static const char *fb_vs_source =
"#version 150									\n"
"										\n"
"										\n"
"const mat4 u_projection =							\n"
"	mat4 (vec4 ( 2.0,  0.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0, -2.0,  0.0,  0.0),					\n"
"	      vec4 ( 0.0,  0.0, -1.0,  0.0),					\n"
"	      vec4 (-1.0,  1.0,  0.0,  1.0));					\n"
"										\n"
"in vec3 i_position;					\n"
"in vec2 i_fb_texcoords;					\n"
"										\n"
"out vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	gl_Position = u_projection * vec4 (i_position, 1.0);			\n"
"	p_texcoords = i_fb_texcoords;						\n"
"}										\n";

static const char *fb_fs_source =
"#version 150									\n"
"										\n"
"uniform sampler2D u_front;							\n"
"uniform sampler2D u_back;							\n"
"uniform sampler2D u_layer1;							\n"
"uniform sampler2D u_layer2;							\n"
"uniform float u_factor;							\n"
"uniform float u_mult_f;							\n"
"uniform float u_mult_b;							\n"
"uniform float u_mult_1;							\n"
"uniform int u_direct_1;\n"
"uniform int u_direct_2;\n"
"uniform int u_keyed_1;							\n"
"uniform int u_keyed_2;							\n"
"uniform float u_mult_2;							\n"
"uniform int u_invert_f;							\n"
"uniform int u_invert_b;							\n"
"										\n"
"in vec2 p_texcoords;								\n"
"										\n"
"void main (void) {								\n"
"	vec2 p_texcoords_inv = vec2 (p_texcoords.s, 1.0 - p_texcoords.t);		\n"
"	vec4 texel_f, texel_b;							\n"
"										\n"
"	if (u_invert_f == 0)							\n"
"		texel_f = texture (u_front, p_texcoords);			\n"
"	else									\n"
"		texel_f = texture (u_front, p_texcoords_inv);			\n"
"	texel_f = texel_f * u_mult_f;						\n"
"										\n"
"	if (u_invert_b == 0)							\n"
"		texel_b = texture (u_back,  p_texcoords);			\n"
"	else									\n"
"		texel_b = texture (u_back,  p_texcoords_inv);			\n"
"	texel_b = texel_b * u_mult_b;						\n"
"										\n"
"	vec4 texel_1 = texture (u_layer1, p_texcoords_inv);			\n"
"	vec4 texel_2 = texture (u_layer2, p_texcoords_inv);			\n"
"										\n"
/* Bitmap alpha is a coverage key, including the value 1 in Planet Harriers'
 * two-bit alpha. Adding 10-bit pixels lets the 3D show through solid HUD art.
 * Layer 1 covers layer 2 on either side of the 3D picture;
 * unit 38 chooses that side and unit 3C enables each layer's alpha key. */
"\tvec4 scene = mix (texel_f, texel_b, u_factor);\n"
"\tvec4 color = vec4 (0.0);\n"
"\tfloat cover_1 = u_keyed_1 != 0 ? step (0.1, texel_1.a) : 1.0;\n"
"\tfloat cover_2 = u_keyed_2 != 0 ? step (0.1, texel_2.a) : 1.0;\n"
"\tbool combined = (u_mult_1 != 0.0 && u_direct_1 == 0) || (u_mult_2 != 0.0 && u_direct_2 == 0);\n"
"\tif (u_mult_2 != 0.0 && u_direct_2 == 0)\n"
"\t\tcolor = mix (color, vec4 (texel_2.rgb * u_mult_2, 1.0), cover_2);\n"
"\tif (u_mult_1 != 0.0 && u_direct_1 == 0)\n"
"\t\tcolor = mix (color, vec4 (texel_1.rgb * u_mult_1, 1.0), cover_1);\n"
/* Scene alpha is fractional coverage, not the binary key used by a
 * bitmap plane. Its RGB has already been weighted during mesh blending. */
"\tcolor = combined ? scene + color * (1.0 - scene.a) : scene;\n"
"\tif (u_mult_2 != 0.0 && u_direct_2 != 0)\n"
"\t\tcolor = mix (color, vec4 (texel_2.rgb * u_mult_2, 1.0), cover_2);\n"
"\tif (u_mult_1 != 0.0 && u_direct_1 != 0)\n"
"\t\tcolor = mix (color, vec4 (texel_1.rgb * u_mult_1, 1.0), cover_1);\n"
"	vk_frag_color = color;							\n"
"}\n";

#define NUM_FRAMEBUFFERS 2

#define OFFSET(member_) \
	((const GLvoid *) offsetof (framebuffer_vertex_t, member_))

static int
build_fb_state (hikaru_renderer_t *hr)
{
	static const GLenum targets[] = { GL_COLOR_ATTACHMENT0 };
	unsigned i;

	/* Compile the framebuffer GLSL program and retrieve the uniforms. */
	hr->framebuffer.program =
		vk_renderer_compile_program (fb_vs_source, fb_fs_source);
	if (!hr->framebuffer.program) return -1;
	VK_ASSERT_NO_GL_ERROR ();

	hr->framebuffer.locs.u_front =
		get_uniform_loc (hr->framebuffer.program, "u_front");
	hr->framebuffer.locs.u_back =
		get_uniform_loc (hr->framebuffer.program, "u_back");
	hr->framebuffer.locs.u_layer1 =
		get_uniform_loc (hr->framebuffer.program, "u_layer1");
	hr->framebuffer.locs.u_layer2 =
		get_uniform_loc (hr->framebuffer.program, "u_layer2");
	hr->framebuffer.locs.u_factor =
		get_uniform_loc (hr->framebuffer.program, "u_factor");
	hr->framebuffer.locs.u_mult_f =
		get_uniform_loc (hr->framebuffer.program, "u_mult_f");
	hr->framebuffer.locs.u_mult_b =
		get_uniform_loc (hr->framebuffer.program, "u_mult_b");
	hr->framebuffer.locs.u_mult_1 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_1");
	hr->framebuffer.locs.u_mult_2 =
		get_uniform_loc (hr->framebuffer.program, "u_mult_2");
    hr->framebuffer.locs.u_direct_1 = get_uniform_loc(hr->framebuffer.program, "u_direct_1");
    hr->framebuffer.locs.u_direct_2 = get_uniform_loc(hr->framebuffer.program, "u_direct_2");
	hr->framebuffer.locs.u_keyed_1 =
		get_uniform_loc (hr->framebuffer.program, "u_keyed_1");
	hr->framebuffer.locs.u_keyed_2 =
		get_uniform_loc (hr->framebuffer.program, "u_keyed_2");
	hr->framebuffer.locs.u_invert_f =
		get_uniform_loc (hr->framebuffer.program, "u_invert_f");
	hr->framebuffer.locs.u_invert_b =
		get_uniform_loc (hr->framebuffer.program, "u_invert_b");

	/* Scene depth is sampleable for exact per-pixel transparency peeling. */
	glGenFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glGenTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glGenTextures (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	VK_ASSERT_NO_GL_ERROR ();

	for (i = 0; i < NUM_FRAMEBUFFERS; i++) {

		/* Bind the i'th framebuffer */
		glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[i]);
		VK_ASSERT_NO_GL_ERROR ();

		/* Create and attach a texture as color buffer. */
		glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[i]);
		VK_ASSERT_NO_GL_ERROR ();
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		VK_ASSERT_NO_GL_ERROR ();
		/* Storage allocated above; no pixel upload yet. */
		VK_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		VK_ASSERT_NO_GL_ERROR ();
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		VK_ASSERT_NO_GL_ERROR ();

		glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, hr->framebuffer.cb[i], 0);
		VK_ASSERT_NO_GL_ERROR ();

		glDrawBuffers (1, targets);
		VK_ASSERT_NO_GL_ERROR ();

		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.db[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 640, 480, 0,
		             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
		                       GL_TEXTURE_2D, hr->framebuffer.db[i], 0);
		VK_ASSERT_NO_GL_ERROR ();

		if (glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			return -1;
	}
	glGenFramebuffers(1, &hr->framebuffer.peel_fb);
	glGenTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_color);
	glGenTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_depth);
	hr->framebuffer.width = 640;
	hr->framebuffer.height = 480;

	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	VK_ASSERT_NO_GL_ERROR ();

	/* Create the VAO/VBO. */
	glGenVertexArrays (1, &hr->framebuffer.vao);
	glBindVertexArray (hr->framebuffer.vao);
	VK_ASSERT_NO_GL_ERROR ();

	glGenBuffers (1, &hr->framebuffer.vbo);
	glBindBuffer (GL_ARRAY_BUFFER, hr->framebuffer.vbo);
	glBufferData (GL_ARRAY_BUFFER,
	              sizeof (fb_vbo_data), fb_vbo_data, GL_STATIC_DRAW);
	VK_ASSERT_NO_GL_ERROR ();

	glVertexAttribPointer (0, 3, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (position));
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
	                       sizeof (fb_vbo_data[0]),
	                       OFFSET (texcoords));
	VK_ASSERT_NO_GL_ERROR ();

	glEnableVertexAttribArray (0);
	glEnableVertexAttribArray (1);
	VK_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (0);
	VK_ASSERT_NO_GL_ERROR ();

	return 0;
}

#undef OFFSET

static void
destroy_fb_state (hikaru_renderer_t *hr)
{
	glBindFramebuffer (GL_FRAMEBUFFER, 0);
	if (hr->framebuffer.vao) glDeleteVertexArrays (1, &hr->framebuffer.vao);
	if (hr->framebuffer.vbo) glDeleteBuffers (1, &hr->framebuffer.vbo);
	glDeleteFramebuffers (NUM_FRAMEBUFFERS, hr->framebuffer.fb);
	glDeleteTextures (NUM_FRAMEBUFFERS, hr->framebuffer.cb);
	glDeleteTextures (NUM_FRAMEBUFFERS, hr->framebuffer.db);
	glDeleteFramebuffers(1, &hr->framebuffer.peel_fb);
	glDeleteTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_color);
	glDeleteTextures(HIKARU_MAX_PEEL_LAYERS + 1, hr->framebuffer.peel_depth);
	hikaru_graphics_delete_occlusion(hr->framebuffer.peel_query);
	VK_ASSERT_NO_GL_ERROR ();

	vk_renderer_destroy_program (hr->framebuffer.program);
}

static void
upload_layer (hikaru_renderer_t *hr, hikaru_layer_t *layer, GLuint *id, float *mult)
{
	void *data;
	uint8_t *staging = NULL;
	uint64_t start;
	unsigned row_bytes;

	LOG ("uploading LAYER %s", get_layer_str (layer));

	if (layer->format == HIKARU_FORMAT_ABGR1555) row_bytes = 640 * 2;
	else if (layer->format == HIKARU_FORMAT_A2BGR10) row_bytes = 640 * 4;
	else VK_ABORT("unsupported Hikaru layer format");
	start = (uint64_t)layer->y0 * 4096 + (uint64_t)layer->x0 * 4;
	if (!valid_layer_span(hr->gpu->fb, layer->x0, layer->y0, row_bytes)) VK_ABORT("Hikaru layer upload exceeds framebuffer");
	data = vk_buffer_get_ptr (hr->gpu->fb, (uint32_t)start);
	if (!data) VK_ABORT("invalid Hikaru layer address");

	/* Upload the layer data to a new texture. */
	glGenTextures (1, id);
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, *id);
	VK_ASSERT_NO_GL_ERROR ();

	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	VK_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	VK_ASSERT_NO_GL_ERROR ();
	glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	VK_ASSERT_NO_GL_ERROR ();

	glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei (GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei (GL_UNPACK_SKIP_PIXELS, 0);
	switch (layer->format) {
	case HIKARU_FORMAT_ABGR1555:
		staging = decode_layer_abgr1555 (hr->gpu->fb, layer->x0, layer->y0);
		if (!staging) {
			glDeleteTextures (1, id);
			*id = 0;
			VK_ABORT("could not allocate Hikaru layer staging buffer");
		}
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 640);

		const int uploaded = upload_abgr16(0, GL_RGB5_A1, 640, 480, GL_UNSIGNED_SHORT_1_5_5_5_REV, staging);
		free (staging);
		staging = NULL;
		if (uploaded) VK_ABORT("could not upload Hikaru layer pixels");

		*mult = 1.0f;
		break;
	case HIKARU_FORMAT_A2BGR10:
		glPixelStorei (GL_UNPACK_ROW_LENGTH, 1024);

		glTexImage2D (GL_TEXTURE_2D, 0,
		              GL_RGB10_A2,
		              640, 480, 0,
		              GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV,
		              data);

		*mult = 4.0f;
		break;
	default:
		VK_ASSERT (0);
	}
	glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
	VK_ASSERT_NO_GL_ERROR ();
}

/* TODO make use of the framebuffer configuration in the GPU:1A registers,
 * right now it only uses the information in the 781/181 commands. */
static void
resize_3d_targets (hikaru_renderer_t *hr)
{
	const unsigned width = hr->base.internal_width, height = hr->base.internal_height;
	if (width == hr->framebuffer.width && height == hr->framebuffer.height) return;
	glActiveTexture (GL_TEXTURE0);
	glDisable (GL_SCISSOR_TEST);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask (GL_TRUE);
	glClearColor (0, 0, 0, 0);
	vk_renderer_clear_depth (1.0);
	for (unsigned i = 0; i < NUM_FRAMEBUFFERS; ++i) {
		glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[i]);
		glBindTexture (GL_TEXTURE_2D, hr->framebuffer.cb[i]);
		glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindTexture(GL_TEXTURE_2D, hr->framebuffer.db[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0,
		             GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (glGetError () != GL_NO_ERROR || glCheckFramebufferStatus (GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			VK_ABORT ("Cannot allocate Hikaru 3D render targets at %ux%u", width, height);
		/* Either target may be sampled before it is next drawn. */
		glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	/* Peel images resize lazily under the current transparency budget; a
	 * resolution change must not eagerly recreate the old layer count. */
	hr->framebuffer.width = width;
	hr->framebuffer.height = height;
}

static void
composite_framebuffers (hikaru_renderer_t *hr, GLuint n1, GLuint n2,
                        GLuint layer1, GLuint layer2, float mult1, float mult2,
                        float factor)
{
    hikaru_gpu_t *gpu = hr->gpu;
    const uint32_t direct = REG1AUNIT(0, 0x38);
	GLuint f = n1 < 2 ? hr->framebuffer.cb[n1] : n1 == 2 ? layer1 : layer2;
	GLuint b = n2 < 2 ? hr->framebuffer.cb[n2] : n2 == 2 ? layer1 : layer2;
	/* Composite all the buffers (front, back, layers). */
	glUseProgram (hr->framebuffer.program);
	VK_ASSERT_NO_GL_ERROR ();

	/* Figure out the GPU framebuffer assignments. */
	glActiveTexture (GL_TEXTURE0 + 0);
	glBindTexture (GL_TEXTURE_2D, f);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 1);
	glBindTexture (GL_TEXTURE_2D, b);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 2);
	glBindTexture (GL_TEXTURE_2D, (f != layer1 && b != layer1) ? layer1 : 0);
	VK_ASSERT_NO_GL_ERROR ();

	glActiveTexture (GL_TEXTURE0 + 3);
	glBindTexture (GL_TEXTURE_2D, (f != layer2 && b != layer2) ? layer2 : 0);
	VK_ASSERT_NO_GL_ERROR ();

	glUniform1i (hr->framebuffer.locs.u_front,  0);
	glUniform1i (hr->framebuffer.locs.u_back,   1);
	glUniform1i (hr->framebuffer.locs.u_layer1, 2);
	glUniform1i (hr->framebuffer.locs.u_layer2, 3);
	glUniform1f (hr->framebuffer.locs.u_factor, factor);
	glUniform1f(hr->framebuffer.locs.u_mult_f, n1 == 2 ? mult1 : n1 == 3 ? mult2 : 1.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_b, n2 == 2 ? mult1 : n2 == 3 ? mult2 : 1.0f);
	/* A layer selected by the combiner is already included in front/back. */
	glUniform1f(hr->framebuffer.locs.u_mult_1,
	            layer1 && f != layer1 && b != layer1 ? mult1 : 0.0f);
	glUniform1f(hr->framebuffer.locs.u_mult_2,
	            layer2 && f != layer2 && b != layer2 ? mult2 : 0.0f);
    glUniform1i(hr->framebuffer.locs.u_direct_1, !!(direct & 1));
    glUniform1i(hr->framebuffer.locs.u_direct_2, !!(direct & 2));
	glUniform1i(hr->framebuffer.locs.u_keyed_1, !!(REG1AUNIT(0, 0x3C) & 2));
	glUniform1i(hr->framebuffer.locs.u_keyed_2, !!(REG1AUNIT(0, 0x3C) & 4));
	glUniform1i(hr->framebuffer.locs.u_invert_f, n1 >= 2);
	glUniform1i(hr->framebuffer.locs.u_invert_b, n2 >= 2);
	VK_ASSERT_NO_GL_ERROR ();

	glBindVertexArray (hr->framebuffer.vao);
	VK_ASSERT_NO_GL_ERROR ();

	glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
	VK_ASSERT_NO_GL_ERROR ();
}

static void
draw (hikaru_renderer_t *hr)
{
	hikaru_gpu_t *gpu = hr->gpu;
	GLuint n1, n2, layer1 = 0, layer2 = 0;
	float factor, mult1 = 0.0f, mult2 = 0.0f;

	/* Figure out the GPU framebuffer assignments */
	VK_LOG ("FRAMEBUFFER 781=%08X 181=%08X", gpu->fb_config._781, gpu->fb_config._181);

	n1 = gpu->fb_config.num_1;
	n2 = gpu->fb_config.num_2;

	factor = 0.0f;
	if (gpu->fb_config.blend)
		factor = gpu->fb_config.alpha * (1.0f / 255);


	resize_3d_targets (hr);

	/* We only care about unit 0 for now. (Unit 1 is probably only used
	 * for the dual-monitor case.) */
	if ((LAYERS.layer[0][0].enabled || n1 == 2 || n2 == 2) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER1])
		upload_layer (hr, &LAYERS.layer[0][0], &layer1, &mult1);

	if ((LAYERS.layer[0][1].enabled || n1 == 3 || n2 == 3) &&
	    !hr->debug.flags[HR_DEBUG_NO_LAYER2])
		upload_layer (hr, &LAYERS.layer[0][1], &layer2, &mult2);


	/* FBO handles and color texture handles occupy separate namespaces. */
	glBindFramebuffer (GL_FRAMEBUFFER, hr->framebuffer.fb[n1 < 2 ? n1 : 0]);
	VK_ASSERT_NO_GL_ERROR ();

	/* A frame with no new meshes keeps the last completed scene, as the
	 * board's displayed buffer does; clearing here turned the 3D
	 * background black on alternate frames while the 2D layers stayed. */
	{
		unsigned meshes = 0;
		for (unsigned vp = 0; vp < 8; ++vp)
			for (unsigned type = 0; type < 8; ++type)
				meshes += hr->num_meshes[vp][type];
		if (meshes)
			draw_scene (hr);
	}
	VK_ASSERT_NO_GL_ERROR ();

	glBindFramebuffer (GL_FRAMEBUFFER, hr->base.target_fbo);
	/* The board's final image is 640x480. Preserve its 4:3 aspect ratio
	 * on resized and high-DPI host drawables, clearing the unused bars. */
	const unsigned output_width = hr->base.width;
	const unsigned output_height = hr->base.height;
	unsigned view_width, view_height;
	if ((uint64_t) output_width * 3 <= (uint64_t) output_height * 4) {
		view_width = output_width;
		view_height = (unsigned) ((uint64_t) output_width * 3 / 4);
	} else {
		view_height = output_height;
		view_width = (unsigned) ((uint64_t) output_height * 4 / 3);
	}
	glDisable (GL_SCISSOR_TEST);
	glColorMask (GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
	glClear (GL_COLOR_BUFFER_BIT);
	glViewport ((output_width - view_width) / 2, (output_height - view_height) / 2,
	            view_width, view_height);
	VK_ASSERT_NO_GL_ERROR ();


	/* Setup the GL state for 2D drawing. */
	glDisable (GL_DEPTH_TEST);
	glDisable (GL_BLEND);
	glDisable (GL_POLYGON_OFFSET_FILL);
	VK_ASSERT_NO_GL_ERROR ();


	composite_framebuffers (hr, n1, n2, layer1, layer2, mult1, mult2, factor);
	/* Opt-in diagnosis for distinguishing a black composite from a host
	 * presentation failure; disabled during normal gameplay. */
	if (getenv ("HIKARU_TRACE_FRAME")) {
		static unsigned frame;
		if ((frame++ % 120) == 0 && view_width && view_height) {
			unsigned char pixel[4] = {0}, internal[4] = {0};
			char master[256], slave[256];
			unsigned mesh_count = 0, triangle_count = 0;
			for (unsigned vp = 0; vp < 8; ++vp)
				for (unsigned type = 0; type < 8; ++type) {
					mesh_count += hr->num_meshes[vp][type];
					for (unsigned m = 0; m < hr->num_meshes[vp][type]; ++m)
						triangle_count += hr->mesh_list[vp][type][m].num_tris;
				}
			hikaru_t *machine = (hikaru_t *) gpu->base.mach;
			snprintf (master, sizeof(master), "%s", vk_cpu_get_debug_string (machine->sh_m));
			snprintf (slave, sizeof(slave), "%s", vk_cpu_get_debug_string (machine->sh_s));
			GLint pack_buffer, pack_alignment, pack_row_length, pack_skip_rows, pack_skip_pixels, read_fbo, draw_buffer;
			glGetIntegerv (GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
			glGetIntegerv (GL_DRAW_BUFFER0, &draw_buffer);
			glGetIntegerv (GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
			glGetIntegerv (GL_PACK_ALIGNMENT, &pack_alignment);
			glGetIntegerv (GL_PACK_ROW_LENGTH, &pack_row_length);
			glGetIntegerv (GL_PACK_SKIP_ROWS, &pack_skip_rows);
			glGetIntegerv (GL_PACK_SKIP_PIXELS, &pack_skip_pixels);
			glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);
			glPixelStorei (GL_PACK_ALIGNMENT, 1);
			glPixelStorei (GL_PACK_ROW_LENGTH, 0);
			glPixelStorei (GL_PACK_SKIP_ROWS, 0);
			glPixelStorei (GL_PACK_SKIP_PIXELS, 0);
			glReadPixels ((GLint) (output_width / 2), (GLint) (output_height / 2),
			              1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
			glBindFramebuffer (GL_READ_FRAMEBUFFER, hr->framebuffer.fb[n1 < 2 ? n1 : 0]);
			glReadPixels (hr->base.internal_width / 2, hr->base.internal_height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, internal);
			glBindFramebuffer (GL_READ_FRAMEBUFFER, (GLuint) read_fbo);
			glPixelStorei (GL_PACK_ALIGNMENT, pack_alignment);
			glPixelStorei (GL_PACK_ROW_LENGTH, pack_row_length);
			glPixelStorei (GL_PACK_SKIP_ROWS, pack_skip_rows);
			glPixelStorei (GL_PACK_SKIP_PIXELS, pack_skip_pixels);
			glBindBuffer (GL_PIXEL_PACK_BUFFER, (GLuint) pack_buffer);
			fprintf (stderr, "Hikaru frame %u: target=%u draw=%x size=%ux%u buffers=%u,%u layers=%u,%u meshes=%u triangles=%u cp=%08x/%u textures=%llu/%llu/%llu center=%02x%02x%02x%02x scene=%02x%02x%02x%02x %s %s\n",
			         frame, hr->base.target_fbo, (unsigned)draw_buffer, output_width, output_height, n1, n2,
			         LAYERS.layer[0][0].enabled, LAYERS.layer[0][1].enabled,
			         mesh_count, triangle_count, gpu->cp.pc, gpu->cp.is_running,
			         (unsigned long long)hr->textures.uploads, (unsigned long long)hr->textures.invalidations, (unsigned long long)hr->textures.wrap_updates,
			         pixel[0], pixel[1], pixel[2], pixel[3],
			         internal[0], internal[1], internal[2], internal[3], master, slave);
		}
	}

	glBindVertexArray (0);
	glUseProgram (0);
	VK_ASSERT_NO_GL_ERROR ();


	/* Get rid of the layer texture. */
	if (layer1)
		glDeleteTextures (1, &layer1);
	if (layer2)
		glDeleteTextures (1, &layer2);
	VK_ASSERT_NO_GL_ERROR ();
}

/****************************************************************************
 Interface
****************************************************************************/

static void
hikaru_renderer_begin_frame (vk_renderer_t *renderer)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;
	unsigned vpi, i;

	hr->meshes.variant.full = ~0;
	clear_frame_meshes (hr);

	hr->num_vps = 0;
	hr->num_mvs = 0;
	hr->num_alternate_mvs = 0;
	hr->num_instances = 0;
	hr->num_mats = 0;
	hr->num_texs = 0;
	hr->num_lss = 0;

	for (vpi = 0; vpi < 8; vpi++)
		for (i = 0; i < 8; i++)
			hr->num_meshes[vpi][i] = 0;
	hr->total_meshes = 0;

	update_debug_flags (hr);

	VK_ASSERT_NO_GL_ERROR ();
}

#ifdef HIKARU_VENDOR_TESTING
/* Exercise the real mesh append path across the original 4096-entry limit. */
int hikaru_vendor_test_frame_state_growth(vk_renderer_t *renderer)
{
    hikaru_renderer_t *hr = (hikaru_renderer_t *)renderer;
    hikaru_gpu_t *gpu = hr->gpu;
    /* Anonymous GPU state has no C11 type name; copy its bytes. */
    void *saved = malloc(sizeof(gpu->state));
    if (!saved) return 0;
    memcpy(saved, &gpu->state, sizeof(gpu->state));
    hikaru_renderer_begin_frame(renderer);
    VP0.depth.func = 0;
    POLY.type = HIKARU_POLYTYPE_OPAQUE;
    MV.has_alternate = true;
    MV.total = 0;
    for (unsigned i = 0; i < 4098; ++i) {
        VP0.clip.r = (float)i;
        MAT0.diffuse[0] = (uint8_t)i;
        TEX0._0C1 = i;
        LS0.flags = i;
        MV.table[0].mtx[0][0] = (float)i;
        MV.alternate.mtx[0][0] = (float)i;
        hikaru_renderer_begin_mesh(renderer, i, false);
        hikaru_renderer_end_mesh(renderer, i + 1);
    }
    int ok = hr->num_vps == 4098 && hr->num_mats == 4098 &&
        hr->num_texs == 4098 && hr->num_lss == 4098 && hr->num_mvs == 4098 &&
        hr->num_alternate_mvs == 4098;
    for (unsigned i = 0; i < 4098; i += 4097) {
        hikaru_mesh_t *mesh = &hr->mesh_list[0][HIKARU_POLYTYPE_OPAQUE][i];
        ok = ok && mesh->vp_index == i && mesh->mat_index == i && mesh->tex_index == i &&
            mesh->ls_index == i && mesh->mv_index == i && mesh->alternate_mv_index == i &&
            hr->vp_list[mesh->vp_index].clip.r == (float)i &&
            hr->mat_list[mesh->mat_index].diffuse[0] == (uint8_t)i &&
            hr->tex_list[mesh->tex_index]._0C1 == i && hr->ls_list[mesh->ls_index].flags == i &&
            hr->mv_list[mesh->mv_index].mtx[0][0] == (float)i &&
            hr->alternate_mv_list[mesh->alternate_mv_index].mtx[0][0] == (float)i;
    }
    hikaru_renderer_begin_frame(renderer);
    ok = ok && !hr->num_vps && !hr->num_mats && !hr->num_texs && !hr->num_lss &&
        !hr->num_mvs && !hr->num_alternate_mvs && !hr->total_meshes &&
        !hr->num_meshes[0][HIKARU_POLYTYPE_OPAQUE];
    VP0.clip.r = 73.0f;
    hikaru_renderer_begin_mesh(renderer, 0, false);
    hikaru_renderer_end_mesh(renderer, 1);
    hikaru_mesh_t *first = &hr->mesh_list[0][HIKARU_POLYTYPE_OPAQUE][0];
    ok = ok && first->vp_index == 0 && hr->vp_list[0].clip.r == 73.0f;
    hikaru_renderer_begin_frame(renderer);
    memcpy(&gpu->state, saved, sizeof(gpu->state));
    free(saved);
    return ok;
}
#endif

static void
hikaru_renderer_end_frame (vk_renderer_t *renderer)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;

	VK_ASSERT_NO_GL_ERROR ();
	/* A host frame can end while the CP is partway through a mesh. Finish
	 * this batch before drawing and clearing the next frame's mesh list. */
	hikaru_gpu_cp_finish_mesh (hr->gpu);

	draw (hr);
	release_vertex_chunks(hr);
	VK_ASSERT_NO_GL_ERROR ();

	LOG (" ==== RENDSTATE STATISTICS ==== ");
	LOG ("  vp  : %u", hr->num_vps);
	LOG ("  mv  : %u", hr->num_mvs);
	LOG ("  mat : %u", hr->num_mats);
	LOG ("  tex : %u", hr->num_texs);
	LOG ("  ls  : %u", hr->num_lss);
}

static void
hikaru_renderer_reset (vk_renderer_t *renderer)
{
	clear_frame_meshes ((hikaru_renderer_t *)renderer);
	hikaru_renderer_invalidate_texcache (renderer, NULL);
	((hikaru_renderer_t *)renderer)->textures.cpu_dirty = false;
}

#ifdef HIKARU_VENDOR_TESTING
/* Render four packed layer pixels through the real uploader and framebuffer
 * program. `back` selects front (0), back (1), direct layer 1 (2),
 * direct layer 2 (3), or both direct layers (4, with scene as layer 2).
 * Multiplier and factor are explicit to exercise format scaling and blending. */
int hikaru_vendor_test_layer_pixels(vk_renderer_t *renderer, int format, int keyed,
                                    const uint32_t packed[4], const uint8_t scene[4],
                                    int back, float multiplier, float factor, uint8_t pixels[4][4])
{
    hikaru_renderer_t *hr = (hikaru_renderer_t *)renderer;
    hikaru_gpu_t *gpu = hr->gpu;
    const uint32_t saved_direct = REG1AUNIT(0, 0x38);
    const uint32_t saved_keyed = REG1AUNIT(0, 0x3C);
    vk_buffer_t *saved_fb = hr->gpu->fb;
    vk_buffer_t *fb = vk_buffer_le32_new(8u * 1024 * 1024, 0);
    GLuint layer_tex = 0, scene_tex = 0, second_tex = 0, target_tex = 0, fbo = 0;
    GLuint saved_cb0 = hr->framebuffer.cb[0];
    unsigned saved_format1 = hr->gpu->state.layers.layer[0][0].format;
    unsigned saved_format2 = hr->gpu->state.layers.layer[0][1].format;
    float upload_mult = 0;
    int ok = 0;
    if (!fb) return 0;
    REG1AUNIT(0, 0x38) = 3;
    REG1AUNIT(0, 0x3C) = keyed ? 6 : 0;
    hikaru_layer_t layer = {0};
    layer.x0 = 7; layer.y0 = 3;
    layer.format = format ? HIKARU_FORMAT_A2BGR10 : HIKARU_FORMAT_ABGR1555;
    for (unsigned i = 0; i < 4; ++i) {
        size_t row = i < 2 ? 0 : 479;
        size_t column = i & 1;
        size_t offset = ((size_t)layer.y0 + row) * 4096 + (size_t)layer.x0 * 4;
        if (!format) column ^= 1; /* adjacent 16-bit lanes are reversed */
        offset += column * (format ? 4 : 2);
        for (unsigned byte = 0; byte < (format ? 4u : 2u); ++byte)
            fb->ptr[offset + byte] = (uint8_t)(packed[i] >> (byte * 8));
    }
    hr->gpu->fb = fb;
    upload_layer(hr, &layer, &layer_tex, &upload_mult);
    hr->gpu->fb = saved_fb;
    glGenTextures(1, &scene_tex); glBindTexture(GL_TEXTURE_2D, scene_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, scene);
    if (back == 4) {
        glGenTextures(1, &second_tex); glBindTexture(GL_TEXTURE_2D, second_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, scene);
    }
    glGenTextures(1, &target_tex); glBindTexture(GL_TEXTURE_2D, target_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto cleanup;
    glViewport(0, 0, 640, 480);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    hr->framebuffer.cb[0] = scene_tex;
    hr->gpu->state.layers.layer[0][0].format = layer.format;
    hr->gpu->state.layers.layer[0][1].format = back == 4 ? HIKARU_FORMAT_ABGR1555 : layer.format;
    composite_framebuffers(hr, back == 0 ? 2 : 0, back == 1 ? 2 : 0,
                           back == 3 ? 0 : layer_tex,
                           back == 3 ? layer_tex : second_tex,
                           back == 3 ? 0 : multiplier,
                           back == 3 ? multiplier : back == 4 ? 1.0f : 0,
                           factor);
    for (unsigned i = 0; i < 4; ++i)
        glReadPixels(i & 1, i < 2 ? 479 : 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels[i]);
    ok = glGetError() == GL_NO_ERROR && upload_mult == (format ? 4.0f : 1.0f);
cleanup:
    REG1AUNIT(0, 0x38) = saved_direct;
    REG1AUNIT(0, 0x3C) = saved_keyed;
    hr->gpu->fb = saved_fb;
    hr->framebuffer.cb[0] = saved_cb0;
    hr->gpu->state.layers.layer[0][0].format = saved_format1;
    hr->gpu->state.layers.layer[0][1].format = saved_format2;
    glDeleteTextures(1, &second_tex);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &target_tex);
    glDeleteTextures(1, &scene_tex); glDeleteTextures(1, &layer_tex);
    vk_buffer_destroy(&fb);
    return ok;
}

/* Exercise raw MMIO -> vblank layer decode -> the production draw path.
 * The scene is a green clear with either zero or full alpha. */
void hikaru_vendor_test_fill_layer_info(vk_device_t *);
int hikaru_vendor_test_routed_layers(vk_renderer_t *renderer, uint8_t pixels[10][3][4])
{
    hikaru_renderer_t *hr = (hikaru_renderer_t *)renderer;
    hikaru_gpu_t *gpu = hr->gpu;
    vk_buffer_t *saved_fb = gpu->fb;
    vk_buffer_t *fb = vk_buffer_le32_new(8u * 1024 * 1024, 0);
    /* Anonymous GPU sub-structs have no C11 type names; copy their bytes. */
    unsigned char saved_regs[sizeof(gpu->regs)];
    unsigned char saved_layers[sizeof(gpu->state.layers)];
    unsigned char saved_config[sizeof(gpu->fb_config)];
    memcpy(saved_regs, &gpu->regs, sizeof(saved_regs));
    memcpy(saved_layers, &gpu->state.layers, sizeof(saved_layers));
    memcpy(saved_config, &gpu->fb_config, sizeof(saved_config));
    int ok = 1;
    if (!fb) return 0;
    gpu->fb = fb;
    /* Layer 0 red, layer 1 blue. Bands 1 and 2 respectively remove one
     * layer, so the probe distinguishes overlap priority from enable. */
    for (unsigned y=0; y<480; ++y) for (unsigned x=0; x<640; ++x) {
        uint16_t a = x >= 160 && x < 320 ? 0 : 0x801f;
        uint16_t b = x >= 320 && x < 480 ? 0 : 0xfc00;
        size_t pixel=(x^1u)*2u; /* 1555 halfword lanes */
        size_t o0=(size_t)y*4096+pixel, o1=(size_t)(y+512)*4096+pixel;
        fb->ptr[o0]=(uint8_t)a; fb->ptr[o0+1]=(uint8_t)(a>>8);
        fb->ptr[o1]=(uint8_t)b; fb->ptr[o1+1]=(uint8_t)(b>>8);
    }
    const uint32_t addresses[]={0x1a000190,0x1a000194,0x1a000198,0x1a00019c,0x1a0001b0,0x1a0001bc};
    const uint32_t values[]={0,0,512u<<9,512u<<9,0,6};
    for(unsigned i=0;i<6;++i)
        if(vk_device_put(&gpu->base,4,addresses[i],values[i])) ok=0;
    memset(&gpu->fb_config,0,sizeof(gpu->fb_config));
    gpu->fb_config.clear[1]=255;
    /* title combined/transparent, title/opaque scene, gameplay direct,
     * mixed paths, both individual enables, all disabled, direct layer1. */
    const uint32_t enables[]={3,3,3,3,3,2,1,0,2,1};
    const uint32_t direct[] ={0,0,3,1,2,0,0,3,3,0};
    const uint8_t scene_alpha[]={0,255,255,0,0,0,0,255,255,128};
    for(unsigned step=0;step<10 && ok;++step) {
        gpu->fb_config.clear[3]=scene_alpha[step];
        if(vk_device_put(&gpu->base,4,0x1a0001b4,enables[step]) ||
           vk_device_put(&gpu->base,4,0x1a0001b8,direct[step])) {ok=0; break;}
        hikaru_vendor_test_fill_layer_info(&gpu->base);
        /* Seed a completed scene. With no new meshes, draw must preserve it
         * even when the pending clear color changes on the next host frame. */
        resize_3d_targets(hr);
        glBindFramebuffer(GL_FRAMEBUFFER, hr->framebuffer.fb[0]);
        draw_scene(hr);
        gpu->fb_config.clear[1]=0;
        draw(hr);
        gpu->fb_config.clear[1]=255;
        for(unsigned x=0;x<3;++x)
            glReadPixels(80+160*x,240,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixels[step][x]);
        if(glGetError()!=GL_NO_ERROR) ok=0;
    }
    gpu->fb=saved_fb;
    memcpy(&gpu->regs, saved_regs, sizeof(gpu->regs));
    memcpy(&gpu->state.layers, saved_layers, sizeof(gpu->state.layers));
    memcpy(&gpu->fb_config, saved_config, sizeof(gpu->fb_config));
    vk_buffer_destroy(&fb);
    return ok;
}

/* Exercise the production draw loop with both chunk offsets and fallback VBOs. */
int hikaru_vendor_test_mesh_vao_pixels(void)
{
    static const char *vs = "#version 150\nin vec3 position; in vec3 diffuse; out vec3 c; void main(){gl_Position=vec4(position,1); c=diffuse;}";
    static const char *fs = "#version 150\nin vec3 c; out vec4 color; void main(){color=vec4(c,1);}";
    static const unsigned char colors[5][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,0},{255,0,255}};
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    if (!hr) return 0;
    hikaru_mesh_t meshes[5] = {{0}};
    hikaru_vertex_body_t vertices[5][3] = {{{0}}};
    hikaru_viewport_t vp = {0};
    hikaru_modelview_t mv = {0};
    hikaru_material_t mat = {0};
    hikaru_texhead_t texhead = {0};
    hikaru_lightset_t light = {0};
    GLuint vert = glCreateShader(GL_VERTEX_SHADER), frag = glCreateShader(GL_FRAGMENT_SHADER);
    GLuint program = glCreateProgram(), fbo = 0, texture = 0, test_depth = 0;
    GLint linked = 0, complete = 0;
    int ok = 1;
    glShaderSource(vert, 1, &vs, NULL); glCompileShader(vert);
    glShaderSource(frag, 1, &fs, NULL); glCompileShader(frag);
    glAttachShader(program, vert); glAttachShader(program, frag);
    glBindAttribLocation(program, 0, "position"); glBindAttribLocation(program, 2, "diffuse");
    glLinkProgram(program); glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!linked || !complete) { ok = 0; goto cleanup; }
    hr->base.internal_width = 640; hr->base.internal_height = 480;
    vp.clip.r = 640; vp.clip.t = 480; vp.clip.f = 1;
    vp.offset.x = 320; vp.offset.y = 240;
    for (unsigned i = 0; i < 4; ++i) mv.mtx[i][i] = 1;
    light.mask = 15;
    hr->vp_list = &vp; hr->num_vps = 1; hr->mv_list = &mv; hr->num_mvs = 1;
    hr->mat_list = &mat; hr->num_mats = 1; hr->tex_list = &texhead; hr->num_texs = 1;
    hr->ls_list = &light; hr->num_lss = 1;
    hr->debug.flags[HR_DEBUG_SELECT_POLYTYPE] = -1;
    hr->mesh_list[0][HIKARU_POLYTYPE_OPAQUE] = meshes;
    hr->num_meshes[0][HIKARU_POLYTYPE_OPAQUE] = 5;
    glGenVertexArrays(1, &hr->meshes.vao);
    memset(&hr->meshes.locs, 0xff, sizeof(hr->meshes.locs));
    for (unsigned i = 0; i < 5; ++i) {
        const float x = -0.8f + 0.4f * i;
        const float coords[3][2] = {{x-0.13f,-0.3f},{x+0.13f,-0.3f},{x,0.3f}};
        for (unsigned v = 0; v < 3; ++v) {
            vertices[i][v].position[0] = coords[v][0]; vertices[i][v].position[1] = coords[v][1];
            memcpy(vertices[i][v].diffuse, colors[i], 3);
        }
        meshes[i].num_tris = 1; meshes[i].num_instances = 1;
        meshes[i].alternate_mv_index = ~0u;
    }
    meshes[0].vertex_chunk = meshes[1].vertex_chunk = 1; meshes[1].first_vertex = 3;
    meshes[4].vertex_chunk = 2;
    hr->vertex_chunks[0].data = (uint8_t *)vertices[0]; hr->vertex_chunks[0].used = sizeof(vertices[0]) * 2;
    hr->vertex_chunks[1].data = (uint8_t *)vertices[4]; hr->vertex_chunks[1].used = sizeof(vertices[4]);
    hr->meshes.variant = get_glsl_variant(hr, &meshes[0], false);
    hr->meshes.program = program; glUseProgram(program);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 2; i < 4; ++i) {
            glGenBuffers(1, &meshes[i].vbo); glBindBuffer(GL_ARRAY_BUFFER, meshes[i].vbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices[i]), vertices[i], GL_STATIC_DRAW);
        }
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        draw_meshes_for_polytype(hr, 0, HIKARU_POLYTYPE_OPAQUE);
        GLint bound; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &bound); ok &= bound == 0;
        for (unsigned i = 0; i < 5; ++i) {
            unsigned char pixel[4] = {0};
            glReadPixels(64 + 128 * i, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            for (unsigned c = 0; c < 3; ++c) ok &= colors[i][c] ? pixel[c] > 240 : pixel[c] < 10;
        }
        ok &= glGetError() == GL_NO_ERROR;
    }
    /* Real opaque draw path: a captured nonzero opcode-101 bias pulls the
     * first triangle in front of a nearly coplanar receiver. The adjacent
     * zero-bias triangle remains occluded, and an ordinary zero-bias black
     * triangle in front still draws black. */
    glGenRenderbuffers(1, &test_depth); glBindRenderbuffer(GL_RENDERBUFFER, test_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, 640, 480);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, test_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = 0; goto cleanup; }
    for (unsigned v = 0; v < 3; ++v) {
        vertices[0][v].position[2] = 0.000002f;
        vertices[1][v].position[2] = 0.000002f;
        vertices[2][v].position[2] = 0.000002f;
        vertices[4][v].position[2] = -0.1f;
        memset(vertices[4][v].diffuse, 0, 3);
    }
    glBindBuffer(GL_ARRAY_BUFFER, hr->vertex_chunks[0].vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices[0]) * 2, vertices[0]);
    glBindBuffer(GL_ARRAY_BUFFER, hr->vertex_chunks[1].vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices[4]), vertices[4]);
    meshes[0].depth_bias = 31; meshes[1].depth_bias = 0;
    meshes[2].vertex_chunk = 2; meshes[2].first_vertex = 0; meshes[2].depth_bias = 0;
    /* A second biased triangle after the zero-bias controls verifies that
     * offset is enabled again when bias changes back to a nonzero value. */
    glGenBuffers(1, &meshes[3].vbo); glBindBuffer(GL_ARRAY_BUFFER, meshes[3].vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices[2]), vertices[2], GL_STATIC_DRAW);
    meshes[3].depth_bias = 31;
    hr->num_meshes[0][HIKARU_POLYTYPE_OPAQUE] = 4;
    vk_renderer_depth_range(0, 1); vk_renderer_clear_depth(0.5); glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_meshes_for_polytype(hr, 0, HIKARU_POLYTYPE_OPAQUE);
    unsigned char biased[4] = {0}, unbiased[4] = {0}, black[4] = {0}, rebiased[4] = {0};
    glReadPixels(64, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, biased);
    glReadPixels(192, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, unbiased);
    glReadPixels(576, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glReadPixels(320, 240, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rebiased);
    if (!(rebiased[0] < 10 && rebiased[1] < 10 && rebiased[2] > 240 &&
          biased[0] > 240 && biased[1] < 10 && biased[2] < 10 &&
          unbiased[0] > 240 && unbiased[1] > 240 && unbiased[2] > 240 &&
          black[0] < 10 && black[1] < 10 && black[2] < 10)) {
        fprintf(stderr, "Hikaru opaque bias: biased=%u,%u,%u unbiased=%u,%u,%u black=%u,%u,%u rebiased=%u,%u,%u\n",
                biased[0],biased[1],biased[2],unbiased[0],unbiased[1],unbiased[2],black[0],black[1],black[2],
                rebiased[0],rebiased[1],rebiased[2]);
        ok = 0;
    }
    ok &= glGetError() == GL_NO_ERROR;
cleanup:
    if (test_depth) glDeleteRenderbuffers(1, &test_depth);
    for (unsigned i = 0; i < 5; ++i) if (meshes[i].vbo) glDeleteBuffers(1, &meshes[i].vbo);
    for (unsigned i = 0; i < 2; ++i) {
        if (hr->vertex_chunks[i].vbo) glDeleteBuffers(1, &hr->vertex_chunks[i].vbo);
        if (hr->vertex_chunks[i].vao) glDeleteVertexArrays(1, &hr->vertex_chunks[i].vao);
    }
    if (hr->meshes.vao) glDeleteVertexArrays(1, &hr->meshes.vao);
    glDeleteFramebuffers(1, &fbo); glDeleteTextures(1, &texture);
    glDeleteProgram(program); glDeleteShader(vert); glDeleteShader(frag);
    free(hr);
    return ok;
}

int hikaru_vendor_test_vertex_chunks(void)
{
	hikaru_renderer_t *hr = calloc (1, sizeof(*hr));
	if (!hr) return 0;
	hikaru_mesh_t a = {0}, b = {0}, fallback = {0}, empty = {0};
	const size_t bytes = 3 * sizeof(hikaru_vertex_body_t);
	int ok = 1;
	memset (hr->push.all, 0x35, bytes);
	hr->push.num_tris = 1;
	upload_vertex_data (hr, &a);
	memset (hr->push.all, 0xa7, bytes);
	upload_vertex_data (hr, &b);
	ok &= a.vertex_chunk == 1 && b.vertex_chunk == 1 && a.first_vertex == 0 && b.first_vertex == 3;
	ok &= hr->vertex_chunks[0].used == bytes * 2;
	if (hr->vertex_chunks[0].data) {
		for (size_t i = 0; i < bytes; ++i)
			ok &= hr->vertex_chunks[0].data[i] == 0x35 && hr->vertex_chunks[0].data[bytes + i] == 0xa7;
	} else ok = 0;
	/* Filling all chunks forces the original per-mesh GL upload. */
	for (unsigned i = 0; i < VERTEX_CHUNK_COUNT; ++i)
		hr->vertex_chunks[i].used = VERTEX_CHUNK_SIZE;
	upload_vertex_data (hr, &fallback);
	ok &= !fallback.vertex_chunk && fallback.vbo && glIsBuffer (fallback.vbo);
	uint8_t copy[3 * sizeof(hikaru_vertex_body_t)];
	glBindBuffer (GL_ARRAY_BUFFER, fallback.vbo);
	glGetBufferSubData (GL_ARRAY_BUFFER, 0, bytes, copy);
	ok &= !memcmp (copy, hr->push.all, bytes);
	glDeleteBuffers (1, &fallback.vbo);
	/* Zero vertices also retain the original upload path. */
	hr->push.num_tris = 0;
	upload_vertex_data (hr, &empty);
	ok &= !empty.vertex_chunk && empty.vbo && glIsBuffer (empty.vbo);
	glDeleteBuffers (1, &empty.vbo);
	/* A reset must discard partially built CPU and GL chunks. */
	glGenBuffers (1, &hr->vertex_chunks[0].vbo);
	glGenVertexArrays (1, &hr->vertex_chunks[0].vao);
	GLuint vbo = hr->vertex_chunks[0].vbo, vao = hr->vertex_chunks[0].vao;
	glBindVertexArray (vao);
	glBindBuffer (GL_ARRAY_BUFFER, vbo);
	ok &= glIsBuffer (vbo) && glIsVertexArray (vao);
	hikaru_renderer_reset (&hr->base);
	ok &= !hr->vertex_chunks[0].used && !hr->vertex_chunks[0].vbo && !hr->vertex_chunks[0].vao;
	ok &= !glIsBuffer (vbo) && !glIsVertexArray (vao) && glGetError() == GL_NO_ERROR;
	if (hr->meshes.vao) glDeleteVertexArrays (1, &hr->meshes.vao);
	for (unsigned i = 0; i < VERTEX_CHUNK_COUNT; ++i) free (hr->vertex_chunks[i].data);
	free (hr);
	return ok;
}
#endif

static void
hikaru_renderer_destroy (vk_renderer_t **renderer_)
{
	if (renderer_) {
		hikaru_renderer_t *hr = (hikaru_renderer_t *) *renderer_;

		destroy_3d_state (hr);
		destroy_fb_state (hr);

		hikaru_renderer_invalidate_texcache (*renderer_, NULL);
	}
}

vk_renderer_t *
hikaru_renderer_new (vk_machine_t *mach, vk_buffer_t *fb, vk_buffer_t *texram[2])
{
	hikaru_renderer_t *hr;
	int ret;

	hr = ALLOC (hikaru_renderer_t);
	if (!hr)
		goto fail;
	/* Register immediately so fatal errors during construction are cleaned up. */
	mach->renderer = &hr->base;

	hr->base.width = 640;
	hr->base.height = 480;
	hr->base.internal_width = 640;
	hr->base.internal_height = 480;
	hr->transparency_peel_capacity = HIKARU_MAX_PEEL_LAYERS;

	hr->base.destroy = hikaru_renderer_destroy;
	hr->base.reset = hikaru_renderer_reset;
	hr->base.begin_frame = hikaru_renderer_begin_frame;
	hr->base.end_frame = hikaru_renderer_end_frame;

#ifdef HIKARU_VENDOR_TESTING
	if (fail_renderer_init_once) {
		fail_renderer_init_once = false;
		VK_ABORT("injected renderer initialization failure");
	}
#endif

	ret = vk_renderer_init ((vk_renderer_t *) hr);
	if (ret)
		goto fail;

	VK_ASSERT_NO_GL_ERROR ();

	memset ((void *) program_cache, 0, sizeof (program_cache));
	num_programs = 0;

	init_debug_flags (hr);

	if (build_3d_state (hr))
		goto fail;
	VK_ASSERT_NO_GL_ERROR ();

	if (build_fb_state (hr))
		goto fail;
	VK_ASSERT_NO_GL_ERROR ();

	return (vk_renderer_t *) hr;

fail:
	if (hr) {
		hikaru_renderer_destroy ((vk_renderer_t **) &hr);
		free(hr);
		mach->renderer = NULL;
	}
	return NULL;
}

void
hikaru_renderer_set_gpu (vk_renderer_t *renderer, void *gpu_as_void)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;
	hikaru_gpu_t *gpu = (hikaru_gpu_t *) gpu_as_void;

	hr->gpu = gpu;
}

void
hikaru_renderer_set_transparency_sorting (vk_renderer_t *renderer,
                                          HikaruTransparencySorting sorting)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *) renderer;
	if (!hr)
		return;
	/* Validate again at the renderer boundary for callers outside the engine API. */
	if (sorting >= HIKARU_TRANSPARENCY_PER_STRIP && sorting <= HIKARU_TRANSPARENCY_PER_PIXEL)
		hr->transparency_sorting = sorting;
}

void
hikaru_renderer_set_transparency_peel_capacity(vk_renderer_t *renderer, unsigned layers)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *)renderer;
	if (hr && layers >= 1 && layers <= HIKARU_MAX_PEEL_LAYERS && hr->transparency_peel_capacity != layers) {
		hr->transparency_peel_capacity = layers;
		hr->transparency_overflow_warned = false;
	}
}

void
hikaru_renderer_set_transparency_memory_budget(vk_renderer_t *renderer, uint64_t bytes)
{
	hikaru_renderer_t *hr = (hikaru_renderer_t *)renderer;
	if (hr && bytes && hr->transparency_memory_budget != bytes) {
		hr->transparency_memory_budget = bytes;
		hr->transparency_overflow_warned = false;
	}
}

#ifdef HIKARU_VENDOR_TESTING
/* Exercise the production variant, light upload, vertex layout and fragment
 * shader at one exact view-space sample. Caller owns hardware-independent
 * expected Lambert/attenuation values; no shader is duplicated here. */
#define SPOT_TEST_VAP(loc,num,type,member,norm) do { \
    glVertexAttribPointer(loc,num,type,norm,sizeof(hikaru_vertex_body_t), \
        (const void *)offsetof(hikaru_vertex_body_t,member)); \
    glEnableVertexAttribArray(loc); \
} while(0)
int hikaru_vendor_test_spot_pixels(const hikaru_light_t *light,
                                   const float position[3],
                                   const float normal[3], int unlit, unsigned slot, unsigned curve,
                                   const uint16_t table[16], uint8_t pixel[4])
{
    hikaru_renderer_t *hr = calloc(1, sizeof(*hr));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    GLuint fbo = 0, target = 0, vao = 0, vbo = 0;
    int ok = 0;
    if (!hr || !gpu) goto cleanup;
    hr->gpu = gpu;
    hr->debug.flags[HR_DEBUG_NO_SPECULAR] = 1; /* Isolate angular diffuse from the legacy alias. */
    for (unsigned j=0; j<8; ++j)
        gpu->state.light_table[slot][curve*8+j].full = ((uint32_t)table[j*2] <<16) | table[j*2+1];
    hikaru_material_t material = {0};
    material.shading_mode = unlit ? 0 : 1;
    hikaru_lightset_t lights = {0};
    lights.mask = 15 ^ (1u<<slot);
    for (unsigned i=0;i<4;i++) {
        lights.lights[i].has_direction = 1;
        lights.lights[i].direction[2] = 1;
        lights.lights[i].attenuation[0] = lights.lights[i].attenuation[1] = 1;
    }
    lights.lights[slot] = *light;
    hikaru_viewport_t viewport = {0};
    hikaru_mesh_t mesh = {0};
    hr->mat_list = &material; hr->num_mats = 1;
    hr->ls_list = &lights; hr->num_lss = 1;
    hr->vp_list = &viewport; hr->num_vps = 1;
    hr->meshes.variant.full = ~0ULL;

    glGenTextures(1, &target); glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 9, 9, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto cleanup;

    hikaru_vertex_body_t vertices[3] = {{0}};
    const float offsets[3][2] = {{-1,-1},{3,-1},{-1,3}};
    for (unsigned i=0;i<3;i++) {
        vertices[i].position[0] = offsets[i][0];
        vertices[i].position[1] = offsets[i][1];
        memcpy(vertices[i].normal, normal, sizeof(vertices[i].normal));
        memset(vertices[i].diffuse, 255, 3);
        vertices[i].alpha = 255;
        vertices[i].specular[3] = 255; /* Keep zero specular RGB; avoid undefined pow(0,0). */
    }
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    SPOT_TEST_VAP(0, 3, GL_FLOAT, position, GL_FALSE);
    SPOT_TEST_VAP(1, 3, GL_FLOAT, normal, GL_FALSE);
    SPOT_TEST_VAP(2, 3, GL_UNSIGNED_BYTE, diffuse, GL_TRUE);
    SPOT_TEST_VAP(3, 3, GL_UNSIGNED_BYTE, ambient, GL_TRUE);
    SPOT_TEST_VAP(4, 4, GL_UNSIGNED_BYTE, specular, GL_TRUE);
    SPOT_TEST_VAP(5, 3, GL_UNSIGNED_SHORT, unknown, GL_TRUE);
    SPOT_TEST_VAP(6, 2, GL_FLOAT, texcoords, GL_FALSE);
    SPOT_TEST_VAP(7, 1, GL_UNSIGNED_BYTE, alpha, GL_TRUE);
    SPOT_TEST_VAP(8, 1, GL_UNSIGNED_BYTE, matrix_select, GL_FALSE);
    upload_glsl_program(hr, &mesh, true);
    const GLfloat identity3[9] = {1,0,0, 0,1,0, 0,0,1};
    GLfloat modelview[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    GLfloat projection[16] = {1,0,0,0, 0,1,0,0, 0,0,0,0, 0,0,0,1};
    for(unsigned i=0;i<3;i++) modelview[12+i] = position[i];
    projection[12] = -position[0]; projection[13] = -position[1];
    glUniformMatrix4fv(hr->meshes.locs.u_projection, 1, GL_FALSE, projection);
    glUniformMatrix4fv(hr->meshes.locs.u_modelview, 1, GL_FALSE, modelview);
    glUniformMatrix4fv(hr->meshes.locs.u_modelview_alternate, 1, GL_FALSE, modelview);
    glUniformMatrix3fv(hr->meshes.locs.u_normal, 1, GL_FALSE, identity3);
    glUniformMatrix3fv(hr->meshes.locs.u_normal_alternate, 1, GL_FALSE, identity3);
    upload_lightset(hr, &mesh);
    glViewport(0,0,9,9);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    glClearColor(1,0,1,1); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES,0,3);
    /* In an odd-sized viewport, the middle pixel is exactly NDC (0,0). */
    glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
    ok = glGetError() == GL_NO_ERROR;
cleanup:
    glDeleteBuffers(1,&vbo); glDeleteVertexArrays(1,&vao);
    glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&target);
    free(gpu);
    free(hr);
    return ok;
}
#undef SPOT_TEST_VAP
#endif
