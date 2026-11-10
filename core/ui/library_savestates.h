// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "emulator.h"
#include "util/worker_thread.h"
#include <functional>
#include <unordered_map>

// Owned by the UI thread. The worker receives copies of the resolver and paths,
// so scans never borrow the scanner's list or mutable emulator configuration.
class LibrarySavestateCache
{
public:
	using Resolver = std::function<SavestateInfo(const std::string&, const std::string&)>;

	~LibrarySavestateCache() { stop(); }

	void reset(Resolver resolver)
	{
		++generation;
		states.clear();
		this->resolver = std::move(resolver);
	}

	const SavestateInfo& get(const std::string& path, const std::string& fileName)
	{
		if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
		{
			SavestateInfo state;
			try {
				state = pending.get();
			} catch (const std::exception& e) {
				WARN_LOG(COMMON, "Cannot inspect library save state: %s", e.what());
			}
			// A folder change or refresh must not publish an older scan's result.
			if (pendingGeneration == generation)
				states.emplace(pendingPath, std::move(state));
		}
		auto found = states.find(path);
		if (found != states.end())
			return found->second;
		if (!pending.valid() && !path.empty() && resolver)
		{
			pendingPath = path;
			pendingGeneration = generation;
			// One game at a time bounds queued work when the user changes views.
			pending = worker.runFuture(resolver, path, fileName);
		}
		static const SavestateInfo empty;
		return empty;
	}

	bool busy() const { return pending.valid(); }

	void stop()
	{
		worker.stop();
		pending = {};
		states.clear();
	}

private:
	WorkerThread worker{"LibrarySaveStates"};
	Resolver resolver;
	std::unordered_map<std::string, SavestateInfo> states;
	std::future<SavestateInfo> pending;
	std::string pendingPath;
	u64 generation = 0;
	u64 pendingGeneration = 0;
};
