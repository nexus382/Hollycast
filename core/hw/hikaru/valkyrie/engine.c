// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#include "engine_internal.h"
#include "mach/hikaru/hikaru.h"
#include "vk/input.h"

unsigned vk_verbosity = 0;
static _Thread_local HikaruEngine *activeEngine;
static HikaruEngine *liveEngine;

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
    GLdouble clearDepth, depthRange[2];
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
    glGetDoublev(GL_DEPTH_CLEAR_VALUE, &s->clearDepth); glGetDoublev(GL_DEPTH_RANGE, s->depthRange);
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
    glClearDepth(1.0);
    glDepthRange(0.0, 1.0);
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
    glClearDepth(s->clearDepth); glDepthRange(s->depthRange[0], s->depthRange[1]);
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
                       const uint8_t *bytes, size_t size)
{
    if (!bytes || size > UINT32_MAX) return -1;
    vk_buffer_t *buffer = vk_buffer_le32_new((unsigned)size, 0);
    if (!buffer) return -1;
    memcpy(buffer->ptr, bytes, size);
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
    default:
        set_error(error, error_size, "Unsupported Hikaru game"); return NULL;
    }
    if (!roms || roms->boot_size != 0x200000 || roms->program_size != program_size || roms->data_size != data_size ||
        !roms->boot || !roms->program || !roms->data || !mie || !mie->reset || !mie->read || !mie->write || !mie->advance || !mie->set_inputs ||
        !protection || !protection->reset || !protection->read || !protection->write || !width || !height) {
        set_error(error, error_size, "Invalid ROM sections, device callbacks, or output dimensions"); return NULL;
    }
    if (!glGetString(GL_VERSION)) { set_error(error, error_size, "A current desktop GL context is required"); return NULL; }
    HikaruEngine *engine = calloc(1, sizeof(*engine));
    if (!engine) { set_error(error, error_size, "Out of memory"); return NULL; }
    liveEngine = engine;
    engine->mie = *mie;
    engine->protection = *protection;
    strcpy(engine->game.name, game_name);
    strcpy(engine->game.mach, "hikaru");
    engine->game.sections = engine->sections;
    engine->game.nsections = 3;
    if (add_section(engine, 0, "bootrom", roms->boot, roms->boot_size) ||
        add_section(engine, 1, "eprom", roms->program, roms->program_size) ||
        add_section(engine, 2, "maskrom", roms->data, roms->data_size)) {
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

int hikaru_engine_run_frame(HikaruEngine *engine, const HikaruEngineInputs *inputs)
{
    if (!engine || !engine->machine || !inputs) return -1;
    engine->frame_started = true;
    engine->error[0] = '\0';
    engine->mie.set_inputs(engine->mie.opaque, inputs);
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
    activeEngine = engine;
    if (setjmp(engine->fatal)) {
        activeEngine = NULL;
        restore_gl_state(&glState);
        return -1;
    }
    vk_renderer_begin_frame(engine->machine->renderer);
    int status = vk_machine_run_frame(engine->machine);
    if (!status && !engine->error[0]) vk_renderer_end_frame(engine->machine->renderer);
    restore_gl_state(&glState);
    activeEngine = NULL;
    if (status || engine->error[0]) return -1;
    return 0;
}
void hikaru_engine_resize(HikaruEngine *engine, uint32_t target_fbo, unsigned width, unsigned height)
{
    if (!engine || !engine->machine || !width || !height) return;
    engine->machine->renderer->target_fbo = target_fbo;
    engine->machine->renderer->width = width;
    engine->machine->renderer->height = height;
}
int hikaru_engine_set_internal_resolution(HikaruEngine *engine, unsigned height)
{
    if (!engine || !engine->machine) return -1;
    if (height < 240 || height > 4320) {
        snprintf(engine->error, sizeof(engine->error), "Hikaru internal resolution must be between 240 and 4320 pixels high");
        return -1;
    }
    const unsigned width = (unsigned)((((uint64_t)height * 4 + 3) / 6) * 2);
    GLint max_texture = 0, max_renderbuffer = 0, max_viewport[2] = {0, 0};
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture);
    glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_renderbuffer);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, max_viewport);
    if (width > (unsigned)max_texture || height > (unsigned)max_texture ||
        width > (unsigned)max_renderbuffer || height > (unsigned)max_renderbuffer ||
        width > (unsigned)max_viewport[0] || height > (unsigned)max_viewport[1]) {
        snprintf(engine->error, sizeof(engine->error), "Hikaru internal resolution exceeds OpenGL limits");
        return -1;
    }
    engine->machine->renderer->internal_width = width;
    engine->machine->renderer->internal_height = height;
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
        save_gl_state(&glState);
        activeEngine = engine;
        if (!setjmp(engine->fatal)) vk_machine_destroy(&engine->machine);
        activeEngine = NULL;
        restore_gl_state(&glState);
    }
    for (unsigned i = 0; i < 3; ++i) vk_buffer_destroy(&engine->sections[i].buffer);
    if (liveEngine == engine) liveEngine = NULL;
    free(engine);
}
