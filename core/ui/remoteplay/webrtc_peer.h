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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace remoteplay {

// WebRTC host peer for one phone (signaling rides the input WebSocket).
// Available only in builds with HOLLYCAST_HAVE_WEBRTC and a platform
// H.264 encoder; otherwise every call is a safe no-op and the phone
// falls back to the MJPEG stream.
bool webrtcAvailable();

using RtcSendCallback = std::function<void(const std::string& json)>;
using RtcInputCallback = std::function<void(const std::string& json)>;

void rtcSessionStart(int player, RtcSendCallback send, RtcInputCallback input = {});
void rtcSessionStop(int player);
void rtcSessionHandleMessage(int player, const std::string& json);
bool rtcSessionConnected(int player);
bool rtcAnySession();
bool rtcAnyConnected();
void rtcPushSample(const std::vector<u8>& sample, bool keyframe, uint32_t rtpTimestamp);
void rtcPushAudioSample(const std::vector<u8>& packet, uint32_t rtpTimestamp);
bool rtcKeyframeRequested();
void rtcClearKeyframeRequest();

} // namespace remoteplay
