// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "oslib/directory.h"
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
template<size_t Size>
bool saveNvram(const std::string& path, const std::array<uint8_t, Size>& data) {
    // Several emulator instances can share a save folder. Reserve our own
    // sibling file before writing so another instance cannot truncate it.
    FILE* file;
    std::string temporary;
    for (size_t attempt = 0; ; ++attempt) {
        temporary = path + ".tmp." + std::to_string(attempt);
        file = nowide::fopen(temporary.c_str(), "wbx");
        if (file) break;
        if (errno != EEXIST) return false;
    }
    const bool written = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    const bool closed = std::fclose(file) == 0;
    // flycast::rename replaces an existing save and accepts UTF-8 on Windows.
    if (!written || !closed || flycast::rename(temporary.c_str(), path.c_str()) != 0) {
        nowide::remove(temporary.c_str());
        return false;
    }
    return true;
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
