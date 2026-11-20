/*
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#pragma once
#include "types.h"

#include <functional>
#include <memory>
#include <vector>

namespace remoteplay {

// Low-latency H.264 encoder for WebRTC Remote Play.
// Samples are AVCC-style (4-byte length-prefixed NAL units); keyframe
// samples are preceded by SPS and PPS NAL units in the same format.
class H264Encoder
{
public:
	using SampleCallback = std::function<void(const std::vector<u8>& sample, bool keyframe)>;

	static std::unique_ptr<H264Encoder> create(int width, int height, int fps, int bitrateKbps,
			SampleCallback callback);
	virtual ~H264Encoder() = default;

	// Encode one RGB24 frame (width*height*3 bytes).
	virtual void encodeRgb(const u8 *rgb, int width, int height) = 0;
	virtual void requestKeyframe() = 0;
};

bool h264EncoderAvailable();

} // namespace remoteplay
