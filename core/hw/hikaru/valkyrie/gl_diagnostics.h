// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct HikaruGlDiagnostics HikaruGlDiagnostics;

#ifdef __cplusplus
extern thread_local bool hikaru_gl_diagnostics_collecting;
#else
extern _Thread_local bool hikaru_gl_diagnostics_collecting;
#endif

typedef enum HikaruGlDiagnosticPhase {
    HIKARU_GL_DIAG_PHASE_FRAME,
    HIKARU_GL_DIAG_PHASE_CLEAR,
    HIKARU_GL_DIAG_PHASE_COLLECT,
    HIKARU_GL_DIAG_PHASE_COPY,
    HIKARU_GL_DIAG_PHASE_RESOLVE,
    HIKARU_GL_DIAG_PHASE_COUNT
} HikaruGlDiagnosticPhase;

typedef enum HikaruGlDiagnosticCounter {
    HIKARU_GL_DIAG_DRAWS,
    HIKARU_GL_DIAG_VERTEX_UPLOADS,
    HIKARU_GL_DIAG_VERTEX_BYTES,
    HIKARU_GL_DIAG_UNIFORM_UPLOADS,
    HIKARU_GL_DIAG_UNIFORM_HITS,
    HIKARU_GL_DIAG_UNIFORM_BYTES,
    HIKARU_GL_DIAG_PALETTE_UPLOADS,
    HIKARU_GL_DIAG_PALETTE_HITS,
    HIKARU_GL_DIAG_PALETTE_BYTES,
    HIKARU_GL_DIAG_TEXTURE_UPLOADS,
    HIKARU_GL_DIAG_TEXTURE_BYTES,
    HIKARU_GL_DIAG_PROGRAM_LOOKUPS,
    HIKARU_GL_DIAG_PROGRAM_HITS,
    HIKARU_GL_DIAG_PROGRAM_COMPILES,
    HIKARU_GL_DIAG_PROGRAM_COMPILE_NS,
    HIKARU_GL_DIAG_QUERIES,
    HIKARU_GL_DIAG_QUERY_WAIT_NS,
    HIKARU_GL_DIAG_FRAGMENT_COUNTER_WAIT_NS,
    HIKARU_GL_DIAG_MASKED_CLEARS,
    HIKARU_GL_DIAG_MASKED_PIXELS,
    HIKARU_GL_DIAG_COUNTER_COUNT
} HikaruGlDiagnosticCounter;

typedef struct HikaruGlDiagnosticSnapshot {
    uint64_t counters[HIKARU_GL_DIAG_COUNTER_COUNT];
    bool counter_measured[HIKARU_GL_DIAG_COUNTER_COUNT];
    uint64_t gpu_ns[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t gpu_samples[HIKARU_GL_DIAG_PHASE_COUNT];
    uint64_t dropped_frames;
    uint64_t invalid_frames;
    bool gpu_supported;
} HikaruGlDiagnosticSnapshot;

HikaruGlDiagnostics *hikaru_gl_diagnostics_create(void);
void hikaru_gl_diagnostics_destroy(HikaruGlDiagnostics **state);
void hikaru_gl_diagnostics_begin_frame(HikaruGlDiagnostics *state);
void hikaru_gl_diagnostics_end_frame(HikaruGlDiagnostics *state, bool valid);
void hikaru_gl_diagnostics_phase_begin(HikaruGlDiagnosticPhase phase);
void hikaru_gl_diagnostics_phase_end(HikaruGlDiagnosticPhase phase);
void hikaru_gl_diagnostics_count(HikaruGlDiagnosticCounter counter, uint64_t amount);
void hikaru_gl_diagnostics_palette(bool active);
void hikaru_gl_diagnostics_uniform(uint64_t bytes);
uint64_t hikaru_gl_diagnostics_cpu_begin(void);
void hikaru_gl_diagnostics_cpu_end(HikaruGlDiagnosticCounter counter, uint64_t start);
void hikaru_gl_diagnostics_snapshot(const HikaruGlDiagnostics *state,
                                    HikaruGlDiagnosticSnapshot *snapshot);
