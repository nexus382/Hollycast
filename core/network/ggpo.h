/*
	Copyright 2021 flyinghead
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
#include <future>
#include <string>
#include <vector>

struct MapleInputState;

namespace ggpo
{

inline constexpr int SERVER_PORT = 19713;

std::vector<std::string> localIPv4Addresses();
std::string routerPublicIPv4Address();
std::future<bool> startNetwork();
void startSession(int localPort, int localPlayerNum);
void stopSession();
void getInput(MapleInputState inputState[4]);
bool nextFrame();
bool active();
int reconnectSecondsRemaining();
int reconnectUnpauseSecondsRemaining();
void cancelReconnect();
bool reconnectFramePending();
bool holdSessionForLibrary();
bool sessionHeldForLibrary();
void returnToHeldSession();
void displayStats();
void endOfFrame();
void sendChatMessage(int playerNum, const std::string& msg);
void receiveChatMessages(void (*callback)(int playerNum, const std::string& msg));

static inline bool rollbacking() {
	extern bool inRollback;

	return inRollback;
}

}
