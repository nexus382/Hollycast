// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>
#include <nowide/cstdio.hpp>

namespace hikaru {
enum class NvramLoadResult { Missing, Loaded, Invalid };

template<size_t Size>
NvramLoadResult loadNvram(const std::string& path, std::array<uint8_t, Size>& data) {
    errno = 0;
    FILE* file = nowide::fopen(path.c_str(), "rb");
    if (!file) return errno == ENOENT ? NvramLoadResult::Missing : NvramLoadResult::Invalid;
    // Commit only a complete, exact-sized read. Callers retain defaults on error.
    std::array<uint8_t, Size> loaded{};
    const bool valid = std::fread(loaded.data(), 1, Size, file) == Size
        && std::fgetc(file) == EOF && !std::ferror(file);
    const bool closed = std::fclose(file) == 0;
    if (!valid || !closed) return NvramLoadResult::Invalid;
    data = loaded;
    return NvramLoadResult::Loaded;
}
} // namespace hikaru
