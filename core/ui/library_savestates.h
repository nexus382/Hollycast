// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "emulator.h"
#include "util/worker_thread.h"
#include <atomic>
#include <functional>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Owned by the UI thread. The worker receives copies of the resolver and paths,
// so scans never borrow the scanner's list or mutable emulator configuration.
class LibrarySavestateCache
{
public:
	using Resolver = std::function<SavestateInfo(const std::string&, const std::string&)>;
	struct ScanRequest { std::string path, fileName; bool device = false; };
	using ScanResult = std::pair<std::string, SavestateInfo>;
	using ScanBatch = std::vector<ScanResult>;
	struct StateChanges { u64 revision = 0; bool fullRefresh = false; std::vector<std::string> paths; };

	~LibrarySavestateCache() { stop(); }

	void reset(Resolver resolver)
	{
		generation.fetch_add(1);
		++revision;
		fullRefreshRevision = revision;
		lastVisibleChangeRevision = 0;
		lastVisibleChangePaths.clear();
		states.clear();
		scanIndex = 0;
		this->resolver = std::move(resolver);
	}

	void setScanGames(std::vector<ScanRequest> games)
	{
		generation.fetch_add(1);
		scanGames = std::move(games);
		scanIndex = 0;
	}

	// Queue a bounded batch so large libraries do not need one UI frame per game.
	void advance()
	{
		pollPending();
		if (pending.valid() || !resolver)
			return;

		std::vector<ScanRequest> batch;
		batch.reserve(32);
		std::unordered_set<std::string> queuedPaths;
		while (scanIndex < scanGames.size() && batch.size() < 32)
		{
			ScanRequest request = scanGames[scanIndex++];
			if (!request.device && !request.path.empty() && states.find(request.path) == states.end()
					&& queuedPaths.insert(request.path).second)
				batch.push_back(std::move(request));
		}
		if (!batch.empty())
			start(std::move(batch));
	}

	const SavestateInfo& get(const std::string& path, const std::string& fileName)
	{
		pollPending();
		auto found = states.find(path);
		if (found != states.end())
			return found->second;
		if (!pending.valid() && !path.empty() && resolver)
		{
			start({ScanRequest{path, fileName, false}});
		}
		static const SavestateInfo empty;
		return empty;
	}

	const SavestateInfo& peek(const std::string& path) const
	{
		auto found = states.find(path);
		if (found != states.end())
			return found->second;
		static const SavestateInfo empty;
		return empty;
	}

	bool busy() const { return pending.valid(); }
	bool has(const std::string& path) const { return states.find(path) != states.end(); }
	u64 getRevision() const { return revision; }

	StateChanges changesSince(u64 previousRevision) const
	{
		StateChanges changes;
		changes.revision = revision;
		if (previousRevision >= revision)
			return changes;
		if (previousRevision < fullRefreshRevision)
		{
			changes.fullRefresh = true;
			return changes;
		}
		if (lastVisibleChangeRevision > previousRevision)
		{
			if (lastVisibleChangeRevision != previousRevision + 1)
				changes.fullRefresh = true;
			else
				changes.paths = lastVisibleChangePaths;
		}
		return changes;
	}

	void stop()
	{
		generation.fetch_add(1);
		worker.stop();
		pending = {};
		states.clear();
		++revision;
		fullRefreshRevision = revision;
	}

private:
	void start(std::vector<ScanRequest> requests)
	{
		pendingGeneration = generation.load();
		const u64 requestGeneration = pendingGeneration;
		const Resolver resolve = resolver;
		pending = worker.runFuture([this, requests = std::move(requests), resolve, requestGeneration]() {
			ScanBatch results;
			results.reserve(requests.size());
			for (const ScanRequest& request : requests)
			{
				// A folder refresh or scanner replacement cancels the rest of this batch.
				if (generation.load() != requestGeneration)
					break;
				SavestateInfo state;
				try {
					state = resolve(request.path, request.fileName);
				} catch (const std::exception& e) {
					WARN_LOG(COMMON, "Cannot inspect library save state: %s", e.what());
				}
				results.emplace_back(request.path, std::move(state));
			}
			return results;
		});
	}

	void pollPending()
	{
		if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
			return;

		ScanBatch results;
		try {
			results = pending.get();
		} catch (const std::exception& e) {
			WARN_LOG(COMMON, "Cannot inspect library save states: %s", e.what());
		}
		// A folder change or refresh must not publish an older scan's results.
		if (pendingGeneration != generation.load())
			return;
		bool visibleStateChanged = false;
		std::vector<std::string> visiblePaths;
		for (auto& result : results)
		{
			auto inserted = states.emplace(std::move(result.first), std::move(result.second));
			if (inserted.second && inserted.first->second.creationDate > 0)
			{
				visibleStateChanged = true;
				visiblePaths.push_back(inserted.first->first);
			}
		}
		if (visibleStateChanged)
		{
			++revision;
			lastVisibleChangeRevision = revision;
			lastVisibleChangePaths = std::move(visiblePaths);
		}
	}

	WorkerThread worker{"LibrarySaveStates"};
	Resolver resolver;
	std::unordered_map<std::string, SavestateInfo> states;
	std::vector<ScanRequest> scanGames;
	std::future<ScanBatch> pending;
	std::atomic<u64> generation{0};
	u64 pendingGeneration = 0;
	u64 revision = 0;
	u64 fullRefreshRevision = 0;
	u64 lastVisibleChangeRevision = 0;
	std::vector<std::string> lastVisibleChangePaths;
	size_t scanIndex = 0;
};
