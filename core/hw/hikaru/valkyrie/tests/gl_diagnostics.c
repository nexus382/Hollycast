// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef __APPLE__
#include <stdio.h>
int main(void) { puts("GL diagnostic GPU fixture skipped on Apple"); return 77; }
#else
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <glad/gl.h>
#include "../gl_diagnostics.h"
#include "../graphics_dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;
static int set_trace_flag(const char *name, const char *value)
{
#ifdef _WIN32
    /* SDL can own a different CRT environment. Match production getenv's CRT. */
    return _putenv_s(name, value);
#else
    return SDL_setenv(name, value, 1);
#endif
}
static PFNGLGETQUERYOBJECTIVPROC original_query_available;
static PFNGLQUERYCOUNTERPROC original_query_counter;
static void GLAD_API_PTR unavailable_query(GLuint id, GLenum name, GLint *value)
{
    if (name == GL_QUERY_RESULT_AVAILABLE) *value = GL_FALSE;
    else original_query_available(id, name, value);
}
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "GL diagnostics failed at line %d: %s\n", __LINE__, #condition); failed = 1; goto done; } } while (0)

int main(void)
{
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    HikaruGlDiagnostics *diagnostics = NULL;
    HikaruGlDiagnosticSnapshot snapshot = {0};
    GLuint buffer = 0, texture = 0;
    const float vertices[4] = {1, 2, 3, 4};
    const unsigned char texel[4] = {64, 128, 192, 255};
    unsigned char before[4], after[4];
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    window = SDL_CreateWindow("Hikaru diagnostics", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window || !(context = SDL_GL_CreateContext(window)) ||
        !gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        puts("GL diagnostics fixture unavailable");
        if (context) SDL_GL_DeleteContext(context);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 77;
    }

    CHECK(set_trace_flag("HIKARU_TRACE_TIMING", "0") == 0);
    CHECK(set_trace_flag("HIKARU_TRACE_GPU_TIMING", "0") == 0);
    diagnostics = hikaru_gl_diagnostics_create();
    CHECK(diagnostics == NULL);
    CHECK(!hikaru_gl_diagnostics_collecting);
    hikaru_gl_diagnostics_begin_frame(NULL);
    hikaru_gl_diagnostics_end_frame(NULL, false);

    CHECK(set_trace_flag("HIKARU_TRACE_TIMING", "1") == 0);
    diagnostics = hikaru_gl_diagnostics_create();
    CHECK(diagnostics != NULL);
    hikaru_gl_diagnostics_begin_frame(diagnostics);
    CHECK(hikaru_gl_diagnostics_collecting);
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
    hikaru_gl_diagnostics_end_frame(diagnostics, true);
    CHECK(!hikaru_gl_diagnostics_collecting);
    hikaru_gl_diagnostics_snapshot(diagnostics, &snapshot);
    CHECK(snapshot.counters[HIKARU_GL_DIAG_VERTEX_UPLOADS] == 1);
    CHECK(snapshot.counters[HIKARU_GL_DIAG_VERTEX_BYTES] == sizeof(vertices));
    CHECK(snapshot.counters[HIKARU_GL_DIAG_TEXTURE_UPLOADS] == 1);
    CHECK(snapshot.counters[HIKARU_GL_DIAG_TEXTURE_BYTES] == sizeof(texel));
    CHECK(snapshot.gpu_samples[HIKARU_GL_DIAG_PHASE_FRAME] == 0);
    hikaru_gl_diagnostics_destroy(&diagnostics);

    CHECK(set_trace_flag("HIKARU_TRACE_GPU_TIMING", "1") == 0);
    diagnostics = hikaru_gl_diagnostics_create();
    CHECK(diagnostics != NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 16, 16);
    glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, before);
    hikaru_gl_diagnostics_begin_frame(diagnostics);
    for (unsigned phase = HIKARU_GL_DIAG_PHASE_CLEAR; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
        hikaru_gl_diagnostics_phase_begin((HikaruGlDiagnosticPhase)phase);
        glClear(GL_COLOR_BUFFER_BIT);
        hikaru_gl_diagnostics_phase_end((HikaruGlDiagnosticPhase)phase);
    }
    hikaru_gl_diagnostics_end_frame(diagnostics, true);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, after);
    CHECK(memcmp(before, after, sizeof(before)) == 0);
    /* Only the fixture flushes/waits. Production polling never forces GPU progress. */
    glFlush();
    const Uint64 deadline = SDL_GetTicks64() + 3000;
    do {
        hikaru_gl_diagnostics_begin_frame(diagnostics);
        hikaru_gl_diagnostics_end_frame(diagnostics, true);
        hikaru_gl_diagnostics_snapshot(diagnostics, &snapshot);
        if (!snapshot.gpu_supported || snapshot.gpu_samples[HIKARU_GL_DIAG_PHASE_RESOLVE]) break;
        SDL_Delay(1);
    } while (SDL_GetTicks64() < deadline);
    if (snapshot.gpu_supported) {
        for (unsigned phase = 0; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase)
            CHECK(snapshot.gpu_samples[phase] > 0);
    } else puts("GPU timers unavailable; CPU diagnostics and rendering remain usable");

    /* Reset/abort cleanup must end only this owner's frame and clear TLS. */
    hikaru_gl_diagnostics_begin_frame(diagnostics);
    hikaru_gl_diagnostics_phase_begin(HIKARU_GL_DIAG_PHASE_CLEAR);
    hikaru_gl_diagnostics_end_frame(diagnostics, false);
    CHECK(!hikaru_gl_diagnostics_collecting);
    hikaru_gl_diagnostics_destroy(&diagnostics);

    /* A busy ring must skip work rather than consume unfinished query data. */
    diagnostics = hikaru_gl_diagnostics_create();
    CHECK(diagnostics != NULL);
    original_query_available = glad_glGetQueryObjectiv;
    glad_glGetQueryObjectiv = unavailable_query;
    for (unsigned frame = 0; frame < 5; ++frame) {
        hikaru_gl_diagnostics_begin_frame(diagnostics);
        glClear(GL_COLOR_BUFFER_BIT);
        hikaru_gl_diagnostics_end_frame(diagnostics, true);
    }
    glad_glGetQueryObjectiv = original_query_available;
    hikaru_gl_diagnostics_snapshot(diagnostics, &snapshot);
    if (snapshot.gpu_supported) {
        CHECK(snapshot.dropped_frames == 1);
        CHECK(snapshot.gpu_samples[HIKARU_GL_DIAG_PHASE_FRAME] == 0);
    }
    hikaru_gl_diagnostics_destroy(&diagnostics);

    /* Capability failure disables only GPU instrumentation, not the renderer. */
    original_query_counter = glad_glQueryCounter;
    glad_glQueryCounter = NULL;
    diagnostics = hikaru_gl_diagnostics_create();
    glad_glQueryCounter = original_query_counter;
    CHECK(diagnostics != NULL);
    hikaru_gl_diagnostics_begin_frame(diagnostics);
    glClear(GL_COLOR_BUFFER_BIT);
    hikaru_gl_diagnostics_end_frame(diagnostics, true);
    hikaru_gl_diagnostics_snapshot(diagnostics, &snapshot);
    CHECK(!snapshot.gpu_supported);
    CHECK(snapshot.gpu_samples[HIKARU_GL_DIAG_PHASE_FRAME] == 0);
    CHECK(glGetError() == GL_NO_ERROR);

done:
    if (original_query_available) glad_glGetQueryObjectiv = original_query_available;
    if (original_query_counter) glad_glQueryCounter = original_query_counter;
    hikaru_gl_diagnostics_destroy(&diagnostics);
    if (texture) glDeleteTextures(1, &texture);
    if (buffer) glDeleteBuffers(1, &buffer);
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    if (!failed) puts("GL diagnostic opt-in, payload, GPU retirement and abort checks: PASS");
    return failed ? 1 : 0;
}
#endif
