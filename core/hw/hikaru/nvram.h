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
// Keep the first pre-change snapshot. Exclusive creation also protects it
// from another instance; an existing unreadable/truncated backup is an error.
template<size_t Size>
bool backupNvram(const std::string& path, const std::array<uint8_t, Size>& data) {
    std::array<uint8_t, Size> existing{};
    const auto previous = loadNvram(path, existing);
    if (previous != NvramLoadResult::Missing) return previous == NvramLoadResult::Loaded;
    FILE* file = nowide::fopen(path.c_str(), "wbx");
    if (!file) return errno == EEXIST && loadNvram(path, existing) == NvramLoadResult::Loaded;
    const bool written = std::fwrite(data.data(), 1, Size, file) == Size;
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed) nowide::remove(path.c_str());
    return written && closed;
}
} // namespace hikaru
