// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "network/phone_vmu_backup.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

static void check(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}

static std::vector<std::uint8_t> read(const fs::path& file)
{
	std::ifstream stream(file, std::ios::binary);
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int main()
{
	const fs::path dir = fs::temp_directory_path()
			/ ("hollycast-vmu-backup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	if (!fs::create_directory(dir))
		return 1;
	struct Cleanup {
		fs::path path;
		~Cleanup() { std::error_code ignored; fs::remove_all(path, ignored); }
	} cleanup {dir};

	try
	{
		const fs::path card = dir / fs::u8path(u8"card-é.bin");
		constexpr std::time_t now = 1700000000;
		std::vector<std::uint8_t> data(phone_vmu::CardSize);
		for (int i = 0; i < 7; ++i)
		{
			data[0] = i;
			check(phone_vmu::backupCard(card, data.data(), data.size(), now), "creating a same-second backup failed");
		}
		check(!phone_vmu::backupCard(card, data.data(), data.size() - 1, now),
				"a partial checkpoint must not replace a restorable backup");
		const fs::path legacy = dir / "other.bin.phone-backup-20200101-000000";
		std::ofstream(legacy, std::ios::binary).put('x');
		std::ofstream(dir / "other.bin.phone-backup-99999999-999999-invalid").put('x');
		auto backups = phone_vmu::newestBackups({dir});
		check(backups.size() == 2 && backups[0].card == card && backups[1].snapshot == legacy,
				"discovery must preserve legacy filenames and exclude malformed ones");
		const std::string latestName = backups[0].snapshot.filename().u8string();
		check(latestName.substr(latestName.size() - 7) == "-000006" && read(backups[0].snapshot) == data,
				"rotation must keep increasing the sequence after pruning earlier backups");
		check(!phone_vmu::restoreCard(backups[1], now) && !fs::exists(backups[1].card),
				"an incomplete snapshot must not create a card");
		std::ofstream(card, std::ios::binary).write("current", 7);
		check(phone_vmu::restoreCard(backups[0], now) && read(card) == data, "restoring a valid card failed");
		backups = phone_vmu::newestBackups({dir});
		const auto previous = read(backups[0].snapshot);
		check(std::string(previous.begin(), previous.end()) == "current", "restore lost the original card contents");
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
