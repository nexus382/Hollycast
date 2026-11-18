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

static GLuint compile_test_shader(GLenum type, const char *const *sources,
                                  GLsizei sourceCount, const char *label)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, sourceCount, (const GLchar *const *)sources, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[2048] = {0};
        glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "fragment-list fixture %s shader compile failed: %s\n", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint make_test_program_from_sources(const char *const *vertexSources,
    GLsizei vertexSourceCount, const char *const *fragmentSources,
    GLsizei fragmentSourceCount, const char *label)
{
    GLuint shaders[2] = {0};
    GLuint program = 0;
    GLint linked = GL_FALSE;
    shaders[0] = compile_test_shader(GL_VERTEX_SHADER, vertexSources,
                                     vertexSourceCount, label);
    if (!shaders[0]) goto fail;
    shaders[1] = compile_test_shader(GL_FRAGMENT_SHADER, fragmentSources,
                                     fragmentSourceCount, label);
    if (!shaders[1]) goto fail;
    program = glCreateProgram();
    if (!program) goto fail;
    glAttachShader(program, shaders[0]);
    glAttachShader(program, shaders[1]);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[2048] = {0};
        glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
        fprintf(stderr, "fragment-list fixture %s program link failed: %s\n", label, log);
    }
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    if (!linked) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
fail:
    if (program) glDeleteProgram(program);
    if (shaders[0]) glDeleteShader(shaders[0]);
    if (shaders[1]) glDeleteShader(shaders[1]);
    return 0;
}

static GLuint make_test_program(void)
{
    static const char *const vertexSources[] = {
        "#version 430 core\nvoid main(){gl_Position=vec4(0.0);}\n"
    };
    static const char *const fragmentSources[] = {
        "#version 430 core\nout vec4 color; void main(){color=vec4(0.0);}\n"
    };
    return make_test_program_from_sources(vertexSources, 1, fragmentSources, 1,
                                          "sentinel");
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

enum { FRAGMENT_TEST_SIZE = 16, FRAGMENT_TEST_SCISSOR = 4 };

typedef struct FragmentTestLayer {
    GLfloat color[4];
    GLfloat depthCode;
    GLuint blendMode;
} FragmentTestLayer;

typedef struct FragmentTestUniforms {
    GLint width, height, capacity, orderBase, colorLevels, blockCapacity;
    GLint color, depthCode, blendMode;
} FragmentTestUniforms;

/* One oversized triangle per draw covers the fixed scissor region. */
static const char *const fragmentTestVertex[] = {
    "#version 430 core\n"
    "flat out uint p_list_primitive;\n"
    "void main(){const vec2 p[3]=vec2[3](vec2(-1.0,-1.0),vec2(3.0,-1.0),vec2(-1.0,3.0));"
    "gl_Position=vec4(p[gl_VertexID],0.0,1.0);p_list_primitive=uint(gl_VertexID/3);}\n"
};

static GLuint make_fragment_test_program(bool collector)
{
    static const char *const referenceFragment[] = {
        "#version 430 core\nuniform vec4 testColor;out vec4 outColor;"
        "void main(){outColor=testColor;}\n"
    };
    const char *const collectorFragment[] = {
        "#version 430 core\n"
        "uniform vec4 testColor;uniform float testDepthCode;uniform uint testBlendMode;\n",
        hikaru_gl_fragment_list_shader_source(),
        "void main(){collectListFragment(testColor,testDepthCode,testBlendMode);}\n"
    };
    if (collector)
        return make_test_program_from_sources(fragmentTestVertex, 1, collectorFragment, 3,
                                              "production collector");
    return make_test_program_from_sources(fragmentTestVertex, 1, referenceFragment, 1,
                                          "blend reference");
}

static GLint required_test_uniform(GLuint program, const char *name)
{
    const GLint location = glGetUniformLocation(program, name);
    if (location < 0)
        fprintf(stderr, "fragment-list fixture uniform is missing: %s\n", name);
    return location;
}

static bool get_fragment_test_uniforms(GLuint program, FragmentTestUniforms *uniforms)
{
    uniforms->width = required_test_uniform(program, "listParams.width");
    uniforms->height = required_test_uniform(program, "listParams.height");
    uniforms->capacity = required_test_uniform(program, "listParams.capacity");
    uniforms->orderBase = required_test_uniform(program, "listParams.orderBase");
    /* Optional so a byte-only collector reaches the raw pixel precision checks. */
    uniforms->colorLevels = glGetUniformLocation(program, "listParams.colorLevels");
    uniforms->blockCapacity = required_test_uniform(program, "listParams.blockCapacity");
    uniforms->color = required_test_uniform(program, "testColor");
    uniforms->depthCode = required_test_uniform(program, "testDepthCode");
    uniforms->blendMode = required_test_uniform(program, "testBlendMode");
    return uniforms->width >= 0 && uniforms->height >= 0 && uniforms->capacity >= 0 &&
        uniforms->orderBase >= 0 && uniforms->blockCapacity >= 0 &&
        uniforms->color >= 0 && uniforms->depthCode >= 0 && uniforms->blendMode >= 0;
}

static void prepare_fragment_test_target(GLuint framebuffer, const GLfloat background[4])
{
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glViewport(0, 0, FRAGMENT_TEST_SIZE, FRAGMENT_TEST_SIZE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearBufferfv(GL_COLOR, 0, background);
    glScissor(6, 6, FRAGMENT_TEST_SCISSOR, FRAGMENT_TEST_SCISSOR);
    glEnable(GL_SCISSOR_TEST);
}

static bool set_fragment_test_blend(GLuint mode)
{
    glBlendEquation(GL_FUNC_ADD);
    switch (mode) {
    case 0:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return true;
    case 1:
        glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ONE, GL_ZERO);
        return true;
    case 2:
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
        return true;
    case 3:
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
        return true;
    default:
        return false;
    }
}

static bool read_fragment_test_pixel(GLuint framebuffer, GLenum format,
                                     uint32_t pixel[4])
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    if (format == GL_RGB10_A2) {
        /* Read packed target codes so RGBA8 conversion cannot hide precision loss. */
        GLuint packed = 0;
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, &packed);
        pixel[0] = packed & 1023u;
        pixel[1] = (packed >> 10u) & 1023u;
        pixel[2] = (packed >> 20u) & 1023u;
        pixel[3] = (packed >> 30u) & 3u;
    } else {
        GLubyte rgba[4] = {0};
        glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        for (unsigned channel = 0; channel < 4; ++channel) pixel[channel] = rgba[channel];
    }
    return test_gl_errors();
}

static bool fragment_test_pixels_match(const uint32_t a[4], const uint32_t b[4],
                                       uint32_t tolerance)
{
    for (unsigned channel = 0; channel < 4; ++channel) {
        const uint32_t difference = a[channel] > b[channel]
            ? a[channel] - b[channel] : b[channel] - a[channel];
        if (difference > tolerance) return false;
    }
    return true;
}

static bool render_fragment_test_reference(GLuint program, GLuint vertexArray,
    GLint colorLocation, GLuint framebuffer, GLenum format,
    const GLfloat background[4], const FragmentTestLayer *layers,
    unsigned layerCount, uint32_t pixel[4])
{
    prepare_fragment_test_target(framebuffer, background);
    glUseProgram(program);
    glBindVertexArray(vertexArray);
    glEnable(GL_BLEND);
    for (unsigned i = 0; i < layerCount; ++i) {
        if (!set_fragment_test_blend(layers[i].blendMode)) return false;
        glUniform4fv(colorLocation, 1, layers[i].color);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    return read_fragment_test_pixel(framebuffer, format, pixel);
}

static bool render_fragment_test_list(HikaruGlFragmentList **state,
    GLuint program, GLuint vertexArray, const FragmentTestUniforms *uniforms,
    GLuint framebuffer, GLuint texture, GLenum format, const GLfloat background[4],
    const FragmentTestLayer *layers, unsigned layerCount, uint32_t pixel[4])
{
    if (!layerCount || (uint64_t)layerCount * FRAGMENT_TEST_SCISSOR *
        FRAGMENT_TEST_SCISSOR >= 4096u) return false;
    prepare_fragment_test_target(framebuffer, background);
    if (!hikaru_gl_fragment_list_begin(state, framebuffer, texture,
        FRAGMENT_TEST_SIZE, FRAGMENT_TEST_SIZE, UINT64_C(1) << 20)) return false;
    const HikaruFragmentListParameters *parameters =
        hikaru_gl_fragment_list_parameters(*state);
    if (!parameters) {
        hikaru_gl_fragment_list_fail(*state);
        (void)hikaru_gl_fragment_list_end(*state);
        return false;
    }
    glUseProgram(program);
    glBindVertexArray(vertexArray);
    glScissor(6, 6, FRAGMENT_TEST_SCISSOR, FRAGMENT_TEST_SCISSOR);
    glEnable(GL_SCISSOR_TEST);
    glUniform1ui(uniforms->width, parameters->width);
    glUniform1ui(uniforms->height, parameters->height);
    glUniform1ui(uniforms->capacity, parameters->capacity);
    if (uniforms->colorLevels >= 0)
        glUniform1f(uniforms->colorLevels, parameters->colorLevels);
    glUniform1ui(uniforms->blockCapacity, parameters->blockCapacity);
    for (unsigned i = 0; i < layerCount; ++i) {
        parameters = hikaru_gl_fragment_list_parameters(*state);
        glUniform1ui(uniforms->orderBase, parameters->orderBase);
        glUniform4fv(uniforms->color, 1, layers[i].color);
        glUniform1f(uniforms->depthCode, layers[i].depthCode);
        glUniform1ui(uniforms->blendMode, layers[i].blendMode);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (i + 1 < layerCount && !hikaru_gl_fragment_list_advance(*state, 1)) {
            hikaru_gl_fragment_list_fail(*state);
            (void)hikaru_gl_fragment_list_end(*state);
            return false;
        }
    }
    if (!hikaru_gl_fragment_list_end(*state)) return false;
    return read_fragment_test_pixel(framebuffer, format, pixel);
}

static bool check_fragment_test_case(HikaruGlFragmentList **state,
    GLuint collectorProgram, GLuint referenceProgram, GLuint vertexArray,
    const FragmentTestUniforms *uniforms, GLint referenceColor,
    GLuint targetFramebuffer, GLuint targetTexture, GLuint referenceFramebuffer,
    GLenum format, const GLfloat background[4], const FragmentTestLayer *layers,
    unsigned layerCount, const uint32_t *exact, uint32_t tolerance,
    const char *label)
{
    uint32_t expected[4], actual[4];
    if (!render_fragment_test_reference(referenceProgram, vertexArray, referenceColor,
        referenceFramebuffer, format, background, layers, layerCount, expected) ||
        !render_fragment_test_list(state, collectorProgram, vertexArray, uniforms,
            targetFramebuffer, targetTexture, format, background, layers, layerCount, actual)) {
        fprintf(stderr, "fragment-list fixture failed: %s\n", label);
        return false;
    }
    if ((exact && (!fragment_test_pixels_match(expected, exact, 0) ||
                   !fragment_test_pixels_match(actual, exact, 0))) ||
        !fragment_test_pixels_match(expected, actual, tolerance)) {
        fprintf(stderr, "fragment-list fixture %s: reference=%u/%u/%u/%u list=%u/%u/%u/%u\n",
            label, expected[0], expected[1], expected[2], expected[3],
            actual[0], actual[1], actual[2], actual[3]);
        return false;
    }
    return true;
}

static bool test_fragment_list_rgb10_precision(void)
{
    const GLenum formats[2] = {GL_RGBA8, GL_RGB10_A2};
    GLuint targetFramebuffer[2] = {0}, targetTexture[2] = {0};
    GLuint referenceFramebuffer[2] = {0}, referenceTexture[2] = {0};
    GLuint collectorProgram = 0, referenceProgram = 0, vertexArray = 0;
    GLint referenceColor = -1;
    FragmentTestUniforms uniforms = {0};
    HikaruGlFragmentList *state = NULL;
    bool ok = false;
    const GLfloat rgba8Background[4] = {
        51.0f/255.0f, 102.0f/255.0f, 153.0f/255.0f, 204.0f/255.0f
    };
    const GLfloat rgb10Black[4] = {0, 0, 0, 0};
    const GLfloat rgb10White[4] = {1, 1, 1, 1};
    const GLfloat rgb10Background[4] = {
        205.0f/1023.0f, 410.0f/1023.0f, 615.0f/1023.0f, 2.0f/3.0f
    };
    const FragmentTestLayer rgba8Layers[4] = {
        {{64.0f/255.0f, 128.0f/255.0f, 192.0f/255.0f, 128.0f/255.0f}, 0, 0},
        {{64.0f/255.0f, 128.0f/255.0f, 192.0f/255.0f, 128.0f/255.0f}, 0, 1},
        {{64.0f/255.0f, 128.0f/255.0f, 192.0f/255.0f, 128.0f/255.0f}, 0, 2},
        {{64.0f/255.0f, 128.0f/255.0f, 192.0f/255.0f, 128.0f/255.0f}, 0, 3}
    };
    const FragmentTestLayer rgb10AlphaLayers[2] = {
        {{410.0f/1023.0f, 615.0f/1023.0f, 820.0f/1023.0f, 63.0f/255.0f}, 0, 0},
        {{820.0f/1023.0f, 205.0f/1023.0f, 410.0f/1023.0f, 128.0f/255.0f}, 0, 3}
    };
    static const unsigned additiveCounts[] = {1, 8, 9, 32};
    static const char *const additiveLabels[] = {
        "RGB10 one-layer 1/5/9 precision",
        "RGB10 eight-layer small resolver",
        "RGB10 nine-layer general resolver",
        "RGB10 32-layer additive precision"
    };
    FragmentTestLayer additiveLayers[32];
    FragmentTestLayer multiplyLayers[64];

    for (unsigned i = 0; i < 2; ++i) {
        if (!make_test_target(&targetFramebuffer[i], &targetTexture[i],
                FRAGMENT_TEST_SIZE, FRAGMENT_TEST_SIZE, formats[i]) ||
            !make_test_target(&referenceFramebuffer[i], &referenceTexture[i],
                FRAGMENT_TEST_SIZE, FRAGMENT_TEST_SIZE, formats[i])) goto cleanup;
    }
    collectorProgram = make_fragment_test_program(true);
    referenceProgram = make_fragment_test_program(false);
    if (!collectorProgram || !referenceProgram) goto cleanup;
    referenceColor = required_test_uniform(referenceProgram, "testColor");
    if (referenceColor < 0 || !get_fragment_test_uniforms(collectorProgram, &uniforms))
        goto cleanup;
    glGenVertexArrays(1, &vertexArray);
    if (!vertexArray) goto cleanup;

    if (!check_fragment_test_case(&state, collectorProgram, referenceProgram, vertexArray,
        &uniforms, referenceColor, targetFramebuffer[0], targetTexture[0],
        referenceFramebuffer[0], formats[0], rgba8Background, rgba8Layers, 4,
        NULL, 1, "RGBA8 control before format switch")) goto cleanup;

    for (unsigned i = 0; i < 32; ++i) {
        additiveLayers[i] = (FragmentTestLayer){
            {1.0f/1023.0f, 5.0f/1023.0f, 9.0f/1023.0f, 1.0f}, 0, 2
        };
    }
    for (unsigned i = 0; i < sizeof(additiveCounts) / sizeof(additiveCounts[0]); ++i) {
        const unsigned count = additiveCounts[i];
        const uint32_t expected[4] = {count, count*5u, count*9u, 3u};
        if (!check_fragment_test_case(&state, collectorProgram, referenceProgram,
            vertexArray, &uniforms, referenceColor, targetFramebuffer[1], targetTexture[1],
            referenceFramebuffer[1], formats[1], rgb10Black, additiveLayers, count,
            expected, 0, additiveLabels[i])) goto cleanup;
    }

    for (unsigned i = 0; i < 64; ++i) {
        multiplyLayers[i] = (FragmentTestLayer){
            {1022.0f/1023.0f, 1022.0f/1023.0f, 1022.0f/1023.0f, 1.0f}, 0, 1
        };
    }
    const uint32_t multiplyExpected[4] = {1022u, 1022u, 1022u, 3u};
    if (!check_fragment_test_case(&state, collectorProgram, referenceProgram,
        vertexArray, &uniforms, referenceColor, targetFramebuffer[1], targetTexture[1],
        referenceFramebuffer[1], formats[1], rgb10White, multiplyLayers, 1,
        multiplyExpected, 0, "RGB10 one-layer 1022/1023 multiply")) goto cleanup;

    /* RTE after each layer subtracts exactly one RGB10 code: 1023 - 64 = 959.
     * Fixed-function UNORM conversions may accumulate driver-specific rounding,
     * so use the resolver's explicit quantization contract for this long chain. */
    const uint32_t multiplyChainExpected[4] = {959u, 959u, 959u, 3u};
    uint32_t multiplyChainActual[4] = {0};
    if (!render_fragment_test_list(&state, collectorProgram, vertexArray, &uniforms,
        targetFramebuffer[1], targetTexture[1], formats[1], rgb10White,
        multiplyLayers, 64, multiplyChainActual) ||
        !fragment_test_pixels_match(multiplyChainActual, multiplyChainExpected, 0)) {
        fprintf(stderr, "RGB10 64-layer multiply: expected=959/959/959/3 list=%u/%u/%u/%u\n",
            multiplyChainActual[0], multiplyChainActual[1],
            multiplyChainActual[2], multiplyChainActual[3]);
        goto cleanup;
    }

    if (!check_fragment_test_case(&state, collectorProgram, referenceProgram,
        vertexArray, &uniforms, referenceColor, targetFramebuffer[1], targetTexture[1],
        referenceFramebuffer[1], formats[1], rgb10Background, rgb10AlphaLayers, 2,
        NULL, 1, "RGB10 eight-bit source alpha and blend mode")) goto cleanup;

    if (!check_fragment_test_case(&state, collectorProgram, referenceProgram, vertexArray,
        &uniforms, referenceColor, targetFramebuffer[0], targetTexture[0],
        referenceFramebuffer[0], formats[0], rgba8Background, rgba8Layers, 4,
        NULL, 1, "RGBA8 control after format switch")) goto cleanup;
    ok = true;

cleanup:
    if (!ok) fprintf(stderr, "GL fragment-list RGB10 fixture failed\n");
    if (state) hikaru_gl_fragment_list_destroy(&state);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDeleteFramebuffers(2, referenceFramebuffer);
    glDeleteTextures(2, referenceTexture);
    glDeleteFramebuffers(2, targetFramebuffer);
    glDeleteTextures(2, targetTexture);
    if (collectorProgram) glDeleteProgram(collectorProgram);
    if (referenceProgram) glDeleteProgram(referenceProgram);
    if (vertexArray) glDeleteVertexArrays(1, &vertexArray);
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

    if (!test_fragment_list_rgb10_precision()) {
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    puts("GL fragment-list RGB10 precision: PASS");

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
