// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "phone_vmu.h"

#ifdef HAVE_PHONE_VMU
#include <ctime>
#include <filesystem>

namespace phone_vmu
{
struct CardBackup {
	std::filesystem::path card;
	std::filesystem::path snapshot;
};

std::vector<CardBackup> newestBackups(const std::vector<std::filesystem::path>& directories);
bool backupCard(const std::filesystem::path& card, const std::uint8_t *data, std::size_t size, std::time_t now);
bool restoreCard(const CardBackup& backup, std::time_t now);
}
#endif
