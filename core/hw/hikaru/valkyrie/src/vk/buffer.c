/* Modified for Hollycast on 2026-10-31; see valkyrie/PROVENANCE.md. */
/* 
 * Valkyrie
 * Copyright (C) 2011, 2012, Stefano Teso
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

#include "vk/buffer.h"

static unsigned
get_file_size (FILE *fp)
{
	unsigned size, offs;
	offs = (unsigned) ftell (fp);
	fseek (fp, 0, SEEK_END);
	size = (unsigned) ftell (fp);
	fseek (fp, offs, SEEK_SET);
	return size;
}

/* Bounds and alignment are checked independently of build assertions. */
static void check_access(vk_buffer_t *buf, unsigned size, uint32_t offs)
{
    if (!buf || !is_size_valid(size) || offs > buf->size || size > buf->size - offs)
        VK_ABORT("invalid buffer access at %08X size %u", offs, size);
}
static uint64_t vk_buffer_le32_get(vk_buffer_t *buf, unsigned size, uint32_t offs)
{
    check_access(buf, size, offs);
    /* The core is built for little-endian hosts. memcpy permits unaligned
     * guest addresses and is lowered to fixed-width loads by the compiler. */
    switch (size) {
    case 1: return buf->ptr[offs];
    case 2: { uint16_t value; memcpy(&value, buf->ptr + offs, sizeof(value)); return value; }
    case 4: { uint32_t value; memcpy(&value, buf->ptr + offs, sizeof(value)); return value; }
    default: { uint64_t value; memcpy(&value, buf->ptr + offs, sizeof(value)); return value; }
    }
}
static void vk_buffer_le32_put(vk_buffer_t *buf, unsigned size, uint32_t offs, uint64_t value)
{
    check_access(buf, size, offs);
    switch (size) {
    case 1: buf->ptr[offs] = (uint8_t)value; break;
    case 2: { uint16_t v = (uint16_t)value; memcpy(buf->ptr + offs, &v, sizeof(v)); break; }
    case 4: { uint32_t v = (uint32_t)value; memcpy(buf->ptr + offs, &v, sizeof(v)); break; }
    default: memcpy(buf->ptr + offs, &value, sizeof(value)); break;
    }
}
static uint64_t vk_buffer_be32_get(vk_buffer_t *buf, unsigned size, uint32_t offs)
{
    uint64_t value = 0; check_access(buf, size, offs);
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | buf->ptr[offs+i];
    return value;
}
static void vk_buffer_be32_put(vk_buffer_t *buf, unsigned size, uint32_t offs, uint64_t value)
{
    check_access(buf, size, offs);
    for (unsigned i = 0; i < size; ++i) buf->ptr[offs+i] = (uint8_t)(value >> (8*(size-i-1)));
}

bool vk_buffer_is_plain_le(const vk_buffer_t *buf)
{
    return buf && buf->get == vk_buffer_le32_get;
}

bool vk_buffer_is_plain_le_writer(const vk_buffer_t *buf)
{
    return buf && buf->put == vk_buffer_le32_put;
}

#ifdef VK_LITTLE_ENDIAN
#define vk_buffer_native_get vk_buffer_le32_get
#define vk_buffer_native_put vk_buffer_le32_put
#else
#error "Big endian detected here... Are you sure?"
#define vk_buffer_native_get vk_buffer_be32_get
#define vk_buffer_native_put vk_buffer_be32_put
#endif

vk_buffer_t *
vk_buffer_new (unsigned size, unsigned alignment)
{
	int ret = 0;

	vk_buffer_t *buf = ALLOC (vk_buffer_t);
	if (!buf)
		goto fail;

	VK_ASSERT (is_pow2 (alignment));

	if (alignment)
		ret = posix_memalign ((void *) &buf->ptr, alignment, size);
	else
		buf->ptr = malloc (size);

	if (!buf->ptr || ret)
		goto fail;

	buf->get = vk_buffer_native_get;
	buf->put = vk_buffer_native_put;

	buf->size = size;
	return buf;

fail:
	vk_buffer_destroy (&buf);
	return NULL;
}

vk_buffer_t *
vk_buffer_le32_new (unsigned size, unsigned alignment)
{
	vk_buffer_t *buf = vk_buffer_new (size, alignment);
	if (buf) {
		buf->get = vk_buffer_le32_get;
		buf->put = vk_buffer_le32_put;
	}
	return buf;
}

vk_buffer_t *
vk_buffer_be32_new (unsigned size, unsigned alignment)
{
	vk_buffer_t *buf = vk_buffer_new (size, alignment);
	if (buf) {
		buf->get = vk_buffer_be32_get;
		buf->put = vk_buffer_be32_put;
	}
	return buf;
}

vk_buffer_t *
vk_buffer_new_from_file (const char *path, unsigned reqsize)
{
	vk_buffer_t *buffer = NULL;
	unsigned size, read_size;
	FILE *fp = NULL;

	fp = fopen (path, "rb");
	if (!fp)
		goto fail;

	size = get_file_size (fp);
	if (size != reqsize && reqsize != ~0)
		goto fail;

	buffer = vk_buffer_new (size, 0);
	if (!buffer)
		goto fail;

	clearerr (fp);
	read_size = fread (buffer->ptr, 1, size, fp);
	if (read_size != size)
		goto fail;
	if (ferror (fp)) {
		perror (NULL);
		goto fail;
	}

	return buffer;
fail:
	vk_buffer_destroy (&buffer);
	if (fp)
		fclose (fp);
	return  NULL;
}

void
vk_buffer_destroy (vk_buffer_t **buf_)
{
	if (buf_) {
		vk_buffer_t *buf = *buf_;
		if (buf)
			free (buf->ptr);
		free (buf);
		*buf_ = NULL;
	}
}

unsigned
vk_buffer_get_size (vk_buffer_t *buf)
{
	return buf ? buf->size : 0;
}

void *
vk_buffer_get_ptr (vk_buffer_t *buf, unsigned offs)
{
	if (buf && offs < buf->size)
		return (void *) &buf->ptr[offs];
	return (void *) NULL;
}

void
vk_buffer_clear (vk_buffer_t *buf)
{
	VK_ASSERT (buf);
	VK_ASSERT (buf->ptr);
	memset (buf->ptr, 0, buf->size);
}

int
vk_buffer_copy (vk_buffer_t *dst, vk_buffer_t *src, unsigned offs, unsigned nbytes)
{
	VK_ASSERT (dst);
	VK_ASSERT (src);
	VK_ASSERT (nbytes > 0);

	/* TODO see games.c */
	return -1;
}

int
vk_buffer_copy_interleave (vk_buffer_t *dst, vk_buffer_t *src, unsigned offs, unsigned nbytes)
{
	VK_ASSERT (dst);
	VK_ASSERT (src);
	VK_ASSERT (nbytes > 0);

	/* TODO see games.c */
	return -1;
}

void
vk_buffer_print_some (vk_buffer_t *buffer, unsigned lo, unsigned hi)
{
	uint8_t *ptr = (uint8_t *) buffer->ptr;
	unsigned i;
	for (i = lo; i < hi; i++)
		printf ("%02X %c\n", ptr[i], ptr[i]);
}

void
vk_buffer_print (vk_buffer_t *buffer)
{
	if (buffer)
		vk_buffer_print_some (buffer, 0, buffer->size);
}

void
vk_buffer_dump (vk_buffer_t *buffer, const char *path)                          
{                                                                               
        FILE *fp;

	/* swap if required */

	if (!buffer || !buffer->ptr || !buffer->size)
		return;
        fp = fopen (path, "wb");
        if (!fp)
                return;
        fwrite (buffer->ptr, 1, buffer->size, fp);
        fclose (fp);
}

void
vk_buffer_dumpf (vk_buffer_t *buffer, const char *fmt, ...)
{
	char path[256] = "";
	va_list va;

	va_start (va, fmt);
	vsnprintf (path, sizeof (path), fmt, va);
	va_end (va);

	vk_buffer_dump (buffer, path);
}

int
vk_buffer_load_state (vk_buffer_t *buffer, vk_state_t *state)
{
	VK_ASSERT (buffer);
	VK_ASSERT (state);

	return vk_state_get (state, buffer->ptr, buffer->size);
}

int
vk_buffer_save_state (vk_buffer_t *buffer, vk_state_t *state)
{
	VK_ASSERT (buffer);
	VK_ASSERT (state);

	return vk_state_put (state, buffer->ptr, buffer->size);
}
