// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 The Hollycast Authors
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audio {

// The scratch buffer also owns converted frames left over from a prior call.
// Calls and reset must be serialized with the backend's sample queue.
class SdlPartialOutput {
public:
    void reset() { carryOffset = carryBytes = 0; }
    size_t pendingBytes() const { return carryBytes; }

    // Converter returns a byte count, or -1 on failure. Conversion storage is
    // supplied by the backend and is never overwritten while carry remains.
    template<typename Converter>
    unsigned render(uint32_t* samples, unsigned& queued, uint8_t* output, unsigned requested,
                    uint8_t* scratch, size_t scratchCapacity, unsigned maxInput,
                    double outputPerInput, bool convert, Converter converter)
    {
        constexpr size_t FrameBytes = sizeof(uint32_t);
        size_t written = 0;
        const size_t outputBytes = size_t(requested) * FrameBytes;
        unsigned consumed = 0;
        if (convert && carryBytes) {
            const size_t n = std::min(carryBytes, outputBytes);
            std::memcpy(output, scratch + carryOffset, n);
            carryOffset += n;
            carryBytes -= n;
            written += n;
            if (!carryBytes) carryOffset = 0;
        }
        while (written < outputBytes && consumed < queued) {
            const unsigned remaining = unsigned((outputBytes - written) / FrameBytes);
            if (!convert) {
                const unsigned n = std::min(queued - consumed, remaining);
                std::memcpy(output + written, samples + consumed, size_t(n) * FrameBytes);
                consumed += n;
                written += size_t(n) * FrameBytes;
                break;
            }
            if (!scratch || !maxInput || outputPerInput <= 0) break;
            const unsigned needed = unsigned(std::ceil(remaining / outputPerInput));
            const unsigned n = std::min({queued - consumed, maxInput, std::max(1u, needed)});
            const int bytes = converter(samples + consumed, n, scratch, scratchCapacity);
            if (bytes < 0 || size_t(bytes) > scratchCapacity || bytes % FrameBytes) break;
            consumed += n;
            const size_t take = std::min(size_t(bytes), outputBytes - written);
            std::memcpy(output + written, scratch, take);
            written += take;
            carryOffset = take;
            carryBytes = size_t(bytes) - take;
        }
        if (consumed) {
            queued -= consumed;
            std::memmove(samples, samples + consumed, size_t(queued) * FrameBytes);
        }
        std::memset(output + written, 0, outputBytes - written);
        return unsigned((outputBytes - written) / FrameBytes);
    }

private:
    size_t carryOffset = 0;
    size_t carryBytes = 0;
};

} // namespace audio
