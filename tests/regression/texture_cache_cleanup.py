# Portions Copyright 2026 The Hollycast Authors
"""Exercise production texture cleanup methods with real unordered_map storage.

The benchmark counts actual dirty-state checks, not GPU time or estimated FPS.
The baseline implementation is read from the supplied immutable Git revision.
"""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output_directory', type=Path)
parser.add_argument('--baseline', metavar='REF', help='also measure cleanup from a Git revision')
args = parser.parse_args()
output = args.output_directory.resolve()
output.mkdir(parents=True, exist_ok=True)


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


cache = (root / 'core/rend/TexCache.h').read_text()
vulkan = (root / 'core/rend/vulkan/texture.cpp').read_text()
vulkan_header = (root / 'core/rend/vulkan/texture.h').read_text()
baseline = (subprocess.check_output(['git', 'show', f'{args.baseline}:core/rend/TexCache.h'], cwd=root, text=True)
            if args.baseline else None)
pieces = [r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
u32 FrameCount = 0;
#define INFO_LOG(...) ((void)0)
struct Dirty {
    u32 value = 0;
    static inline std::uint64_t reads = 0;
    Dirty() = default;
    Dirty(u32 value) : value(value) {}
    operator u32() const { ++reads; return value; }
};
struct Texture {
    u64 id = 0;
    Dirty dirty;
    static inline bool refuse = false;
    static inline size_t deletes = 0;
    static inline std::unordered_map<u64, size_t> attempts;
    bool Delete() {
        ++deletes; ++attempts[id];
        return !refuse;
    }
};
template<typename Texture> struct BaseTextureCache {
''', function(cache, 'void CollectCleanup('),
           function(cache, 'void Clear('),
           'template<typename DeleteTexture>', function(cache, 'void collectCleanup('), r'''
    std::unordered_map<u64, Texture> cache;
    std::optional<u64> nextCleanupKey;
};
struct BaselineCache {
    std::unordered_map<u64, Texture> cache;
''', (function(baseline, 'void CollectCleanup(') if baseline else ''), r'''
};
struct TextureCache : BaseTextureCache<Texture> {
    std::vector<std::unordered_set<Texture*>> inFlightTextures{3};
    void Cleanup();
''', function(vulkan_header, 'bool clearTexture('), '};',
           function(vulkan, 'void TextureCache::Cleanup('), r'''
static void fill(BaseTextureCache<Texture>& cache, size_t size, u32 dirty = 0, u64 first = 0) {
    for (size_t i = 0; i < size; ++i) cache.cache.emplace(first + i, Texture{first + i, dirty});
}
static void resetCounts() {
    Dirty::reads = 0; Texture::deletes = 0; Texture::attempts.clear(); Texture::refuse = false;
}
static void finish(BaseTextureCache<Texture>& cache) {
    // The six-attempt limit bounds deletion throughput, so allow one call per
    // entry plus a complete clean scan to account for cursor position.
    size_t limit = cache.cache.size() + 130;
    while (!cache.cache.empty() && limit-- != 0) cache.CollectCleanup();
    assert(cache.cache.empty());
}
int main() {
    // Preserve strict age comparison and clean/dirty sentinel semantics.
    BaseTextureCache<Texture> age;
    age.cache.emplace(0, Texture{0, 0});
    age.cache.emplace(1, Texture{1, 1});
    age.cache.emplace(2, Texture{2, 2});
    resetCounts(); FrameCount = 120;
    age.CollectCleanup(); assert(Texture::deletes == 0);
    FrameCount = 121;
    age.CollectCleanup(); assert(Texture::deletes == 0);
    FrameCount = 122;
    age.CollectCleanup();
    assert(age.cache.count(0) && !age.cache.count(1) && age.cache.count(2));
    FrameCount = 123;
    age.CollectCleanup(); assert(age.cache.size() == 1 && age.cache.count(0));
    assert(Texture::deletes == 2);
    std::cout << "PASS production cleanup age boundary and clean sentinel\n";

    BaseTextureCache<Texture> mixed;
    fill(mixed, 4096); resetCounts(); FrameCount = 500;
    size_t eligible = 0;
    for (auto& [id, texture] : mixed.cache) {
        texture.dirty = u32(id % 600);
        eligible += id % 600 != 0 && id % 600 < 380;
    }
    for (int i = 0; i < 1024; ++i) mixed.CollectCleanup();
    assert(Texture::deletes == eligible && mixed.cache.size() == 4096 - eligible);
    for (u64 id = 0; id < 4096; ++id)
        assert(bool(mixed.cache.count(id)) == (id % 600 == 0 || id % 600 >= 380));
    std::cout << "PASS mixed clean/recent/stale cache preserves exactly eligible membership\n";

    // Eligible entries retain the original maximum of six attempts per call.
    BaseTextureCache<Texture> stale;
    fill(stale, 128, 1); resetCounts(); FrameCount = 1000;
    stale.CollectCleanup(); assert(Texture::deletes == 6 && stale.cache.size() == 122);
    finish(stale);
    assert(Texture::deletes == 128);
    for (const auto& [id, attempts] : Texture::attempts) assert(attempts == 1);

    // A refused deletion must retain the texture, advance past it, and retry
    // it on a later sweep after the asynchronous owner releases it.
    BaseTextureCache<Texture> refused;
    fill(refused, 20, 1); resetCounts(); Texture::refuse = true;
    for (int i = 0; i < 4; ++i) {
        size_t before = Texture::deletes;
        refused.CollectCleanup(); assert(Texture::deletes - before <= 6);
    }
    assert(refused.cache.size() == 20 && Texture::attempts.size() == 20);
    Texture::refuse = false;
    finish(refused);
    for (const auto& [id, attempts] : Texture::attempts) assert(attempts == 2);
    std::cout << "PASS six-attempt limit and refusal progress/retry\n";

    // Stored keys, rather than iterators, must survive rehash and insertion.
    BaseTextureCache<Texture> rehashed;
    fill(rehashed, 1000); resetCounts();
    rehashed.CollectCleanup(); assert(rehashed.nextCleanupKey);
    auto buckets = rehashed.cache.bucket_count();
    rehashed.cache.rehash(buckets * 4);
    assert(rehashed.cache.bucket_count() > buckets);
    fill(rehashed, 2000, 0, 1000);
    for (auto& [id, texture] : rehashed.cache) texture.dirty = 1;
    finish(rehashed);
    assert(Texture::deletes == 3000);
    for (const auto& [id, attempts] : Texture::attempts) assert(attempts == 1);

    // If another cache operation removes the resume key, fall back to begin.
    BaseTextureCache<Texture> erased;
    fill(erased, 1000); resetCounts(); erased.CollectCleanup();
    assert(erased.nextCleanupKey);
    erased.cache.erase(*erased.nextCleanupKey);
    for (auto& [id, texture] : erased.cache) texture.dirty = 1;
    finish(erased); assert(Texture::deletes == 999);

    BaseTextureCache<Texture> cleared;
    fill(cleared, 1000); resetCounts(); cleared.CollectCleanup();
    assert(cleared.nextCleanupKey);
    cleared.Clear();
    assert(cleared.cache.empty() && !cleared.nextCleanupKey && Texture::deletes == 1000);
    fill(cleared, 10, 1, 4000);
    finish(cleared); assert(Texture::deletes == 1010);
    cleared.CollectCleanup();
    std::cout << "PASS rehash, insertion, erased resume key, Clear and refill\n";

    // Execute Vulkan's actual callback and clearTexture method: all flight
    // sets must discard a deleted texture pointer before map node destruction.
    TextureCache vk;
    fill(vk, 24, 1); resetCounts();
    for (auto& [id, texture] : vk.cache)
        for (auto& set : vk.inFlightTextures) set.insert(&texture);
    for (int i = 0; i < 4; ++i) vk.Cleanup();
    assert(vk.cache.empty() && Texture::deletes == 24);
    for (const auto& set : vk.inFlightTextures) assert(set.empty());
    std::cout << "PASS production Vulkan Cleanup callback clears in-flight references\n";

    // Deterministic work benchmark, holding map size and scene count constant.
    constexpr size_t textureCount = 120000, scenes = 120;
    BaseTextureCache<Texture> incremental;
    fill(incremental, textureCount);
    resetCounts();
#ifdef COMPARE_BASELINE
    BaselineCache old;
    for (size_t i = 0; i < textureCount; ++i) old.cache.emplace(i, Texture{i, 0});
    for (size_t i = 0; i < scenes; ++i) old.CollectCleanup();
    auto baselineReads = Dirty::reads;
    assert(baselineReads == textureCount * scenes);
    std::cout << "Baseline clean-cache dirty checks: " << baselineReads << '\n';
    Dirty::reads = 0;
#endif
    for (size_t i = 0; i < scenes; ++i) incremental.CollectCleanup();
    auto incrementalReads = Dirty::reads;
    assert(incrementalReads <= scenes * (textureCount / 120 + 1));
    assert(incrementalReads >= textureCount); // one complete sweep still occurs
    assert(Texture::deletes == 0 && incremental.cache.size() == textureCount);
    std::cout << "Incremental clean-cache dirty checks: " << incrementalReads << " (" << textureCount
        << " textures, " << scenes << " scenes)\n";
}
''']
source = output / 'texture_cache_cleanup.cpp'
source.write_text('\n'.join(pieces))
binary = output / 'texture_cache_cleanup'
subprocess.run(['clang++', *(['-DCOMPARE_BASELINE'] if baseline else []),
                '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-g', str(source), '-o', str(binary)], check=True)
subprocess.run([str(binary)], check=True)
