// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
// Portions Copyright 2026 The Hollycast Authors
#include "engine_internal.h"
#include "cpu/sh/sh4.h"
#include "mach/hikaru/hikaru.h"
#include "mach/hikaru/hikaru-gpu-private.h"
#include "mach/hikaru/hikaru-renderer.h"
#include "mach/hikaru/hikaru-renderer-private.h"
#include "vk/input.h"
#include "graphics_dispatch.h"
#include <math.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

unsigned vk_verbosity = 0;
static _Thread_local HikaruEngine *activeEngine;
/* Rendering can overlap machine execution. Its abort target and diagnostics
 * must never share the producer's engine->fatal/error storage. */
static _Thread_local jmp_buf *graphicsFatal;
static _Thread_local char *graphicsError;
static _Thread_local size_t graphicsErrorSize;
static HikaruEngine *liveEngine;
/* Engine creation already enforces one live instance. Never recycle a source
 * token, including after allocator reuse; exhaustion permanently disables reuse. */
static uint64_t next_texture_source = 1;

static uint64_t profile_clock(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    return QueryPerformanceCounter(&counter) ? (uint64_t)counter.QuadPart : 0;
#else
    struct timespec counter;
    if (clock_gettime(CLOCK_MONOTONIC, &counter) != 0) return 0;
    return (uint64_t)counter.tv_sec * UINT64_C(1000000000) + (uint64_t)counter.tv_nsec;
#endif
}

static void profile_init(HikaruEngine *engine)
{
    if (!getenv("HIKARU_TRACE_TIMING")) return;
#ifdef _WIN32
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return;
    engine->profile.frequency = (uint64_t)frequency.QuadPart;
#else
    engine->profile.frequency = UINT64_C(1000000000);
#endif
    engine->profile.enabled = profile_clock() != 0;
}

HikaruProfilePhase hikaru_profile_switch(HikaruEngine *engine, HikaruProfilePhase phase)
{
    if (!engine || !engine->profile.collecting) return HIKARU_PROFILE_OTHER;
    const uint64_t now = profile_clock();
    const HikaruProfilePhase previous = engine->profile.phase;
    if (engine->profile.last && now >= engine->profile.last)
        engine->profile.frame[previous] += now - engine->profile.last;
    engine->profile.last = now;
    engine->profile.phase = phase;
    return previous;
}

static void profile_begin_frame(HikaruEngine *engine)
{
    if (!engine->profile.enabled) return;
    memset(engine->profile.frame, 0, sizeof(engine->profile.frame));
    engine->profile.phase = HIKARU_PROFILE_OTHER;
    engine->profile.last = profile_clock();
    engine->profile.collecting = true;
}

static void profile_end_frame(HikaruEngine *engine, bool success)
{
    if (!engine->profile.collecting) return;
    hikaru_profile_switch(engine, HIKARU_PROFILE_OTHER);
    engine->profile.collecting = false;
    if (!success) return;
    for (unsigned i = 0; i < HIKARU_PROFILE_COUNT; ++i)
        engine->profile.window[i] += engine->profile.frame[i];
    if (++engine->profile.frames != 60) return;
    const double scale = 1000.0 / (double)engine->profile.frequency / engine->profile.frames;
    double ms[HIKARU_PROFILE_COUNT], total = 0;
    for (unsigned i = 0; i < HIKARU_PROFILE_COUNT; ++i) {
        ms[i] = (double)engine->profile.window[i] * scale;
        total += ms[i];
    }
    const hikaru_t *board = (const hikaru_t *)engine->machine;
    fprintf(stderr, "Hikaru phase timing: game=%s frames=%u cpu_mhz=%u master_ms=%.3f slave_ms=%.3f memctl_ms=%.3f mie_ms=%.3f aica_ms=%.3f gpu_cp_ms=%.3f render_submit_ms=%.3f gl_state_ms=%.3f other_ms=%.3f total_ms=%.3f\n",
        engine->game.name, engine->profile.frames, board->cpu_clock_mhz,
        ms[HIKARU_PROFILE_MASTER], ms[HIKARU_PROFILE_SLAVE], ms[HIKARU_PROFILE_MEMCTL],
        ms[HIKARU_PROFILE_MIE], ms[HIKARU_PROFILE_SOUND], ms[HIKARU_PROFILE_GPU],
        ms[HIKARU_PROFILE_RENDER], ms[HIKARU_PROFILE_GL_STATE], ms[HIKARU_PROFILE_OTHER], total);
    memset(engine->profile.window, 0, sizeof(engine->profile.window));
    engine->profile.frames = 0;
}

int hikaru_engine_get_cabinet_defaults(const HikaruEngine *engine, uint8_t *data, size_t size)
{
    if (!engine || !engine->machine || !data || size != 128) return -1;
    const hikaru_t *machine = (const hikaru_t *)engine->machine;
    if (!machine->ram_m || machine->ram_m->size < 0xa00) return -1;
    memcpy(data, machine->ram_m->ptr + 0x980, size);
    return 0;
}

int hikaru_engine_get_bram(const HikaruEngine *engine, uint8_t *data, size_t size)
{
    if (!engine || !engine->machine || !data || size != HIKARU_ENGINE_BRAM_SIZE) return -1;
    const vk_buffer_t *bram = ((const hikaru_t *)engine->machine)->bram;
    if (!bram || !bram->ptr || bram->size != size) return -1;
    memcpy(data, bram->ptr, size);
    return 0;
}
int hikaru_engine_set_bram(HikaruEngine *engine, const uint8_t *data, size_t size)
{
    if (!engine || !engine->machine || !data || size != HIKARU_ENGINE_BRAM_SIZE) return -1;
    vk_buffer_t *bram = ((hikaru_t *)engine->machine)->bram;
    if (!bram || !bram->ptr || bram->size != size) return -1;
    memcpy(bram->ptr, data, size);
    return 0;
}

uint64_t vk_host_diagnostic(void *host, unsigned kind, uint32_t address)
{
    HikaruEngine *engine = host;
    if (!engine || kind < 1 || kind > 3) return 1;
    const unsigned group = kind - 1;
    uint64_t *count = NULL;
    for (unsigned i = 0; i < 32; ++i) {
        if (!engine->diagnostics[group][i].count) {
            engine->diagnostics[group][i].address = address;
            count = &engine->diagnostics[group][i].count;
            break;
        }
        if (engine->diagnostics[group][i].address == address) {
            count = &engine->diagnostics[group][i].count;
            break;
        }
    }
    if (!count) count = &engine->otherDiagnostics[group];
    if (*count != UINT64_MAX) ++*count;
    return (*count && !(*count & (*count - 1))) ? *count : 0;
}

_Noreturn void vk_host_abort(const char *file, int line, const char *function, const char *format, ...)
{
    (void)file; (void)line; (void)function;
    if (graphicsFatal) {
        va_list args;
        va_start(args, format);
        vsnprintf(graphicsError, graphicsErrorSize, format, args);
        va_end(args);
        longjmp(*graphicsFatal, 1);
    }
    if (activeEngine) {
        va_list args;
        va_start(args, format);
        vsnprintf(activeEngine->error, sizeof(activeEngine->error), format, args);
        va_end(args);
        longjmp(activeEngine->fatal, 1);
    }
    abort(); /* Internal contract violation: core invoked outside its C API. */
}
void vk_host_attach_machine(void *host, void *machine)
{
    if (host) ((HikaruEngine *)host)->machine = machine;
}
bool vk_input_get_key(unsigned key) { (void)key; return false; }
void vk_input_set_key(unsigned key, bool state) { (void)key; (void)state; }

vk_buffer_t *vk_game_get_section_data(vk_game_t *game, const char *name)
{
    for (unsigned i = 0; i < game->nsections; ++i)
        if (!strcmp(game->sections[i].name, name)) return game->sections[i].buffer;
    return NULL;
}
static void set_error(char *out, size_t size, const char *message)
{
    if (out && size) snprintf(out, size, "%s", message);
}
typedef struct {
    GLint drawFbo, readFbo, renderBuffer, program, vao, arrayBuffer, unpackBuffer, activeTexture, textures[4];
    GLint viewport[4], scissor[4], depthFunc, cullFace, frontFace;
    GLint blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha, blendEqRgb, blendEqAlpha;
    GLint unpackAlignment, unpackRowLength, unpackSkipRows, unpackSkipPixels;
    GLfloat polygonFactor, polygonUnits, clearColor[4];
    GLfloat clearDepth, depthRange[2];
    GLboolean depthMask, colorMask[4], depthTest, blend, cull, scissorTest, polygonOffset, rasterizerDiscard, stencilTest;
} HostGlState;
static void save_gl_state(HostGlState *s)
{
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->drawFbo); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->readFbo);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderBuffer); glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpackBuffer);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->arrayBuffer); glGetIntegerv(GL_ACTIVE_TEXTURE, &s->activeTexture);
    for (unsigned i = 0; i < 4; ++i) { glActiveTexture(GL_TEXTURE0 + i); glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->textures[i]); }
    glActiveTexture((GLenum)s->activeTexture);
    glGetIntegerv(GL_VIEWPORT, s->viewport); glGetIntegerv(GL_SCISSOR_BOX, s->scissor);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depthFunc); glGetIntegerv(GL_CULL_FACE_MODE, &s->cullFace); glGetIntegerv(GL_FRONT_FACE, &s->frontFace);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blendSrcRgb); glGetIntegerv(GL_BLEND_DST_RGB, &s->blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blendSrcAlpha); glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blendEqRgb); glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blendEqAlpha);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpackAlignment); glGetIntegerv(GL_UNPACK_ROW_LENGTH, &s->unpackRowLength);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &s->unpackSkipRows); glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &s->unpackSkipPixels);
    glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &s->polygonFactor); glGetFloatv(GL_POLYGON_OFFSET_UNITS, &s->polygonUnits);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clearColor); glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depthMask); glGetBooleanv(GL_COLOR_WRITEMASK, s->colorMask);
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &s->clearDepth); glGetFloatv(GL_DEPTH_RANGE, s->depthRange);
    s->depthTest = glIsEnabled(GL_DEPTH_TEST); s->blend = glIsEnabled(GL_BLEND); s->cull = glIsEnabled(GL_CULL_FACE);
    s->scissorTest = glIsEnabled(GL_SCISSOR_TEST); s->polygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
    s->rasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
    s->stencilTest = glIsEnabled(GL_STENCIL_TEST);
}
static void set_enabled(GLenum capability, GLboolean enabled) { if (enabled) glEnable(capability); else glDisable(capability); }
static void prepare_gl(void)
{
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glFrontFace(GL_CCW);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_RASTERIZER_DISCARD);
    glDisable(GL_STENCIL_TEST);
    vk_renderer_clear_depth(1.0);
    vk_renderer_depth_range(0.0, 1.0);
}
static void restore_gl_state(const HostGlState *s)
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->drawFbo); glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->readFbo);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->renderBuffer); glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->unpackBuffer);
    glUseProgram((GLuint)s->program); glBindVertexArray((GLuint)s->vao); glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->arrayBuffer);
    for (unsigned i = 0; i < 4; ++i) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, (GLuint)s->textures[i]); }
    glActiveTexture((GLenum)s->activeTexture); glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor[0], s->scissor[1], s->scissor[2], s->scissor[3]);
    glDepthFunc((GLenum)s->depthFunc); glDepthMask(s->depthMask); glColorMask(s->colorMask[0], s->colorMask[1], s->colorMask[2], s->colorMask[3]);
    glCullFace((GLenum)s->cullFace); glFrontFace((GLenum)s->frontFace);
    glBlendFuncSeparate((GLenum)s->blendSrcRgb, (GLenum)s->blendDstRgb, (GLenum)s->blendSrcAlpha, (GLenum)s->blendDstAlpha);
    glBlendEquationSeparate((GLenum)s->blendEqRgb, (GLenum)s->blendEqAlpha); glPolygonOffset(s->polygonFactor, s->polygonUnits);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpackAlignment); glPixelStorei(GL_UNPACK_ROW_LENGTH, s->unpackRowLength);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, s->unpackSkipRows); glPixelStorei(GL_UNPACK_SKIP_PIXELS, s->unpackSkipPixels);
    glClearColor(s->clearColor[0], s->clearColor[1], s->clearColor[2], s->clearColor[3]);
    vk_renderer_clear_depth(s->clearDepth); vk_renderer_depth_range(s->depthRange[0], s->depthRange[1]);
    set_enabled(GL_DEPTH_TEST, s->depthTest); set_enabled(GL_BLEND, s->blend); set_enabled(GL_CULL_FACE, s->cull);
    set_enabled(GL_SCISSOR_TEST, s->scissorTest); set_enabled(GL_POLYGON_OFFSET_FILL, s->polygonOffset);
    set_enabled(GL_RASTERIZER_DISCARD, s->rasterizerDiscard);
    set_enabled(GL_STENCIL_TEST, s->stencilTest);
}
#ifdef HIKARU_VENDOR_TESTING
void hikaru_vendor_test_with_gl_guard(void (*draw)(void))
{
    HostGlState state;
    save_gl_state(&state);
    prepare_gl();
    draw();
    restore_gl_state(&state);
}
#endif
static int add_section(HikaruEngine *engine, unsigned index, const char *name,
                       const uint8_t *bytes, size_t size, bool borrowed)
{
    if (!bytes || size > UINT32_MAX) return -1;
    vk_buffer_t *buffer = borrowed ? vk_buffer_le32_view(bytes, (unsigned)size) : vk_buffer_le32_new((unsigned)size, 0);
    if (!buffer) return -1;
    if (!borrowed) memcpy(buffer->ptr, bytes, size);
    snprintf(engine->sections[index].name, sizeof(engine->sections[index].name), "%s", name);
    engine->sections[index].buffer = buffer;
    return 0;
}
HikaruEngine *hikaru_engine_create(const HikaruEngineRoms *roms,
    const HikaruEngineMie *mie, const HikaruEngineProtection *protection,
    uint32_t target_fbo, unsigned width, unsigned height,
    int bios_bypass, char *error, size_t error_size)
{
    return hikaru_engine_create_game(HIKARU_ENGINE_GAME_AIRTRIX, roms, mie, protection,
        target_fbo, width, height, bios_bypass, error, error_size);
}
HikaruEngine *hikaru_engine_create_game(HikaruEngineGame game, const HikaruEngineRoms *roms,
    const HikaruEngineMie *mie, const HikaruEngineProtection *protection,
    uint32_t target_fbo, unsigned width, unsigned height,
    int bios_bypass, char *error, size_t error_size)
{
    if (liveEngine) { set_error(error, error_size, "Only one Hikaru engine may be active"); return NULL; }
    const char *game_name;
    size_t program_size, data_size;
    switch (game) {
    case HIKARU_ENGINE_GAME_AIRTRIX:
        game_name = "airtrix"; program_size = 0x800000; data_size = 0x8000000;
        break;
    case HIKARU_ENGINE_GAME_BRAVEFF:
        game_name = "braveff"; program_size = 0x4000000; data_size = 0x10000000;
        break;
    case HIKARU_ENGINE_GAME_PHARRIER:
        game_name = "pharrier"; program_size = 0x4000000; data_size = 0x10000000;
        break;
    case HIKARU_ENGINE_GAME_VON4:
        game_name = "von4"; program_size = 0x4000000; data_size = 0x10000000;
        break;
    case HIKARU_ENGINE_GAME_SGNASCAR:
        game_name = "sgnascar"; program_size = 0x4000000; data_size = 0x10000000;
        break;
    case HIKARU_ENGINE_GAME_SWRACER:
        game_name = "swracer"; program_size = 0x4000000; data_size = 0x10000000;
        break;
    default:
        set_error(error, error_size, "Unsupported Hikaru game"); return NULL;
    }
    if (!roms || roms->boot_size != 0x200000 || roms->program_size != program_size || roms->data_size != data_size ||
        !roms->boot || !roms->program || !roms->data || !mie || !mie->reset || !mie->read || !mie->write || !mie->advance || !mie->set_inputs ||
        !protection || !protection->reset || !protection->read || !protection->write || !width || !height) {
        set_error(error, error_size, "Invalid ROM sections, device callbacks, or output dimensions"); return NULL;
    }
    if (!glGetString(GL_VERSION)) { set_error(error, error_size, "A current OpenGL 3.2 or OpenGL ES 3.0 context is required"); return NULL; }
    HikaruEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) { set_error(error, error_size, "Out of memory"); return NULL; }
    profile_init(engine);
    engine->texture_source = next_texture_source;
    if (next_texture_source) ++next_texture_source;
    engine->texture_generation[0] = engine->texture_generation[1] = 1;
    engine->transparency_peel_capacity = HIKARU_DEFAULT_TRANSPARENCY_LAYERS;
    engine->transparency_memory_budget = HIKARU_DEFAULT_TRANSPARENCY_BUDGET;
    engine->internal_width = 640;
    engine->internal_height = 480;
    engine->output_aspect_ratio = 4.0f / 3.0f;
    liveEngine = engine;
    engine->mie = *mie;
    engine->protection = *protection;
    strcpy(engine->game.name, game_name);
    strcpy(engine->game.mach, "hikaru");
    engine->game.sections = engine->sections;
    engine->game.nsections = 3;
    if (add_section(engine, 0, "bootrom", roms->boot, roms->boot_size, false) ||
        add_section(engine, 1, "eprom", roms->program, roms->program_size, roms->borrow_cartridge != 0) ||
        add_section(engine, 2, "maskrom", roms->data, roms->data_size, roms->borrow_cartridge != 0)) {
        set_error(error, error_size, "Could not allocate ROM sections"); hikaru_engine_destroy(engine); return NULL;
    }
    // Upstream BIOS version comparison is NUL-terminated; reject malformed data.
    if (!memchr(engine->sections[0].buffer->ptr + 0xd4, 0, 64)) {
        set_error(error, error_size, "Invalid BIOS version string"); hikaru_engine_destroy(engine); return NULL;
    }
    HostGlState glState;
    save_gl_state(&glState);
    prepare_gl();
    activeEngine = engine;
    if (setjmp(engine->fatal)) {
        activeEngine = NULL;
        restore_gl_state(&glState);
        set_error(error, error_size, engine->error);
        hikaru_engine_destroy(engine);
        return NULL;
    }
    vk_machine_t *created = hikaru_new(&engine->game, engine, bios_bypass);
    restore_gl_state(&glState);
    if (!created) { activeEngine = NULL; set_error(error, error_size, "Hikaru machine initialization failed"); hikaru_engine_destroy(engine); return NULL; }
    hikaru_engine_resize(engine, target_fbo, width, height);
    hikaru_renderer_set_transparency_sorting(engine->machine->renderer, engine->transparency_sorting);
    hikaru_renderer_set_transparency_peel_capacity(engine->machine->renderer, engine->transparency_peel_capacity);
    hikaru_renderer_set_transparency_memory_budget(engine->machine->renderer, engine->transparency_memory_budget);
    engine->protection.reset(engine->protection.opaque);
    vk_machine_reset(engine->machine, VK_RESET_TYPE_HARD);
    if (engine->error[0]) { activeEngine = NULL; set_error(error, error_size, engine->error); hikaru_engine_destroy(engine); return NULL; }
    activeEngine = NULL;
    set_error(error, error_size, "");
    return engine;
}
int hikaru_engine_set_sound(HikaruEngine *engine, const HikaruEngineSound *sound)
{
    if (!engine || !engine->machine || !sound || !engine->mie.set_sound_irq || engine->sound_attached || engine->frame_started ||
        !sound->attach || !sound->detach || !sound->reset || !sound->set_reset ||
        !sound->read || !sound->write || !sound->advance || !sound->irq) return -1;
    hikaru_t *board = (hikaru_t *)engine->machine;
    if (!board->aica_ram_m || !board->aica_ram_s || !board->aica_ram_m->ptr || !board->aica_ram_s->ptr ||
        board->aica_ram_m->size != 8u * 1024 * 1024 || board->aica_ram_s->size != 8u * 1024 * 1024 ||
        sound->attach(sound->opaque, 0, board->aica_ram_m->ptr, board->aica_ram_m->size) ||
        sound->attach(sound->opaque, 1, board->aica_ram_s->ptr, board->aica_ram_s->size) ||
        sound->reset(sound->opaque)) {
        sound->detach(sound->opaque);
        snprintf(engine->error, sizeof(engine->error), "Cannot attach Hikaru sound boards");
        return -1;
    }
    engine->sound = *sound;
    engine->sound_attached = true;
    engine->sound_reset = -1;
    return 0;
}
int hikaru_engine_sync_sound_reset(HikaruEngine *engine)
{
    if (!engine || !engine->sound_attached) return 0;
    uint64_t control = 0;
    if (engine->mie.read(engine->mie.opaque, 1, 0x00800006, &control)) {
        snprintf(engine->error, sizeof(engine->error), "Cannot read Hikaru sound reset pin");
        return -1;
    }
    const int asserted = !(control & 0x20);
    if (engine->sound_reset != asserted) {
        if (engine->sound.set_reset(engine->sound.opaque, asserted)) {
            snprintf(engine->error, sizeof(engine->error), "Cannot update Hikaru sound reset pin");
            return -1;
        }
        engine->sound_reset = asserted;
    }
    return 0;
}
int hikaru_engine_set_cpu_clock_mhz(HikaruEngine *engine, unsigned mhz)
{
    if (!engine || !engine->machine || mhz < 100 || mhz > 300) return -1;
    hikaru_t *hikaru = (hikaru_t *)engine->machine;
    if (hikaru->cpu_clock_mhz != mhz) {
        hikaru->cpu_clock_mhz = mhz;
        hikaru->cpu_cycle_remainder = 0;
    }
    return 0;
}

uint64_t hikaru_engine_state_layout(uint32_t encoding)
{
#ifdef _WIN64
    /* These are the shipped V1 Windows x64 prefix dimensions. Growing a
     * legacy low table would silently invalidate every existing state. */
    _Static_assert(offsetof(hikaru_gpu_t, state) == 1072, "V1 GPU state offset changed");
    _Static_assert(sizeof(((hikaru_gpu_t *)0)->state) == 972192, "V1 GPU state size changed");
    _Static_assert(offsetof(hikaru_gpu_t, idma_completed) == 973268, "V1 GPU prefix changed");
#endif
    if (encoding != HIKARU_STATE_ENCODING_LEGACY && encoding != HIKARU_STATE_ENCODING_CURRENT) return 0;
    // Include compiler/architecture-sensitive native structures, never pointers
    // or host resources themselves. Bump the host payload version on changes.
    // Reproduce the old complete-struct size, including its tail padding,
    // without including the appended banks in the V1 compatibility tag.
    const size_t legacy_end = offsetof(hikaru_gpu_t, idma_completed) + sizeof(((hikaru_gpu_t *)0)->idma_completed);
    const size_t alignment = _Alignof(hikaru_gpu_t);
    const size_t gpu_size = encoding == HIKARU_STATE_ENCODING_LEGACY
        ? (legacy_end + alignment - 1) / alignment * alignment : sizeof(hikaru_gpu_t);
    const uint32_t layout[] = {sizeof(void *), sizeof(bool), sizeof(sh4_t),
        (uint32_t)gpu_size, sizeof(hikaru_rombd_config_t),
        offsetof(sh4_t, regs), offsetof(sh4_t, intc), offsetof(sh4_t, tmu),
        offsetof(hikaru_gpu_t, state), offsetof(hikaru_gpu_t, idma_completed),
        sizeof(((hikaru_gpu_t *)0)->state), sizeof(((sh4_t *)0)->regs),
#ifdef _MSC_VER
        0x4d530000u | _MSC_VER,
#elif defined(__clang__)
        0x434c0000u | __clang_major__,
#else
        0x47430000u | __GNUC__,
#endif
    };
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned i = 0; i < NUMELEM(layout); ++i) {
        hash ^= layout[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int transfer_engine_state(HikaruEngine *engine, vk_state_t *state)
{
    if (!engine || !engine->machine || activeEngine ||
        (state->encoding != HIKARU_STATE_ENCODING_LEGACY && state->encoding != HIKARU_STATE_ENCODING_CURRENT)) return -1;
    /* RAM is restored before devices. Invalidate even if a later field rejects
     * the load, so no packet can reuse bytes from the partially replaced RAM. */
    if (state->mode == VK_STATE_LOAD) {
        hikaru_engine_note_texram_write(engine, 3u);
        /* Rejected partial loads must also invalidate decoded GL textures,
         * since only a successful load reaches the full renderer reset. */
        hikaru_renderer_note_cpu_texram_write(engine->machine->renderer);
    }
    int result = vk_machine_transfer_state(engine->machine, state);
    if (!result) {
        result |= state->mode == VK_STATE_SAVE
            ? vk_state_put(state, &engine->sound_reset, sizeof(engine->sound_reset))
            : vk_state_get(state, &engine->sound_reset, sizeof(engine->sound_reset));
    }
    return result || state->failed ? -1 : 0;
}

size_t hikaru_engine_state_size(HikaruEngine *engine, uint32_t encoding)
{
    vk_state_t state = {0};
    state.mode = VK_STATE_SAVE;
    state.encoding = encoding;
    state.capacity = SIZE_MAX;
    return transfer_engine_state(engine, &state) ? 0 : state.offset;
}

int hikaru_engine_save_state(HikaruEngine *engine, uint8_t *data, size_t size)
{
    if (!data || size != hikaru_engine_state_size(engine, HIKARU_STATE_ENCODING_CURRENT)) return -1;
    vk_state_t state = {0};
    state.mode = VK_STATE_SAVE;
    state.encoding = HIKARU_STATE_ENCODING_CURRENT;
    state.data = data;
    state.capacity = size;
    return transfer_engine_state(engine, &state) || state.offset != size ? -1 : 0;
}

int hikaru_engine_load_state(HikaruEngine *engine, const uint8_t *data, size_t size, uint32_t encoding)
{
    if (!data || !engine || !engine->machine || !engine->machine->renderer ||
        (encoding != HIKARU_STATE_ENCODING_LEGACY && encoding != HIKARU_STATE_ENCODING_CURRENT) ||
        size != hikaru_engine_state_size(engine, encoding)) return -1;
    vk_state_t state = {0};
    state.mode = VK_STATE_LOAD;
    state.encoding = encoding;
    state.data = (uint8_t *)data;
    state.capacity = size;
    if (transfer_engine_state(engine, &state) || state.offset != size ||
        engine->sound_reset < -1 || engine->sound_reset > 1) return -1;
    // Texture and mesh handles belong to the current host renderer, not the
    // snapshot. Rebuild them from restored RAM at this graphics-safe boundary.
    HostGlState host;
    save_gl_state(&host);
    vk_renderer_reset(engine->machine->renderer);
    restore_gl_state(&host);
    return 0;
}

int hikaru_engine_run_frame(HikaruEngine *engine, const HikaruEngineInputs *inputs)
{
    if (!engine || !engine->machine || !engine->machine->renderer || !inputs) return -1;
    engine->frame_started = true;
    engine->error[0] = '\0';
    engine->mie.set_inputs(engine->mie.opaque, inputs);
    profile_begin_frame(engine);
    hikaru_profile_switch(engine, HIKARU_PROFILE_GL_STATE);
    HostGlState glState;
    save_gl_state(&glState);
    if (getenv("HIKARU_TRACE_FRAME")) {
        static bool traced;
        if (!traced) {
            fprintf(stderr, "Hikaru host depth before guard: clear=%g range=%g,%g stencil=%u raster_discard=%u scissor=%u:%d,%d,%d,%d\n",
                    glState.clearDepth, glState.depthRange[0], glState.depthRange[1],
                    (unsigned)glState.stencilTest, (unsigned)glState.rasterizerDiscard,
                    (unsigned)glState.scissorTest, glState.scissor[0], glState.scissor[1], glState.scissor[2], glState.scissor[3]);
            traced = true;
        }
    }
    prepare_gl();
    hikaru_profile_switch(engine, HIKARU_PROFILE_OTHER);
    activeEngine = engine;
    if (setjmp(engine->fatal)) {
        activeEngine = NULL;
        profile_end_frame(engine, false);
        restore_gl_state(&glState);
        return -1;
    }
    hikaru_renderer_t *renderer = (hikaru_renderer_t *)engine->machine->renderer;
    if (renderer->capture_started || renderer->capture_invalidate_all) {
        // A displayed packet can trail live machine RAM. Switching back to
        // direct drawing must not reuse textures from that older snapshot;
        // the next capture transition also starts with a full invalidation.
        hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
        renderer->capture_started = false;
        renderer->capture_invalidate_all = false;
    }
    vk_renderer_begin_frame(engine->machine->renderer);
    int status = vk_machine_run_frame(engine->machine);
    hikaru_profile_switch(engine, HIKARU_PROFILE_RENDER);
    if (!status && !engine->error[0]) vk_renderer_end_frame(engine->machine->renderer);
    hikaru_profile_switch(engine, HIKARU_PROFILE_GL_STATE);
    restore_gl_state(&glState);
    profile_end_frame(engine, !status && !engine->error[0]);
    activeEngine = NULL;
    if (status || engine->error[0]) return -1;
    return 0;
}

int hikaru_engine_build_frame(HikaruEngine *engine, const HikaruEngineInputs *inputs, HikaruFrame *frame)
{
    if (!engine || !engine->machine || !engine->machine->renderer || !inputs || !frame || !frame->capture)
        return -1;
    vk_renderer_t *renderer = engine->machine->renderer;
    hikaru_gpu_t *gpu = (hikaru_gpu_t *)((hikaru_t *)engine->machine)->gpu;
    engine->frame_started = true;
    engine->error[0] = '\0';
    frame->ready = false;
    engine->mie.set_inputs(engine->mie.opaque, inputs);
    profile_begin_frame(engine);
    activeEngine = engine;
    if (setjmp(engine->fatal)) {
        hikaru_renderer_end_capture(renderer, gpu);
        activeEngine = NULL;
        profile_end_frame(engine, false);
        return -1;
    }
    hikaru_renderer_t *capture = frame->capture;
    capture->base.internal_width = engine->internal_width;
    capture->base.internal_height = engine->internal_height;
    capture->transparency_sorting = engine->transparency_sorting;
    capture->transparency_peel_capacity = engine->transparency_peel_capacity;
    capture->transparency_memory_budget = engine->transparency_memory_budget;
    hikaru_renderer_begin_capture(renderer, gpu, frame);
    int status = vk_machine_run_frame(engine->machine);
    if (!status && !engine->error[0]) hikaru_renderer_seal_capture(renderer, gpu, frame);
    hikaru_renderer_end_capture(renderer, gpu);
    activeEngine = NULL;
    profile_end_frame(engine, !status && !engine->error[0]);
    return status || engine->error[0] ? -1 : 0;
}

void hikaru_engine_discard_frames(HikaruEngine *engine)
{
    if (engine && engine->machine && engine->machine->renderer)
        ((hikaru_renderer_t *)engine->machine->renderer)->capture_invalidate_all = true;
}

int hikaru_engine_draw_frame(HikaruEngine *engine, HikaruFrame *frame, char *error, size_t error_size)
{
    if (!engine || !engine->machine || !engine->machine->renderer || !frame || !frame->ready || graphicsFatal) {
        set_error(error, error_size, "Invalid Hikaru graphics packet or draw state");
        return -1;
    }
    vk_renderer_t *renderer = engine->machine->renderer;
    HostGlState host;
    save_gl_state(&host);
    jmp_buf fatal;
    char localError[256];
    graphicsError = error && error_size ? error : localError;
    graphicsErrorSize = error && error_size ? error_size : sizeof(localError);
    graphicsError[0] = '\0';
    graphicsFatal = &fatal;
    volatile bool cleaning = false;
    int failed = setjmp(fatal);
    if (!failed) {
        prepare_gl();
        hikaru_renderer_draw_packet(renderer, frame);
    }
    if (!cleaning) {
        cleaning = true;
        hikaru_renderer_finish_packet(renderer, frame);
    }
    // A valid draw attempt consumes its packet even if graphics preparation
    // failed before the renderer borrowed it. Recovery must rebuild the frame.
    frame->ready = false;
    graphicsFatal = NULL;
    graphicsError = NULL;
    graphicsErrorSize = 0;
    restore_gl_state(&host);
    return failed ? -1 : 0;
}

#ifdef HIKARU_VENDOR_TESTING
int hikaru_vendor_test_frame_packet_ownership(HikaruEngine *engine)
{
    if (!engine || !engine->machine || !engine->machine->renderer) return 0;
    activeEngine = engine;
    int result = 0;
    if (!setjmp(engine->fatal))
        result = hikaru_vendor_test_packet_ownership(engine->machine->renderer,
            ((hikaru_t *)engine->machine)->gpu);
    activeEngine = NULL;
    return result;
}
#endif

void hikaru_engine_resize(HikaruEngine *engine, uint32_t target_fbo, unsigned width, unsigned height)
{
    if (!engine || !engine->machine || !engine->machine->renderer || !width || !height) return;
    engine->machine->renderer->target_fbo = target_fbo;
    engine->machine->renderer->width = width;
    engine->machine->renderer->height = height;
    engine->machine->renderer->output_aspect_ratio = engine->output_aspect_ratio;
    engine->machine->renderer->internal_width = engine->internal_width;
    engine->machine->renderer->internal_height = engine->internal_height;
}
void hikaru_engine_set_output_aspect_ratio(HikaruEngine *engine, float aspect_ratio)
{
    if (!engine || !isfinite(aspect_ratio) || aspect_ratio <= 0.0f) return;
    engine->output_aspect_ratio = aspect_ratio;
    if (engine->machine && engine->machine->renderer)
        engine->machine->renderer->output_aspect_ratio = aspect_ratio;
}
int hikaru_engine_release_graphics(HikaruEngine *engine)
{
    if (!engine || !engine->machine) return -1;
    if (!engine->machine->renderer) return 0;
    HostGlState state;
    save_gl_state(&state);
    activeEngine = engine;
    const int failed = setjmp(engine->fatal);
    if (!failed) {
        vk_renderer_destroy(&engine->machine->renderer);
        ((hikaru_gpu_t *)((hikaru_t *)engine->machine)->gpu)->renderer = NULL;
    }
    if (failed) {
        // Destroy aborted part-way: host GL objects die with the context,
        // so drop stale pointers instead of reusing them on a new context.
        // The renderer struct itself is leaked in this rare failure path.
        ((hikaru_gpu_t *)((hikaru_t *)engine->machine)->gpu)->renderer = NULL;
        engine->machine->renderer = NULL;
    }
    activeEngine = NULL;
    restore_gl_state(&state);
    return failed ? -1 : 0;
}
int hikaru_engine_restore_graphics(HikaruEngine *engine, uint32_t target_fbo, unsigned width, unsigned height)
{
    if (!engine || !engine->machine || !width || !height) return -1;
    if (engine->machine->renderer) return 0;
    hikaru_t *board = (hikaru_t *)engine->machine;
    HostGlState state;
    save_gl_state(&state);
    prepare_gl();
    activeEngine = engine;
    int failed = setjmp(engine->fatal);
    if (!failed) {
        vk_renderer_t *renderer = hikaru_renderer_new(engine->machine, board->fb, board->texram);
        if (renderer) {
            hikaru_renderer_set_gpu(renderer, board->gpu);
            hikaru_renderer_set_transparency_sorting(renderer, engine->transparency_sorting);
            hikaru_renderer_set_transparency_peel_capacity(renderer, engine->transparency_peel_capacity);
            hikaru_renderer_set_transparency_memory_budget(renderer, engine->transparency_memory_budget);
            ((hikaru_gpu_t *)board->gpu)->renderer = renderer;
            hikaru_engine_resize(engine, target_fbo, width, height);
        } else {
            snprintf(engine->error, sizeof(engine->error), "Cannot restore Hikaru graphics");
            failed = 1;
        }
    }
    if (failed && engine->machine->renderer) {
        // Construction registers the renderer before it can abort; remove
        // the partial renderer so a later frame can retry cleanly.
        vk_renderer_t *partial = engine->machine->renderer;
        ((hikaru_gpu_t *)board->gpu)->renderer = NULL;
        engine->machine->renderer = NULL;
        if (!setjmp(engine->fatal)) vk_renderer_destroy(&partial);
    }
    activeEngine = NULL;
    restore_gl_state(&state);
    return failed ? -1 : 0;
}
int hikaru_engine_set_internal_resolution(HikaruEngine *engine, unsigned height, float aspect_ratio)
{
    if (!engine || !engine->machine || !engine->machine->renderer) return -1;
    if (height < 240 || height > 4320) {
        snprintf(engine->error, sizeof(engine->error), "Hikaru internal resolution must be between 240 and 4320 pixels high");
        return -1;
    }
    if (!isfinite(aspect_ratio) || aspect_ratio < 4.0f / 3.0f) {
        snprintf(engine->error, sizeof(engine->error), "Hikaru internal aspect ratio must be finite and at least 4:3");
        return -1;
    }
    // Round to an even target width while retaining the requested vertical detail.
    const double requested_width = round((double)height * aspect_ratio / 2.0) * 2.0;
    GLint max_texture = 0, max_renderbuffer = 0, max_viewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport);
    if (requested_width > max_texture || height > (unsigned)max_texture ||
        requested_width > max_renderbuffer || height > (unsigned)max_renderbuffer ||
        requested_width > max_viewport[0] || height > (unsigned)max_viewport[1]) {
        snprintf(engine->error, sizeof(engine->error), "Hikaru internal resolution exceeds OpenGL limits");
        return -1;
    }
    const unsigned width = (unsigned)requested_width;
    engine->machine->renderer->internal_width = width;
    engine->machine->renderer->internal_height = height;
    engine->internal_width = width;
    engine->internal_height = height;
    return 0;
}
int hikaru_engine_set_transparency_sorting(HikaruEngine *engine, HikaruTransparencySorting sorting)
{
    if (!engine || sorting < HIKARU_TRANSPARENCY_PER_STRIP || sorting > HIKARU_TRANSPARENCY_PER_PIXEL)
        return -1;
    engine->transparency_sorting = sorting;
    if (engine->machine && engine->machine->renderer)
        hikaru_renderer_set_transparency_sorting(engine->machine->renderer, sorting);
    engine->error[0] = '\0';
    return 0;
}
int hikaru_engine_set_transparency_peel_capacity(HikaruEngine *engine, unsigned layers)
{
    if (!engine || layers < 1 || layers > HIKARU_MAX_TRANSPARENCY_LAYERS) return -1;
    engine->transparency_peel_capacity = layers;
    if (engine->machine && engine->machine->renderer)
        hikaru_renderer_set_transparency_peel_capacity(engine->machine->renderer, layers);
    engine->error[0] = '\0';
    return 0;
}
int hikaru_engine_set_transparency_memory_budget(HikaruEngine *engine, uint64_t bytes)
{
    if (!engine || !bytes) return -1;
    engine->transparency_memory_budget = bytes;
    if (engine->machine && engine->machine->renderer)
        hikaru_renderer_set_transparency_memory_budget(engine->machine->renderer, bytes);
    engine->error[0] = '\0';
    return 0;
}
const char *hikaru_engine_last_error(const HikaruEngine *engine)
{ return engine ? engine->error : "Invalid engine"; }
void hikaru_engine_destroy(HikaruEngine *engine)
{
    if (!engine) return;
    if (engine->sound_attached) {
        engine->sound.detach(engine->sound.opaque);
        engine->sound_attached = false;
    }
    if (engine->machine) {
        HostGlState glState;
        const bool hasGraphics = engine->machine->renderer != NULL;
        if (hasGraphics) save_gl_state(&glState);
        activeEngine = engine;
        if (!setjmp(engine->fatal)) vk_machine_destroy(&engine->machine);
        activeEngine = NULL;
        if (hasGraphics) restore_gl_state(&glState);
    }
    for (unsigned i = 0; i < 3; ++i) vk_buffer_destroy(&engine->sections[i].buffer);
    if (liveEngine == engine) liveEngine = NULL;
    free(engine);
}
