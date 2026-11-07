// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
/* Exercise real cache uploads and invalidations through a no-context dispatch.
 * Texture IDs track ownership independently of the cache occupancy index. */
#include "mach/hikaru/hikaru-renderer-private.h"
#include "mach/hikaru/hikaru-renderer.h"
#include "graphics_dispatch.h"

hikaru_texture_t *get_texture(hikaru_renderer_t *, hikaru_texhead_t *);
uint64_t hikaru_vendor_test_texture_cache_visits(void);

static unsigned checks, failures, generated, deleted, live, uploaded_levels;
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
    ++uploaded_levels;
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
                unsigned layouts = 0;
                for (unsigned way = 0; way < NUMELEM(renderer->textures.cache[bank][y][x]); ++way)
                    layouts += renderer->textures.cache[bank][y][x][way].id != 0;
                const bool indexed = (renderer->textures.occupied[bank][y][x >> 5] &
                                      (UINT32_C(1) << (x & 31))) != 0;
                CHECK((layouts != 0) == indexed);
                CHECK(renderer->textures.most_recent[bank][y][x] <
                      NUMELEM(renderer->textures.cache[bank][y][x]));
                in_bank += layouts;
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

static void alternating_layouts(hikaru_renderer_t *renderer)
{
    /* Planet Harriers uses the same RAM origin for a reflection and ordinary
     * geometry with different dimensions or formats. Neither view changes RAM. */
    for (unsigned format_alias = 0; format_alias < 2; ++format_alias) {
        hikaru_texhead_t small = head(1, 32, 0), large = small;
        small.logw = 2;
        small.logh = 3;
        large.logw = format_alias ? small.logw : 4;
        large.logh = format_alias ? small.logh : 4;
        large.format = format_alias ? HIKARU_FORMAT_ABGR4444 : small.format;
        const unsigned before = generated, prior_deletes = deleted;
        hikaru_texture_t *texture = get_texture(renderer, &small);
        CHECK(texture != NULL);
        const GLuint small_id = texture ? texture->id : 0;
        texture = get_texture(renderer, &large);
        CHECK(texture != NULL);
        const GLuint large_id = texture ? texture->id : 0;
        const unsigned initial_levels = uploaded_levels;
        for (unsigned i = 0; i < 8; ++i) {
            texture = get_texture(renderer, &small);
            CHECK(texture && texture->id == small_id);
            texture = get_texture(renderer, &large);
            CHECK(texture && texture->id == large_id);
        }
        CHECK(generated - before == 2 && deleted == prior_deletes);
        CHECK(uploaded_levels == initial_levels);
        check_cache(renderer, 2);
        hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
        CHECK(deleted - prior_deletes == 2);
        check_cache(renderer, 0);
    }
}

static void bounded_replacement(hikaru_renderer_t *renderer)
{
    hikaru_texhead_t first = head(0, 32, 7), second = first, third = first;
    second.format = HIKARU_FORMAT_ABGR4444;
    third.format = HIKARU_FORMAT_LA8;
    hikaru_texture_t *texture = get_texture(renderer, &first);
    CHECK(texture != NULL);
    const GLuint first_id = texture ? texture->id : 0;
    CHECK(get_texture(renderer, &second) != NULL);
    CHECK(get_texture(renderer, &first) != NULL); /* Keep the first layout hot. */
    const unsigned prior_deletes = deleted;
    const uint64_t prior_uploads = renderer->textures.uploads;
    CHECK(get_texture(renderer, &third) != NULL);
    CHECK(deleted - prior_deletes == 1);
    CHECK(renderer->textures.uploads - prior_uploads == 1);
    texture = get_texture(renderer, &first);
    CHECK(texture && texture->id == first_id);
    CHECK(renderer->textures.uploads - prior_uploads == 1);
    check_cache(renderer, 2);
    CHECK(get_texture(renderer, &second) != NULL);
    CHECK(deleted - prior_deletes == 2);
    check_cache(renderer, 2);
    hikaru_renderer_invalidate_texcache(&renderer->base, NULL);
    check_cache(renderer, 0);
}

static void failed_upload_and_cpu_dirty(hikaru_renderer_t *renderer)
{
    hikaru_texhead_t wanted = head(0, 32, 7), original = wanted;
    hikaru_texture_t *texture = get_texture(renderer, &wanted);
    CHECK(texture != NULL);
    const GLuint original_id = texture ? texture->id : 0;
    const unsigned prior_deletes = deleted;
    const uint64_t prior_uploads = renderer->textures.uploads;
    wanted.format = 7;
    CHECK(get_texture(renderer, &wanted) == NULL);
    CHECK(deleted - prior_deletes == 1);
    CHECK(renderer->textures.uploads == prior_uploads);
    check_cache(renderer, 1);

    /* A failed format or partial mip upload leaves the other layout intact. */
    vk_buffer_t *other_bank = renderer->gpu->texram[1];
    renderer->gpu->texram[1] = NULL;
    wanted.format = HIKARU_FORMAT_ABGR4444;
    CHECK(get_texture(renderer, &wanted) == NULL);
    renderer->gpu->texram[1] = other_bank;
    CHECK(deleted - prior_deletes == 2);
    texture = get_texture(renderer, &original);
    CHECK(texture && texture->id == original_id);
    CHECK(renderer->textures.uploads == prior_uploads);
    check_cache(renderer, 1);
    CHECK(get_texture(renderer, &wanted) != NULL);
    check_cache(renderer, 2);

    /* Failure while replacing one of two live layouts also keeps the other. */
    wanted.format = 7;
    CHECK(get_texture(renderer, &original) != NULL);
    CHECK(get_texture(renderer, &wanted) == NULL);
    CHECK(deleted - prior_deletes == 4);
    texture = get_texture(renderer, &original);
    CHECK(texture && texture->id == original_id);
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

static void retained_layout_invalidation(hikaru_renderer_t *renderer)
{
    hikaru_texhead_t small = head(0, 0, 0), large = small;
    large.logw = large.logh = 2;
    hikaru_texture_t *texture = get_texture(renderer, &small);
    CHECK(texture != NULL);
    const GLuint small_id = texture ? texture->id : 0;
    CHECK(get_texture(renderer, &large) != NULL);
    check_cache(renderer, 2);
    const unsigned prior_deletes = deleted;
    const uint64_t visits = hikaru_vendor_test_texture_cache_visits();
    /* Inside the larger base level, outside every mip of the smaller view. */
    hikaru_texhead_t transfer = head(0, 2, 2);
    hikaru_renderer_invalidate_texcache(&renderer->base, &transfer);
    CHECK(hikaru_vendor_test_texture_cache_visits() - visits == 2);
    CHECK(deleted - prior_deletes == 1);
    check_cache(renderer, 1);
    texture = get_texture(renderer, &small);
    CHECK(texture && texture->id == small_id);
    CHECK(get_texture(renderer, &large) != NULL);
    /* Both layouts sample this mip in the opposite bank. */
    transfer = head(1, 64, 32);
    hikaru_renderer_invalidate_texcache(&renderer->base, &transfer);
    CHECK(deleted - prior_deletes == 3);
    check_cache(renderer, 0);
    CHECK(renderer->textures.is_clear[0] && renderer->textures.is_clear[1]);
}

static void capture_queue(hikaru_renderer_t *renderer)
{
    hikaru_renderer_t *capture = calloc(1, sizeof(*capture));
    CHECK(capture != NULL);
    if (!capture) return;
    capture->capture_only = true;
    hikaru_texhead_t wanted = head(1, 0, 0);
    CHECK(get_texture(renderer, &wanted) != NULL);
    hikaru_texhead_t alternate = wanted;
    alternate.format = HIKARU_FORMAT_ABGR4444;
    CHECK(get_texture(renderer, &alternate) != NULL);
    const unsigned prior_deletes = deleted;
    const uint64_t visits = hikaru_vendor_test_texture_cache_visits();
    hikaru_renderer_invalidate_texcache(&capture->base, &wanted);
    CHECK(capture->num_invalidations == 1 && !capture->invalidate_all);
    CHECK(deleted == prior_deletes && hikaru_vendor_test_texture_cache_visits() == visits);
    check_cache(renderer, 2);
    /* This is the consumer's production replay operation, after capture ends. */
    hikaru_renderer_invalidate_texcache(&renderer->base, &capture->invalidations[0]);
    check_cache(renderer, 0);
    for (unsigned i = 0; i < NUMELEM(capture->invalidations); ++i)
        hikaru_renderer_invalidate_texcache(&capture->base, &wanted);
    CHECK(capture->invalidate_all && !capture->num_invalidations);
    CHECK(deleted - prior_deletes == 2);
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
    alternating_layouts(renderer);
    bounded_replacement(renderer);
    failed_upload_and_cpu_dirty(renderer);
    retained_layout_invalidation(renderer);
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
