// Portions Copyright 2026 The Hollycast Authors
#pragma once
#include "types.h"
#include <functional>
#include <memory>
#include <vector>

namespace remoteplay {
// Worker-thread encoder. Accepts the AICA's PCM16 stereo at 44.1 kHz and
// emits one 20 ms Opus packet at a time, with a 48 kHz RTP clock.
class OpusAudioEncoder
{
public:
	using PacketCallback = std::function<void(const std::vector<u8>&)>;
	static std::unique_ptr<OpusAudioEncoder> create(PacketCallback callback);
	virtual ~OpusAudioEncoder() = default;
	virtual void encode(const s16 *stereo, size_t frames) = 0;
};
}
