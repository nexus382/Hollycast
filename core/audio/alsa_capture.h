// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace audio {
namespace alsa {

template <typename FrameCount, typename Recover>
uint32_t finishMonoCapture(void *buffer, uint32_t requestedFrames, FrameCount readFrames,
		FrameCount noDataResult, Recover recover)
{
	if (readFrames < 0 && readFrames != noDataResult)
		recover(readFrames);

	if (requestedFrames == 0)
		return 0;

	const uint64_t framesRead = readFrames > 0 ? static_cast<uint64_t>(readFrames) : 0;
	const uint32_t boundedFrames = static_cast<uint32_t>(
			std::min<uint64_t>(framesRead, requestedFrames));
	const size_t capturedBytes = static_cast<size_t>(boundedFrames) * sizeof(int16_t);
	const size_t requestedBytes = static_cast<size_t>(requestedFrames) * sizeof(int16_t);
	uint8_t *bytes = static_cast<uint8_t *>(buffer);
	std::memset(bytes + capturedBytes, 0, requestedBytes - capturedBytes);
	return boundedFrames;
}

} // namespace alsa
} // namespace audio
