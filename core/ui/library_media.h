// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "ui/boxart/boxart.h"
#include "ui/game_scanner.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
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

	struct GameRequest
	{
		GameMedia game;
		u32 media;
		u32 manualMedia;
		Boxart::MediaProgress outcome;
	};

	std::vector<GameRequest> games;
	Boxart::MediaProgress progress;
	std::string label;
	size_t cursor = 0;
	bool checked = false, notified = false;
	double nextCheck = 0, finishedAt = 0;

	void rebuild(std::vector<GameMedia> sources, u32 automaticMedia, std::string label,
			u32 manualMedia = 0, const std::string& queuedLabel = {})
	{
		std::unordered_map<std::string, u32> previousManualMedia;
		// Keep a manual library selection attached to the disc sources present when
		// it was requested, including during partial inventory and cache refresh.
		if (std::any_of(games.begin(), games.end(), [](const GameRequest& request) {
			return (request.manualMedia & request.outcome.pendingMedia) != 0;
		}))
			for (const auto& request : games)
				if (request.manualMedia)
					previousManualMedia.emplace(request.game.path, request.manualMedia);

		this->label = std::move(label);
		notified = false;
		games.clear();
		games.reserve(sources.size());
		progress = {};
		cursor = 0;
		checked = false;
		nextCheck = 0;
		finishedAt = 0;

		automaticMedia |= artwork::metadataMedia;
		manualMedia &= artwork::allMedia;
		for (auto& source : sources)
		{
			const auto previous = previousManualMedia.find(source.path);
			const u32 retained = previous == previousManualMedia.end() ? 0 : previous->second;
			if (!queuedLabel.empty() && (retained & ~(automaticMedia | manualMedia)))
				this->label = queuedLabel;
			const u32 selected = manualMedia | retained;
			const u32 requested = automaticMedia | selected;
			size_t units = 1; // Metadata is always inventoried.
			for (size_t i = 0; i < artwork::mediaTypes.size(); ++i)
				if (requested & (1u << i)) ++units;
			Boxart::MediaProgress outcome;
			outcome.total = outcome.pending = units;
			outcome.pendingMedia = requested;
			games.push_back({std::move(source), requested, selected, outcome});
			progress.total += units;
			progress.pending += units;
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
			auto& request = games[cursor];
			auto& prior = request.outcome;
			const bool firstInventory = !checked;
			const u32 requestedMedia = prior.pendingMedia;
			Boxart::MediaProgress current;
			if (requestedMedia)
				current = boxart.mediaProgress(request.game, requestedMedia);

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
			const u32 queueMedia = firstInventory ? request.media : current.pendingMedia;
			if (queueMedia)
			{
				boxart.queueBoxart(request.game, queueMedia);
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
