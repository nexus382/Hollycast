/*
	Copyright 2022 flyinghead
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
#pragma once
#include "scraper.h"
#include "artwork_media.h"
#include "stdclass.h"
#include "cfg/option.h"

#include <future>
#include <memory>
#include <mutex>
#include <array>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>
#include <chrono>
#include <atomic>
#include <condition_variable>

struct GameMedia;
namespace screenscraper { class Session; }

enum class BoxartSourceMode
{
	ScrapedOnly = 0,  // Only use scraped artwork from the box art database / online scraper
	PhysicalMediaOnly = 1, // Shows the Physical media image of the disc on the Rom
	CustomThenScraped = 2, // Use custom boxart, and then when no customs img is available, use scrapped media.
};

//! Singleton class responsible for managing boxart and playtime information.
class Boxart
{
public:
	GameBoxart getBoxartAndLoad(const GameMedia& media, bool startFetch = true);
	GameBoxart getBoxart(const GameMedia& media);
	std::string getLibraryCoverMediaPath(const GameMedia& media);
	std::string getCustomMediaPath(const GameMedia& media, config::LibraryCoverMediaMode mediaMode, bool verifyLocal = false);
	// Art of the game's disc: its physical media if known, otherwise the label on the disc
	// itself. Empty until that has been read, which happens in the background.
	std::string getDiscArtPath(const GameMedia& media);
	void term();
	void refreshCache();
	void invalidateOfflineArtwork();
	void refreshLibraryPlaytimeDatabase();
	void startPlaytime(const std::string& gameId, const std::string& gamePath);
	void resumePlaytime();
	void checkpointPlaytime(bool isPaused);
	void refreshCustomBoxartIndex(bool force = false);
	// The baseline is covers, hover miximages, title logos and metadata. Theme requirements add image bits.
	void queueBoxart(const GameMedia& media, u32 requestedMedia = artwork::basicMedia);
	void startFetch();
	int activeMediaProvider() const { return activeProvider.load(); }
	bool customMediaReady();
	bool customMediaFailed();
	struct MediaProgress {
		size_t total = 0, ready = 0, pending = 0, unavailable = 0;
		u32 pendingMedia = 0;
	};
	// Hold extra-media jobs while the UI incrementally inventories/enqueues a library.
	void setMediaBatchPending(bool pending);
	MediaProgress mediaProgress(const GameMedia& media, u32 requestedMedia);
	void retryArtworkMedia();
	struct DownloadProgress { size_t games = 0, covers = 0; };
	DownloadProgress downloadProgress();

private:
	friend class BoxartWorkTest_AcceptsOnlyCurrentSourceRevision_Test;
	friend class ScreenScraperQueueTest;

	enum class CustomIndexState
	{
		NotRequested,
		Building,
		Ready,
		Failed,
	};

	GameBoxart getPhysicalBoxart(const GameMedia& media);
	std::string getCustomBoxartPath(const GameMedia& media);
	std::string getCustomBoxartPathForMediaMode(const GameMedia& media, config::LibraryCoverMediaMode mediaMode,
			bool *indexReady = nullptr, bool verifyFile = false);
	GameBoxart getBoxartAndQueue(const GameMedia& media, bool startFetch, u32 requestedMedia = artwork::basicMedia);
	bool shouldFetchOnline(bool arcade = false) const;
	GameBoxart& gameForMedia(const GameMedia& media);
	bool isCurrentWork(const GameBoxart& item) const;
	//! @pre mutex must be locked. Merge requests added while a worker was running.
	void completeArtworkWork(GameBoxart& item, bool queueOnline, int provider);
	bool takeScreenScraperWork(GameBoxart& item, u64 generation, int provider = 1);
	void runScreenScraperQueue(std::shared_ptr<screenscraper::Session> session, u64 generation,
			const std::string& directory, std::unique_ptr<Scraper> launchbox = {}, std::unique_ptr<Scraper> tgdb = {});
	void loadDatabase();
	void loadLibraryPlaytimeDatabase();
	//! @pre playtimeMutex must be locked.
	void applyLibraryPlaytimeUnlocked(GameBoxart& boxart) const;
	void recoverDatabases(const std::string& databaseDir, const std::string& artworkDir);
	void reviewDatabaseArtwork();
	void saveDatabase();
	void markDatabaseDirty();
	void buildCustomBoxartIndex();
	std::string getDatabaseDirectory() const;
	std::string getSaveDirectory() const {
		return get_writable_data_path("/boxart/");
	}
	std::string getCustomBoxartDirectory() const {
		std::string path = config::BoxartPath.get();
		// File-system paths must end with a separator; Android SAF URIs must stay unchanged.
		if (!path.empty() && path.find("content://") != 0 && path.back() != '/' && path.back() != '\\')
			path += '/';
		return path;
	}
	void fetchBoxart();

	using CustomBoxartIndex = std::array<std::unordered_map<std::string, std::string>,
			static_cast<size_t>(config::LibraryCoverMediaMode::Count)>;
	std::unordered_map<std::string, GameBoxart> games;
	std::unordered_map<std::string, GameBoxart> physicalCache;

	// Independent of artwork refreshes; serialize access to all playtime data.
	std::mutex playtimeMutex;
	std::unordered_map<std::string, u64> libraryPlaytimeByGameId;
	bool playtimeLoaded = false;
	bool playtimeRunning = false;
	bool playtimeDirty = false;
	bool playtimeWritable = true;
	std::string playtimeGameId;
	std::string playtimeDatabasePath;
	std::chrono::steady_clock::time_point playtimeStart;
	std::chrono::steady_clock::time_point playtimeCheckpoint;
	std::chrono::steady_clock::duration playtimeRemainder{};
	std::future<bool> playtimeWrite;

	CustomBoxartIndex customBoxartByName;
	std::string customBoxartRoot;
	std::string requestedCustomBoxartRoot;
	std::mutex mutex;
	std::unique_ptr<Scraper> scraper;
	int scraperProvider = -1;
	std::unique_ptr<Scraper> arcadeScraper;
	u64 nextWorkRevision = 0;
	bool databaseLoaded = false;
	bool databaseDirty = false;
	std::mutex databaseWriteMutex;
	// Incremented while mutex is held so a completed save never clears a newer update.
	u64 databaseGeneration = 0;
	bool customIndexLoaded = false;
	CustomIndexState customIndexState = CustomIndexState::NotRequested;
	u64 customIndexGeneration = 0;
	bool customIndexShuttingDown = false;

	std::vector<GameBoxart> toFetch;
	std::condition_variable artworkWake;
	std::atomic<u64> screenGeneration{0};
	std::atomic<int> activeProvider{-1};
	std::shared_ptr<screenscraper::Session> screenSessionSnapshot; // UI-thread owned.
	bool mediaBatchPending = false; // Protected by mutex; preserves cover priority during queue refill.
	bool physicalWorking = false; // Protected by mutex; producer can overlap network workers.
	size_t activeScreenJobs = 0; // Protected by mutex; keep idle workers alive until the queue drains.
	// Futures are destroyed first, while the queue's synchronization state still exists.
	std::future<void> physicalFetching;
	std::future<void> onlineFetching;
	std::future<void> screenFetching;
	std::future<void> customIndexFetching;

	std::map<std::string, std::string> discLabels; // By game path. Empty until read, or when there is none.
	std::vector<std::string> discLabelQueue;
	std::future<void> discLabelFetching;
	bool discLabelsShuttingDown = false;

	static constexpr char const *DB_NAME = "flycast-gamedb.json";
};

// Where a game's on-disc label is kept. A ".none" file beside it records a disc without one.
std::string discLabelCachePath(const std::string& saveDirectory, const std::string& gamePath);
