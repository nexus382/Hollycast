// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "cfg/option.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "stdclass.h"
#include "types.h"
#include "ui/library_savestates.h"

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
		oldOnline = settings.network.online;
		oldMultiboard = settings.naomi.multiboard;
		oldHardcore = settings.raHardcoreMode;
		settings.network.online = false;
		settings.naomi.multiboard = false;
		settings.raHardcoreMode = false;
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
		settings.network.online = oldOnline;
		settings.naomi.multiboard = oldMultiboard;
		settings.raHardcoreMode = oldHardcore;
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
	bool oldOnline = false;
	decltype(settings.naomi.multiboard) oldMultiboard = {};
	bool oldHardcore = false;
};

struct TestSavestateHeader
{
	char magic[8] = {'F', 'L', 'Y', 'S', 'A', 'V', 'E', '1'};
	u64 creationDate = 1;
	u32 version = 1;
	u32 pngSize = 0;
};

void writeState(const std::filesystem::path& path, u64 creationDate)
{
	TestSavestateHeader header;
	header.creationDate = creationDate;
	std::ofstream file(path, std::ios::binary);
	file.write(reinterpret_cast<const char*>(&header), sizeof(header));
	ASSERT_TRUE(file.good());
}

TEST_F(LibrarySavestateTest, CustomSavestateFolderMatchesRuntimeFilenameAndSlot)
{
	const auto customDirectory = directory / "custom states";
	std::filesystem::create_directories(customDirectory);
	std::ofstream(stateFile(customDirectory, "My Game_2.state")) << "state";
	config::SavestatePath = std::vector<std::string>{customDirectory.string()};

	const std::string expected = hostfs::getSavestatePath(2, false);
	EXPECT_EQ(expected, hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 2, hostfs::getSavestateFolders()));
	EXPECT_EQ(stateFile(customDirectory, "My Game_2.state").string(), expected);
}

TEST_F(LibrarySavestateTest, GameDirectoryAndMissingStateMatchRuntimeResolver)
{
	std::ofstream(stateFile(gameDirectory)) << "state";
	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders()));

	std::filesystem::remove(stateFile(gameDirectory));
	(void)hostfs::getSavestatePath(0, true); // Clear the live resolver's path cache for this fixture.
	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders()));
}

TEST_F(LibrarySavestateTest, GameIdAloneDoesNotAdvertiseAStateTheLoaderCannotOpen)
{
	config::SavestatePath = std::vector<std::string>{directory.string()};
	std::ofstream(stateFile(directory, "TEST-GAME-ID.state")) << "state";

	EXPECT_EQ(hostfs::getSavestatePath(0, false),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders()));
	EXPECT_NE(stateFile(directory, "TEST-GAME-ID.state").string(),
			hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders()));
}

TEST_F(LibrarySavestateTest, InspectionDoesNotChangeCurrentGameSettings)
{
	const auto activePath = settings.content.path;
	const auto activeFileName = settings.content.fileName;
	const auto activeGameId = settings.content.gameId;
	(void)hostfs::getSavestatePathForGame(contentPath.string(), settings.content.fileName, 0, hostfs::getSavestateFolders());
	EXPECT_EQ(activePath, settings.content.path);
	EXPECT_EQ(activeFileName, settings.content.fileName);
	EXPECT_EQ(activeGameId, settings.content.gameId);
}

TEST_F(LibrarySavestateTest, NoStateDoesNotOfferResumeOrIncludeTransientStates)
{
	EXPECT_TRUE(dc_getLatestStateForGame(contentPath.string()).path.empty());
	EXPECT_TRUE(dc_getLatestStateForGame("").path.empty());
	writeState(stateFile(directory, "My Game.state.net"), 900);
	writeState(stateFile(directory, "My Game.state.tmp"), 1000);
	EXPECT_TRUE(dc_getLatestStateForGame(contentPath.string()).path.empty());
}

TEST_F(LibrarySavestateTest, NewestManualStateIncludesFirstAndLastSlots)
{
	const auto first = stateFile(directory);
	const auto last = stateFile(directory, "My Game_9.state");
	writeState(first, 300);
	writeState(last, 200);
	writeState(stateFile(directory, "My Game_10.state"), 100);
	EXPECT_EQ(first, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	writeState(last, 400);
	const auto newest = dc_getLatestStateForGame(contentPath.string());
	EXPECT_EQ(last, std::filesystem::path(newest.path));
	EXPECT_EQ(400, newest.creationDate);
}

TEST_F(LibrarySavestateTest, NewestAutosaveUsesCreationDateRatherThanFileModificationTime)
{
	const auto manual = stateFile(directory);
	const auto automatic = stateFile(directory, "My Game_10.state");
	writeState(manual, 100);
	writeState(automatic, 200);
	const auto now = std::filesystem::file_time_type::clock::now();
	std::filesystem::last_write_time(manual, now + std::chrono::hours(2));
	std::filesystem::last_write_time(automatic, now - std::chrono::hours(2));
	const auto newest = dc_getLatestStateForGame(contentPath.string());
	EXPECT_EQ(automatic, std::filesystem::path(newest.path));
	EXPECT_EQ(200, newest.creationDate);
}

TEST_F(LibrarySavestateTest, EqualDatesPreferAutosaveThenLowestManualSlot)
{
	writeState(stateFile(directory), 500);
	writeState(stateFile(directory, "My Game_9.state"), 500);
	const auto automatic = stateFile(directory, "My Game_10.state");
	writeState(automatic, 500);
	EXPECT_EQ(automatic, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	std::filesystem::remove(automatic);
	EXPECT_EQ(stateFile(directory), std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
}

TEST_F(LibrarySavestateTest, LegacyStateFallsBackToFileModificationTime)
{
	writeState(stateFile(directory), 100);
	const auto legacy = stateFile(directory, "My Game_4.state");
	{ std::ofstream file(legacy, std::ios::binary); file << "legacy state without a header"; }
	const auto newest = dc_getLatestStateForGame(contentPath.string());
	EXPECT_EQ(legacy, std::filesystem::path(newest.path));
	EXPECT_EQ(hostfs::storage().getFileInfo(legacy.string()).updateTime, newest.creationDate);
}

TEST_F(LibrarySavestateTest, DiscoveryRechecksNewAndDeletedStates)
{
	const auto old = stateFile(directory, "My Game_3.state");
	const auto recent = stateFile(directory, "My Game_7.state");
	writeState(old, 100);
	EXPECT_EQ(old, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	writeState(recent, 200);
	EXPECT_EQ(recent, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	std::filesystem::remove(recent);
	EXPECT_EQ(old, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	std::filesystem::remove(old);
	EXPECT_TRUE(dc_getLatestStateForGame(contentPath.string()).path.empty());
}

TEST_F(LibrarySavestateTest, NewestStateCanComeFromCustomDataOrGameFolder)
{
	const auto custom = directory / "custom";
	std::filesystem::create_directory(custom);
	config::SavestatePath = std::vector<std::string>{custom.string()};
	const auto customState = stateFile(custom, "My Game_2.state");
	const auto dataState = stateFile(directory, "My Game_10.state");
	const auto adjacentState = stateFile(gameDirectory);
	writeState(customState, 300);
	writeState(dataState, 200);
	writeState(adjacentState, 100);
	EXPECT_EQ(customState, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	writeState(adjacentState, 400);
	EXPECT_EQ(adjacentState, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
	writeState(dataState, 500);
	EXPECT_EQ(dataState, std::filesystem::path(dc_getLatestStateForGame(contentPath.string()).path));
}

TEST_F(LibrarySavestateTest, SameBasenameGamesKeepTheirOwnStateWithoutChangingRuntimeCache)
{
	const auto secondDirectory = directory / "second game";
	std::filesystem::create_directory(secondDirectory);
	const auto secondGame = secondDirectory / contentPath.filename();
	{ std::ofstream file(secondGame); file << "fixture"; }
	writeState(stateFile(gameDirectory), 100);
	writeState(stateFile(secondDirectory), 200);
	const auto activeState = hostfs::getSavestatePath(0, false);
	const auto selected = dc_getLatestStateForGame(secondGame.string());
	EXPECT_EQ(stateFile(secondDirectory), std::filesystem::path(selected.path));
	EXPECT_NE(activeState, selected.path);
	EXPECT_EQ(activeState, hostfs::getSavestatePath(0, false));
	EXPECT_EQ(contentPath.string(), settings.content.path);
	EXPECT_EQ(contentPath.filename().string(), settings.content.fileName);
	EXPECT_EQ("TEST-GAME-ID", settings.content.gameId);
}

TEST_F(LibrarySavestateTest, ExplicitMissingResumeStateReportsFailure)
{
	ASSERT_TRUE(dc_savestateAllowed());
	EXPECT_FALSE(dc_loadstateForPath(""));
	EXPECT_FALSE(dc_loadstateForPath(stateFile(directory, "missing.state").string()));
}

TEST_F(LibrarySavestateTest, BackgroundInspectionUsesCapturedFoldersAndDecodedFilename)
{
	const auto captured = hostfs::getSavestateFolders();
	const auto saved = stateFile(directory, "Decoded Game_10.state");
	writeState(saved, 500);
	const auto changedDirectory = directory / "changed";
	std::filesystem::create_directory(changedDirectory);
	config::SavestatePath = std::vector<std::string>{changedDirectory.string()};
	set_user_data_dir(changedDirectory.string());

	// The scanner already supplies the decoded filename; no ROM metadata lookup
	// or access to the changed global folders is needed on the worker.
	const auto state = dc_getLatestStateForGame("content://provider/document/opaque-id", "Decoded Game.cdi", captured);
	EXPECT_EQ(saved, std::filesystem::path(state.path));
	EXPECT_EQ(500, state.creationDate);
}

TEST_F(LibrarySavestateTest, PathInspectionReadsReplacedAndDeletedFilesWithoutASharedCache)
{
	const auto saved = stateFile(directory);
	writeState(saved, 100);
	EXPECT_EQ(100, dc_getStateCreationDateForPath(saved.string()));
	writeState(saved, 200);
	EXPECT_EQ(200, dc_getStateCreationDateForPath(saved.string()));
	std::filesystem::remove(saved);
	EXPECT_EQ(0, dc_getStateCreationDateForPath(saved.string()));
}

SavestateInfo waitForCachedState(LibrarySavestateCache& cache, const std::string& path)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	SavestateInfo state;
	do {
		state = cache.get(path, "Game.cdi");
		if (!cache.busy())
			return state;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	ADD_FAILURE() << "Background save inspection did not complete";
	return state;
}

TEST(LibrarySavestateCacheTest, SlowStorageDoesNotBlockTheCallerOrQueueTheWholeLibrary)
{
	std::promise<void> entered;
	std::promise<void> release;
	const auto gate = release.get_future().share();
	std::atomic<int> calls{0};
	LibrarySavestateCache cache;
	cache.reset([&](const std::string& path, const std::string&) {
		++calls;
		entered.set_value();
		gate.wait();
		return SavestateInfo{path + ".state", 100};
	});
	auto lookup = std::async(std::launch::async, [&] { return cache.get("first", "Game.cdi"); });
	const auto started = entered.get_future().wait_for(std::chrono::seconds(5));
	const auto returned = lookup.wait_for(std::chrono::seconds(1));
	// Release even on a regression so a failing assertion cannot strand the worker.
	if (returned == std::future_status::ready)
		for (int i = 0; i < 100; ++i)
			cache.get("other" + std::to_string(i), "Game.cdi");
	release.set_value();
	EXPECT_EQ(std::future_status::ready, started);
	EXPECT_EQ(std::future_status::ready, returned);
	EXPECT_TRUE(lookup.get().path.empty());
	EXPECT_EQ("first.state", waitForCachedState(cache, "first").path);
	EXPECT_EQ(1, calls.load());
}

TEST(LibrarySavestateCacheTest, RefreshDiscardsAnInFlightResult)
{
	std::promise<void> entered;
	std::promise<void> release;
	const auto gate = release.get_future().share();
	LibrarySavestateCache cache;
	cache.reset([&](const std::string&, const std::string&) {
		entered.set_value();
		gate.wait();
		return SavestateInfo{"old.state", 100};
	});
	cache.get("game", "Game.cdi");
	const auto started = entered.get_future().wait_for(std::chrono::seconds(5));
	cache.reset([](const std::string&, const std::string&) { return SavestateInfo{"new.state", 200}; });
	release.set_value();
	EXPECT_EQ(std::future_status::ready, started);
	const auto state = waitForCachedState(cache, "game");
	EXPECT_EQ("new.state", state.path);
	EXPECT_EQ(200, state.creationDate);
}

TEST(LibrarySavestateCacheTest, StorageFailureIsCachedUntilRefresh)
{
	std::atomic<int> calls{0};
	LibrarySavestateCache cache;
	cache.reset([&](const std::string&, const std::string&) -> SavestateInfo {
		++calls;
		throw hostfs::StorageException("Storage was disconnected");
	});
	EXPECT_TRUE(waitForCachedState(cache, "game").path.empty());
	EXPECT_TRUE(cache.get("game", "Game.cdi").path.empty());
	EXPECT_FALSE(cache.busy());
	EXPECT_EQ(1, calls.load());
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
