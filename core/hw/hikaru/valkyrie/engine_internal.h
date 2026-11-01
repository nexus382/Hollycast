// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine.h"
#include "vk/machine.h"
#include <setjmp.h>
struct HikaruEngine {
    HikaruEngineMie mie;
    HikaruEngineProtection protection;
    HikaruEngineSound sound;
    bool sound_attached, frame_started;
    int sound_reset;
    vk_game_t game;
    vk_game_section_t sections[3];
    vk_machine_t *machine;
    char error[256];
    jmp_buf fatal;
    struct { uint32_t address; uint64_t count; } diagnostics[3][32];
    uint64_t otherDiagnostics[3];
};
/* Returns the occurrence count on the first and power-of-two occurrences. */
uint64_t vk_host_diagnostic(void *host, unsigned kind, uint32_t address);
// Synchronize the hosted MIE's SND_RES pin when it changes.
int hikaru_engine_sync_sound_reset(HikaruEngine *engine);
