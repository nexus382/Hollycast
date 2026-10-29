// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "ui/boxart/boxart.h"
#include "ui/boxart/launchbox.h"
#include "ui/game_scanner.h"
#include "reios/reios.h"
#include "imgread/isofs.h"
#include <stb_image.h>

#include <chrono>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

TEST(BoxartWorkTest, AcceptsOnlyCurrentSourceRevision)
{
	Boxart boxart;
	GameBoxart request;
	request.fileName = "game.cue";
	request.gamePath = "/games/first/game.cue";
	request.workRevision = 7;

	EXPECT_FALSE(boxart.isCurrentWork(request)) << "An absent source must reject work";
	boxart.games.emplace(request.sourceKey(), request);
	EXPECT_TRUE(boxart.isCurrentWork(request)) << "The current source revision must accept work";

	boxart.games.at(request.sourceKey()).workRevision = 8;
	EXPECT_FALSE(boxart.isCurrentWork(request)) << "Replacing a revision must reject the older request";
	request.workRevision = 8;
	EXPECT_TRUE(boxart.isCurrentWork(request)) << "The replacement revision must accept its matching request";

	GameBoxart futureRequest = request;
	futureRequest.workRevision = 9;
	EXPECT_FALSE(boxart.isCurrentWork(futureRequest)) << "A future revision must also reject work";

	GameBoxart otherPathRequest = request;
	otherPathRequest.gamePath = "/games/second/game.cue";
	EXPECT_FALSE(boxart.isCurrentWork(otherPathRequest)) << "Matching basenames must not alias different paths";
	EXPECT_TRUE(boxart.isCurrentWork(request)) << "The original path must remain current";

	GameBoxart arcadeRequest = request;
	arcadeRequest.arcade = true;
	EXPECT_FALSE(boxart.isCurrentWork(arcadeRequest)) << "Arcade work must not match a disc source at the same path";
	boxart.games.emplace(arcadeRequest.sourceKey(), arcadeRequest);
	EXPECT_TRUE(boxart.isCurrentWork(arcadeRequest)) << "A matching arcade source must accept work";

	GameBoxart completedRequest = request;
	completedRequest.uniqueId = "SCRAPED-0001";
	completedRequest.boxartPath = "/artwork/completed.png";
	EXPECT_TRUE(boxart.isCurrentWork(completedRequest)) << "Worker output metadata must not change request eligibility";

	boxart.games.erase(request.sourceKey());
	EXPECT_FALSE(boxart.isCurrentWork(request)) << "Removing a source must reject its previously matching request";
	EXPECT_TRUE(boxart.isCurrentWork(arcadeRequest)) << "Removing one source must leave another source current";
}

namespace {

#ifndef _WIN32
class PausedArcadeScan
{
public:
	PausedArcadeScan(Boxart& boxart, const std::filesystem::path& path)
		: boxart(boxart), path(path), fifoCreated(::mkfifo(path.c_str(), 0600) == 0)
	{}

	~PausedArcadeScan()
	{
		unblockReader();
		if (refresh.valid())
			refresh.wait();
		boxart.term();
		if (writer >= 0)
			::close(writer);
	}

	bool created() const { return fifoCreated; }

	bool waitForReader()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do {
			writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
			if (writer >= 0)
				return true;
			if (errno != ENXIO && errno != EINTR)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		return false;
	}

	void startRefresh()
	{
		refresh = std::async(std::launch::async, [this]() { boxart.refreshCache(); });
	}

	void finishRefresh()
	{
		unblockReader();
		refresh.get();
	}

private:
	void unblockReader()
	{
		if (!fifoCreated || released)
			return;
		if (writer < 0)
			writer = ::open(path.c_str(), O_RDWR | O_NONBLOCK);
		if (writer < 0)
			return;
		std::string payload(0x20, ' ');
		payload.replace(0, sizeof("STALE-SCAN") - 1, "STALE-SCAN");
		size_t written = 0;
		while (written < payload.size())
		{
			const ssize_t count = ::write(writer, payload.data() + written, payload.size() - written);
			if (count < 0 && errno == EINTR)
				continue;
			if (count <= 0)
				break;
			written += count;
		}
		released = true;
	}

	Boxart& boxart;
	std::filesystem::path path;
	bool fifoCreated;
	int writer = -1;
	bool released = false;
	std::future<void> refresh;
};
#endif

class BoxartTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		oldDataPath = get_writable_data_path("");
		oldFetchBoxart = config::FetchBoxart;
		oldSourceMode = config::BoxartSourceMode;
		oldOnlineSource = config::BoxartOnlineSource;
		oldBoxartPath = config::BoxartPath;
		directory = std::filesystem::temp_directory_path()
				/ ("hollycast-boxart-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(directory);
		set_user_data_dir(directory.string());
		config::FetchBoxart = false;
		config::BoxartSourceMode = static_cast<int>(BoxartSourceMode::ScrapedOnly);
		config::BoxartOnlineSource = static_cast<int>(config::OnlineArtworkSource::TheGamesDB);
		config::BoxartPath = "";
		boxart = std::make_unique<Boxart>();
		media.name = "No artwork";
		media.fileName = "game.cue";
		media.path = (directory / media.fileName).string();
		writeDisc(directory, "TEST-0001", "Metadata survives");
	}

	void TearDown() override
	{
		boxart->term();
		boxart.reset();
		config::FetchBoxart = oldFetchBoxart;
		config::BoxartSourceMode = oldSourceMode;
		config::BoxartOnlineSource = oldOnlineSource;
		config::BoxartPath = oldBoxartPath;
		set_user_data_dir(oldDataPath);
		std::filesystem::remove_all(directory);
	}

	void writeDisc(const std::filesystem::path& location, const std::string& product, const std::string& title, bool withArtwork = false)
	{
		std::filesystem::create_directories(location);
		std::vector<u8> sectors(24 * 2048);
		ip_meta_t metadata{};
		std::memset(&metadata, ' ', sizeof(metadata));
		std::memcpy(metadata.hardware_id, "SEGA SEGAKATANA ", sizeof(metadata.hardware_id));
		std::memcpy(metadata.maker_id, "SEGA ENTERPRISES", sizeof(metadata.maker_id));
		std::memcpy(metadata.product_number, product.data(), std::min(product.size(), sizeof(metadata.product_number)));
		std::memcpy(metadata.software_name, title.data(), std::min(title.size(), sizeof(metadata.software_name)));
		std::memcpy(metadata.area_symbols, "JUE     ", sizeof(metadata.area_symbols));
		std::memcpy(sectors.data(), &metadata, sizeof(metadata));
		u8 *pvd = sectors.data() + 16 * 2048;
		pvd[0] = 1;
		std::memcpy(pvd + 1, "CD001", 5);
		pvd[6] = 1;
		pvd[156] = 34;
		pvd[158] = 20;
		pvd[165] = 20;
		pvd[167] = 8;
		pvd[172] = 8;
		pvd[181] = 2;
		if (withArtwork)
		{
			const std::string name = "0GDTEX.PVR;1";
			u8 *entry = sectors.data() + 20 * 2048;
			entry[0] = 33 + name.size();
			entry[2] = entry[9] = 21;
			entry[10] = entry[17] = 16 + 8 * 8 * 2;
			entry[32] = name.size();
			std::memcpy(entry + 33, name.data(), name.size());
			u8 *pvr = sectors.data() + 21 * 2048;
			std::memcpy(pvr, "PVRT", 4);
			pvr[4] = 8 + 8 * 8 * 2;
			pvr[8] = 1;
			pvr[9] = 9;
			pvr[12] = pvr[14] = 8;
			for (int pixel = 0; pixel < 8 * 8; ++pixel)
			{
				pvr[16 + pixel * 2] = 0xe0;
				pvr[17 + pixel * 2] = 0x07;
			}
		}
		std::ofstream track(location / "track.bin", std::ios::binary);
		track.write(reinterpret_cast<const char *>(sectors.data()), sectors.size());
		std::ofstream cue(location / "game.cue");
		cue << "FILE \"track.bin\" BINARY\nTRACK 01 MODE1/2048\nINDEX 01 00:00:00\n";
	}

	GameBoxart finishOfflineFetch()
	{
		return finishOfflineFetch(media);
	}

	GameBoxart finishOfflineFetch(const GameMedia& game)
	{
		boxart->getBoxartAndLoad(game);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		GameBoxart art;
		do {
			art = boxart->getBoxartAndLoad(game);
			if (!art.busy)
				return art;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < deadline);
		ADD_FAILURE() << "Offline artwork fetch did not finish";
		return art;
	}

	void restart()
	{
		boxart->term();
		boxart = std::make_unique<Boxart>();
	}

	json readDatabase()
	{
		std::ifstream file(directory / "boxart" / "flycast-gamedb.json");
		json database;
		file >> database;
		return database;
	}

	void writeDatabase(const json& database)
	{
		std::filesystem::create_directories(directory / "boxart");
		std::ofstream file(directory / "boxart" / "flycast-gamedb.json");
		file << database.dump();
	}

	void replaceTrackByte(size_t offset, char value)
	{
		std::fstream track(directory / "track.bin", std::ios::binary | std::ios::in | std::ios::out);
		track.seekp(offset);
		track.put(value);
	}

	std::filesystem::path directory;
	GameMedia media;
	std::unique_ptr<Boxart> boxart;
	std::string oldDataPath;
	bool oldFetchBoxart;
	int oldSourceMode;
	int oldOnlineSource;
	std::string oldBoxartPath;
};

TEST_F(BoxartTest, RenamedDiscRestoresLaunchBoxCoverAcrossRestart)
{
	const GameBoxart offline = finishOfflineFetch();
	const std::string image = (directory / "boxart" / "launchbox.png").string();
	std::ofstream(image) << "cached cover";
	const json matches = {{LaunchBoxCatalog::identity(offline), {
		{"id", 2416}, {"name", "Matched game"}, {"url", "https://images.launchbox-app.com/cover.png"}, {"path", image}}}};
	std::ofstream(directory / "boxart" / "launchbox-matches.json")
		<< json({{"version", 1}, {"matches", matches}}).dump();
	config::BoxartOnlineSource = static_cast<int>(config::OnlineArtworkSource::LaunchBox);
	config::FetchBoxart = true;
	boxart->invalidateOfflineArtwork();
	const GameBoxart original = finishOfflineFetch();
	ASSERT_EQ(2416, original.launchboxId);
	ASSERT_EQ(image, original.boxartPath);
	restart();
	std::filesystem::rename(media.path, directory / "A renamed game.cue");
	media.fileName = "A renamed game.cue";
	media.name = "An arbitrary display name";
	media.path = (directory / media.fileName).string();
	const GameBoxart renamed = finishOfflineFetch();
	EXPECT_EQ("TEST-0001", renamed.uniqueId);
	EXPECT_EQ("Metadata survives", renamed.searchName);
	EXPECT_EQ(2416, renamed.launchboxId);
	EXPECT_EQ(image, renamed.boxartPath);
	EXPECT_EQ(static_cast<int>(config::OnlineArtworkSource::LaunchBox), renamed.scrapedProvider);
	restart();
	EXPECT_EQ(2416, boxart->getBoxart(media).launchboxId);
}

TEST_F(BoxartTest, SwitchingProviderRearmsExistingOnlineResult)
{
	finishOfflineFetch();
	json database = readDatabase();
	database[0]["scraped"] = true;
	database[0]["scraped_provider"] = 0;
	const std::string image = (directory / "old.png").string();
	std::ofstream(image) << "previous artwork";
	database[0]["boxart_path"] = image;
	database[0]["boxart_url"] = "https://example.test/old.png";
	writeDatabase(database);
	restart();
	config::BoxartOnlineSource = static_cast<int>(config::OnlineArtworkSource::LaunchBox);
	boxart->queueBoxart(media);
	const GameBoxart pending = boxart->getBoxart(media);
	EXPECT_FALSE(pending.scraped);
	EXPECT_EQ(image, pending.boxartPath);
	EXPECT_EQ(OfflineArtState::NoArtwork, pending.offlineArtState);
}

TEST_F(BoxartTest, NoArtworkDoesNotRepeatOfflineFetchAndPreservesMetadata)
{
	const GameBoxart art = finishOfflineFetch();
	ASSERT_EQ(OfflineArtState::NoArtwork, art.offlineArtState);
	ASSERT_FALSE(art.busy);
	EXPECT_EQ("TEST-0001", art.uniqueId);
	EXPECT_EQ("Metadata survives", art.searchName);
	EXPECT_EQ(GameBoxart::JAPAN | GameBoxart::USA | GameBoxart::EUROPE, art.region);
	EXPECT_TRUE(art.boxartPath.empty());

	for (int request = 0; request < 10; ++request)
	{
		const GameBoxart cached = boxart->getBoxartAndLoad(media);
		ASSERT_FALSE(cached.busy) << "Completed no-art result queued another offline fetch";
		EXPECT_EQ("TEST-0001", cached.uniqueId);
		EXPECT_EQ("Metadata survives", cached.searchName);
	}
}

TEST_F(BoxartTest, DatabaseReviewRetriesOfflineArtwork)
{
	ASSERT_EQ("TEST-0001", finishOfflineFetch().uniqueId);
	writeDisc(directory, "TEST-0002", "Replacement metadata");
	boxart->refreshCache();
	const GameBoxart refreshed = finishOfflineFetch();
	EXPECT_EQ("TEST-0002", refreshed.uniqueId);
	EXPECT_EQ("Replacement metadata", refreshed.searchName);
}

TEST_F(BoxartTest, RefreshDiscardsInFlightNoArtworkResult)
{
#ifdef _WIN32
	GTEST_SKIP() << "Requires a POSIX FIFO to pause the physical artwork scan";
#else
	media.fileName = "paused.bin";
	media.path = (directory / media.fileName).string();
	media.arcade = true;
	PausedArcadeScan scan(*boxart, media.path);
	ASSERT_TRUE(scan.created());
	ASSERT_TRUE(boxart->getBoxartAndLoad(media).busy);
	ASSERT_TRUE(scan.waitForReader()) << "Physical artwork worker did not open the FIFO";

	scan.startRefresh();
	bool cancelled = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	do {
		if (!boxart->getBoxart(media).busy)
		{
			cancelled = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	scan.finishRefresh();

	const GameBoxart refreshed = boxart->getBoxart(media);
	EXPECT_TRUE(refreshed.uniqueId.empty()) << "Refresh published stale metadata " << refreshed.uniqueId;
	EXPECT_FALSE(refreshed.busy);
	EXPECT_TRUE(cancelled) << "Refresh did not cancel the in-flight artwork request";

	ASSERT_TRUE(std::filesystem::remove(media.path));
	std::ofstream replacement(media.path, std::ios::binary);
	const std::string prefix(0x30, '\0');
	std::string payload(0x20, ' ');
	payload.replace(0, sizeof("FRESH-SCAN") - 1, "FRESH-SCAN");
	replacement.write(prefix.data(), prefix.size());
	replacement.write(payload.data(), payload.size());
	replacement.close();
	ASSERT_TRUE(replacement);
	const GameBoxart fresh = finishOfflineFetch();
	EXPECT_EQ("FRESH-SCAN", fresh.uniqueId);
	EXPECT_FALSE(fresh.busy);
#endif
}

TEST_F(BoxartTest, NoArtworkSurvivesRestartUntilRefresh)
{
	ASSERT_EQ(OfflineArtState::NoArtwork, finishOfflineFetch().offlineArtState);
	restart();
	const json database = readDatabase();
	ASSERT_EQ(1u, database.size());
	EXPECT_EQ("no_artwork", database[0]["offline_art_state"]);
	EXPECT_EQ(true, database[0]["parsed"]);
	EXPECT_EQ(media.path, database[0]["game_path"]);
	writeDisc(directory, "TEST-0002", "Changed disc");
	const GameBoxart cached = boxart->getBoxartAndLoad(media);
	EXPECT_FALSE(cached.busy);
	EXPECT_EQ(OfflineArtState::NoArtwork, cached.offlineArtState);
	EXPECT_EQ("TEST-0001", cached.uniqueId);
}

TEST_F(BoxartTest, ExplicitLibraryInvalidationRearmsNoArtwork)
{
	ASSERT_EQ("TEST-0001", finishOfflineFetch().uniqueId);
	writeDisc(directory, "TEST-0002", "Changed disc");
	boxart->invalidateOfflineArtwork();
	const GameBoxart refreshed = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::NoArtwork, refreshed.offlineArtState);
	EXPECT_EQ("TEST-0002", refreshed.uniqueId);
	EXPECT_EQ("Changed disc", refreshed.searchName);
	for (int frame = 0; frame < 10; ++frame)
		EXPECT_FALSE(boxart->getBoxartAndLoad(media).busy);
}

TEST_F(BoxartTest, InvalidationSurvivesImmediateRestart)
{
	ASSERT_EQ(OfflineArtState::NoArtwork, finishOfflineFetch().offlineArtState);
	restart();
	ASSERT_EQ(OfflineArtState::NoArtwork, boxart->getBoxart(media).offlineArtState);
	writeDisc(directory, "TEST-0002", "Changed disc");
	boxart->invalidateOfflineArtwork();
	restart();
	const GameBoxart refreshed = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::NoArtwork, refreshed.offlineArtState);
	EXPECT_EQ("TEST-0002", refreshed.uniqueId);
	EXPECT_EQ("Changed disc", refreshed.searchName);
}

TEST_F(BoxartTest, DuplicateBasenamesKeepIndependentSourceResultsAcrossRestart)
{
	ASSERT_EQ("TEST-0001", finishOfflineFetch().uniqueId);
	const auto otherDirectory = directory / "other";
	writeDisc(otherDirectory, "TEST-0002", "Other disc");
	GameMedia other = media;
	other.path = (otherDirectory / "game.cue").string();
	ASSERT_EQ("TEST-0002", finishOfflineFetch(other).uniqueId);
	restart();
	for (int frame = 0; frame < 10; ++frame)
	{
		const GameBoxart first = boxart->getBoxartAndLoad(media);
		const GameBoxart second = boxart->getBoxartAndLoad(other);
		EXPECT_FALSE(first.busy);
		EXPECT_FALSE(second.busy);
		EXPECT_EQ("TEST-0001", first.uniqueId);
		EXPECT_EQ("TEST-0002", second.uniqueId);
	}
	EXPECT_EQ(2u, readDatabase().size());
}

TEST_F(BoxartTest, LegacyNegativeRetriesOnceAndMigratesToSourceRecord)
{
	writeDatabase(json::array({{
		{"file_name", media.fileName}, {"name", media.name},
		{"unique_id", "OLD-ID"}, {"parsed", true}, {"scraped", false}
	}}));
	const GameBoxart retried = finishOfflineFetch();
	ASSERT_EQ(OfflineArtState::NoArtwork, retried.offlineArtState);
	EXPECT_EQ("TEST-0001", retried.uniqueId);
	restart();
	const json database = readDatabase();
	ASSERT_EQ(1u, database.size());
	EXPECT_EQ(media.path, database[0]["game_path"]);
	EXPECT_EQ("no_artwork", database[0]["offline_art_state"]);
	writeDisc(directory, "TEST-0002", "Changed disc");
	const GameBoxart cached = boxart->getBoxartAndLoad(media);
	EXPECT_FALSE(cached.busy);
	EXPECT_EQ("TEST-0001", cached.uniqueId);
}

TEST_F(BoxartTest, InvalidHeaderStopsPollingButRetriesAfterRestart)
{
	replaceTrackByte(0, 0);
	const GameBoxart failed = finishOfflineFetch();
	ASSERT_EQ(OfflineArtState::Failed, failed.offlineArtState);
	EXPECT_TRUE(failed.uniqueId.empty());
	writeDisc(directory, "TEST-0002", "Recovered disc");
	for (int frame = 0; frame < 10; ++frame)
	{
		const GameBoxart cached = boxart->getBoxartAndLoad(media);
		EXPECT_FALSE(cached.busy);
		EXPECT_EQ(OfflineArtState::Failed, cached.offlineArtState);
	}
	restart();
	const json database = readDatabase();
	ASSERT_EQ(1u, database.size());
	EXPECT_EQ("pending", database[0]["offline_art_state"]);
	EXPECT_EQ(false, database[0]["parsed"]);
	EXPECT_EQ("TEST-0002", finishOfflineFetch().uniqueId);
}

TEST_F(BoxartTest, InvalidIsoPreservesMetadataAndRefreshRetries)
{
	replaceTrackByte(16 * 2048, 0);
	const GameBoxart failed = finishOfflineFetch();
	ASSERT_EQ(OfflineArtState::Failed, failed.offlineArtState);
	EXPECT_EQ("TEST-0001", failed.uniqueId);
	EXPECT_EQ("Metadata survives", failed.searchName);
	EXPECT_EQ(GameBoxart::JAPAN | GameBoxart::USA | GameBoxart::EUROPE, failed.region);
	writeDisc(directory, "TEST-0002", "Recovered ISO");
	EXPECT_FALSE(boxart->getBoxartAndLoad(media).busy);
	boxart->invalidateOfflineArtwork();
	const GameBoxart recovered = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::NoArtwork, recovered.offlineArtState);
	EXPECT_EQ("TEST-0002", recovered.uniqueId);
}

TEST_F(BoxartTest, PhysicalOnlyNoArtworkPersistsAndRefreshRearms)
{
	config::BoxartSourceMode = static_cast<int>(BoxartSourceMode::PhysicalMediaOnly);
	const GameBoxart first = boxart->getBoxartAndLoad(media);
	ASSERT_EQ(OfflineArtState::NoArtwork, first.offlineArtState);
	EXPECT_EQ("TEST-0001", first.uniqueId);
	restart();
	writeDisc(directory, "TEST-0002", "Changed physical disc");
	EXPECT_EQ("TEST-0001", boxart->getBoxartAndLoad(media).uniqueId);
	boxart->invalidateOfflineArtwork();
	const GameBoxart refreshed = boxart->getBoxartAndLoad(media);
	EXPECT_EQ(OfflineArtState::NoArtwork, refreshed.offlineArtState);
	EXPECT_EQ("TEST-0002", refreshed.uniqueId);
}

TEST_F(BoxartTest, EmptyIsoRootIsNotPersistedAsNoArtwork)
{
	replaceTrackByte(16 * 2048 + 167, 0);
	replaceTrackByte(16 * 2048 + 172, 0);
	const GameBoxart failed = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::Failed, failed.offlineArtState);
	EXPECT_EQ("TEST-0001", failed.uniqueId);
	restart();
	EXPECT_EQ("pending", readDatabase()[0]["offline_art_state"]);
}

TEST_F(BoxartTest, IsoRootOutsideDiscIsNotPersistedAsNoArtwork)
{
	replaceTrackByte(16 * 2048 + 158, static_cast<char>(200));
	replaceTrackByte(16 * 2048 + 165, static_cast<char>(200));
	const GameBoxart failed = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::Failed, failed.offlineArtState);
	EXPECT_EQ("Metadata survives", failed.searchName);
	restart();
	EXPECT_EQ("pending", readDatabase()[0]["offline_art_state"]);
}

TEST_F(BoxartTest, UnreadableIsoRootDoesNotReportValid)
{
	std::unique_ptr<Disc> disc(OpenDisc(media.path));
	ASSERT_NE(nullptr, disc);
	std::filesystem::resize_file(directory / "track.bin", 19 * 2048);
	IsoFs filesystem(disc.get());
	bool valid = true;
	std::unique_ptr<IsoFs::Directory> root(filesystem.getRoot(&valid));
	EXPECT_FALSE(valid);
}

TEST_F(BoxartTest, RefreshFindsNewEmbeddedArtworkAndPreservesMetadata)
{
	ASSERT_EQ(OfflineArtState::NoArtwork, finishOfflineFetch().offlineArtState);
	writeDisc(directory, "TEST-0002", "Artwork added", true);
	boxart->invalidateOfflineArtwork();
	const GameBoxart recovered = finishOfflineFetch();
	ASSERT_EQ(OfflineArtState::Complete, recovered.offlineArtState);
	EXPECT_EQ("TEST-0002", recovered.uniqueId);
	EXPECT_EQ("Artwork added", recovered.searchName);
	int width = 0, height = 0, channels = 0;
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
			stbi_load(recovered.boxartPath.c_str(), &width, &height, &channels, 4), stbi_image_free);
	ASSERT_NE(nullptr, pixels);
	EXPECT_EQ(8, width);
	EXPECT_EQ(8, height);
	EXPECT_EQ(0, pixels.get()[0]);
	EXPECT_EQ(255, pixels.get()[1]);
	EXPECT_EQ(0, pixels.get()[2]);
	EXPECT_EQ(255, pixels.get()[3]);
	for (int frame = 0; frame < 10; ++frame)
		EXPECT_EQ(recovered.boxartPath, boxart->getBoxartAndLoad(media).boxartPath);
}

TEST_F(BoxartTest, FailedImageWriteRetainsMetadataAndCanRetry)
{
	writeDisc(directory, "TEST-0001", "Metadata survives", true);
	{
		std::ofstream blocked(directory / "blocked");
		blocked << "This file prevents creation of an artwork directory";
	}
	set_user_data_dir((directory / "blocked").string());
	const GameBoxart failed = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::Failed, failed.offlineArtState);
	EXPECT_EQ("TEST-0001", failed.uniqueId);
	EXPECT_EQ("Metadata survives", failed.searchName);
	EXPECT_TRUE(failed.boxartPath.empty());
	EXPECT_FALSE(boxart->getBoxartAndLoad(media).busy);
	set_user_data_dir(directory.string());
	std::filesystem::create_directories(directory / "boxart");
	boxart->invalidateOfflineArtwork();
	const GameBoxart recovered = finishOfflineFetch();
	EXPECT_EQ(OfflineArtState::Complete, recovered.offlineArtState);
	EXPECT_EQ("TEST-0001", recovered.uniqueId);
	EXPECT_TRUE(std::filesystem::exists(recovered.boxartPath));
}

TEST_F(BoxartTest, NoArtworkStillQueuesIndependentOnlineFallback)
{
	ASSERT_EQ(OfflineArtState::NoArtwork, finishOfflineFetch().offlineArtState);
	config::FetchBoxart = true;
	boxart->queueBoxart(media);
	const GameBoxart queued = boxart->getBoxart(media);
	EXPECT_TRUE(queued.busy);
	EXPECT_EQ(OfflineArtState::NoArtwork, queued.offlineArtState);
	EXPECT_EQ("TEST-0001", queued.uniqueId);
	EXPECT_EQ("Metadata survives", queued.searchName);
	EXPECT_FALSE(queued.scraped);
	config::FetchBoxart = false;
}

TEST_F(BoxartTest, EsdeMediaPrefersCanonicalFoldersOverLegacyAliases)
{
	namespace fs = std::filesystem;
	const fs::path root = directory / "downloaded_media";
	// Windows produces backslash paths here, matching a folder chosen by the user.
	config::BoxartPath = root.string();
	media.fileName = "A Game (USA).chd";
	media.name = "A different display title";
	media.gameName.clear();
	for (const char* folder : {"images", "miximg", "miximages", "cover", "covers", "screenshotS",
			"wheel", "marquees", "3dboxes", "physicalmedia", "titlescreens", "box-2d-side", "spines"})
	{
		fs::create_directories(root / folder);
		std::ofstream(root / folder / "A Game (USA).png", std::ios::binary) << folder;
	}
	boxart->refreshCustomBoxartIndex(true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	std::string cover;
	do {
		cover = boxart->getCustomMediaPath(media, config::LibraryCoverMediaMode::Cover);
		if (!cover.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < deadline);
	ASSERT_FALSE(cover.empty()) << "The configured media directory was not indexed";
	EXPECT_EQ(fs::path(cover).lexically_normal(), (root / "covers" / "A Game (USA).png").lexically_normal());
	const std::pair<config::LibraryCoverMediaMode, const char*> expected[] = {
		{config::LibraryCoverMediaMode::MixImage, "miximages"},
		{config::LibraryCoverMediaMode::Screenshot, "screenshotS"},
		{config::LibraryCoverMediaMode::Title, "marquees"},
		{config::LibraryCoverMediaMode::Case, "3dboxes"},
		{config::LibraryCoverMediaMode::Physical, "physicalmedia"},
		{config::LibraryCoverMediaMode::TitleScreen, "titlescreens"},
		{config::LibraryCoverMediaMode::Spine, "spines"}
	};
	for (const auto& source : expected)
		EXPECT_EQ(fs::path(boxart->getCustomMediaPath(media, source.first)).lexically_normal(),
				(root / source.second / "A Game (USA).png").lexically_normal());
	// A missing category must stay empty, rather than borrowing an old default image.
	EXPECT_TRUE(boxart->getCustomMediaPath(media, config::LibraryCoverMediaMode::FanArt).empty());
	media.fileName = "Not scraped.chd";
	EXPECT_TRUE(boxart->getCustomMediaPath(media, config::LibraryCoverMediaMode::Cover).empty());
}

}
