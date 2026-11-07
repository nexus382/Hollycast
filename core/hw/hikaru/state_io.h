// SPDX-License-Identifier: GPL-3.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace hikaru {
// A legacy layout is accepted only with its legacy encoding. Checking the
// pair prevents a valid ABI tag from authorizing a different payload size.
inline void validateStateEncoding(uint32_t version, uint64_t layout, uint64_t legacyLayout, uint64_t currentLayout) {
    if (!((version == 1 && legacyLayout && layout == legacyLayout) ||
          (version == 2 && currentLayout && layout == currentLayout)))
        throw std::runtime_error("Incompatible Hikaru state format or platform");
}
// Private, versioned Hikaru payload. Opaque CPU structures are accepted only
// after the outer header has checked the producer's ABI and machine layout.
class StateWriter {
public:
    static constexpr size_t MaxSize = 256 * 1024 * 1024;
    explicit StateWriter(bool measureOnly = false) : measureOnly_(measureOnly) {}
    void reserve(size_t capacity) {
        if (capacity > MaxSize) throw std::runtime_error("Hikaru state is too large");
        if (!measureOnly_) data_.reserve(capacity);
    }
    uint8_t* append(size_t count) {
        if (count > MaxSize - size_) throw std::runtime_error("Hikaru state is too large");
        const size_t offset = size_;
        size_ += count;
        if (measureOnly_) return nullptr;
        data_.resize(size_);
        return data_.data() + offset;
    }
    void bytes(const void* source, size_t count) {
        if (count > MaxSize - size_) throw std::runtime_error("Hikaru state is too large");
        if (!measureOnly_ && count) {
            const auto* begin = static_cast<const uint8_t*>(source);
            data_.insert(data_.end(), begin, begin + count);
        }
        size_ += count;
    }
    template<class T> void value(const T& item) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            const uint8_t encoded = item ? 1 : 0;
            bytes(&encoded, sizeof(encoded));
        } else bytes(&item, sizeof(item));
    }
    template<class T, size_t N> void value(const std::array<T, N>& items) {
        for (const auto& item : items) value(item);
    }
    template<class T, size_t N> void value(const T (&items)[N]) {
        for (const auto& item : items) value(item);
    }
    template<class T> void pod(const T& item) {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        bytes(&item, sizeof(item));
    }
    size_t size() const { return size_; }
    bool measuring() const { return measureOnly_; }
    const std::vector<uint8_t>& data() const { return data_; }
private:
    bool measureOnly_;
    size_t size_ = 0;
    std::vector<uint8_t> data_;
};

class StateReader {
public:
    StateReader(const uint8_t* data, size_t size) : data_(data), remaining_(size) {
        require(size <= StateWriter::MaxSize && (data || !size), "Invalid Hikaru state size");
    }
    explicit StateReader(const std::vector<uint8_t>& data) : StateReader(data.data(), data.size()) {}
    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }
    const uint8_t* view(size_t count) {
        require(count <= remaining_, "Truncated Hikaru state");
        const uint8_t* result = data_;
        if (count) data_ += count;
        remaining_ -= count;
        return result;
    }
    void bytes(void* destination, size_t count) {
        require(count <= remaining_, "Truncated Hikaru state");
        if (count) { std::memcpy(destination, data_, count); data_ += count; }
        remaining_ -= count;
    }
    template<class T> void value(T& item) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t encoded;
            bytes(&encoded, sizeof(encoded));
            require(encoded <= 1, "Invalid Hikaru state flag");
            item = encoded != 0;
        } else bytes(&item, sizeof(item));
    }
    template<class T, size_t N> void value(std::array<T, N>& items) {
        for (auto& item : items) value(item);
    }
    template<class T, size_t N> void value(T (&items)[N]) {
        for (auto& item : items) value(item);
    }
    template<class T> void pod(T& item) {
        static_assert(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T>);
        bytes(&item, sizeof(item));
    }
    size_t remaining() const { return remaining_; }
    void finish() const { require(!remaining_, "Unexpected Hikaru state data"); }
private:
    const uint8_t* data_;
    size_t remaining_;
};
} // namespace hikaru