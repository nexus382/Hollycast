// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
// Copyright 2026 The Hollycast Authors
/* Synthetic board creation and real memory-controller reads without
 * commercial images. Also verifies graphics recreation preserves the machine.
 * The legacy entry point must remain Air Trix. */
#define GL_SILENCE_DEPRECATION
#include <SDL.h>
#include "engine_internal.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-gpu-private.h"
#include "cpu/sh/sh4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hikaru_vendor_test_fail_renderer_init(void);
static unsigned checks, failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #condition); ++failures; } } while (0)
static void reset(void *opaque) { (void)opaque; }
static int read_bus(void *opaque, unsigned size, uint32_t address, uint64_t *value)
{ (void)opaque; (void)size; (void)address; *value = 0; return 0; }
static int write_bus(void *opaque, unsigned size, uint32_t address, uint64_t value)
{ (void)opaque; (void)size; (void)address; (void)value; return 0; }
static int advance(void *opaque, uint32_t cycles)
{ (void)opaque; (void)cycles; return 0; }
static void inputs(void *opaque, const HikaruEngineInputs *value)
{ (void)opaque; (void)value; }
static const HikaruEngineMie mie = {NULL, reset, read_bus, write_bus, advance, inputs, NULL};
static const HikaruEngineProtection protection = {NULL, reset, read_bus, write_bus};

static void reject(HikaruEngineGame game, const HikaruEngineRoms *roms, const char *message)
{
    char error[256] = {0};
    HikaruEngine *engine = hikaru_engine_create_game(game, roms, &mie, &protection,
        0, 640, 480, 0, error, sizeof(error));
    CHECK(!engine && strstr(error, message));
    if (engine) hikaru_engine_destroy(engine);
}
static void check_bus(hikaru_t *board, unsigned bank, uint32_t offset, uint8_t expected)
{
    uint8_t value = 0;
    CHECK(!vk_device_put(board->memctl_m, 1, 0x0400001a, bank));
    CHECK(!vk_device_get(board->memctl_m, 1, 0x02000000 | offset, &value) && value == expected);
}
static void check_board(HikaruEngine *engine, const char *name, unsigned eprom_bank, unsigned mask_bank)
{
    hikaru_t *board = (hikaru_t *)engine->machine;
    CHECK(!strcmp(engine->game.name, name));
    CHECK(board->rombd_config.has_rom && !board->rombd_config.maskrom_is_stretched);
    CHECK(board->rombd_config.eprom_bank_size == eprom_bank &&
          board->rombd_config.maskrom_bank_size == mask_bank);
    CHECK(!board->sh_m->patch && !board->sh_s->patch);
}

int main(int argc, char **argv)
{
    const int gles = argc == 2 && !strcmp(argv[1], "--gles");
    uint8_t byte = 0;
    HikaruEngineRoms sizes = {&byte, 0x200000, &byte, 0x4000000, &byte, 0x10000000};
    /* These must reject before reading ROM bytes or asking for a GL context. */
    reject((HikaruEngineGame)-1, &sizes, "Unsupported Hikaru game");
    reject((HikaruEngineGame)3, &sizes, "Unsupported Hikaru game");
    reject(HIKARU_ENGINE_GAME_AIRTRIX, &sizes, "Invalid ROM sections");
    sizes.program_size = 0x800000; sizes.data_size = 0x8000000;
    reject(HIKARU_ENGINE_GAME_BRAVEFF, &sizes, "Invalid ROM sections");
    reject(HIKARU_ENGINE_GAME_PHARRIER, &sizes, "Invalid ROM sections");
    sizes.program_size = 0x4000000; sizes.data_size = 0x10000000 - 1;
    reject(HIKARU_ENGINE_GAME_BRAVEFF, &sizes, "Invalid ROM sections");
    sizes.data_size = 0x10000000; sizes.program_size--;
    reject(HIKARU_ENGINE_GAME_BRAVEFF, &sizes, "Invalid ROM sections");
    if (failures) return 1;

    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL video: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, gles ? 0 : 2);
    SDL_Window *window = SDL_CreateWindow("Hikaru game selection", 0, 0, 640, 480,
        SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); SDL_Quit(); return 2; }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (!context) { fprintf(stderr, "SDL GL: %s\n", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 2; }
#ifndef __APPLE__
    if (!(gles ? gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress) : gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress))) { fprintf(stderr, "glad: GL load failed\n"); SDL_DestroyWindow(window); SDL_Quit(); return 2; }
#endif
    uint8_t *boot = calloc(1, 0x200000), *program = calloc(1, 0x4000000),
        *data = calloc(1, 0x10000000);
    if (!boot || !program || !data) { failures++; goto cleanup; }
    /* A synthetic branch-to-self and delay-slot NOP permit frame execution. */
    boot[0] = 0xfe; boot[1] = 0xaf; boot[2] = 0x09;
    memcpy(boot + 0xd4, "SAMURAI BootROM Version 0.92", 29);
    /* Match the actual manifest: three EPROM pairs at 16 MiB slot strides,
     * with only 4 MiB populated in each slot; the fourth slot is empty. */
    program[0] = 0x10; program[0x1000000] = 0x24;
    program[0x2000000] = 0x38; program[0x23fffff] = 0x49;
    data[0] = 0x50; data[0x1000000] = 0x61;
    data[0xb000000] = 0x7b; data[0xbffffff] = 0x7e;
    HikaruEngineRoms roms = {boot, 0x200000, program, 0x4000000, data, 0x10000000};
    char error[256] = {0};
    HikaruEngine *engine = hikaru_engine_create_game(HIKARU_ENGINE_GAME_BRAVEFF,
        &roms, &mie, &protection, 0, 640, 480, 0, error, sizeof(error));
    CHECK(engine != NULL);
    if (!engine) { fprintf(stderr, "Brave create: %s\n", error); goto cleanup; }
    check_board(engine, "braveff", 2, 8);
    hikaru_t *board = (hikaru_t *)engine->machine;
    CHECK(board->eprom->size == roms.program_size && board->maskrom->size == roms.data_size);
    CHECK(board->eprom->ptr != program && board->maskrom->ptr != data);
    program[0] = 0xee; data[0] = 0xee;
    check_bus(board, 0x10, 0, 0x10);
    check_bus(board, 0x10, 0x400000, 0x10); /* 2 MiB chips: 4 MiB interleaved bank mirrors. */
    check_bus(board, 0x11, 0, 0x24);
    check_bus(board, 0x11, 0x400000, 0x24);
    check_bus(board, 0x12, 0, 0x38);
    check_bus(board, 0x12, 0xffffff, 0x49);
    check_bus(board, 0x13, 0, 0); /* Unpopulated EPROM slot. */
    check_bus(board, 0x20, 0, 0x50);
    check_bus(board, 0x21, 0, 0x61);
    check_bus(board, 0x2b, 0, 0x7b);
    check_bus(board, 0x2b, 0xffffff, 0x7e);
    check_bus(board, 0x2c, 0, 0); /* Remaining MASKROM slots are unpopulated. */
    check_bus(board, 0x2f, 0xffffff, 0);
    hikaru_engine_destroy(engine);

    /* Planet Harriers has four 8 MiB program pairs in 16 MiB slots and
     * sixteen contiguous 16 MiB MASKROM banks. Read through the real MEMCTL
     * to catch a packed-source stride accidentally selecting a socket gap. */
    program[0] = 0x80; program[0x1000000] = 0x91;
    program[0x2000000] = 0xa2; program[0x3000000] = 0xb3;
    program[0x37fffff] = 0xc4;
    data[0] = 0xd5; data[0x1000000] = 0xe6;
    data[0xf000000] = 0xf7; data[0xfffffff] = 0xf8;
    roms.borrow_cartridge = 1;
    engine = hikaru_engine_create_game(HIKARU_ENGINE_GAME_PHARRIER,
        &roms, &mie, &protection, 0, 640, 480, 0, error, sizeof(error));
    CHECK(engine != NULL);
    if (!engine) { fprintf(stderr, "Planet Harriers create: %s\n", error); goto cleanup; }
    check_board(engine, "pharrier", 4, 16);
    board = (hikaru_t *)engine->machine;
    CHECK(board->rombd_config.eprom_source_stride == 0x1000000);
    CHECK(board->eprom->ptr == program && board->maskrom->ptr == data);
    CHECK(board->bootrom->ptr != boot);
    vk_buffer_put(board->eprom, 1, 0, 0);
    vk_buffer_clear(board->maskrom);
    CHECK(program[0] == 0x80 && data[0] == 0xd5);
    check_bus(board, 0x10, 0, 0x80);
    check_bus(board, 0x10, 0x800000, 0x80); /* 8 MiB within-bank mirror. */
    check_bus(board, 0x11, 0, 0x91);
    check_bus(board, 0x12, 0, 0xa2);
    check_bus(board, 0x13, 0, 0xb3);
    check_bus(board, 0x13, 0x7fffff, 0xc4);
    check_bus(board, 0x13, 0xffffff, 0xc4);
    check_bus(board, 0x20, 0, 0xd5);
    check_bus(board, 0x21, 0, 0xe6);
    check_bus(board, 0x2f, 0, 0xf7);
    check_bus(board, 0x2f, 0xffffff, 0xf8);
    hikaru_engine_destroy(engine);

    /* Original callers still select Air Trix after the other boards are freed. */
    program[0x400000] = 0x24;
    roms.program_size = 0x800000; roms.data_size = 0x8000000;
    engine = hikaru_engine_create(&roms, &mie, &protection, 0, 640, 480, 0, error, sizeof(error));
    CHECK(engine != NULL);
    if (!engine) { fprintf(stderr, "Air Trix create: %s\n", error); goto cleanup; }
    check_board(engine, "airtrix", 4, 16);
    check_bus((hikaru_t *)engine->machine, 0x10, 0x400000, 0x24);
    board = (hikaru_t *)engine->machine;
    sh4_t *master = (sh4_t *)board->sh_m;
    hikaru_gpu_t *gpu = (hikaru_gpu_t *)board->gpu;
    const uint32_t pc = master->regs.pc;
    board->ram_m->ptr[17] = 0x5a;
    board->texram[0]->ptr[23] = 0xa5;
    board->aica_ram_m->ptr[31] = 0x3c;
    board->bram->ptr[9] = 0x96;
    HikaruEngineInputs frame = {{0}, {0}, 0, 0, {0}};
    CHECK(!hikaru_engine_release_graphics(engine));
    CHECK(!hikaru_engine_release_graphics(engine)); /* Idempotent teardown. */
    CHECK(!engine->machine->renderer && !gpu->renderer);
    CHECK(hikaru_engine_run_frame(engine, &frame) == -1);
    SDL_GL_DeleteContext(context);
    context = SDL_GL_CreateContext(window);
    if (!context) { fprintf(stderr, "SDL replacement GL: %s\n", SDL_GetError()); failures++; hikaru_engine_destroy(engine); goto cleanup; }
#ifndef __APPLE__
    if (!(gles ? gladLoadGLES2((GLADloadfunc)SDL_GL_GetProcAddress) : gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress))) {
        failures++; hikaru_engine_destroy(engine); goto cleanup;
    }
#endif
    /* An aborted construction must leave a retryable machine. */
    hikaru_vendor_test_fail_renderer_init();
    CHECK(hikaru_engine_restore_graphics(engine, 0, 640, 480) == -1);
    CHECK(!engine->machine->renderer && !gpu->renderer);
    CHECK(!hikaru_engine_restore_graphics(engine, 0, 640, 480));
    CHECK(engine->machine->renderer && gpu->renderer == engine->machine->renderer);
    CHECK(engine->machine == &board->base && master->regs.pc == pc);
    CHECK(board->ram_m->ptr[17] == 0x5a && board->texram[0]->ptr[23] == 0xa5);
    CHECK(board->aica_ram_m->ptr[31] == 0x3c && board->bram->ptr[9] == 0x96);
    CHECK(!hikaru_engine_run_frame(engine, &frame));
    CHECK(!hikaru_engine_release_graphics(engine));
    SDL_GL_DeleteContext(context);
    context = NULL;
    hikaru_engine_destroy(engine); /* Destruction after suspension needs no GL. */
cleanup:
    free(boot); free(program); free(data);
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    printf("Hikaru game selection: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
