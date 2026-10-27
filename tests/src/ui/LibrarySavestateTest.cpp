// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "types.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

class LibrarySavestateTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		oldDataPath = get_writable_data_path("");
		oldSavestatePaths = config::SavestatePath.get();
		oldContentPath = settings.content.path;
		oldFileName = settings.content.fileName;
		oldGameId = settings.content.gameId;
		const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = std::filesystem::temp_directory_path() / ("hollycast-savestate-" + std::to_string(nonce));
		gameDirectory = directory / "games";
		std::filesystem::create_directories(gameDirectory);
		contentPath = gameDirectory / "My Game.cdi";
		std::ofstream(contentPath) << "fixture";
		set_user_data_dir(directory.string());
		settings.content.path = contentPath.string();
		settings.content.fileName = contentPath.filename().string();
		settings.content.gameId = "TEST-GAME-ID";
		config::SavestatePath = std::vector<std::string>{};
		// The live resolver caches by filename; clear both slots this fixture uses.
		(void)hostfs::getSavestatePath(0, true);
		(void)hostfs::getSavestatePath(2, true);
	}

	void TearDown() override
	{
		config::SavestatePath = oldSavestatePaths;
		settings.content.path = oldContentPath;
		settings.content.fileName = oldFileName;
		settings.content.gameId = oldGameId;
		set_user_data_dir(oldDataPath);
		// Do not leave the temporary fixture path in the runtime resolver cache.
		(void)hostfs::getSavestatePath(0, true);
		(void)hostfs::getSavestatePath(2, true);
		std::filesystem::remove_all(directory);
	}

	std::filesystem::path stateFile(const std::filesystem::path& folder, const std::string& name = "My Game.state") const
	{
		return folder / name;
	}

	std::filesystem::path directory;
	std::filesystem::path gameDirectory;
	std::filesystem::path contentPath;
	std::string oldDataPath;
	std::vector<std::string> oldSavestatePaths;
	std::string oldContentPath;
	std::string oldFileName;
	std::string oldGameId;
};

struct TestSavestateHeader
{
	char magic[8] = {'F', 'L', 'Y', 'S', 'A', 'V', 'E', '1'};
	u64 creationDate = 1;
	u32 version = 1;
	u32 pngSize = 0;
};

TEST_F(LibrarySavestateTest, CustomSavestateFolderMatchesRuntimeFilenameAndSlot)
{
	const auto customDirectory = directory / "custom states";
	std::filesystem::create_directories(customDirectory);
	std::ofstream(stateFile(customDirectory, "My Game_2.state")) << "state";
	config::SavestatePath = std::vector<std::string>{customDirectory.string()};

	const std::string expected = hostfs::getSavestatePath(2, false);
	EXPECT_EQ(expected, hostfs::getSavestatePathForGame(contentPath.string(), settings.content.gameId, 2));
	EXPECT_EQ(stateFile(customDirectory, "My Game_2.state").string(), expected);
}

TEST_F(LibrarySavestateTest, GameDirectoryAndMissingStateMatchRuntimeResolver)
{
	std::ofstream(stateFile(gameDirectory)) << "state";
	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.gameId, 0));

	std::filesystem::remove(stateFile(gameDirectory));
	(void)hostfs::getSavestatePath(0, true); // Clear the live resolver's path cache for this fixture.
	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.gameId, 0));
}

TEST_F(LibrarySavestateTest, GameIdAloneDoesNotAdvertiseAStateTheLoaderCannotOpen)
{
	config::SavestatePath = std::vector<std::string>{directory.string()};
	std::ofstream(stateFile(directory, "TEST-GAME-ID.state")) << "state";

	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.gameId, 0));
	EXPECT_NE(stateFile(directory, "TEST-GAME-ID.state").string(),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.gameId, 0));
}

TEST_F(LibrarySavestateTest, InspectionDoesNotChangeCurrentGameSettings)
{
	const auto activePath = settings.content.path;
	const auto activeFileName = settings.content.fileName;
	const auto activeGameId = settings.content.gameId;
	(void)hostfs::getSavestatePathForGame(contentPath.string(), "OTHER-GAME", 0);
	EXPECT_EQ(activePath, settings.content.path);
	EXPECT_EQ(activeFileName, settings.content.fileName);
	EXPECT_EQ(activeGameId, settings.content.gameId);
}

TEST_F(LibrarySavestateTest, ScreenshotReaderAcceptsValidPngAndRejectsTruncatedOrOversizedHeaders)
{
	const auto screenshotPath = directory / "screenshot.state";
	const u8 pngSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	TestSavestateHeader header;
	std::vector<u8> screenshot;
	header.pngSize = sizeof(pngSignature);
	{
		std::ofstream file(screenshotPath, std::ios::binary);
		file.write(reinterpret_cast<const char*>(&header), sizeof(header));
		file.write(reinterpret_cast<const char*>(pngSignature), sizeof(pngSignature));
	}
	dc_getStateScreenshotForPath(screenshotPath.string(), screenshot);
	ASSERT_EQ(sizeof(pngSignature), screenshot.size());
	EXPECT_TRUE(std::equal(std::begin(pngSignature), std::end(pngSignature), screenshot.begin()));

	{
		std::ofstream file(screenshotPath, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(&header), sizeof(header) - 1);
	}
	dc_getStateScreenshotForPath(screenshotPath.string(), screenshot);
	EXPECT_TRUE(screenshot.empty());

	header.pngSize = 64 * 1024 * 1024 + 1;
	{
		std::ofstream file(screenshotPath, std::ios::binary | std::ios::trunc);
		file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	}
	dc_getStateScreenshotForPath(screenshotPath.string(), screenshot);
	EXPECT_TRUE(screenshot.empty());
}

} // namespace
