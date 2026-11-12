// Portions Copyright 2026 The Hollycast Authors
/* Modified for Hollycast on 2026-11-03; see valkyrie/PROVENANCE.md. */
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

#include "vk/state.h"

#define VK_STATE_VERSION	4

vk_state_t *
vk_state_new (const char *path, uint32_t mode)
{
	char template[32] = "", header[32];
	vk_state_t *state;
	size_t size;

	VK_ASSERT (path);
	VK_ASSERT (mode == VK_STATE_LOAD || mode == VK_STATE_SAVE);

	state = ALLOC (vk_state_t);
	if (!state)
		return NULL;

	state->fp = fopen (path, (mode == VK_STATE_LOAD) ? "rb" : "wb");
	if (!state->fp)
		goto fail;

	state->mode = mode;
	state->encoding = 2;

	sprintf (template, "valkyrie state %08X\n", VK_STATE_VERSION);
	if (mode == VK_STATE_LOAD) {
		size = fread ((void *) header, 1, sizeof (template), state->fp);
		if (size != sizeof (template)) goto fail;
		if (memcmp(template, header, sizeof(header))) {
			/* Version 3 predates the upper material and texture-head banks. */
			sprintf(template, "valkyrie state %08X\n", 3);
			if (memcmp(template, header, sizeof(header))) goto fail;
			state->encoding = 1;
		}
	} else {
		size = fwrite ((void *) template, 1, sizeof (template), state->fp);
		if (size != sizeof (template))
			goto fail;
	}

	return state;

fail:
	vk_state_destroy (&state, -1);
	return NULL;
}

void
vk_state_destroy (vk_state_t **state_, int ret)
{
	vk_state_t *state;

	VK_ASSERT (state_);
	state = *state_;
	if (!state) return;

	if (state->fp) fclose (state->fp);
	(void) ret;

	free (state);
	*state_ = NULL;
}

int
vk_state_put (vk_state_t *state, void *src, uint32_t size)
{
	size_t num;

	if (!state || state->mode != VK_STATE_SAVE) return -1;
	if (!state->fp) {
		if (state->offset > state->capacity || size > state->capacity - state->offset) { state->failed = true; return -1; }
		if (state->data && size) memcpy (state->data + state->offset, src, size);
		state->offset += size;
		return 0;
	}

	VK_LOG ("state: W%08X %p", size, src);

	num = fwrite (src, 1, size, state->fp);
	return (num != size || ferror (state->fp)) ? -1 : 0;
}

int
vk_state_get (vk_state_t *state, void *dst, uint32_t size)
{
	size_t num;

	if (!state || state->mode != VK_STATE_LOAD) return -1;
	if (!state->fp) {
		if (!state->data || state->offset > state->capacity || size > state->capacity - state->offset) { state->failed = true; return -1; }
		if (size) memcpy (dst, state->data + state->offset, size);
		state->offset += size;
		return 0;
	}

	VK_LOG ("state: R%08X %p", size, dst);

	num = fread (dst, 1, size, state->fp);
	return (num != size || ferror (state->fp)) ? -1 : 0;
}
