// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_fragment_list.h"
#include "gl_diagnostics.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)

struct HikaruGlFragmentList { bool failed, disabled; };

bool hikaru_gl_fragment_list_begin(HikaruGlFragmentList **state, GLuint framebuffer,
                                  GLuint texture, unsigned width, unsigned height,
                                  uint64_t budget)
{
    (void)framebuffer; (void)texture; (void)width; (void)height; (void)budget;
    if (state && !*state) *state = calloc(1, sizeof(**state));
    return false;
}
bool hikaru_gl_fragment_list_end(HikaruGlFragmentList *state) { (void)state; return false; }
bool hikaru_gl_fragment_list_active(const HikaruGlFragmentList *state) { (void)state; return false; }
const HikaruFragmentListParameters *hikaru_gl_fragment_list_parameters(const HikaruGlFragmentList *state) { (void)state; return NULL; }
bool hikaru_gl_fragment_list_advance(HikaruGlFragmentList *state, uint32_t count) { (void)state; (void)count; return false; }
void hikaru_gl_fragment_list_fail(HikaruGlFragmentList *state) { if (state) state->failed = state->disabled = true; }
void hikaru_gl_fragment_list_reset(HikaruGlFragmentList *state) { if (state) state->failed = state->disabled = false; }
void hikaru_gl_fragment_list_destroy(HikaruGlFragmentList **state) { if (state) { free(*state); *state = NULL; } }
const char *hikaru_gl_fragment_list_shader_source(void) { return ""; }

#else

#define GL_LIST_BINDINGS 6u
#define GL_LIST_NODE_BYTES 16u
#define GL_LIST_STATUS_BYTES 16u

typedef struct GlSavedState {
    GLint drawFramebuffer, readFramebuffer, program, vertexArray;
    GLint activeTexture, texture0, sampler0, unpackBuffer;
    GLint viewport[4], scissor[4];
    GLint polygonMode[2];
    bool separatePolygonModes;
    GLint genericStorageBuffer;
    GLint storageBuffer[GL_LIST_BINDINGS];
    GLint64 storageStart[GL_LIST_BINDINGS], storageSize[GL_LIST_BINDINGS];
    GLboolean blend, depthTest, cull, scissorTest, stencilTest, rasterizerDiscard, dither;
    GLboolean depthMask, colorMask[4];
} GlSavedState;

typedef struct GlResources {
    GLuint heads, status, nodes[4];
    GLuint background, collectorFramebuffer, resolveFramebuffer;
    GLuint sceneTexture;
    unsigned width, height, capacity;
    GLenum colorFormat;
} GlResources;

typedef struct GlResolver {
    GLuint program, vertexArray;
    GLint backgroundUniform;
    GLint widthUniform, blockCapacityUniform;
    GLint colorLevelsUniform, alphaLevelsUniform;
} GlResolver;

struct HikaruGlFragmentList {
    GlResources resources;
    GlResolver resolver;
    HikaruFragmentPoolPolicy policy;
    HikaruFragmentListParameters parameters;
    GlSavedState saved;
    GLuint sceneTexture;
    GLint64 maxStorageBlock;
    GLint maxFramebufferWidth, maxFramebufferHeight, maxTextureSize;
    unsigned policyWidth, policyHeight;
    GLenum policyFormat;
    uint64_t collections, resolves, fallbacks, overflows, allocations;
    uint32_t lastUsed;
    bool lastOverflow;
    bool checked, supported, limitsChecked, limitsSupported, policyExtentValid, compatibilityProfile;
    bool active, failed, disabled;
};

static bool trace_timing_enabled(void)
{
    static _Thread_local int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("HIKARU_TRACE_TIMING");
        enabled = value && value[0] && strcmp(value, "0") != 0;
    }
    return enabled != 0;
}

static void trace_collection(HikaruGlFragmentList *state)
{
    const uint64_t sample = ++state->collections;
    if (trace_timing_enabled() && (sample == 1 || sample % 60 == 0)) {
        fprintf(stderr,
            "Hikaru GL fragment lists: extent=%ux%u capacity=%u used=%u overflow=%u fallback=%llu resolves=%llu allocs=%llu node_bytes=%llu\n",
            state->parameters.width, state->parameters.height,
            state->parameters.capacity, state->lastUsed,
            state->lastOverflow ? 1u : 0u,
            (unsigned long long)state->fallbacks,
            (unsigned long long)state->resolves,
            (unsigned long long)state->allocations,
            (unsigned long long)state->parameters.capacity * GL_LIST_NODE_BYTES);
    }
}

static const char fragmentListSource[] =
    "#define LIST_HELPER_GUARD if(gl_SampleMaskIn[0]==0)return;\n"
    "#define LIST_HEADS_LAYOUT layout(std430,binding=0)\n"
    "#define LIST_NODES_LAYOUT layout(std430,binding=1)\n"
    "#define LIST_STATUS_LAYOUT layout(std430,binding=5)\n"
    HIKARU_FRAGMENT_LIST_TYPES_SOURCE
    "uniform HikaruListParameters listParams;\n"
    HIKARU_FRAGMENT_LIST_BUFFERS_SOURCE
    HIKARU_FRAGMENT_LIST_COLLECT_SOURCE;

const char *hikaru_gl_fragment_list_shader_source(void)
{
    return fragmentListSource;
}

static bool gl43_available(void)
{
    if (!glad_glGetString || !glad_glGetIntegerv) return false;
    const GLubyte *version = glGetString(GL_VERSION);
    if (!version || strncmp((const char *)version, "OpenGL ES", 9) == 0)
        return false;
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 4 || (major == 4 && minor < 3)) return false;
    return glad_glGetIntegerv && glad_glGetIntegeri_v && glad_glGetInteger64i_v &&
        glad_glGetInteger64v && glad_glGetBooleanv && glad_glBindBufferBase &&
        glad_glBindBuffer && glad_glBindBufferRange && glad_glBufferData &&
        glad_glGetBufferParameteri64v && glad_glClearBufferData && glad_glMemoryBarrier &&
        glad_glGetBufferSubData && glad_glCopyImageSubData && glad_glFramebufferParameteri &&
        glad_glGetFramebufferAttachmentParameteriv && glad_glCheckFramebufferStatus &&
        glad_glBindFramebuffer && glad_glDrawBuffer && glad_glReadBuffer &&
        glad_glGenBuffers && glad_glDeleteBuffers && glad_glGenTextures &&
        glad_glDeleteTextures && glad_glTexImage2D && glad_glTexParameteri &&
        glad_glGetTexLevelParameteriv &&
        glad_glGenFramebuffers && glad_glDeleteFramebuffers && glad_glFramebufferTexture2D &&
        glad_glGenVertexArrays && glad_glDeleteVertexArrays && glad_glBindVertexArray &&
        glad_glCreateShader && glad_glShaderSource && glad_glCompileShader &&
        glad_glGetShaderiv && glad_glGetShaderInfoLog && glad_glDeleteShader &&
        glad_glCreateProgram && glad_glAttachShader && glad_glLinkProgram &&
        glad_glGetProgramiv && glad_glGetProgramInfoLog && glad_glDeleteProgram &&
        glad_glGetUniformLocation && glad_glUseProgram && glad_glUniform1i &&
        glad_glUniform1ui && glad_glUniform1f && glad_glActiveTexture &&
        glad_glBindTexture && glad_glBindSampler && glad_glViewport &&
        glad_glScissor && glad_glEnable && glad_glDisable && glad_glIsEnabled && glad_glColorMask &&
        glad_glDepthMask && glad_glDrawArrays && glad_glPolygonMode && glad_glGetError;
}

static bool take_gl_errors(void)
{
    bool failed = false;
    while (glGetError() != GL_NO_ERROR) failed = true;
    return failed;
}

static bool query_limits(GLint64 *maxBlock, GLint *maxBindings,
                         GLint *maxFragmentBlocks, GLint *maxCombinedBlocks,
                         GLint *maxFramebufferWidth, GLint *maxFramebufferHeight,
                         GLint *maxTextureSize)
{
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, maxBlock);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, maxBindings);
    glGetIntegerv(GL_MAX_FRAGMENT_SHADER_STORAGE_BLOCKS, maxFragmentBlocks);
    glGetIntegerv(GL_MAX_COMBINED_SHADER_STORAGE_BLOCKS, maxCombinedBlocks);
    glGetIntegerv(GL_MAX_FRAMEBUFFER_WIDTH, maxFramebufferWidth);
    glGetIntegerv(GL_MAX_FRAMEBUFFER_HEIGHT, maxFramebufferHeight);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, maxTextureSize);
    return *maxBlock >= GL_LIST_STATUS_BYTES && *maxBindings >= (GLint)GL_LIST_BINDINGS &&
        *maxFragmentBlocks >= (GLint)GL_LIST_BINDINGS &&
        *maxCombinedBlocks >= (GLint)GL_LIST_BINDINGS &&
        *maxFramebufferWidth > 0 && *maxFramebufferHeight > 0 && *maxTextureSize > 0;
}

static bool cache_limits(HikaruGlFragmentList *state)
{
    if (state->limitsChecked) return state->limitsSupported;
    state->limitsChecked = true;
    GLint profile = 0;
    glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    state->compatibilityProfile = (profile & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0;
    GLint maxBindings = 0, maxFragmentBlocks = 0, maxCombinedBlocks = 0;
    state->limitsSupported = query_limits(&state->maxStorageBlock, &maxBindings,
        &maxFragmentBlocks, &maxCombinedBlocks, &state->maxFramebufferWidth,
        &state->maxFramebufferHeight, &state->maxTextureSize);
    if (!state->limitsSupported) state->supported = false;
    return state->limitsSupported;
}

static void save_gl_state(GlSavedState *saved)
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved->drawFramebuffer);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved->readFramebuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved->vertexArray);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved->unpackBuffer);
    /* Core exposes one mode; compatibility can expose separate front/back. */
    saved->polygonMode[1] = 0;
    glGetIntegerv(GL_POLYGON_MODE, saved->polygonMode);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved->activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved->texture0);
    glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &saved->sampler0);
    glActiveTexture((GLenum)saved->activeTexture);
    glGetIntegerv(GL_VIEWPORT, saved->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, saved->scissor);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &saved->genericStorageBuffer);
    for (GLuint i = 0; i < GL_LIST_BINDINGS; ++i) {
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, i, &saved->storageBuffer[i]);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, i, &saved->storageStart[i]);
        glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, i, &saved->storageSize[i]);
    }
    saved->blend = glIsEnabled(GL_BLEND);
    saved->depthTest = glIsEnabled(GL_DEPTH_TEST);
    saved->cull = glIsEnabled(GL_CULL_FACE);
    saved->scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    saved->stencilTest = glIsEnabled(GL_STENCIL_TEST);
    saved->rasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
    saved->dither = glIsEnabled(GL_DITHER);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &saved->depthMask);
    glGetBooleanv(GL_COLOR_WRITEMASK, saved->colorMask);
}

static void restore_gl_state(const GlSavedState *saved)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved->drawFramebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved->readFramebuffer);
    for (GLuint i = 0; i < GL_LIST_BINDINGS; ++i) {
        const GLuint buffer = (GLuint)saved->storageBuffer[i];
        if (buffer && saved->storageSize[i] > 0)
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER, i, buffer,
                              (GLintptr)saved->storageStart[i],
                              (GLsizeiptr)saved->storageSize[i]);
        else
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i, buffer);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, (GLuint)saved->genericStorageBuffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved->texture0);
    glBindSampler(0, (GLuint)saved->sampler0);
    glActiveTexture((GLenum)saved->activeTexture);
    glUseProgram((GLuint)saved->program);
    glBindVertexArray((GLuint)saved->vertexArray);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved->unpackBuffer);
    glViewport(saved->viewport[0], saved->viewport[1], saved->viewport[2], saved->viewport[3]);
    glScissor(saved->scissor[0], saved->scissor[1], saved->scissor[2], saved->scissor[3]);
    if (saved->separatePolygonModes && saved->polygonMode[0] != saved->polygonMode[1]) {
        glPolygonMode(GL_FRONT, (GLenum)saved->polygonMode[0]);
        glPolygonMode(GL_BACK, (GLenum)saved->polygonMode[1]);
    } else glPolygonMode(GL_FRONT_AND_BACK, (GLenum)saved->polygonMode[0]);
    glColorMask(saved->colorMask[0], saved->colorMask[1], saved->colorMask[2], saved->colorMask[3]);
    glDepthMask(saved->depthMask);
    if (saved->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (saved->depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (saved->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (saved->scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (saved->stencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (saved->rasterizerDiscard) glEnable(GL_RASTERIZER_DISCARD); else glDisable(GL_RASTERIZER_DISCARD);
    if (saved->dither) glEnable(GL_DITHER); else glDisable(GL_DITHER);
}

static void delete_storage_buffers(GlResources *resources)
{
    if (resources->heads) glDeleteBuffers(1, &resources->heads);
    if (resources->status) glDeleteBuffers(1, &resources->status);
    glDeleteBuffers(4, resources->nodes);
    resources->heads = resources->status = 0;
    memset(resources->nodes, 0, sizeof(resources->nodes));
}

static void delete_storage(GlResources *resources)
{
    delete_storage_buffers(resources);
    if (resources->background) glDeleteTextures(1, &resources->background);
    if (resources->collectorFramebuffer) glDeleteFramebuffers(1, &resources->collectorFramebuffer);
    if (resources->resolveFramebuffer) glDeleteFramebuffers(1, &resources->resolveFramebuffer);
    memset(resources, 0, sizeof(*resources));
}

static void delete_resolver(GlResolver *resolver)
{
    if (resolver->program) glDeleteProgram(resolver->program);
    if (resolver->vertexArray) glDeleteVertexArrays(1, &resolver->vertexArray);
    memset(resolver, 0, sizeof(*resolver));
}

static bool allocate_buffer(GLuint *buffer, uint64_t bytes)
{
    if (!bytes || bytes > (uint64_t)PTRDIFF_MAX) return false;
    glGenBuffers(1, buffer);
    if (!*buffer) return false;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, *buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)bytes, NULL, GL_DYNAMIC_DRAW);
    GLint64 actual = 0;
    glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &actual);
    return actual == (GLint64)bytes;
}

static bool allocate_storage_buffers(GlResources *resources, uint64_t pixels,
                                     uint32_t capacity, uint64_t maxBlock)
{
    const uint64_t headBytes = pixels * sizeof(uint32_t);
    const uint64_t nodeBytes = (uint64_t)(capacity / 4u) * GL_LIST_NODE_BYTES;
    if (nodeBytes > maxBlock || headBytes > maxBlock || !capacity || capacity % 4u)
        return false;
    if (!allocate_buffer(&resources->heads, headBytes) ||
        !allocate_buffer(&resources->status, GL_LIST_STATUS_BYTES))
        goto fail;
    for (unsigned i = 0; i < 4; ++i)
        if (!allocate_buffer(&resources->nodes[i], nodeBytes)) goto fail;
    if (take_gl_errors()) {
        delete_storage_buffers(resources);
        return false;
    }
    return true;

fail:
    take_gl_errors();
    delete_storage_buffers(resources);
    return false;
}

static GLuint compile_shader(GLenum type, const char *const *sources, GLsizei count)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, count, sources, NULL);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[768] = {0};
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "Hikaru GL fragment-list shader compile failed: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint create_resolve_program(GlResolver *resolver)
{
    static const char *const vertexSource[] = {
        "#version 430 core\n"
        "void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);"
        "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}\n"
    };
    static const char *const fragmentSource[] = {
        "#version 430 core\n",
        HIKARU_FRAGMENT_LIST_TYPES_SOURCE,
        "uniform HikaruListParameters listParams;\n",
        "#define LIST_HEADS_LAYOUT layout(std430,binding=0)\n"
        "#define LIST_NODES_LAYOUT layout(std430,binding=1)\n"
        "#define LIST_STATUS_LAYOUT layout(std430,binding=5)\n",
        HIKARU_FRAGMENT_LIST_BUFFERS_SOURCE,
        "uniform sampler2D listBackground;\n"
        "layout(location=0) out vec4 outColor;\n",
        HIKARU_FRAGMENT_LIST_RESOLVE_HEAD_SOURCE,
        HIKARU_FRAGMENT_LIST_RESOLVE_SMALL_SOURCE,
        HIKARU_FRAGMENT_LIST_RESOLVE_TAIL_SOURCE
    };
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vertexSource,
                                   (GLsizei)(sizeof(vertexSource) / sizeof(vertexSource[0])));
    if (!vertex) return 0;
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fragmentSource,
                                     (GLsizei)(sizeof(fragmentSource) / sizeof(fragmentSource[0])));
    if (!fragment) { glDeleteShader(vertex); return 0; }
    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[768] = {0};
            glGetProgramInfoLog(program, sizeof(log), NULL, log);
            fprintf(stderr, "Hikaru GL fragment-list resolve link failed: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!program) return 0;
    resolver->backgroundUniform = glGetUniformLocation(program, "listBackground");
    resolver->widthUniform = glGetUniformLocation(program, "listParams.width");
    resolver->blockCapacityUniform = glGetUniformLocation(program, "listParams.blockCapacity");
    resolver->colorLevelsUniform = glGetUniformLocation(program, "listParams.colorLevels");
    resolver->alphaLevelsUniform = glGetUniformLocation(program, "listParams.alphaLevels");
    if (resolver->backgroundUniform < 0 || resolver->widthUniform < 0 ||
        resolver->blockCapacityUniform < 0 || resolver->colorLevelsUniform < 0 ||
        resolver->alphaLevelsUniform < 0) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static bool ensure_resolver(HikaruGlFragmentList *state)
{
    if (state->resolver.program && state->resolver.vertexArray) return true;
    const bool collectDiagnostics = hikaru_gl_diagnostics_collecting;
    if (collectDiagnostics)
        hikaru_gl_diagnostics_count(HIKARU_GL_DIAG_PROGRAM_COMPILES, 1);
    const uint64_t compileStart = collectDiagnostics
        ? hikaru_gl_diagnostics_cpu_begin() : 0;
    state->resolver.program = create_resolve_program(&state->resolver);
    if (!state->resolver.program) {
        if (collectDiagnostics)
            hikaru_gl_diagnostics_cpu_end(HIKARU_GL_DIAG_PROGRAM_COMPILE_NS, compileStart);
        return false;
    }
    glGenVertexArrays(1, &state->resolver.vertexArray);
    if (!state->resolver.vertexArray) {
        delete_resolver(&state->resolver);
        if (collectDiagnostics)
            hikaru_gl_diagnostics_cpu_end(HIKARU_GL_DIAG_PROGRAM_COMPILE_NS, compileStart);
        return false;
    }
    if (collectDiagnostics)
        hikaru_gl_diagnostics_cpu_end(HIKARU_GL_DIAG_PROGRAM_COMPILE_NS, compileStart);
    return true;
}

static bool create_resources(GlResources *resources, unsigned width, unsigned height,
                             GLenum colorFormat, GLuint sceneTexture, uint32_t capacity,
                             uint64_t maxBlock)
{
    memset(resources, 0, sizeof(*resources));
    const uint64_t pixels = (uint64_t)width * height;
    if (!allocate_storage_buffers(resources, pixels, capacity, maxBlock)) goto fail;

    glGenTextures(1, &resources->background);
    if (!resources->background) goto fail;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resources->background);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)colorFormat, (GLsizei)width, (GLsizei)height,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLint actualWidth = 0, actualHeight = 0, actualFormat = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &actualFormat);
    if (actualWidth != (GLint)width || actualHeight != (GLint)height ||
        actualFormat != (GLint)colorFormat) goto fail;

    glGenFramebuffers(1, &resources->collectorFramebuffer);
    glGenFramebuffers(1, &resources->resolveFramebuffer);
    if (!resources->collectorFramebuffer || !resources->resolveFramebuffer) goto fail;
    glBindFramebuffer(GL_FRAMEBUFFER, resources->collectorFramebuffer);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_WIDTH, (GLint)width);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_HEIGHT, (GLint)height);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_LAYERS, 0);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_SAMPLES, 0);
    glFramebufferParameteri(GL_FRAMEBUFFER, GL_FRAMEBUFFER_DEFAULT_FIXED_SAMPLE_LOCATIONS, GL_TRUE);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto fail;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources->resolveFramebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           sceneTexture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) goto fail;
    resources->width = width;
    resources->height = height;
    resources->capacity = capacity;
    resources->colorFormat = colorFormat;
    resources->sceneTexture = sceneTexture;
    if (take_gl_errors()) goto fail;
    return true;

fail:
    take_gl_errors();
    delete_storage(resources);
    return false;
}

static bool validate_scene_target(GLuint framebuffer, GLuint texture,
                                  unsigned width, unsigned height, GLenum *format)
{
    if (!framebuffer || !texture) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return false;
    GLint objectType = GL_NONE, objectName = 0;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &objectType);
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &objectName);
    if (objectType != GL_TEXTURE || (GLuint)objectName != texture) return false;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    GLint samples = 0;
    glGetIntegerv(GL_SAMPLES, &samples);
    if (samples > 1) return false;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    GLint texWidth = 0, texHeight = 0, internalFormat = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
    if (texWidth != (GLint)width || texHeight != (GLint)height) return false;
    if (internalFormat != GL_RGBA8 && internalFormat != GL_RGB10_A2) return false;
    *format = (GLenum)internalFormat;
    return true;
}

static bool ensure_resources(HikaruGlFragmentList *state, unsigned width, unsigned height,
                             GLenum format, GLuint sceneTexture, uint32_t capacity,
                             uint64_t maxBlock)
{
    GlResources *current = &state->resources;
    if (!current->collectorFramebuffer || current->width != width ||
        current->height != height || current->colorFormat != format) {
        GlResources replacement = {0};
        if (!create_resources(&replacement, width, height, format, sceneTexture,
                              capacity, maxBlock))
            return false;
        delete_storage(current);
        *current = replacement;
        state->policy.hint = capacity;
        hikaru_fragment_pool_reset_window(&state->policy);
    } else if (current->capacity != capacity) {
        GlResources replacement = {0};
        if (!allocate_storage_buffers(&replacement, (uint64_t)width * height,
                                       capacity, maxBlock))
            return false;
        delete_storage_buffers(current);
        current->heads = replacement.heads;
        current->status = replacement.status;
        memcpy(current->nodes, replacement.nodes, sizeof(current->nodes));
        current->capacity = capacity;
        state->policy.hint = capacity;
        hikaru_fragment_pool_reset_window(&state->policy);
    }
    if (current->sceneTexture != sceneTexture) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, current->resolveFramebuffer);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               sceneTexture, 0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
        current->sceneTexture = sceneTexture;
    }
    return true;
}

static void bind_storage(const GlResources *resources)
{
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, resources->heads);
    for (GLuint i = 0; i < 4; ++i)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, i + 1, resources->nodes[i]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, resources->status);
}

static bool calculate_capacity(HikaruGlFragmentList *state, uint64_t pixels,
                               uint64_t budget, uint64_t maxBlock,
                               uint32_t *capacity)
{
    if (pixels > (UINT64_MAX - 32u) / 8u || pixels > UINT32_MAX / sizeof(uint32_t))
        return false;
    const uint64_t fixedBytes = pixels * 8u + 32u;
    const uint64_t budgetNodes = budget > fixedBytes ? (budget - fixedBytes) / GL_LIST_NODE_BYTES : 0;
    const uint64_t blockNodes = (maxBlock / GL_LIST_NODE_BYTES) * 4u;
    uint64_t maximum = budgetNodes < blockNodes ? budgetNodes : blockNodes;
    if (maximum > UINT32_MAX) maximum = UINT32_MAX;
    maximum &= ~UINT64_C(3);
    if (maximum < 4) return false;
    uint64_t desired = state->policy.hint
        ? (state->policy.hint < 4096u ? 4096u : state->policy.hint)
        : (pixels < 4096u ? 4096u : pixels);
    if (desired > maximum) desired = maximum;
    desired &= ~UINT64_C(3);
    if (desired < 4) return false;
    *capacity = (uint32_t)desired;
    return true;
}

bool hikaru_gl_fragment_list_begin(HikaruGlFragmentList **statePointer,
                                   GLuint sceneFramebuffer, GLuint sceneTexture,
                                   unsigned width, unsigned height, uint64_t budget)
{
    if (!statePointer) return false;
    if (!*statePointer) {
        *statePointer = calloc(1, sizeof(**statePointer));
        if (!*statePointer) return false;
    }
    HikaruGlFragmentList *state = *statePointer;
    if (state->active || state->disabled) return false;
    if (!state->checked) {
        state->supported = gl43_available();
        state->checked = true;
    }
    if (!state->supported || !cache_limits(state)) return false;
    if (!width || !height || width > INT_MAX || height > INT_MAX) {
        delete_storage(&state->resources);
        return false;
    }

    save_gl_state(&state->saved);
    state->saved.separatePolygonModes = state->compatibilityProfile;
    const GLint64 maxBlock = state->maxStorageBlock;
    GLenum colorFormat = 0;
    const bool targetFits = (GLint64)width <= state->maxFramebufferWidth &&
        (GLint64)height <= state->maxFramebufferHeight && (GLint)width <= state->maxTextureSize &&
        (GLint)height <= state->maxTextureSize;
    const bool targetValid = targetFits &&
        validate_scene_target(sceneFramebuffer, sceneTexture, width, height, &colorFormat);
    if (!targetValid) {
        take_gl_errors();
        restore_gl_state(&state->saved);
        delete_storage(&state->resources);
        return false;
    }
    const uint64_t pixels = (uint64_t)width * height;
    if (maxBlock <= 0 || pixels > (uint64_t)maxBlock / 4u) {
        restore_gl_state(&state->saved);
        delete_storage(&state->resources);
        return false;
    }
    if (!state->policyExtentValid || state->policyWidth != width ||
        state->policyHeight != height || state->policyFormat != colorFormat) {
        memset(&state->policy, 0, sizeof(state->policy));
        state->policyWidth = width;
        state->policyHeight = height;
        state->policyFormat = colorFormat;
        state->policyExtentValid = true;
    }
    uint32_t capacity = 0;
    if (!calculate_capacity(state, pixels, budget, (uint64_t)maxBlock, &capacity)) {
        restore_gl_state(&state->saved);
        delete_storage(&state->resources);
        return false;
    }
    const GlResources *current = &state->resources;
    const bool storageChanges = !current->heads || current->width != width ||
        current->height != height || current->colorFormat != colorFormat ||
        current->capacity != capacity;
    if (!ensure_resolver(state) ||
        !ensure_resources(state, width, height, colorFormat, sceneTexture,
                          capacity, (uint64_t)maxBlock)) {
        take_gl_errors();
        restore_gl_state(&state->saved);
        delete_storage(&state->resources);
        state->disabled = true;
        fprintf(stderr, "Hikaru: GL fragment-list setup unavailable; using depth peeling\n");
        return false;
    }
    if (storageChanges) ++state->allocations;

    GlResources *resources = &state->resources;
    state->parameters = (HikaruFragmentListParameters){
        width, height, capacity, 0,
        colorFormat == GL_RGB10_A2 ? 1023.f : 255.f,
        colorFormat == GL_RGB10_A2 ? 3.f : 255.f,
        capacity / 4u
    };
    state->lastUsed = 0;
    state->lastOverflow = false;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    if (hikaru_gl_diagnostics_collecting)
        hikaru_gl_diagnostics_phase_begin(HIKARU_GL_DIAG_PHASE_CLEAR);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, resources->heads);
    const GLuint zero = 0;
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                      GL_UNSIGNED_INT, &zero);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, resources->status);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER,
                      GL_UNSIGNED_INT, &zero);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    if (hikaru_gl_diagnostics_collecting)
        hikaru_gl_diagnostics_phase_end(HIKARU_GL_DIAG_PHASE_CLEAR);
    if (hikaru_gl_diagnostics_collecting)
        hikaru_gl_diagnostics_phase_begin(HIKARU_GL_DIAG_PHASE_COLLECT);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources->collectorFramebuffer);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    bind_storage(resources);
    if (take_gl_errors()) {
        if (hikaru_gl_diagnostics_collecting)
            hikaru_gl_diagnostics_phase_end(HIKARU_GL_DIAG_PHASE_COLLECT);
        restore_gl_state(&state->saved);
        delete_storage(resources);
        state->disabled = true;
        fprintf(stderr, "Hikaru: GL fragment-list setup failed; using depth peeling\n");
        return false;
    }
    state->sceneTexture = sceneTexture;
    state->failed = false;
    state->active = true;
    return true;
}

static void set_resolve_parameters(const HikaruGlFragmentList *state)
{
    const GlResolver *resolver = &state->resolver;
    const HikaruFragmentListParameters *parameters = &state->parameters;
    glUniform1i(resolver->backgroundUniform, 0);
    glUniform1ui(resolver->widthUniform, parameters->width);
    glUniform1ui(resolver->blockCapacityUniform, parameters->blockCapacity);
    glUniform1f(resolver->colorLevelsUniform, parameters->colorLevels);
    glUniform1f(resolver->alphaLevelsUniform, parameters->alphaLevels);
}

bool hikaru_gl_fragment_list_end(HikaruGlFragmentList *state)
{
    if (!state || !state->active) return false;
    GlResources *resources = &state->resources;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    if (hikaru_gl_diagnostics_collecting)
        hikaru_gl_diagnostics_phase_end(HIKARU_GL_DIAG_PHASE_COLLECT);
    GLuint status[4] = {0};
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, resources->status);
    const uint64_t counterWaitStart = hikaru_gl_diagnostics_collecting
        ? hikaru_gl_diagnostics_cpu_begin() : 0;
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(status), status);
    if (hikaru_gl_diagnostics_collecting)
        hikaru_gl_diagnostics_cpu_end(HIKARU_GL_DIAG_FRAGMENT_COUNTER_WAIT_NS,
                                      counterWaitStart);
    const bool readbackFailed = take_gl_errors();
    state->lastUsed = status[0];
    state->lastOverflow = status[1] || status[0] > state->parameters.capacity;
    if (state->lastOverflow) ++state->overflows;
    bool complete = !state->failed && !readbackFailed;
    if (readbackFailed) state->disabled = true;
    if (complete && state->lastOverflow) {
        hikaru_fragment_pool_grow(&state->policy, status[0], resources->capacity);
        complete = false;
    } else if (complete) {
        hikaru_fragment_pool_observe(&state->policy, status[0], resources->capacity);
    }
    if (complete) {
        if (status[0]) {
            if (hikaru_gl_diagnostics_collecting)
                hikaru_gl_diagnostics_phase_begin(HIKARU_GL_DIAG_PHASE_COPY);
            glCopyImageSubData(state->sceneTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                               resources->background, GL_TEXTURE_2D, 0, 0, 0, 0,
                               (GLsizei)resources->width, (GLsizei)resources->height, 1);
            if (hikaru_gl_diagnostics_collecting)
                hikaru_gl_diagnostics_phase_end(HIKARU_GL_DIAG_PHASE_COPY);
            glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
            if (take_gl_errors()) {
                state->disabled = true;
                complete = false;
            }
        }
        if (complete && status[0]) {
            glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources->resolveFramebuffer);
            glViewport(0, 0, (GLsizei)resources->width, (GLsizei)resources->height);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_RASTERIZER_DISCARD);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_DITHER);
            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_FALSE);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, resources->background);
            glBindSampler(0, 0);
            bind_storage(resources);
            glUseProgram(state->resolver.program);
            set_resolve_parameters(state);
            glBindVertexArray(state->resolver.vertexArray);
            if (hikaru_gl_diagnostics_collecting)
                hikaru_gl_diagnostics_phase_begin(HIKARU_GL_DIAG_PHASE_RESOLVE);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            if (hikaru_gl_diagnostics_collecting)
                hikaru_gl_diagnostics_phase_end(HIKARU_GL_DIAG_PHASE_RESOLVE);
            if (take_gl_errors()) {
                state->disabled = true;
                /* A resolve error can leave a partial fullscreen write. Roll
                 * the original target back before selecting depth peeling. */
                glCopyImageSubData(resources->background, GL_TEXTURE_2D, 0, 0, 0, 0,
                                   state->sceneTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                                   (GLsizei)resources->width, (GLsizei)resources->height, 1);
                glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
                if (take_gl_errors())
                    fprintf(stderr, "Hikaru: GL fragment-list rollback failed after resolve error\n");
                complete = false;
            } else ++state->resolves;
        }
    }
    if (complete && resources->heads)
        glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT |
                        GL_TEXTURE_FETCH_BARRIER_BIT);
    state->active = false;
    state->failed = false;
    state->sceneTexture = 0;
    restore_gl_state(&state->saved);
    if (!complete) ++state->fallbacks;
    trace_collection(state);
    if (!complete) delete_storage(resources);
    return complete;
}

bool hikaru_gl_fragment_list_active(const HikaruGlFragmentList *state)
{
    return state && state->active;
}

const HikaruFragmentListParameters *hikaru_gl_fragment_list_parameters(
    const HikaruGlFragmentList *state)
{
    return state && state->active && !state->failed ? &state->parameters : NULL;
}

bool hikaru_gl_fragment_list_advance(HikaruGlFragmentList *state,
                                    uint32_t primitiveCount)
{
    if (!state || !state->active || state->failed) return false;
    if (primitiveCount > UINT32_MAX - state->parameters.orderBase) {
        state->failed = true;
        return false;
    }
    state->parameters.orderBase += primitiveCount;
    return true;
}

void hikaru_gl_fragment_list_fail(HikaruGlFragmentList *state)
{
    if (!state) return;
    state->failed = true;
    state->disabled = true;
}

void hikaru_gl_fragment_list_reset(HikaruGlFragmentList *state)
{
    if (!state) return;
    /* Reset can be requested on the producer thread without a current GL
     * context. Rendering owns state restoration and guarantees no active list
     * crosses this safe boundary. */
    state->active = false;
    state->failed = false;
    state->disabled = false;
    memset(&state->parameters, 0, sizeof(state->parameters));
    memset(&state->policy, 0, sizeof(state->policy));
}

void hikaru_gl_fragment_list_destroy(HikaruGlFragmentList **statePointer)
{
    if (!statePointer || !*statePointer) return;
    HikaruGlFragmentList *state = *statePointer;
    if (state->active) {
        state->active = false;
        restore_gl_state(&state->saved);
    }
    delete_storage(&state->resources);
    delete_resolver(&state->resolver);
    free(state);
    *statePointer = NULL;
}

#endif
