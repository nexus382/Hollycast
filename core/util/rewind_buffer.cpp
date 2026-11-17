// SPDX-License-Identifier: GPL-2.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "util/rewind_buffer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zlib.h>

void RewindBuffer::configure(std::size_t maxBytes, std::size_t maxStates, int compressionLevel)
{
	if (maxBytes_ == maxBytes && maxStates_ == maxStates && compressionLevel_ == compressionLevel)
		return;

	clear();
	maxBytes_ = maxBytes;
	maxStates_ = maxStates;
	compressionLevel_ = compressionLevel;
}

std::vector<std::uint8_t> RewindBuffer::compress(const std::uint8_t* data, std::size_t size, int compressionLevel)
{
	if (data == nullptr || size == 0 || size > MaxStateSize
			|| size > static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
		throw std::invalid_argument("Invalid rewind state size");

	const uLong sourceSize = static_cast<uLong>(size);
	const uLong bound = compressBound(sourceSize);
	if (bound == 0 || bound > static_cast<uLong>(std::numeric_limits<std::size_t>::max())
			|| bound > static_cast<uLong>(std::numeric_limits<uLongf>::max()))
		throw std::runtime_error("Invalid compressed rewind state bound");

	std::vector<Bytef> compressed(static_cast<std::size_t>(bound));
	uLongf compressedSize = static_cast<uLongf>(bound);
	const int result = compress2(compressed.data(), &compressedSize, reinterpret_cast<const Bytef*>(data), sourceSize,
								 compressionLevel);
	if (result != Z_OK)
		throw std::runtime_error("Failed to compress rewind state");
	if (compressedSize == 0 || compressedSize > bound)
		throw std::runtime_error("Invalid compressed rewind state size");

	// Keep only the produced bytes; the bound-sized tail is temporary.
	compressed.resize(static_cast<std::size_t>(compressedSize));
	return std::vector<std::uint8_t>(compressed.begin(), compressed.end());
}

bool RewindBuffer::push(const std::uint8_t* data, std::size_t size, std::uint64_t timestamp)
{
	if (data == nullptr || size == 0 || size > MaxStateSize || maxBytes_ == 0 || maxStates_ == 0)
		return false;
	return pushCompressed(compress(data, size, compressionLevel_), size, timestamp);
}

bool RewindBuffer::pushCompressed(std::vector<std::uint8_t>&& compressed, std::size_t uncompressedSize, std::uint64_t timestamp)
{
	if (compressed.empty() || uncompressedSize == 0 || uncompressedSize > MaxStateSize
			|| maxBytes_ == 0 || maxStates_ == 0)
		return false;

	const std::size_t payloadSize = compressed.size();
	if (payloadSize > maxBytes_)
		return false;

	State state;
	state.compressedData = std::move(compressed);
	state.uncompressedSize = uncompressedSize;
	state.timestamp = states_.empty() ? timestamp : std::max(timestamp, states_.back().timestamp);

	while (!states_.empty() &&
		   (states_.size() >= maxStates_ || memoryUsed_ > maxBytes_ - payloadSize))
	{
		evictOldest();
	}

	states_.push_back(std::move(state));
	memoryUsed_ += payloadSize;
	return true;
}

bool RewindBuffer::pop(std::vector<std::uint8_t>& output, std::optional<std::uint64_t> currentTimestamp)
{
	if (states_.empty())
		return false;

	std::size_t targetIndex = states_.size() - 1;
	if (currentTimestamp && states_.size() > 1 && states_.back().timestamp == *currentTimestamp)
		--targetIndex;
	const State& state = states_[targetIndex];
	if (state.uncompressedSize > MaxStateSize
			|| state.uncompressedSize > static_cast<std::size_t>(std::numeric_limits<uLongf>::max())
			|| state.compressedData.size() > static_cast<std::size_t>(std::numeric_limits<uLong>::max()))
	{
		throw std::runtime_error("Invalid rewind state size");
	}

	output.resize(state.uncompressedSize);
	uLongf outputSize = static_cast<uLongf>(state.uncompressedSize);
	const int result = uncompress(reinterpret_cast<Bytef*>(output.data()), &outputSize,
								  reinterpret_cast<const Bytef*>(state.compressedData.data()),
								  static_cast<uLong>(state.compressedData.size()));
	if (result != Z_OK)
		throw std::runtime_error("Failed to decompress rewind state");
	if (outputSize != state.uncompressedSize)
		throw std::runtime_error("Rewind state size mismatch");

	while (states_.size() > targetIndex)
	{
		memoryUsed_ -= states_.back().compressedData.size();
		states_.pop_back();
	}
	return true;
}

void RewindBuffer::clear()
{
	states_.clear();
	memoryUsed_ = 0;
}

void RewindBuffer::discardOlderThan(std::uint64_t timestamp)
{
	while (!states_.empty() && states_.front().timestamp < timestamp)
		evictOldest();
}

std::size_t RewindBuffer::size() const
{
	return states_.size();
}

std::size_t RewindBuffer::memoryUsed() const
{
	return memoryUsed_;
}

void RewindBuffer::evictOldest()
{
	memoryUsed_ -= states_.front().compressedData.size();
	states_.pop_front();
}
