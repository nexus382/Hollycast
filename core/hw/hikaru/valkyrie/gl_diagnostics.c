// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "gl_diagnostics.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#define HIKARU_GL_DIAG_DEFINED_NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define HIKARU_GL_DIAG_DEFINED_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef HIKARU_GL_DIAG_DEFINED_NOMINMAX
#undef NOMINMAX
#undef HIKARU_GL_DIAG_DEFINED_NOMINMAX
#endif
#ifdef HIKARU_GL_DIAG_DEFINED_LEAN_AND_MEAN
#undef WIN32_LEAN_AND_MEAN
#undef HIKARU_GL_DIAG_DEFINED_LEAN_AND_MEAN
#endif
#else
#include <time.h>
#endif

#if !defined(__APPLE__) && !defined(ANDROID) && !defined(__ANDROID__) && \
    !defined(USE_GLES) && !defined(USE_GLES2)
#define HIKARU_GL_DIAG_DESKTOP_QUERIES 1
#include <glad/gl.h>
#endif

#define HIKARU_GL_DIAG_QUERY_SLOTS 4u
#define HIKARU_GL_DIAG_REPORT_INTERVAL 60u

_Thread_local bool hikaru_gl_diagnostics_collecting;

typedef struct HikaruGlDiagnosticGpuSlot {
    uint8_t ended_phases;
    bool in_flight;
    bool valid;
} HikaruGlDiagnosticGpuSlot;

struct HikaruGlDiagnostics {
    uint64_t counters[HIKARU_GL_DIAG_COUNTER_COUNT];
    uint64_t reported_counters[HIKARU_GL_DIAG_COUNTER_COUNT];
    bool counter_measured[HIKARU_GL_DIAG_COUNTER_COUNT];
    uint64_t gpu_ns[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t gpu_samples[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t reported_gpu_ns[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t reported_gpu_samples[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t frame_count;
    uint64_t dropped_frames;
    uint64_t invalid_frames;
    uint64_t reported_dropped_frames, reported_invalid_frames;
    bool cpu_enabled;
    bool counters_enabled;
    bool gpu_requested;
    bool gpu_supported;
    bool frame_active;
    bool transaction_claimed;
    bool transaction_markers_active;
    bool palette_uniforms;
    uint8_t open_phases;
    uint8_t used_phases;
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    GLuint query_ids[HIKARU_GL_DIAG_QUERY_SLOTS][HIKARU_GL_DIAG_PHASE_COUNT][2];
    HikaruGlDiagnosticGpuSlot gpu_slots[HIKARU_GL_DIAG_QUERY_SLOTS];
    int active_gpu_slot;
    unsigned next_gpu_slot;
    unsigned timestamp_bits;
#endif
};

static _Thread_local HikaruGlDiagnostics *current_diagnostics;

static bool env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

#if defined(_WIN32)
static INIT_ONCE performance_frequency_once = INIT_ONCE_STATIC_INIT;
static LARGE_INTEGER performance_frequency;

static BOOL CALLBACK initialize_performance_frequency(PINIT_ONCE once,
                                                       PVOID parameter,
                                                       PVOID *context)
{
    (void)once;
    (void)parameter;
    (void)context;
    return QueryPerformanceFrequency(&performance_frequency);
}
#endif

static bool monotonic_ns(uint64_t *value)
{
    if (!value) return false;
#if defined(_WIN32)
    LARGE_INTEGER counter;
    if (!InitOnceExecuteOnce(&performance_frequency_once,
                             initialize_performance_frequency, NULL, NULL) ||
        performance_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&counter) ||
        counter.QuadPart < 0)
        return false;
    const uint64_t ticks = (uint64_t)counter.QuadPart;
    const uint64_t frequency = (uint64_t)performance_frequency.QuadPart;
    *value = (ticks / frequency) * UINT64_C(1000000000) +
        (ticks % frequency) * UINT64_C(1000000000) / frequency;
    return true;
#else
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0 || time.tv_sec < 0 ||
        time.tv_nsec < 0)
        return false;
    *value = (uint64_t)time.tv_sec * UINT64_C(1000000000) +
        (uint64_t)time.tv_nsec;
    return true;
#endif
}

static void add_counter(HikaruGlDiagnostics *state,
                        HikaruGlDiagnosticCounter counter, uint64_t amount)
{
    if (!state || !state->counters_enabled || counter < 0 ||
        counter >= HIKARU_GL_DIAG_COUNTER_COUNT)
        return;
    state->counters[counter] += amount;
    if (counter != HIKARU_GL_DIAG_MASKED_PIXELS)
        state->counter_measured[counter] = true;
}

#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
static bool parse_version(const char *version, int *major, int *minor)
{
    if (!version || !major || !minor) return false;
    while (*version && (*version < '0' || *version > '9')) ++version;
    if (!*version) return false;
    char *end = NULL;
    const long parsed_major = strtol(version, &end, 10);
    if (end == version || *end != '.') return false;
    const char *minor_start = end + 1;
    const long parsed_minor = strtol(minor_start, &end, 10);
    if (end == minor_start || parsed_major > 100 || parsed_minor > 100 ||
        parsed_major < 0 || parsed_minor < 0)
        return false;
    *major = (int)parsed_major;
    *minor = (int)parsed_minor;
    return true;
}

static bool has_extension(const char *wanted, int major)
{
    if (major >= 3 && glad_glGetStringi && glad_glGetIntegerv) {
        GLint extension_count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
        for (GLint i = 0; i < extension_count; ++i) {
            const GLubyte *extension = glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (extension && strcmp((const char *)extension, wanted) == 0)
                return true;
        }
        return false;
    }

    const GLubyte *extensions = glGetString(GL_EXTENSIONS);
    if (!extensions) return false;
    const size_t wanted_length = strlen(wanted);
    const char *cursor = (const char *)extensions;
    while ((cursor = strstr(cursor, wanted)) != NULL) {
        const bool starts_token = cursor == (const char *)extensions || cursor[-1] == ' ';
        const bool ends_token = cursor[wanted_length] == '\0' ||
            cursor[wanted_length] == ' ';
        if (starts_token && ends_token) return true;
        cursor += wanted_length;
    }
    return false;
}

static bool timer_query_supported(unsigned *timestamp_bits)
{
    if (!glad_glGetString || !glad_glGetIntegerv || !glad_glGetQueryiv ||
        !glad_glGenQueries || !glad_glDeleteQueries || !glad_glQueryCounter ||
        !glad_glGetQueryObjectiv || !glad_glGetQueryObjectui64v)
        return false;

    const GLubyte *version_string = glGetString(GL_VERSION);
    if (!version_string || strncmp((const char *)version_string, "OpenGL ES", 9) == 0)
        return false;
    int major = 0, minor = 0;
    if (!parse_version((const char *)version_string, &major, &minor)) return false;
    const bool core_support = major > 3 || (major == 3 && minor >= 3);
    if (!core_support && !has_extension("GL_ARB_timer_query", major)) return false;

    GLint counter_bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &counter_bits);
    if (counter_bits <= 0) return false;
    *timestamp_bits = counter_bits >= 64 ? 64u : (unsigned)counter_bits;
    return true;
}

static void delete_queries(HikaruGlDiagnostics *state)
{
    if (!state || !glad_glDeleteQueries) return;
    GLuint ids[HIKARU_GL_DIAG_QUERY_SLOTS * HIKARU_GL_DIAG_PHASE_COUNT * 2u];
    GLsizei count = 0;
    for (unsigned slot = 0; slot < HIKARU_GL_DIAG_QUERY_SLOTS; ++slot) {
        for (unsigned phase = 0; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint) {
                const GLuint id = state->query_ids[slot][phase][endpoint];
                if (id) ids[count++] = id;
            }
        }
    }
    if (count) glDeleteQueries(count, ids);
    memset(state->query_ids, 0, sizeof(state->query_ids));
    memset(state->gpu_slots, 0, sizeof(state->gpu_slots));
    state->gpu_supported = false;
    state->active_gpu_slot = -1;
}

static bool allocate_queries(HikaruGlDiagnostics *state)
{
    unsigned timestamp_bits = 0;
    if (!timer_query_supported(&timestamp_bits)) return false;

    const GLsizei count = (GLsizei)(HIKARU_GL_DIAG_QUERY_SLOTS *
        HIKARU_GL_DIAG_PHASE_COUNT * 2u);
    GLuint ids[HIKARU_GL_DIAG_QUERY_SLOTS * HIKARU_GL_DIAG_PHASE_COUNT * 2u] = {0};
    glGenQueries(count, ids);
    unsigned index = 0;
    for (unsigned slot = 0; slot < HIKARU_GL_DIAG_QUERY_SLOTS; ++slot) {
        for (unsigned phase = 0; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
            for (unsigned endpoint = 0; endpoint < 2; ++endpoint)
                state->query_ids[slot][phase][endpoint] = ids[index++];
        }
    }
    for (unsigned i = 0; i < (unsigned)count; ++i) {
        if (!ids[i]) {
            delete_queries(state);
            return false;
        }
    }
    state->timestamp_bits = timestamp_bits;
    state->active_gpu_slot = -1;
    state->gpu_supported = true;
    return true;
}

static uint8_t phase_bit(HikaruGlDiagnosticPhase phase)
{
    return (uint8_t)(1u << (unsigned)phase);
}

static void issue_timestamp(HikaruGlDiagnostics *state,
                            HikaruGlDiagnosticPhase phase, unsigned endpoint)
{
    if (!state->gpu_supported || state->active_gpu_slot < 0 ||
        phase < 0 || phase >= HIKARU_GL_DIAG_PHASE_COUNT || endpoint > 1)
        return;
    const GLuint query = state->query_ids[state->active_gpu_slot][phase][endpoint];
    if (query) glQueryCounter(query, GL_TIMESTAMP);
}

static bool query_available(GLuint query)
{
    GLint available = GL_FALSE;
    glGetQueryObjectiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
    return available == GL_TRUE;
}

static void collect_slot(HikaruGlDiagnostics *state, unsigned index)
{
    HikaruGlDiagnosticGpuSlot *slot = &state->gpu_slots[index];
    bool all_available = slot->in_flight;
    for (unsigned phase = 0; all_available && phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
        if (!(slot->ended_phases & phase_bit((HikaruGlDiagnosticPhase)phase))) continue;
        all_available = query_available(state->query_ids[index][phase][0]) &&
            query_available(state->query_ids[index][phase][1]);
    }
    if (!all_available) return;

    /* Read timestamp results only after every issued endpoint reports ready. */
    if (slot->valid) {
        uint64_t durations[HIKARU_GL_DIAG_PHASE_COUNT] = {0};
        bool valid = true;
        for (unsigned phase = 0; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
            if (!(slot->ended_phases & phase_bit((HikaruGlDiagnosticPhase)phase))) continue;
            GLuint64 begin = 0, end = 0;
            glGetQueryObjectui64v(state->query_ids[index][phase][0], GL_QUERY_RESULT, &begin);
            glGetQueryObjectui64v(state->query_ids[index][phase][1], GL_QUERY_RESULT, &end);
            if (state->timestamp_bits == 64 && end < begin) valid = false;
            durations[phase] = (uint64_t)(end - begin);
            if (state->timestamp_bits < 64)
                durations[phase] &= (UINT64_C(1) << state->timestamp_bits) - 1;
        }
        if (valid) {
            for (unsigned phase = 0; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
                if (!(slot->ended_phases & phase_bit((HikaruGlDiagnosticPhase)phase))) continue;
                state->gpu_ns[phase] += durations[phase];
                ++state->gpu_samples[phase];
                state->reported_gpu_ns[phase] += durations[phase];
                ++state->reported_gpu_samples[phase];
            }
        } else ++state->invalid_frames;
    }
    memset(slot, 0, sizeof(*slot));
}

static void poll_slots(HikaruGlDiagnostics *state)
{
    for (unsigned i = 0; i < HIKARU_GL_DIAG_QUERY_SLOTS; ++i)
        collect_slot(state, i);
}

static void reserve_gpu_slot(HikaruGlDiagnostics *state)
{
    state->active_gpu_slot = -1;
    if (!state->gpu_supported) return;
    poll_slots(state);
    for (unsigned offset = 0; offset < HIKARU_GL_DIAG_QUERY_SLOTS; ++offset) {
        const unsigned index = (state->next_gpu_slot + offset) % HIKARU_GL_DIAG_QUERY_SLOTS;
        if (state->gpu_slots[index].in_flight) continue;
        memset(&state->gpu_slots[index], 0, sizeof(state->gpu_slots[index]));
        state->active_gpu_slot = (int)index;
        state->next_gpu_slot = (index + 1u) % HIKARU_GL_DIAG_QUERY_SLOTS;
        state->open_phases |= phase_bit(HIKARU_GL_DIAG_PHASE_FRAME);
        issue_timestamp(state, HIKARU_GL_DIAG_PHASE_FRAME, 0);
        return;
    }
    /* Do not reuse a timestamp pair while its previous frame is still queued. */
    ++state->dropped_frames;
}
#endif

static void print_counter(FILE *stream, const HikaruGlDiagnostics *state,
                          HikaruGlDiagnosticCounter counter, const char *name)
{
    if (state->counter_measured[counter]) {
        const uint64_t value = state->counters[counter] - state->reported_counters[counter];
        fprintf(stream, " %s=%llu", name, (unsigned long long)value);
    } else {
        fprintf(stream, " %s=unavailable", name);
    }
}

static void print_costs(HikaruGlDiagnostics *state)
{
    FILE *stream = stderr;
    fprintf(stream, "Hikaru GL costs:");
    print_counter(stream, state, HIKARU_GL_DIAG_UNIFORM_UPLOADS, "uniform_uploads");
    print_counter(stream, state, HIKARU_GL_DIAG_UNIFORM_HITS, "uniform_hits");
    print_counter(stream, state, HIKARU_GL_DIAG_UNIFORM_BYTES, "uniform_bytes");
    print_counter(stream, state, HIKARU_GL_DIAG_PALETTE_UPLOADS, "palette_uploads");
    print_counter(stream, state, HIKARU_GL_DIAG_PALETTE_HITS, "palette_hits");
    print_counter(stream, state, HIKARU_GL_DIAG_PALETTE_BYTES, "palette_bytes");
    print_counter(stream, state, HIKARU_GL_DIAG_VERTEX_UPLOADS, "vertex_uploads");
    print_counter(stream, state, HIKARU_GL_DIAG_VERTEX_BYTES, "vertex_bytes");
    print_counter(stream, state, HIKARU_GL_DIAG_TEXTURE_UPLOADS, "texture_uploads");
    print_counter(stream, state, HIKARU_GL_DIAG_TEXTURE_BYTES, "texture_bytes");
    print_counter(stream, state, HIKARU_GL_DIAG_PROGRAM_LOOKUPS, "program_lookups");
    print_counter(stream, state, HIKARU_GL_DIAG_PROGRAM_HITS, "program_hits");
    print_counter(stream, state, HIKARU_GL_DIAG_PROGRAM_COMPILES, "program_compiles");
    print_counter(stream, state, HIKARU_GL_DIAG_PROGRAM_COMPILE_NS, "program_compile_ns");
    print_counter(stream, state, HIKARU_GL_DIAG_QUERIES, "queries");
    print_counter(stream, state, HIKARU_GL_DIAG_QUERY_WAIT_NS, "query_wait_ns");
    print_counter(stream, state, HIKARU_GL_DIAG_FRAGMENT_COUNTER_WAIT_NS,
                  "fragment_counter_wait_ns");
    print_counter(stream, state, HIKARU_GL_DIAG_MASKED_CLEARS, "masked_clears");
    fprintf(stream, " masked_pixels=unavailable");
    print_counter(stream, state, HIKARU_GL_DIAG_DRAWS, "draws");
    fputc('\n', stream);
    memcpy(state->reported_counters, state->counters, sizeof(state->counters));
}

#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
static void print_gpu_phase_duration(FILE *stream, const HikaruGlDiagnostics *state,
                                    HikaruGlDiagnosticPhase phase, const char *name)
{
    const uint64_t samples = state->reported_gpu_samples[phase];
    if (state->gpu_supported && samples) {
        const double average_ms = (double)state->reported_gpu_ns[phase] /
            (double)samples / 1000000.0;
        fprintf(stream, " %s_ms=%.3f", name, average_ms);
    } else {
        fprintf(stream, " %s_ms=unavailable", name);
    }
}

static void print_gpu_phase_samples(FILE *stream, const HikaruGlDiagnostics *state,
                                    HikaruGlDiagnosticPhase phase, const char *name)
{
    fprintf(stream, " %s_samples=%llu", name,
            (unsigned long long)state->reported_gpu_samples[phase]);
}
#endif

static void print_gpu_timing(const HikaruGlDiagnostics *state)
{
    FILE *stream = stderr;
    fprintf(stream, "Hikaru GL GPU timing:");
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    print_gpu_phase_duration(stream, state, HIKARU_GL_DIAG_PHASE_FRAME, "frame");
    print_gpu_phase_duration(stream, state, HIKARU_GL_DIAG_PHASE_CLEAR, "clear");
    print_gpu_phase_duration(stream, state, HIKARU_GL_DIAG_PHASE_COLLECT, "collect");
    print_gpu_phase_duration(stream, state, HIKARU_GL_DIAG_PHASE_COPY, "copy");
    print_gpu_phase_duration(stream, state, HIKARU_GL_DIAG_PHASE_RESOLVE, "resolve");
    print_gpu_phase_samples(stream, state, HIKARU_GL_DIAG_PHASE_FRAME, "frame");
    print_gpu_phase_samples(stream, state, HIKARU_GL_DIAG_PHASE_CLEAR, "clear");
    print_gpu_phase_samples(stream, state, HIKARU_GL_DIAG_PHASE_COLLECT, "collect");
    print_gpu_phase_samples(stream, state, HIKARU_GL_DIAG_PHASE_COPY, "copy");
    print_gpu_phase_samples(stream, state, HIKARU_GL_DIAG_PHASE_RESOLVE, "resolve");
#else
    fprintf(stream,
        " frame_ms=unavailable clear_ms=unavailable collect_ms=unavailable"
        " copy_ms=unavailable resolve_ms=unavailable frame_samples=0 clear_samples=0"
        " collect_samples=0 copy_samples=0 resolve_samples=0");
#endif
    fprintf(stream, " dropped=%llu invalid=%llu supported=%u\n",
            (unsigned long long)(state->dropped_frames - state->reported_dropped_frames),
            (unsigned long long)(state->invalid_frames - state->reported_invalid_frames),
            state->gpu_supported ? 1u : 0u);
}

static void report_if_due(HikaruGlDiagnostics *state)
{
    if (!state->counters_enabled || state->frame_count % HIKARU_GL_DIAG_REPORT_INTERVAL)
        return;
    if (state->counters_enabled) print_costs(state);
    if (state->gpu_requested) {
        print_gpu_timing(state);
        state->reported_dropped_frames = state->dropped_frames;
        state->reported_invalid_frames = state->invalid_frames;
        memset(state->reported_gpu_ns, 0, sizeof(state->reported_gpu_ns));
        memset(state->reported_gpu_samples, 0, sizeof(state->reported_gpu_samples));
    }
}

HikaruGlDiagnostics *hikaru_gl_diagnostics_create(void)
{
    const bool cpu_requested = env_flag_enabled("HIKARU_TRACE_TIMING");
    const bool gpu_requested = env_flag_enabled("HIKARU_TRACE_GPU_TIMING");
    if (!cpu_requested && !gpu_requested) return NULL;
    HikaruGlDiagnostics *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    uint64_t clock_probe;
    state->cpu_enabled = monotonic_ns(&clock_probe);
    state->gpu_requested = gpu_requested;
    state->counters_enabled = true;
    for (unsigned i = 0; i < HIKARU_GL_DIAG_COUNTER_COUNT; ++i)
        state->counter_measured[i] = i != HIKARU_GL_DIAG_MASKED_PIXELS;
    if (!state->cpu_enabled) {
        state->counter_measured[HIKARU_GL_DIAG_PROGRAM_COMPILE_NS] = false;
        state->counter_measured[HIKARU_GL_DIAG_QUERY_WAIT_NS] = false;
        state->counter_measured[HIKARU_GL_DIAG_FRAGMENT_COUNTER_WAIT_NS] = false;
    }
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    state->active_gpu_slot = -1;
    if (state->gpu_requested) allocate_queries(state);
#endif
    return state;
}

void hikaru_gl_diagnostics_destroy(HikaruGlDiagnostics **state_pointer)
{
    if (!state_pointer || !*state_pointer) return;
    HikaruGlDiagnostics *state = *state_pointer;
    if (current_diagnostics == state)
        hikaru_gl_diagnostics_end_frame(state, false);
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    delete_queries(state);
#endif
    free(state);
    *state_pointer = NULL;
}

void hikaru_gl_diagnostics_begin_frame(HikaruGlDiagnostics *state)
{
    if (!state) return;
    if (current_diagnostics)
        hikaru_gl_diagnostics_end_frame(current_diagnostics, false);
    if (!state->counters_enabled) return;
    if (state->frame_active) {
        state->frame_active = false;
        if (state->gpu_requested) ++state->invalid_frames;
    }

    state->frame_active = true;
    state->transaction_claimed = false;
    state->transaction_markers_active = false;
    state->palette_uniforms = false;
    state->open_phases = 0;
    state->used_phases = 0;
    current_diagnostics = state;
    hikaru_gl_diagnostics_collecting = true;
    ++state->frame_count;
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    if (state->gpu_supported) reserve_gpu_slot(state);
#endif
}

void hikaru_gl_diagnostics_end_frame(HikaruGlDiagnostics *state, bool valid)
{
    if (!state || current_diagnostics != state || !state->frame_active) return;

#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    if (state->open_phases & (uint8_t)~phase_bit(HIKARU_GL_DIAG_PHASE_FRAME))
        valid = false; /* Retire unmatched endpoints, but never publish a made-up span. */
    for (unsigned phase = 1; phase < HIKARU_GL_DIAG_PHASE_COUNT; ++phase) {
        const uint8_t bit = (uint8_t)(1u << phase);
        if (!(state->open_phases & bit)) continue;
        issue_timestamp(state, (HikaruGlDiagnosticPhase)phase, 1);
        state->open_phases &= (uint8_t)~bit;
        if (state->active_gpu_slot >= 0)
            state->gpu_slots[state->active_gpu_slot].ended_phases |= bit;
    }
    const uint8_t frame_bit = phase_bit(HIKARU_GL_DIAG_PHASE_FRAME);
    if (state->open_phases & frame_bit) {
        issue_timestamp(state, HIKARU_GL_DIAG_PHASE_FRAME, 1);
        state->open_phases &= (uint8_t)~frame_bit;
        if (state->active_gpu_slot >= 0)
            state->gpu_slots[state->active_gpu_slot].ended_phases |= frame_bit;
    }
    if (state->active_gpu_slot >= 0) {
        HikaruGlDiagnosticGpuSlot *slot = &state->gpu_slots[state->active_gpu_slot];
        slot->in_flight = true;
        slot->valid = valid;
        state->active_gpu_slot = -1;
    }
#endif

    state->open_phases = 0;
    if (!valid) ++state->invalid_frames;
    state->frame_active = false;
    state->transaction_claimed = false;
    state->transaction_markers_active = false;
    state->palette_uniforms = false;
    current_diagnostics = NULL;
    hikaru_gl_diagnostics_collecting = false;
    report_if_due(state);
}

void hikaru_gl_diagnostics_phase_begin(HikaruGlDiagnosticPhase phase)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    if (!state || !state->frame_active || phase <= HIKARU_GL_DIAG_PHASE_FRAME ||
        phase >= HIKARU_GL_DIAG_PHASE_COUNT)
        return;
    if (phase == HIKARU_GL_DIAG_PHASE_CLEAR) {
        if (state->transaction_claimed) {
            /* The fragment-list clear begins each PP transaction. */
            state->transaction_markers_active = false;
            return;
        }
        state->transaction_claimed = true;
        state->transaction_markers_active = true;
    } else if (!state->transaction_claimed || !state->transaction_markers_active) {
        return;
    }
    const uint8_t bit = (uint8_t)(1u << (unsigned)phase);
    if ((state->open_phases | state->used_phases) & bit) return;
    state->open_phases |= bit;
    state->used_phases |= bit;
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    issue_timestamp(state, phase, 0);
#endif
}

void hikaru_gl_diagnostics_phase_end(HikaruGlDiagnosticPhase phase)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    if (!state || !state->frame_active || phase <= HIKARU_GL_DIAG_PHASE_FRAME ||
        phase >= HIKARU_GL_DIAG_PHASE_COUNT)
        return;
    const uint8_t bit = (uint8_t)(1u << (unsigned)phase);
    if (!(state->open_phases & bit)) return;
#if defined(HIKARU_GL_DIAG_DESKTOP_QUERIES)
    issue_timestamp(state, phase, 1);
    if (state->active_gpu_slot >= 0)
        state->gpu_slots[state->active_gpu_slot].ended_phases |= bit;
#endif
    state->open_phases &= (uint8_t)~bit;
}

void hikaru_gl_diagnostics_count(HikaruGlDiagnosticCounter counter, uint64_t amount)
{
    add_counter(current_diagnostics, counter, amount);
}

void hikaru_gl_diagnostics_palette(bool active)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    if (state && state->frame_active) state->palette_uniforms = active;
}

void hikaru_gl_diagnostics_uniform(uint64_t bytes)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    if (!state || !state->frame_active) return;
    const HikaruGlDiagnosticCounter uploads = state->palette_uniforms
        ? HIKARU_GL_DIAG_PALETTE_UPLOADS : HIKARU_GL_DIAG_UNIFORM_UPLOADS;
    const HikaruGlDiagnosticCounter byte_count = state->palette_uniforms
        ? HIKARU_GL_DIAG_PALETTE_BYTES : HIKARU_GL_DIAG_UNIFORM_BYTES;
    add_counter(state, uploads, 1);
    add_counter(state, byte_count, bytes);
}

uint64_t hikaru_gl_diagnostics_cpu_begin(void)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    uint64_t start = 0;
    if (!state || !state->cpu_enabled || !state->frame_active || !monotonic_ns(&start))
        return 0;
    return start;
}

void hikaru_gl_diagnostics_cpu_end(HikaruGlDiagnosticCounter counter, uint64_t start)
{
    HikaruGlDiagnostics *state = current_diagnostics;
    uint64_t end = 0;
    if (!state || !state->cpu_enabled || !state->frame_active ||
        counter < 0 || counter >= HIKARU_GL_DIAG_COUNTER_COUNT || start == 0 ||
        !monotonic_ns(&end) || end < start)
        return;
    add_counter(state, counter, end - start);
}

void hikaru_gl_diagnostics_snapshot(const HikaruGlDiagnostics *state,
                                    HikaruGlDiagnosticSnapshot *snapshot)
{
    if (!snapshot) return;
    memset(snapshot, 0, sizeof(*snapshot));
    if (!state) return;
    memcpy(snapshot->counters, state->counters, sizeof(state->counters));
    memcpy(snapshot->counter_measured, state->counter_measured,
           sizeof(state->counter_measured));
    memcpy(snapshot->gpu_ns, state->gpu_ns, sizeof(state->gpu_ns));
    memcpy(snapshot->gpu_samples, state->gpu_samples, sizeof(state->gpu_samples));
    snapshot->dropped_frames = state->dropped_frames;
    snapshot->invalid_frames = state->invalid_frames;
    snapshot->gpu_supported = state->gpu_supported;
}
