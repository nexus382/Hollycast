// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "engine.h"
#include "vk/machine.h"
#include <setjmp.h>
/* Wall-time phases are exclusive, including DMA nested inside a CPU store.
 * Collected only on the emulation thread when HIKARU_TRACE_TIMING is set. */
typedef enum {
    HIKARU_PROFILE_OTHER, HIKARU_PROFILE_MASTER, HIKARU_PROFILE_SLAVE,
    HIKARU_PROFILE_MEMCTL, HIKARU_PROFILE_MIE, HIKARU_PROFILE_SOUND,
    HIKARU_PROFILE_GPU, HIKARU_PROFILE_RENDER, HIKARU_PROFILE_GL_STATE,
    HIKARU_PROFILE_COUNT
} HikaruProfilePhase;

struct HikaruEngine {
    HikaruEngineMie mie;
    HikaruEngineProtection protection;
    HikaruEngineSound sound;
    bool sound_attached, frame_started;
    int sound_reset;
    HikaruTransparencySorting transparency_sorting;
    unsigned transparency_peel_capacity;
    uint64_t transparency_memory_budget;
    unsigned internal_width, internal_height; // Host-only capture configuration.
    vk_game_t game;
    vk_game_section_t sections[3];
    vk_machine_t *machine;
    char error[256];
    jmp_buf fatal;
    struct { uint32_t address; uint64_t count; } diagnostics[3][32];
    uint64_t otherDiagnostics[3];
    struct {
        bool enabled, collecting;
        unsigned frames;
        HikaruProfilePhase phase;
        uint64_t frequency, last;
        uint64_t frame[HIKARU_PROFILE_COUNT], window[HIKARU_PROFILE_COUNT];
    } profile;
    /* Producer-owned versions are never serialized or included in the state
     * layout. The source token also survives graphics-context recreation. */
    uint64_t texture_source, texture_generation[2];
};
/* Reset/load invalidate only after the worker is drained. Consumers keep their
 * immutable packet bytes; zero stays sticky after wrap and disables reuse. */
static inline void hikaru_engine_note_texram_write(HikaruEngine *engine, unsigned banks)
{
    if (!engine) return;
    for (unsigned bank = 0; bank < 2; ++bank)
        if ((banks & (1u << bank)) && engine->texture_generation[bank])
            ++engine->texture_generation[bank];
}
/* Returns the occurrence count on the first and power-of-two occurrences. */
uint64_t vk_host_diagnostic(void *host, unsigned kind, uint32_t address);
// Synchronize the hosted MIE's SND_RES pin when it changes.
int hikaru_engine_sync_sound_reset(HikaruEngine *engine);
/* A disabled profile performs no host-clock reads. Restore the returned phase
 * after nested device work so it is not counted twice in the parent phase. */
HikaruProfilePhase hikaru_profile_switch(HikaruEngine *engine, HikaruProfilePhase phase);
