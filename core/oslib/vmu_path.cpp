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
#include "vmu_path.h"
#include "cfg/option.h"
#include "stdclass.h"
#include "storage.h"
#include <string_view>

namespace hostfs {

bool hasVmuExtension(const std::string& name)
{
	const auto extension = get_file_extension(name);
	return extension == "bin" || extension == "vmu";
}

bool isConfiguredVmuFileNameValid(const std::string& name)
{
	if (name.find_first_of("/\\:*?|<>\"") != std::string::npos)
		return false;
	std::string lower = name;
	string_tolower(lower);
	return lower != "dc_nvmem.bin" && lower != "dc_flash.bin" && hasVmuExtension(lower);
}

std::string sharedVmuFileName(const std::string& port)
{
	if (port.size() == 2 && port[0] >= 'A' && port[0] <= 'D' && port[1] >= '1' && port[1] <= '2')
	{
		const auto& selected = config::MapleVmuSlotFileNames[port[0] - 'A'][port[1] - '1'].get();
		if (isConfiguredVmuFileNameValid(selected))
			return selected;
	}
	return defaultVmuFileName(port);
}

static std::string customVmuFilePath(const std::string& name)
{
	if (!config::VMUPath.get().empty())
	{
		try {
			return storage().getSubPath(config::VMUPath, name);
		} catch (const StorageException&) {
		}
	}
	return {};
}

std::string findVmuFile(const std::string& name)
{
	const auto custom = customVmuFilePath(name);
	if (!custom.empty())
	{
		try {
			if (storage().exists(custom))
				return custom;
		} catch (const StorageException&) {
		}
	}
	const auto writable = get_writable_data_path(name);
	if (file_exists(writable))
		return writable;
	const auto readable = get_readonly_data_path(name);
	return storage().exists(readable) ? readable : std::string();
}

std::string getVmuPath(const std::string& port, bool save)
{
	std::string vmuName;
	if (port == "A1" && config::PerGameVmu)
	{
		if (settings.platform.isConsole() && !settings.content.gameId.empty())
		{
			constexpr std::string_view INVALID_CHARS { " /\\:*?|<>\"" };
			vmuName = settings.content.gameId;
			for (char &c: vmuName)
				if (INVALID_CHARS.find(c) != INVALID_CHARS.npos)
					c = '_';
			vmuName += perGameVmuSuffix;
		}
		else if (!settings.content.path.empty())
		{
			return get_game_save_prefix() + perGameVmuSuffix;
		}
	}

	if (vmuName.empty())
		vmuName = sharedVmuFileName(port);

	if (save)
	{
		const auto custom = customVmuFilePath(vmuName);
		return custom.empty() ? get_writable_data_path(vmuName) : custom;
	}

	const auto found = findVmuFile(vmuName);
	if (!found.empty())
		return found;
	if (port == "A1" && config::PerGameVmu && !settings.content.path.empty())
	{
		// Older standalone releases named per-game cards after the ROM.
		const auto legacy = get_game_save_prefix() + perGameVmuSuffix;
		if (file_exists(legacy))
			return legacy;
	}
	// VMU saves used to live in .reicast, outside its data directory.
	const auto legacy = get_readonly_config_path(vmuName);
	return file_exists(legacy) ? legacy : get_writable_data_path(vmuName);
}

}
