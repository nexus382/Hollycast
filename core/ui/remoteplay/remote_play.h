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
#include <array>
#include <string>

namespace remoteplay {

enum class PlayerState { Available, Joining, Connected };

struct ServerStatus
{
	bool running = false;
	int port = 0;
	std::string pin;		// current session PIN (host display only)
	std::string localUrl;	// http://127.0.0.1:<port>
	std::string error;
	std::array<PlayerState, 4> players = {};
	int streamViewers = 0;
};

// Start the HTTP/WebSocket server on config::RemotePlayPort.
// Generates a fresh 6-digit PIN unless config::RemotePlayPin is set.
bool serverStart(std::string& error);
void serverStop();
bool serverIsRunning();
ServerStatus serverStatus();

// Starts the server (and tunnel, if enabled) when RemotePlayAutoStart is set.
void autoStartIfEnabled();

// Called on the render thread after presentation, outside renderer/OSD
// callbacks. Services at most one pending video capture.
void capturePresentedFrame();

} // namespace remoteplay
