// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "ui/boxart/boxart.h"
#include "ui/game_scanner.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace library {

// UI-thread owned media inventory. Boxart owns and deduplicates per-source work.
// Totals count image/metadata categories, not HTTP requests or physical disc reads.
struct MediaBatch
{
	struct AdvanceResult
	{
		size_t processed = 0;
		bool completedPass = false;
		bool notifyPending = false;
	};

	std::vector<GameMedia> games;
	std::vector<Boxart::MediaProgress> outcomes;
	Boxart::MediaProgress progress;
	u32 mask = 0;
	std::string label;
	size_t cursor = 0;
	bool checked = false, notified = false;
	double nextCheck = 0, finishedAt = 0;

	void reset(std::vector<GameMedia> games, u32 requestedMedia, std::string label)
	{
		this->games = std::move(games);
		this->mask = requestedMedia | artwork::metadataMedia;
		this->label = std::move(label);
		progress = {};
		cursor = 0;
		checked = false;
		notified = false;
		nextCheck = 0;
		finishedAt = 0;

		size_t units = 1; // Metadata is always inventoried.
		for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
			if (mask & (1u << i)) ++units;
		progress.total = progress.pending = units * this->games.size();
		outcomes.assign(this->games.size(), {});
		for (auto& outcome : outcomes)
		{
			outcome.total = outcome.pending = units;
			outcome.pendingMedia = mask;
		}
	}

	AdvanceResult advance(Boxart& boxart, size_t maxItems = 16,
			std::chrono::milliseconds timeBudget = std::chrono::milliseconds(2))
	{
		AdvanceResult result;
		if (games.empty() || maxItems == 0)
			return result;

		const auto deadline = std::chrono::steady_clock::now() + timeBudget;
		// A single filesystem call cannot be preempted; bound the rest of each slice.
		const size_t end = std::min(cursor + maxItems, games.size());
		while (cursor < end)
		{
			auto& prior = outcomes[cursor];
			const bool firstInventory = !checked;
			const u32 requestedMedia = firstInventory ? mask : prior.pendingMedia;
			Boxart::MediaProgress current;
			if (requestedMedia)
				current = boxart.mediaProgress(games[cursor], requestedMedia);

			progress.ready += current.ready;
			progress.pending = progress.pending - prior.pending + current.pending;
			progress.unavailable += current.unavailable;
			prior.ready += current.ready;
			prior.unavailable += current.unavailable;
			prior.pending = current.pending;
			prior.pendingMedia = current.pendingMedia;

			// The first request also schedules offline disc parsing when every online
			// category is already complete. Re-offer outstanding bits on later polls;
			// Boxart suppresses duplicates while a request is active.
			const u32 queueMedia = firstInventory ? mask : current.pendingMedia;
			if (queueMedia)
			{
				boxart.queueBoxart(games[cursor], queueMedia);
				if (current.pendingMedia && !notified)
				{
					result.notifyPending = true;
					notified = true;
				}
			}

			++cursor;
			++result.processed;
			if (std::chrono::steady_clock::now() >= deadline)
				break;
		}

		if (cursor == games.size())
		{
			cursor = 0;
			checked = true;
			result.completedPass = true;
		}
		return result;
	}
};

} // namespace library
