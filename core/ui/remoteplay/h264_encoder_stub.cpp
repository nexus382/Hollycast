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
// No platform H.264 encoder on this platform yet: WebRTC video is
// unavailable and Remote Play falls back to the MJPEG stream.
#include "h264_encoder.h"

namespace remoteplay {

std::unique_ptr<H264Encoder> H264Encoder::create(int, int, int, int, SampleCallback)
{
	return nullptr;
}

bool h264EncoderAvailable() { return false; }

} // namespace remoteplay
