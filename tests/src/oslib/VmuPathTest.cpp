// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "oslib/vmu_path.h"
#include "stdclass.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;

class VmuPathTest : public ::testing::Test
{
protected:
	const fs::path root = fs::temp_directory_path() / ("hollycast-vmu-path-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const fs::path custom = root / "cards";
	const fs::path data = root / "data";

	void SetUp() override
	{
		fs::create_directories(custom);
		fs::create_directories(data);
		set_user_data_dir(data.string());
		config::VMUPath = custom.string();
		config::PerGameVmu = false;
		settings.platform.system = DC_PLATFORM_DREAMCAST;
		settings.content.gameId = "A GAME";
		settings.content.fileName = "Disc.cdi";
		settings.content.path = (root / "Disc.cdi").string();
	}

	void TearDown() override
	{
		set_user_data_dir(oldDataDir);
		config::VMUPath = oldVmuDir;
		config::PerGameVmu = oldPerGame;
		config::MapleVmuSlotFileNames[0][0] = oldA1;
		config::MapleVmuSlotFileNames[2][1] = oldC2;
		settings.content = oldContent;
		settings.platform.system = oldSystem;
		fs::remove_all(root);
	}

private:
	const std::string oldDataDir = get_writable_data_path("");
	const std::string oldVmuDir = config::VMUPath.get();
	const std::string oldA1 = config::MapleVmuSlotFileNames[0][0].get();
	const std::string oldC2 = config::MapleVmuSlotFileNames[2][1].get();
	const bool oldPerGame = config::PerGameVmu;
	const int oldSystem = settings.platform.system;
	const decltype(settings.content) oldContent = settings.content;
};

TEST_F(VmuPathTest, SharedSlotsReadExistingCardsAndWriteToTheConfiguredFolder)
{
	config::MapleVmuSlotFileNames[2][1] = "My Card.VMU";
	std::ofstream(data / "My Card.VMU") << "default data";
	EXPECT_EQ(data / "My Card.VMU", fs::path(hostfs::getVmuPath("C2", false)));
	EXPECT_EQ(custom / "My Card.VMU", fs::path(hostfs::getVmuPath("C2", true)));

	std::ofstream(custom / "My Card.VMU") << "custom data";
	EXPECT_EQ(custom / "My Card.VMU", fs::path(hostfs::getVmuPath("C2", false)));

	config::MapleVmuSlotFileNames[2][1] = "dc_Flash.BIN";
	EXPECT_EQ(custom / "vmu_save_C2.bin", fs::path(hostfs::getVmuPath("C2", true)));
}

TEST_F(VmuPathTest, PerGameCardsOverrideOnlyA1AndCanReadLegacyRomNames)
{
	config::PerGameVmu = true;
	config::MapleVmuSlotFileNames[0][0] = "shared.bin";
	config::MapleVmuSlotFileNames[2][1] = "second.vmu";

	const auto legacy = data / "Disc.cdi_vmu_save_A1.bin";
	std::ofstream(legacy) << "old card";
	EXPECT_EQ(legacy, fs::path(hostfs::getVmuPath("A1", false)));
	const auto product = custom / "A_GAME_vmu_save_A1.bin";
	EXPECT_EQ(product, fs::path(hostfs::getVmuPath("A1", true)));
	std::ofstream(product) << "new card";
	EXPECT_EQ(product, fs::path(hostfs::getVmuPath("A1", false)));
	EXPECT_EQ(custom / "second.vmu", fs::path(hostfs::getVmuPath("C2", true)));

	settings.content.gameId.clear();
	EXPECT_EQ(legacy, fs::path(hostfs::getVmuPath("A1", true)));
}
}
