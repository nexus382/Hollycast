/*
	Copyright 2024 flyinghead
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
#include "types.h"
#include "emulator.h"
#include "cfg/option.h"
#include "gui.h"
#include "achievements/achievements.h"
#include "discord_rpc.h"
#include <ctime>
#include <mutex>

namespace
{
constexpr char HollyCastDiscordApplicationId[] = "1575720391937097859";
// Use the HollyCast Discord application's icon for both artwork layouts.
constexpr char HollyCastDiscordIcon[] =
		"https://cdn.discordapp.com/app-icons/1575720391937097859/75e969a31f243abdd21b4fcc82090fa9.png";
}

class DiscordPresence
{
public:
	DiscordPresence()
	{
		EventManager::listen(Event::Start, handleEmuEvent, this);
		EventManager::listen(Event::Terminate, handleEmuEvent, this);
		EventManager::listen(Event::Resume, handleEmuEvent, this);
		EventManager::listen(Event::Network, handleEmuEvent, this);
		EventManager::listen(Event::RichPresence, handleEmuEvent, this);
	}

	~DiscordPresence()
	{
		shutdown();
		EventManager::unlisten(Event::Start, handleEmuEvent, this);
		EventManager::unlisten(Event::Terminate, handleEmuEvent, this);
		EventManager::unlisten(Event::Resume, handleEmuEvent, this);
		EventManager::unlisten(Event::Network, handleEmuEvent, this);
		EventManager::unlisten(Event::RichPresence, handleEmuEvent, this);
	}

private:
	void initialize()
	{
		if (!initialized)
			Discord_Initialize(HollyCastDiscordApplicationId, nullptr, 0, nullptr);
		initialized = true;
	}

	void shutdown()
	{
		if (initialized)
			Discord_Shutdown();
		initialized = false;
	}

	void sendPresence()
	{
		initialize();
		DiscordRichPresence presence{};
		presence.state = settings.content.title.c_str();
		presence.startTimestamp = startTimestamp;

		const std::string imageUrl = gui_getCurGameBoxartUrl();
		presence.largeImageKey = HollyCastDiscordIcon;
		presence.largeImageText = "HollyCast";
		if (!imageUrl.empty())
		{
			presence.largeImageKey = imageUrl.c_str();
			presence.largeImageText = settings.content.title.c_str();
			presence.smallImageKey = HollyCastDiscordIcon;
			presence.smallImageText = "HollyCast";
		}

		std::string details = achievements::getRichPresence();
		if (settings.network.online)
			details = details.empty() ? "Online" : "Online - " + details;
		if (!details.empty())
			presence.details = details.c_str();
		Discord_UpdatePresence(&presence);
	}

	static void handleEmuEvent(Event event, void *p)
	{
		if (settings.naomi.slave || settings.naomi.drivingSimSlave != 0)
			return;
		DiscordPresence *inst = (DiscordPresence *)p;
		std::lock_guard<std::mutex> lock(inst->presenceMutex);
		switch (event)
		{
		case Event::Start:
			inst->startTimestamp = time(nullptr);
			[[fallthrough]];
		case Event::Network:
		case Event::RichPresence:
			if (config::DiscordPresence && inst->startTimestamp != 0)
				inst->sendPresence();
			break;
		case Event::Resume:
			if (config::DiscordPresence && !inst->initialized && inst->startTimestamp != 0)
				// Discord presence enabled
				inst->sendPresence();
			else if (!config::DiscordPresence && inst->initialized)
			{
				// Discord presence disabled
				Discord_ClearPresence();
				inst->shutdown();
			}
			break;
		case Event::Terminate:
			if (inst->initialized)
				Discord_ClearPresence();
			inst->startTimestamp = 0;
			break;
		default:
			break;
		}
	}

	std::mutex presenceMutex;
	bool initialized = false;
	int64_t startTimestamp = 0;
};

static DiscordPresence discordPresence;
