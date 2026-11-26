// Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "hw/maple/maple_devs.h"
#include "oslib/vmu_path.h"
#include "stdclass.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
namespace fs = std::filesystem;

class VmuMigrationTest : public ::testing::Test
{
protected:
	struct InjectedVmu : maple_sega_vmu
	{
		bool failWrite = false;
		bool failPublish = false;
		bool failReopen = false;
		bool memoryOnlySession = false;
		bool appearCorrectDestination = false;
		std::string appearingDestination;

		bool isMemoryOnlySession() const override { return memoryOnlySession; }

		bool writeMigrationImage(FILE *destination, const u8 *image, size_t size) override
		{
			if (failWrite)
				return false;
			if (!maple_sega_vmu::writeMigrationImage(destination, image, size))
				return false;
			if (!appearingDestination.empty())
			{
				std::ofstream appeared(appearingDestination, std::ios::binary);
				if (appearCorrectDestination)
					appeared.write(reinterpret_cast<const char *>(image), size);
				else
					appeared << "concurrent destination";
			}
			return true;
		}

		FILE *reopenMigrationDestination(const std::string& path) override
		{
			return failReopen ? nullptr : maple_sega_vmu::reopenMigrationDestination(path);
		}

		bool publishMigrationImage(const std::string& temporary, const std::string& destination) override
		{
			return failPublish ? false : maple_sega_vmu::publishMigrationImage(temporary, destination);
		}
	};

	fs::path root = fs::temp_directory_path() / ("hollycast-vmu-migration-"
			+ std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::path data = root / "data";
	fs::path custom = root / "cards";
	std::string oldDataDir = get_writable_data_path("");
	std::string oldVmuDir = config::VMUPath.get();
	std::string oldA1 = config::MapleVmuSlotFileNames[0][0].get();
	bool oldPerGame = config::PerGameVmu;
	int oldSystem = settings.platform.system;
	decltype(settings.content) oldContent = settings.content;

	void SetUp() override
	{
		fs::create_directories(data);
		fs::create_directories(custom);
		set_user_data_dir(data.string());
		config::VMUPath = custom.string();
		config::PerGameVmu = true;
		settings.platform.system = DC_PLATFORM_DREAMCAST;
		settings.content.gameId = "VMU GAME";
		settings.content.fileName = "Disc.cdi";
		settings.content.path = (root / "Disc.cdi").string();
	}

	void TearDown() override
	{
		set_user_data_dir(oldDataDir);
		config::VMUPath = oldVmuDir;
		config::MapleVmuSlotFileNames[0][0] = oldA1;
		config::PerGameVmu = oldPerGame;
		settings.content = oldContent;
		settings.platform.system = oldSystem;
		fs::remove_all(root);
	}

	std::vector<u8> patternedImage() const
	{
		std::vector<u8> image(128_KB);
		for (size_t i = 0; i < image.size(); i++)
			image[i] = static_cast<u8>((i * 31 + i / 512) & 0xff);
		return image;
	}

	void writeImage(const fs::path& path, const std::vector<u8>& image) const
	{
		std::ofstream file(path, std::ios::binary);
		ASSERT_TRUE(file);
		file.write(reinterpret_cast<const char *>(image.data()), image.size());
		ASSERT_TRUE(file);
	}

	std::vector<u8> readImage(const fs::path& path) const
	{
		std::ifstream file(path, std::ios::binary);
		return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	}

	fs::path legacyPerGamePath() const
	{
		return fs::path(get_game_save_prefix() + hostfs::perGameVmuSuffix);
	}

	fs::path preferredPath() const
	{
		return fs::path(hostfs::getVmuPath("A1", true));
	}

	std::shared_ptr<maple_sega_vmu> setupVmu(bool failWrite = false, bool failReopen = false,
			bool memoryOnlySession = false, const std::string& appearingDestination = {}, bool failPublish = false,
			bool appearCorrectDestination = false)
	{
		auto vmu = std::make_shared<InjectedVmu>();
		vmu->failWrite = failWrite;
		vmu->failPublish = failPublish;
		vmu->failReopen = failReopen;
		vmu->memoryOnlySession = memoryOnlySession;
		vmu->appearingDestination = appearingDestination;
		vmu->appearCorrectDestination = appearCorrectDestination;
		vmu->Setup(0, 0, -1, false);
		return vmu;
	}

	u32 writeBlock(maple_sega_vmu& vmu, u32 blockNumber, u8 value)
	{
		std::array<u32, 131> input {};
		std::array<u32, 131> output {};
		input[0] = MDCF_BlockWrite;
		input[1] = MFID_1_Storage;
		input[2] = SWAP32(blockNumber);
		u8 *block = reinterpret_cast<u8 *>(input.data()) + 3 * sizeof(u32);
		std::fill(block, block + 512, value);
		vmu.RawDma(input.data(), sizeof(input), output.data());
		return output[0] & 0xff;
	}
};

TEST_F(VmuMigrationTest, ShortSourceSurvivesMigration)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const std::vector<u8> shortImage(128_KB - 1, 0x4a);
	writeImage(source, shortImage);

	{
		auto vmu = setupVmu();
		EXPECT_EQ(nullptr, vmu->file);
		EXPECT_TRUE(std::all_of(std::begin(vmu->flash_data), std::end(vmu->flash_data), [](u8 b) { return b == 0; }));
	}

	EXPECT_TRUE(fs::exists(source));
	EXPECT_EQ(shortImage, readImage(source));
	EXPECT_FALSE(fs::exists(destination));
}

TEST_F(VmuMigrationTest, UnreadableSourceIsNotReplacedByABlankCard)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	fs::create_directory(source);

	{
		auto vmu = setupVmu();
		EXPECT_EQ(nullptr, vmu->file);
		EXPECT_EQ(MDRE_FileError, writeBlock(*vmu, 2, 0x6d));
		EXPECT_TRUE(std::all_of(std::begin(vmu->flash_data), std::end(vmu->flash_data),
				[](u8 value) { return value == 0; }));
	}

	EXPECT_TRUE(fs::is_directory(source));
	EXPECT_FALSE(fs::exists(destination));
}

TEST_F(VmuMigrationTest, FailedDestinationCreationKeepsSourceAndCanRetry)
{
	const fs::path source = legacyPerGamePath();
	const auto image = patternedImage();
	writeImage(source, image);
	fs::remove(custom);
	std::ofstream(custom) << "not a directory";
	config::VMUPath = custom.string();
	const fs::path destination = preferredPath();

	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
	}
	EXPECT_EQ(image, readImage(source));
	EXPECT_TRUE(fs::is_regular_file(custom));

	fs::remove(custom);
	fs::create_directories(custom);
	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
	}
	EXPECT_EQ(image, readImage(destination));
	EXPECT_FALSE(fs::exists(source));
}

TEST_F(VmuMigrationTest, FailedMigrationWriteKeepsSourceAndCanRetry)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	{
		auto vmu = setupVmu(true, false);
		ASSERT_NE(nullptr, vmu->file);
	}
	EXPECT_EQ(image, readImage(source));
	EXPECT_FALSE(fs::exists(destination));

	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
	}
	EXPECT_EQ(image, readImage(destination));
	EXPECT_FALSE(fs::exists(source));
}

TEST_F(VmuMigrationTest, FailedReopenKeepsSourceAndVerifiedDestination)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	{
		auto vmu = setupVmu(false, true);
		ASSERT_NE(nullptr, vmu->file);
	}
	EXPECT_EQ(image, readImage(source));
	EXPECT_EQ(image, readImage(destination));

	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
	}
	EXPECT_EQ(image, readImage(source));
}

TEST_F(VmuMigrationTest, DestinationAppearingDuringMigrationIsNeverReplaced)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	const std::string appearedBytes = "concurrent destination";
	writeImage(source, image);

	{
		auto vmu = setupVmu(false, false, false, destination.string());
		ASSERT_NE(nullptr, vmu->file);
	}
	EXPECT_EQ(image, readImage(source));
	EXPECT_EQ(std::vector<u8>(appearedBytes.begin(), appearedBytes.end()), readImage(destination));
}

TEST_F(VmuMigrationTest, MatchingDestinationAppearingDuringMigrationIsUsedSafely)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	{
		auto vmu = setupVmu(false, false, false, destination.string(), false, true);
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
	}
	EXPECT_EQ(image, readImage(source));
	EXPECT_EQ(image, readImage(destination));
}

TEST_F(VmuMigrationTest, ExistingDestinationAndSourceAreBothPreserved)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto sourceImage = patternedImage();
	std::vector<u8> destinationImage(128_KB, 0x93);
	writeImage(source, sourceImage);
	writeImage(destination, destinationImage);

	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(destinationImage.begin(), destinationImage.end(), std::begin(vmu->flash_data)));
	}

	EXPECT_EQ(sourceImage, readImage(source));
	EXPECT_EQ(destinationImage, readImage(destination));
}

TEST_F(VmuMigrationTest, GuestReadsCardWithoutMigratingOrPersistingChanges)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	{
		auto vmu = setupVmu(false, false, true);
		EXPECT_TRUE(vmu->memoryOnly);
		EXPECT_EQ(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
		EXPECT_EQ(MDRS_DeviceReply, writeBlock(*vmu, 2, 0x6d));
		EXPECT_TRUE(vmu->fullSave());
	}

	EXPECT_EQ(image, readImage(source));
	EXPECT_FALSE(fs::exists(destination));
}

TEST_F(VmuMigrationTest, SharedLegacyCardMovesAndBlockWritesReachCommittedFile)
{
	config::PerGameVmu = false;
	const fs::path source = data / "vmu_save_A1.bin";
	config::MapleVmuSlotFileNames[0][0] = "vmu_save_A1.bin";
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	auto vmu = setupVmu();
	ASSERT_NE(nullptr, vmu->file);
	EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));

	std::array<u32, 131> input {};
	std::array<u32, 131> output {};
	input[0] = MDCF_BlockWrite;
	input[1] = MFID_1_Storage;
	input[2] = SWAP32(2); // Block 2, phase 0.
	u8 *block = reinterpret_cast<u8 *>(input.data()) + 3 * sizeof(u32);
	std::fill(block, block + 512, 0x6d);
	const u32 responseSize = vmu->RawDma(input.data(), sizeof(input), output.data());
	EXPECT_EQ(4u, responseSize);
	EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);
	vmu.reset();

	auto expected = image;
	std::fill(expected.begin() + 2 * 512, expected.begin() + 3 * 512, 0x6d);
	EXPECT_EQ(expected, readImage(destination));
	EXPECT_FALSE(fs::exists(source));
}

TEST_F(VmuMigrationTest, UnsupportedPublicationKeepsLegacyBackingWritableAndRetriesAfterRestart)
{
	const fs::path source = legacyPerGamePath();
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(source, image);

	{
		auto vmu = setupVmu(false, false, false, {}, true);
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
		EXPECT_EQ(image, readImage(source));
		EXPECT_EQ(MDRS_DeviceReply, writeBlock(*vmu, 2, 0x6d));
	}

	auto expected = image;
	std::fill(expected.begin() + 2 * 512, expected.begin() + 3 * 512, 0x6d);
	EXPECT_EQ(expected, readImage(source));
	EXPECT_FALSE(fs::exists(destination));

	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(expected.begin(), expected.end(), std::begin(vmu->flash_data)));
	}
	EXPECT_EQ(expected, readImage(destination));
	EXPECT_FALSE(fs::exists(source));
}

TEST_F(VmuMigrationTest, CurrentCardAndGenuinelyMissingCardStillOpen)
{
	const fs::path destination = preferredPath();
	const auto image = patternedImage();
	writeImage(destination, image);
	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::equal(image.begin(), image.end(), std::begin(vmu->flash_data)));
	}

	fs::remove(destination);
	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(fs::exists(destination));
		EXPECT_NE(0, std::memcmp(vmu->flash_data, image.data(), image.size()));
	}
}

TEST_F(VmuMigrationTest, ValidEmptyCardStillGetsInitialized)
{
	const fs::path destination = preferredPath();
	writeImage(destination, std::vector<u8>(128_KB, 0));
	{
		auto vmu = setupVmu();
		ASSERT_NE(nullptr, vmu->file);
		EXPECT_TRUE(std::any_of(std::begin(vmu->flash_data), std::end(vmu->flash_data), [](u8 b) { return b != 0; }));
	}
	const auto savedImage = readImage(destination);
	EXPECT_TRUE(std::any_of(savedImage.begin(), savedImage.end(), [](u8 b) { return b != 0; }));
}

}
