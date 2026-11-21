// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <string>

namespace hostfs {

inline constexpr char perGameVmuSuffix[] = "_vmu_save_A1.bin";

inline std::string vmuSlotLabel(int bus, int slot)
{
	return {static_cast<char>('A' + bus), static_cast<char>('1' + slot)};
}

inline std::string defaultVmuFileName(const std::string& port)
{
	return "vmu_save_" + port + ".bin";
}

bool isConfiguredVmuFileNameValid(const std::string& name);
bool hasVmuExtension(const std::string& name);
std::string sharedVmuFileName(const std::string& port);
std::string findVmuFile(const std::string& name);
std::string getVmuPath(const std::string& port, bool save);

}
