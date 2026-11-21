// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif

#include "../graphics/fragment_list_shared.h"

typedef struct HikaruGlFragmentList HikaruGlFragmentList;

bool hikaru_gl_fragment_list_begin(HikaruGlFragmentList **state,
                                  GLuint sceneFramebuffer, GLuint sceneTexture,
                                  unsigned width, unsigned height, uint64_t budget);
bool hikaru_gl_fragment_list_end(HikaruGlFragmentList *state);
bool hikaru_gl_fragment_list_active(const HikaruGlFragmentList *state);
const HikaruFragmentListParameters *hikaru_gl_fragment_list_parameters(
    const HikaruGlFragmentList *state);
bool hikaru_gl_fragment_list_advance(HikaruGlFragmentList *state,
                                     uint32_t primitiveCount);
void hikaru_gl_fragment_list_fail(HikaruGlFragmentList *state);
void hikaru_gl_fragment_list_reset(HikaruGlFragmentList *state);
void hikaru_gl_fragment_list_destroy(HikaruGlFragmentList **state);
const char *hikaru_gl_fragment_list_shader_source(void);
