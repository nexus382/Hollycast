// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#ifdef __APPLE__
#include <stdio.h>

int main(void)
{
    puts("fragment-list pixel test skipped: desktop OpenGL 4.3 is unavailable on Apple");
    return 77;
}
#else
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#include <glad/gl.h>
#include "../gl_fragment_list.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int hikaru_vendor_test_fragment_list_pixels(void);

enum { TEST_STORAGE_BINDINGS = 6 };

typedef struct TestGlState {
    GLint drawFramebuffer, readFramebuffer, program, vertexArray;
    GLint activeTexture, texture0, sampler0, unpackBuffer;
    GLint viewport[4], scissor[4], polygonMode[2];
    GLint genericStorageBuffer, storageBuffer[TEST_STORAGE_BINDINGS];
    GLint64 storageStart[TEST_STORAGE_BINDINGS], storageSize[TEST_STORAGE_BINDINGS];
    GLboolean colorMask[4], depthMask;
    GLboolean blend, depthTest, cull, scissorTest, stencilTest, rasterizerDiscard, dither;
} TestGlState;

static bool test_gl_errors(void)
{
    bool clean = true;
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "fragment-list fixture GL error: 0x%04x\n", (unsigned)error);
        clean = false;
    }
    return clean;
}

static void capture_test_gl_state(TestGlState *state)
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->drawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->readFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vertexArray);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state->activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture0);
    glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &state->sampler0);
    glActiveTexture((GLenum)state->activeTexture);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &state->unpackBuffer);
    glGetIntegerv(GL_VIEWPORT, state->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, state->scissor);
    glGetIntegerv(GL_POLYGON_MODE, state->polygonMode);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &state->genericStorageBuffer);
    for (GLuint i = 0; i < TEST_STORAGE_BINDINGS; ++i) {
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &state->storageBuffer[i]);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, i, &state->storageStart[i]);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, i, &state->storageSize[i]);
    }
    glGetBooleanv(GL_COLOR_WRITEMASK, state->colorMask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state->depthMask);
    state->blend = glIsEnabled(GL_BLEND);
    state->depthTest = glIsEnabled(GL_DEPTH_TEST);
    state->cull = glIsEnabled(GL_CULL_FACE);
    state->scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    state->stencilTest = glIsEnabled(GL_STENCIL_TEST);
    state->rasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
    state->dither = glIsEnabled(GL_DITHER);
}

static bool same_test_gl_state(const TestGlState *a, const TestGlState *b)
{
    const bool same = a->drawFramebuffer == b->drawFramebuffer && a->readFramebuffer == b->readFramebuffer &&
        a->program == b->program && a->vertexArray == b->vertexArray &&
        a->activeTexture == b->activeTexture && a->texture0 == b->texture0 &&
        a->sampler0 == b->sampler0 && a->unpackBuffer == b->unpackBuffer &&
        a->genericStorageBuffer == b->genericStorageBuffer &&
        !memcmp(a->viewport, b->viewport, sizeof(a->viewport)) &&
        !memcmp(a->scissor, b->scissor, sizeof(a->scissor)) &&
        /* Core GL defines one polygon mode; the second slot is not state. */
        a->polygonMode[0] == b->polygonMode[0] &&
        !memcmp(a->storageBuffer, b->storageBuffer, sizeof(a->storageBuffer)) &&
        !memcmp(a->storageStart, b->storageStart, sizeof(a->storageStart)) &&
        !memcmp(a->storageSize, b->storageSize, sizeof(a->storageSize)) &&
        !memcmp(a->colorMask, b->colorMask, sizeof(a->colorMask)) &&
        a->depthMask == b->depthMask && a->blend == b->blend &&
        a->depthTest == b->depthTest && a->cull == b->cull &&
        a->scissorTest == b->scissorTest && a->stencilTest == b->stencilTest &&
        a->rasterizerDiscard == b->rasterizerDiscard && a->dither == b->dither;
    if (!same) {
        fprintf(stderr, "restored GL state: FBO=%d/%d -> %d/%d program=%d/%d VAO=%d/%d unit=%d/%d texture=%d/%d sampler=%d/%d PBO=%d/%d polygon=%d/%d genericSSBO=%d/%d\n",
            a->drawFramebuffer, a->readFramebuffer, b->drawFramebuffer, b->readFramebuffer,
            a->program, b->program, a->vertexArray, b->vertexArray, a->activeTexture, b->activeTexture,
            a->texture0, b->texture0, a->sampler0, b->sampler0, a->unpackBuffer, b->unpackBuffer,
            a->polygonMode[0], b->polygonMode[0], a->genericStorageBuffer, b->genericStorageBuffer);
        for (unsigned i = 0; i < TEST_STORAGE_BINDINGS; ++i)
            if (a->storageBuffer[i] != b->storageBuffer[i] || a->storageStart[i] != b->storageStart[i] ||
                a->storageSize[i] != b->storageSize[i])
                fprintf(stderr, "SSBO %u: %d/%lld/%lld -> %d/%lld/%lld\n", i,
                    a->storageBuffer[i], (long long)a->storageStart[i], (long long)a->storageSize[i],
                    b->storageBuffer[i], (long long)b->storageStart[i], (long long)b->storageSize[i]);
    }
    return same;
}

static GLuint make_test_program(void)
{
    static const char *const sources[] = {
        "#version 430 core\nvoid main(){gl_Position=vec4(0.0);}\n",
        "#version 430 core\nout vec4 color; void main(){color=vec4(0.0);}\n"
    };
    GLuint shaders[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        shaders[i] = glCreateShader(i == 0 ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER);
        if (!shaders[i]) goto fail;
        glShaderSource(shaders[i], 1, &sources[i], NULL);
        glCompileShader(shaders[i]);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        if (!compiled) goto fail;
    }
    GLuint program = glCreateProgram();
    if (!program) goto fail;
    glAttachShader(program, shaders[0]);
    glAttachShader(program, shaders[1]);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    if (!linked) { glDeleteProgram(program); return 0; }
    return program;
fail:
    if (shaders[0]) glDeleteShader(shaders[0]);
    if (shaders[1]) glDeleteShader(shaders[1]);
    return 0;
}

static bool make_test_target(GLuint *framebuffer, GLuint *texture,
                             unsigned width, unsigned height, GLenum format)
{
    glGenTextures(1, texture);
    glBindTexture(GL_TEXTURE_2D, *texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, (GLsizei)width, (GLsizei)height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, *framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           *texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    return *texture && *framebuffer &&
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static bool test_empty_resize_state(void)
{
    HikaruGlFragmentList *state = NULL;
    GLuint sceneFramebuffer[2] = {0}, sceneTexture[2] = {0};
    GLuint sentinelFramebuffer[2] = {0}, sentinelTexture[2] = {0};
    GLuint storage[TEST_STORAGE_BINDINGS] = {0}, genericStorage = 0;
    GLuint unpackBuffer = 0, texture0 = 0, texture3 = 0, sampler = 0, vao = 0, program = 0;
    bool ok = false;
    const char *stage = "scene targets";
    GLint storageAlignment = 0;

    for (unsigned i = 0; i < 2; ++i) {
        const unsigned width = i ? 48 : 32, height = i ? 32 : 24;
        const GLenum format = i ? GL_RGB10_A2 : GL_RGBA8;
        if (!make_test_target(&sceneFramebuffer[i], &sceneTexture[i], width, height, format) ||
            !make_test_target(&sentinelFramebuffer[i], &sentinelTexture[i], 4, 4, GL_RGBA8))
            goto cleanup;
    }
    stage = "storage buffers";
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storageAlignment);
    if (storageAlignment <= 0) goto cleanup;
    glGenBuffers(TEST_STORAGE_BINDINGS, storage);
    for (GLuint i = 0; i < TEST_STORAGE_BINDINGS; ++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, storage[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, storageAlignment + 64, NULL, GL_DYNAMIC_DRAW);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, storage[i], storageAlignment, 64);
    }
    glGenBuffers(1, &genericStorage);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, genericStorage);
    glBufferData(GL_SHADER_STORAGE_BUFFER, 128, NULL, GL_DYNAMIC_DRAW);
    glGenBuffers(1, &unpackBuffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 1 << 16, NULL, GL_STREAM_DRAW);
    glGenTextures(1, &texture0);
    glGenTextures(1, &texture3);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, texture3);
    glGenSamplers(1, &sampler);
    glBindSampler(0, sampler);
    glGenVertexArrays(1, &vao);
    stage = "sentinel program";
    program = make_test_program();
    if (!program || !vao || !unpackBuffer || !genericStorage) goto cleanup;
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, sentinelFramebuffer[0]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, sentinelFramebuffer[1]);
    glViewport(3, 5, 17, 19);
    glScissor(2, 4, 13, 15);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_STENCIL_TEST);
    glEnable(GL_RASTERIZER_DISCARD);
    glDisable(GL_DITHER);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    stage = "sentinel GL state";
    if (!test_gl_errors()) goto cleanup;

    /* A rejected budget must restore caller bindings and permit a later retry. */
    TestGlState before = {0}, after = {0};
    stage = "budget rejection state";
    capture_test_gl_state(&before);
    (void)hikaru_gl_fragment_list_begin(&state, sceneFramebuffer[0], sceneTexture[0],
                                       32, 24, 1);
    capture_test_gl_state(&after);
    if (hikaru_gl_fragment_list_active(state) || !same_test_gl_state(&before, &after) ||
        !test_gl_errors()) goto cleanup;

    for (unsigned i = 0; i < 2; ++i) {
        const unsigned width = i ? 48 : 32, height = i ? 32 : 24;
        capture_test_gl_state(&before);
        stage = i ? "48x32 begin" : "32x24 begin";
        const bool began = hikaru_gl_fragment_list_begin(&state, sceneFramebuffer[i],
            sceneTexture[i], width, height, UINT64_C(1) << 20);
        if (!began || !hikaru_gl_fragment_list_active(state) || !test_gl_errors()) goto cleanup;
        stage = i ? "48x32 end" : "32x24 end";
        if (!hikaru_gl_fragment_list_end(state)) goto cleanup;
        stage = i ? "48x32 restored state" : "32x24 restored state";
        capture_test_gl_state(&after);
        if (!same_test_gl_state(&before, &after) || !test_gl_errors()) goto cleanup;
    }
    ok = true;

cleanup:
    if (!ok) {
        fprintf(stderr, "fragment-list fixture failed at %s\n", stage);
        test_gl_errors();
    }
    hikaru_gl_fragment_list_destroy(&state);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    for (GLuint i = 0; i < TEST_STORAGE_BINDINGS; ++i)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    if (program) glDeleteProgram(program);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (sampler) glDeleteSamplers(1, &sampler);
    if (unpackBuffer) glDeleteBuffers(1, &unpackBuffer);
    if (genericStorage) glDeleteBuffers(1, &genericStorage);
    glDeleteBuffers(TEST_STORAGE_BINDINGS, storage);
    for (unsigned i = 0; i < 2; ++i) {
        if (sentinelFramebuffer[i]) glDeleteFramebuffers(1, &sentinelFramebuffer[i]);
        if (sentinelTexture[i]) glDeleteTextures(1, &sentinelTexture[i]);
        if (sceneFramebuffer[i]) glDeleteFramebuffers(1, &sceneFramebuffer[i]);
        if (sceneTexture[i]) glDeleteTextures(1, &sceneTexture[i]);
    }
    if (texture0) glDeleteTextures(1, &texture0);
    if (texture3) glDeleteTextures(1, &texture3);
    if (!test_gl_errors()) ok = false;
    return ok;
}

static int skip(const char *reason)
{
    fprintf(stderr, "fragment-list pixel test skipped: %s\n", reason);
    return 77;
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL video: %s\n", SDL_GetError());
        return skip("no desktop OpenGL context is available");
    }
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3)) {
        SDL_Quit();
        return skip("SDL cannot request a desktop OpenGL 4.3 core context");
    }

    SDL_Window *window = SDL_CreateWindow("Hikaru fragment lists", 0, 0, 16, 16,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        fprintf(stderr, "SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return skip("a hidden desktop GL window could not be created");
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) {
        fprintf(stderr, "SDL GL context: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return skip("desktop OpenGL 4.3 is unavailable");
    }
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return skip("the desktop GL entry points could not be loaded");
    }

    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    GLint storageBindings = 0, fragmentStorageBlocks = 0, combinedStorageBlocks = 0;
    GLint maxFramebufferWidth = 0, maxFramebufferHeight = 0, maxTextureSize = 0;
    GLint64 maxStorageBlock = 0;
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &storageBindings);
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, &fragmentStorageBlocks);
    glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, &combinedStorageBlocks);
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxStorageBlock);
    glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, &maxFramebufferWidth);
    glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, &maxFramebufferHeight);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    if (major < 4 || (major == 4 && minor < 3) || storageBindings < TEST_STORAGE_BINDINGS ||
        fragmentStorageBlocks < TEST_STORAGE_BINDINGS || combinedStorageBlocks < TEST_STORAGE_BINDINGS ||
        maxStorageBlock < 48 * 32 * (GLint64)sizeof(GLuint) ||
        maxFramebufferWidth < 48 || maxFramebufferHeight < 32 || maxTextureSize < 48) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return skip("OpenGL 4.3 fragment shader storage buffers are unavailable");
    }

    if (!test_empty_resize_state()) {
        fprintf(stderr, "GL fragment-list empty resize/state preservation failed\n");
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    puts("GL fragment-list empty resize/state preservation: PASS");

    const int passed = hikaru_vendor_test_fragment_list_pixels();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (!passed) {
        fprintf(stderr, "GPU per-pixel fragment-list output did not match the reference\n");
        return 1;
    }
    puts("GPU per-pixel fragment-list pixels: PASS");
    return 0;
}
#endif
