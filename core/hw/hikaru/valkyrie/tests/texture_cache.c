// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
/* Exercise real cache uploads and invalidations through a no-context dispatch.
 * Texture IDs track ownership independently of the cache occupancy index. */
#include "mach/hikaru/hikaru-renderer-private.h"
#include "mach/hikaru/hikaru-renderer.h"
#include "graphics_dispatch.h"

hikaru_texture_t *get_texture(hikaru_renderer_t *, hikaru_texhead_t *);
uint64_t hikaru_vendor_test_texture_cache_visits(void);

static unsigned checks, failures, generated, deleted, live;
static bool texture_ids[256];
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #condition); ++failures; } } while (0)

static void gen_textures(GLsizei count, GLuint *ids)
{
    for (GLsizei i = 0; i < count; ++i) {
        if (generated + 1 >= NUMELEM(texture_ids)) abort();
        ids[i] = ++generated;
        texture_ids[ids[i]] = true;
        ++live;
    }
}

static void delete_textures(GLsizei count, const GLuint *ids)
{
    for (GLsizei i = 0; i < count; ++i) {
        if (!ids[i]) continue;
        CHECK(ids[i] <= generated && texture_ids[ids[i]]);
        if (ids[i] <= generated && texture_ids[ids[i]]) {
            texture_ids[ids[i]] = false;
            --live;
            ++deleted;
        }
    }
}

static GLenum get_error(void) { return GL_NO_ERROR; }
static const GLubyte *get_string(GLenum name)
{ (void)name; return (const GLubyte *)"3.2 cache fixture"; }
static void active_texture(GLenum texture) { (void)texture; }
static void bind_texture(GLenum target, GLuint texture)
{ (void)target; (void)texture; }
static void tex_parameter(GLenum target, GLenum name, GLint value)
{ (void)target; (void)name; (void)value; }
static void pixel_store(GLenum name, GLint value) { (void)name; (void)value; }
static void tex_image(GLenum target, GLint level, GLint internal_format,
                      GLsizei width, GLsizei height, GLint border,
                      GLenum format, GLenum type, const void *data)
{
    (void)target; (void)level; (void)internal_format; (void)width; (void)height;
    (void)border; (void)format; (void)type; (void)data;
}

static hikaru_texhead_t head(unsigned bank, unsigned x, unsigned y)
{
    hikaru_texhead_t result = {0};
    result.bank = bank;
    result.slotx = 0x80 + x;
    result.sloty = 0xc0 + y;
    return result;
}

static void check_cache(const hikaru_renderer_t *renderer, unsigned expected)
{
    unsigned count = 0;
    for (unsigned bank = 0; bank < 2; ++bank) {
        unsigned in_bank = 0;
        for (unsigned y = 0; y < 0x40; ++y)
            for (unsigned x = 0; x < 0x80; ++x) {
                const bool allocated = renderer->textures.cache[bank][y][x].id != 0;
                const bool indexed = (renderer->textures.occupied[bank][y][x >> 5] &
                                      (UINT32_C(1) << (x & 31))) != 0;
                CHECK(allocated == indexed);
                in_bank += allocated;
            }
        CHECK(!renderer->textures.is_clear[bank] || !in_bank);
        count += in_bank;
    }
    CHECK(count == expected && live == expected);
}

static void sparse_boundaries(hikaru_renderer_t *renderer)
{
    static const unsigned columns[] = {0, 31, 32, 63, 64, 95, 96, 127};
    for (unsigned bank = 0; bank < 2; ++bank)
        for (unsigned i = 0; i < NUMELEM(columns); ++i) {
            hikaru_texhead_t wanted = head(bank, columns[i], bank ? 63 : 0);
            hikaru_texture_t *texture = get_texture(renderer, &wanted);
            CHECK(texture && texture->id);
        }
    check_cache(renderer, 16);
    const unsigned before = generated;
    hikaru_texhead_t first = head(0, 0, 0), last = head(1, 127, 63);
    CHECK(get_texture(renderer, &first) && get_texture(renderer, &last));
    CHECK(generated == before);

    /* This row and all its mip rows are disjoint from the two cached rows. */
    hikaru_texhead_t disjoint = head(0, 0, 16);
    const uint64_t visits = hikaru_vendor_test_texture_cache_visits();
    hikaru_renderer_invalidate_texcache(&renderer->base, &disjoint);
    CHECK(hikaru_vendor_test_texture_cache_visits() - visits == 16);
    check_cache(renderer, 16);
    const unsigned prior_deletes = deleted;
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    CHECK(deleted - prior_deletes == 16);
    check_cache(renderer, 0);
    hikaru_renderer_invalidate_texcache(&renderer->base, &disjoint);
    CHECK(hikaru_vendor_test_texture_cache_visits() - visits == 16);
}

static void overlap_cases(hikaru_renderer_t *renderer)
{
    for (unsigned format = 0; format < 2; ++format)
        for (unsigned mip = 0; mip < 2; ++mip) {
            hikaru_texhead_t wanted = head(0, 0, 0);
            wanted.format = format ? HIKARU_FORMAT_ABGR1111 : HIKARU_FORMAT_ABGR1555;
            wanted.logw = format ? 1 : 0;
            hikaru_texhead_t transfer = mip ? head(1, 64, 32) : head(0, format ? 1 : 0, 0);
            CHECK(get_texture(renderer, &wanted) != NULL);
            check_cache(renderer, 1);
            const uint64_t visits = hikaru_vendor_test_texture_cache_visits();
            const unsigned prior_deletes = deleted;
            hikaru_renderer_invalidate_texcache(&renderer->base, &transfer);
            CHECK(hikaru_vendor_test_texture_cache_visits() - visits == 1);
            CHECK(deleted - prior_deletes == 1);
            check_cache(renderer, 0);
            CHECK(renderer->textures.is_clear[0] && renderer->textures.is_clear[1]);
            hikaru_renderer_invalidate_texcache(&renderer->base, &transfer);
            CHECK(hikaru_vendor_test_texture_cache_visits() - visits == 1);
        }
}

static void replacement_and_reuse(hikaru_renderer_t *renderer)
{
    hikaru_texhead_t wanted = head(0, 32, 7);
    CHECK(get_texture(renderer, &wanted) != NULL);
    const unsigned prior_deletes = deleted;
    const uint64_t prior_uploads = renderer->textures.uploads;
    wanted.format = HIKARU_FORMAT_ABGR4444;
    CHECK(get_texture(renderer, &wanted) != NULL);
    CHECK(deleted - prior_deletes == 1);
    CHECK(renderer->textures.uploads - prior_uploads == 1);
    check_cache(renderer, 1);

    /* The replacement destroys its predecessor before rejecting this format. */
    wanted.format = 7;
    CHECK(get_texture(renderer, &wanted) == NULL);
    CHECK(deleted - prior_deletes == 3);
    CHECK(renderer->textures.uploads - prior_uploads == 1);
    check_cache(renderer, 0);
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    CHECK(deleted - prior_deletes == 3);

    wanted.format = HIKARU_FORMAT_ABGR1555;
    CHECK(get_texture(renderer, &wanted) != NULL);
    /* Also reject a partially uploaded replacement whose next mip has no RAM. */
    vk_buffer_t *other_bank = renderer->gpu->texram[1];
    renderer->gpu->texram[1] = NULL;
    wanted.format = HIKARU_FORMAT_ABGR4444;
    CHECK(get_texture(renderer, &wanted) == NULL);
    renderer->gpu->texram[1] = other_bank;
    check_cache(renderer, 0);
    CHECK(get_texture(renderer, &wanted) != NULL);
    check_cache(renderer, 1);

    hikaru_texhead_t other = head(1, 127, 63), invalid = {0};
    CHECK(get_texture(renderer, &other) != NULL);
    hikaru_renderer_note_cpu_texram_write(&renderer->base);
    CHECK(renderer->textures.cpu_dirty);
    CHECK(get_texture(renderer, &invalid) == NULL);
    CHECK(!renderer->textures.cpu_dirty);
    check_cache(renderer, 0);
    CHECK(get_texture(renderer, &other) != NULL);
    check_cache(renderer, 1);
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    check_cache(renderer, 0);
}

static void capture_queue(hikaru_renderer_t *renderer)
{
    hikaru_renderer_t *capture = calloc(1, sizeof(*capture));
    CHECK(capture != NULL);
    if (!capture) return;
    capture->capture_only = true;
    hikaru_texhead_t wanted = head(1, 0, 0);
    CHECK(get_texture(renderer, &wanted) != NULL);
    const unsigned prior_deletes = deleted;
    const uint64_t visits = hikaru_vendor_test_texture_cache_visits();
    hikaru_renderer_invalidate_texcache(&capture->base, &wanted);
    CHECK(capture->num_invalidations == 1 && !capture->invalidate_all);
    CHECK(deleted == prior_deletes && hikaru_vendor_test_texture_cache_visits() == visits);
    check_cache(renderer, 1);
    /* This is the consumer's production replay operation, after capture ends. */
    hikaru_renderer_invalidate_texcache(&renderer->base, &capture->invalidations[0]);
    check_cache(renderer, 0);
    for (unsigned i = 0; i < NUMELEM(capture->invalidations); ++i)
        hikaru_renderer_invalidate_texcache(&capture->base, &wanted);
    CHECK(capture->invalidate_all && !capture->num_invalidations);
    CHECK(deleted - prior_deletes == 1);
    check_cache(capture, 0);
    free(capture);
}

int main(void)
{
    static const HikaruGraphicsDispatch dispatch = {
        .ActiveTexture = active_texture, .BindTexture = bind_texture,
        .DeleteTextures = delete_textures, .GenTextures = gen_textures,
        .GetError = get_error, .GetString = get_string,
        .PixelStorei = pixel_store, .TexImage2D = tex_image, .TexParameteri = tex_parameter,
    };
    hikaru_renderer_t *renderer = calloc(1, sizeof(*renderer));
    hikaru_gpu_t *gpu = calloc(1, sizeof(*gpu));
    if (!renderer || !gpu) { free(renderer); free(gpu); return 1; }
    renderer->gpu = gpu;
    for (unsigned bank = 0; bank < 2; ++bank)
        gpu->texram[bank] = vk_buffer_le32_new(4u * 1024 * 1024, 0);
    if (!gpu->texram[0] || !gpu->texram[1]) { ++failures; goto cleanup; }
    const HikaruGraphicsDispatch *previous = hikaru_graphics_set_dispatch(&dispatch);
    sparse_boundaries(renderer);
    overlap_cases(renderer);
    replacement_and_reuse(renderer);
    capture_queue(renderer);
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    CHECK(!live && generated == deleted);
    hikaru_graphics_set_dispatch(previous);
cleanup:
    for (unsigned bank = 0; bank < 2; ++bank) vk_buffer_destroy(&gpu->texram[bank]);
    free(gpu);
    free(renderer);
    printf("Hikaru texture cache: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
