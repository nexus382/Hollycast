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
#include <string>

namespace remoteplay {

enum class TunnelState { NotStarted, Starting, Running, Failed, Stopped };

struct TunnelStatus
{
	TunnelState state = TunnelState::NotStarted;
	std::string publicUrl;	// https://*.trycloudflare.com once Running
	std::string error;
	std::string binaryPath;	// resolved cloudflared, empty if not found
};

// Locate cloudflared: config path, our installed copy, PATH, install dirs.
std::string findCloudflared();
// Where the one-click install puts cloudflared (Hollycast's data folder).
std::string installedCloudflaredPath();
// Official release download URL for this platform/architecture.
std::string cloudflaredDownloadUrl();

enum class InstallState { NotStarted, Downloading, Extracting, Done, Failed };

struct InstallStatus
{
	InstallState state = InstallState::NotStarted;
	std::string message;
	std::string path;	// installed binary once Done
};

// Download cloudflared into Hollycast's data folder on a worker thread.
// No admin rights needed. Poll installStatus() for progress.
void installCloudflared();
InstallStatus installStatus();
// Extract an https://*.trycloudflare.com URL from cloudflared output, or "".
std::string parseQuickTunnelUrl(const std::string& text);

bool tunnelStart(int localPort, std::string& error);
void tunnelStop();
TunnelStatus tunnelStatus();

} // namespace remoteplay
