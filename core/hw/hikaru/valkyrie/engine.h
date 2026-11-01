// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct HikaruEngine HikaruEngine;
typedef enum {
    HIKARU_ENGINE_GAME_AIRTRIX = 0,
    HIKARU_ENGINE_GAME_BRAVEFF = 1
} HikaruEngineGame;
#define HIKARU_ENGINE_BRAM_SIZE 65536u
typedef struct {
    const uint8_t *boot; size_t boot_size;
    const uint8_t *program; size_t program_size;
    const uint8_t *data; size_t data_size;
} HikaruEngineRoms;
typedef struct {
    uint16_t players[2];
    uint16_t analog[8];
    uint8_t test, service, coin[2];
} HikaruEngineInputs;
typedef struct {
    void *opaque;
    void (*reset)(void *opaque);
    int (*read)(void *opaque, unsigned size, uint32_t address, uint64_t *value);
    int (*write)(void *opaque, unsigned size, uint32_t address, uint64_t value);
    int (*advance)(void *opaque, uint32_t sh4_cycles);
    void (*set_inputs)(void *opaque, const HikaruEngineInputs *inputs);
    // Set AICA pending level; return its level after the MIE interrupt mask.
    // Required only when attaching a real sound backend.
    int (*set_sound_irq)(void *opaque, int asserted);
} HikaruEngineMie;
typedef struct {
    void *opaque;
    void (*reset)(void *opaque);
    int (*read)(void *opaque, unsigned size, uint32_t bus_address, uint64_t *value);
    int (*write)(void *opaque, unsigned size, uint32_t bus_address, uint64_t value);
} HikaruEngineProtection;

// Optional dual sound-board backend. Calls are serialized on the emulation
// thread and callbacks must not throw. RAM is engine-owned until detach.
typedef struct {
    void *opaque;
    int (*attach)(void *opaque, unsigned board, uint8_t *ram, size_t size);
    void (*detach)(void *opaque);
    int (*reset)(void *opaque);
    int (*set_reset)(void *opaque, int asserted); // MIE SND_RES pin
    int (*read)(void *opaque, unsigned board, uint32_t offset, unsigned size, uint64_t *value);
    int (*write)(void *opaque, unsigned board, uint32_t offset, unsigned size, uint64_t value);
    int (*advance)(void *opaque, uint32_t sh4_cycles);
    int (*irq)(void *opaque, unsigned board); // 0 clear, 1 asserted, -1 error
} HikaruEngineSound;

// Requires a current desktop OpenGL context for create, frame and destroy.
// Calls must be serialized; only one instance is supported. ROM contents are
// copied. BIOS EEPROM bypass is an upstream compatibility patch and must be
// explicitly enabled. The caller retains ownership of MIE, protection, context
// and target; protection callbacks receive absolute external-bus addresses.
// Host callbacks must not throw exceptions through this C boundary.
// The original entry point selects Air Trix for existing callers.
HikaruEngine *hikaru_engine_create(const HikaruEngineRoms *roms,
    const HikaruEngineMie *mie, const HikaruEngineProtection *protection,
    uint32_t target_fbo, unsigned width, unsigned height,
    int allow_bios_eeprom_bypass, char *error, size_t error_size);
// Only the two games listed above are accepted. ROM sections are already
// assembled: both use a 2 MiB BIOS; Air Trix uses 8/128 MiB program/data,
// Brave Firefighters uses 64/256 MiB regions including unpopulated sockets.
// Protection callbacks remain required; the host supplies an unprotected bus
// backend for a board without a security processor. No game patches are enabled.
HikaruEngine *hikaru_engine_create_game(HikaruEngineGame game, const HikaruEngineRoms *roms,
    const HikaruEngineMie *mie, const HikaruEngineProtection *protection,
    uint32_t target_fbo, unsigned width, unsigned height,
    int allow_bios_eeprom_bypass, char *error, size_t error_size);
int hikaru_engine_run_frame(HikaruEngine *engine, const HikaruEngineInputs *inputs);
// Configure both SH-4s in MHz (100..300). Device clocks remain at the board
// baseline; changing the clock resets the fractional CPU-cycle accumulator.
int hikaru_engine_set_cpu_clock_mhz(HikaruEngine *engine, unsigned mhz);
// Installs both real boards before the first frame; failure leaves the legacy
// compatibility path in place. Detach is called before RAM is destroyed.
int hikaru_engine_set_sound(HikaruEngine *engine, const HikaruEngineSound *sound);
// Battery-backed RAM is transferred as exactly HIKARU_ENGINE_BRAM_SIZE bytes.
// Calls are serialized with frame execution. A new machine starts with blank RAM.
int hikaru_engine_get_bram(const HikaruEngine *engine, uint8_t *data, size_t size);
int hikaru_engine_set_bram(HikaruEngine *engine, const uint8_t *data, size_t size);
void hikaru_engine_resize(HikaruEngine *engine, uint32_t target_fbo, unsigned width, unsigned height);
// Set the 4:3 internal 3D vertical resolution. Requires a current GL context;
// targets are reallocated at the next frame. The default is 480 pixels.
int hikaru_engine_set_internal_resolution(HikaruEngine *engine, unsigned height);
const char *hikaru_engine_last_error(const HikaruEngine *engine);
void hikaru_engine_destroy(HikaruEngine *engine);
#ifdef __cplusplus
}
#endif
