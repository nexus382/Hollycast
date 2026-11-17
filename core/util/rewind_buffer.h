// SPDX-License-Identifier: GPL-2.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

// Bounded LIFO history of compressed full states. Not thread-safe: every
// call, including size()/memoryUsed(), must stay on the history owner's
// (rendering) thread. push()/pop() return false for invalid or over-budget
// input and throw std::runtime_error on zlib failure. compress() is the one
// pure exception: it touches no instance state and may run on a worker.
class RewindBuffer
{
public:
	static constexpr std::size_t MaxStateSize = 128U * 1024U * 1024U;

	// compressionLevel is a zlib level (1 fastest, 9 maximum). It is part of
	// the configuration identity: changing capture settings clears history.
	void configure(std::size_t maxBytes, std::size_t maxStates, int compressionLevel = 1);
	// Timestamps must be monotonic; an older timestamp is clamped up to the
	// newest stored one so discardOlderThan() can evict from the front.
	bool push(const std::uint8_t* data, std::size_t size, std::uint64_t timestamp = 0);
	// Stores a payload produced by compress() without compressing again.
	bool pushCompressed(std::vector<std::uint8_t>&& compressed, std::size_t uncompressedSize, std::uint64_t timestamp = 0);
	// When currentTimestamp matches the newest state, pop selects its predecessor
	// if available. The selected state and any newer state are removed only after
	// a successful decode; on failure history is retained and output is unspecified.
	bool pop(std::vector<std::uint8_t>& output,
			std::optional<std::uint64_t> currentTimestamp = std::nullopt);
	void discardOlderThan(std::uint64_t timestamp);
	void clear();

	static std::vector<std::uint8_t> compress(const std::uint8_t* data, std::size_t size, int compressionLevel);

	std::size_t size() const;
	// Counts retained compressed payload bytes, excluding container bookkeeping.
	std::size_t memoryUsed() const;

private:
	struct State
	{
		std::vector<std::uint8_t> compressedData;
		std::size_t uncompressedSize = 0;
		std::uint64_t timestamp = 0;
	};

	void evictOldest();

	std::deque<State> states_;
	std::size_t maxBytes_ = 0;
	std::size_t maxStates_ = 0;
	std::size_t memoryUsed_ = 0;
	int compressionLevel_ = 1;
};
