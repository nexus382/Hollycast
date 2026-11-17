// SPDX-License-Identifier: GPL-2.0-or-later
// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "util/rewind_buffer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{
std::vector<std::uint8_t> MakeNoise(std::size_t size, std::uint32_t seed)
{
	std::vector<std::uint8_t> bytes(size);
	for (std::uint8_t& byte : bytes)
	{
		seed = seed * 1664525U + 1013904223U;
		byte = static_cast<std::uint8_t>(seed >> 24);
	}
	return bytes;
}
}

TEST(RewindBufferTest, RoundTripsVariableStateSizesInLifoOrder)
{
	RewindBuffer buffer;
	buffer.configure(1U * 1024U * 1024U, 8);
	const std::vector<std::uint8_t> small = {0, 1, 2, 3};
	const std::vector<std::uint8_t> medium(513, 0x5a);
	const std::vector<std::uint8_t> large = MakeNoise(4097, 1);

	ASSERT_TRUE(buffer.push(small.data(), small.size()));
	ASSERT_TRUE(buffer.push(medium.data(), medium.size()));
	ASSERT_TRUE(buffer.push(large.data(), large.size()));
	EXPECT_EQ(3U, buffer.size());

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(large, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(medium, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(small, output);
	EXPECT_FALSE(buffer.pop(output));
	EXPECT_EQ(0U, buffer.memoryUsed());
}

TEST(RewindBufferTest, PopDiscardsFutureBranch)
{
	RewindBuffer buffer;
	buffer.configure(1024, 8);
	const std::vector<std::uint8_t> first = {1};
	const std::vector<std::uint8_t> second = {2};
	const std::vector<std::uint8_t> oldFuture = {3};
	const std::vector<std::uint8_t> newFuture = {4};

	ASSERT_TRUE(buffer.push(first.data(), first.size()));
	ASSERT_TRUE(buffer.push(second.data(), second.size()));
	ASSERT_TRUE(buffer.push(oldFuture.data(), oldFuture.size()));

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(oldFuture, output);
	ASSERT_TRUE(buffer.push(newFuture.data(), newFuture.size()));
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(newFuture, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(second, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(first, output);
	EXPECT_FALSE(buffer.pop(output));
}

TEST(RewindBufferTest, EvictsOldestStateWhenCountLimitIsReached)
{
	RewindBuffer buffer;
	buffer.configure(1024, 2);
	const std::vector<std::uint8_t> first = {1};
	const std::vector<std::uint8_t> second = {2};
	const std::vector<std::uint8_t> third = {3};

	ASSERT_TRUE(buffer.push(first.data(), first.size()));
	ASSERT_TRUE(buffer.push(second.data(), second.size()));
	ASSERT_TRUE(buffer.push(third.data(), third.size()));
	EXPECT_EQ(2U, buffer.size());

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(third, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(second, output);
	EXPECT_FALSE(buffer.pop(output));
}

TEST(RewindBufferTest, EvictsOldestStateToStayWithinByteLimit)
{
	RewindBuffer buffer;
	buffer.configure(3000, 8);
	const std::vector<std::uint8_t> first = MakeNoise(2048, 1);
	const std::vector<std::uint8_t> second = MakeNoise(2048, 2);

	ASSERT_TRUE(buffer.push(first.data(), first.size()));
	ASSERT_GT(buffer.memoryUsed(), 1500U);
	ASSERT_LT(buffer.memoryUsed(), 3000U);
	ASSERT_TRUE(buffer.push(second.data(), second.size()));
	EXPECT_EQ(1U, buffer.size());
	EXPECT_LE(buffer.memoryUsed(), 3000U);

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(second, output);
	EXPECT_FALSE(buffer.pop(output));
}

TEST(RewindBufferTest, OverBudgetSnapshotDoesNotDiscardExistingHistory)
{
	RewindBuffer buffer;
	buffer.configure(64, 8);
	const std::vector<std::uint8_t> existing(4096, 0);
	const std::vector<std::uint8_t> tooLarge = MakeNoise(1024, 4);

	ASSERT_TRUE(buffer.push(existing.data(), existing.size()));
	const std::size_t bytesBefore = buffer.memoryUsed();
	EXPECT_FALSE(buffer.push(tooLarge.data(), tooLarge.size()));
	EXPECT_EQ(1U, buffer.size());
	EXPECT_EQ(bytesBefore, buffer.memoryUsed());

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(existing, output);
}

TEST(RewindBufferTest, ZeroLimitsAndInvalidInputsDisableBuffer)
{
	RewindBuffer buffer;
	const std::uint8_t byte = 1;
	EXPECT_FALSE(buffer.push(&byte, 1));

	buffer.configure(128, 0);
	EXPECT_FALSE(buffer.push(&byte, 1));
	buffer.configure(0, 8);
	EXPECT_FALSE(buffer.push(&byte, 1));
	buffer.configure(128, 8);
	EXPECT_FALSE(buffer.push(nullptr, 1));
	EXPECT_FALSE(buffer.push(&byte, 0));
	EXPECT_EQ(0U, buffer.size());
	EXPECT_EQ(0U, buffer.memoryUsed());
}

TEST(RewindBufferTest, RejectsOversizedRawStateWithoutReadingIt)
{
	RewindBuffer buffer;
	buffer.configure(1024, 1);
	const std::uint8_t byte = 0;
	const std::size_t oversized = RewindBuffer::MaxStateSize + 1U;

	EXPECT_FALSE(buffer.push(&byte, oversized));
	EXPECT_EQ(0U, buffer.size());
}

TEST(RewindBufferTest, ChangedConfigurationAndClearReleaseStoredStates)
{
	RewindBuffer buffer;
	buffer.configure(1024, 4);
	const std::vector<std::uint8_t> state(512, 0xaa);

	ASSERT_TRUE(buffer.push(state.data(), state.size()));
	EXPECT_GT(buffer.memoryUsed(), 0U);
	buffer.configure(1024, 5);
	EXPECT_EQ(0U, buffer.size());
	EXPECT_EQ(0U, buffer.memoryUsed());

	ASSERT_TRUE(buffer.push(state.data(), state.size()));
	buffer.clear();
	EXPECT_EQ(0U, buffer.size());
	EXPECT_EQ(0U, buffer.memoryUsed());
}

TEST(RewindBufferTest, RepeatedPushesRemainWithinBothLimits)
{
	RewindBuffer buffer;
	buffer.configure(12000, 3);
	std::vector<std::uint8_t> output;

	for (std::uint32_t seed = 1; seed <= 40; ++seed)
	{
		const std::vector<std::uint8_t> state = MakeNoise(2048, seed);
		ASSERT_TRUE(buffer.push(state.data(), state.size()));
		EXPECT_LE(buffer.size(), 3U);
		EXPECT_LE(buffer.memoryUsed(), 12000U);
	}

	EXPECT_EQ(3U, buffer.size());
	for (std::uint32_t seed = 40; seed > 37; --seed)
	{
		ASSERT_TRUE(buffer.pop(output));
		EXPECT_EQ(MakeNoise(2048, seed), output);
	}
	EXPECT_FALSE(buffer.pop(output));
	EXPECT_EQ(0U, buffer.memoryUsed());
}

TEST(RewindBufferTest, SameConfigurationPreservesHistory)
{
	RewindBuffer buffer;
	buffer.configure(1024, 4);
	const std::vector<std::uint8_t> first = {1};
	const std::vector<std::uint8_t> second = {2};
	ASSERT_TRUE(buffer.push(first.data(), first.size()));
	ASSERT_TRUE(buffer.push(second.data(), second.size()));
	const std::size_t bytesBefore = buffer.memoryUsed();

	// The engine reconfigures with unchanged limits on a regular cadence;
	// that must never wipe the recorded history it just built.
	buffer.configure(1024, 4);
	EXPECT_EQ(2U, buffer.size());
	EXPECT_EQ(bytesBefore, buffer.memoryUsed());
	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(second, output);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(first, output);
}

TEST(RewindBufferTest, EmptyPopLeavesOutputUnchanged)
{
	RewindBuffer buffer;
	buffer.configure(1024, 4);
	const std::vector<std::uint8_t> sentinel = {9, 8, 7};
	std::vector<std::uint8_t> output = sentinel;
	EXPECT_FALSE(buffer.pop(output));
	EXPECT_EQ(sentinel, output);
}

TEST(RewindBufferTest, NonMonotonicTimestampsCannotStrandOldStates)
{
	RewindBuffer buffer;
	buffer.configure(1024, 20);
	const std::uint8_t newest = 1, stale = 2, middle = 3;
	ASSERT_TRUE(buffer.push(&newest, 1, 300));
	ASSERT_TRUE(buffer.push(&stale, 1, 100));
	ASSERT_TRUE(buffer.push(&middle, 1, 200));
	// Guest time can step back after a restore; pushed timestamps are
	// clamped monotonic so an age cutoff still evicts from the front.
	buffer.discardOlderThan(301);
	EXPECT_EQ(0U, buffer.size());
	EXPECT_EQ(0U, buffer.memoryUsed());

	ASSERT_TRUE(buffer.push(&newest, 1, 300));
	ASSERT_TRUE(buffer.push(&stale, 1, 100));
	buffer.discardOlderThan(300);
	EXPECT_EQ(2U, buffer.size());
	buffer.discardOlderThan(0);
	EXPECT_EQ(2U, buffer.size());
}

TEST(RewindBufferTest, MaximumCompressionRoundTrips)
{
	RewindBuffer buffer;
	buffer.configure(1024 * 1024, 4, 9);
	const std::vector<std::uint8_t> state = MakeNoise(4097, 7);
	ASSERT_TRUE(buffer.push(state.data(), state.size()));
	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(state, output);
}

TEST(RewindBufferTest, PrecompressedPayloadRoundTrips)
{
	// The capture worker compresses off-thread, then hands the payload to
	// the history owner; it must land byte-identical without recompressing.
	RewindBuffer buffer;
	buffer.configure(1024 * 1024, 4, 1);
	const std::vector<std::uint8_t> state = MakeNoise(4097, 11);
	std::vector<std::uint8_t> compressed = RewindBuffer::compress(state.data(), state.size(), 9);
	ASSERT_TRUE(buffer.pushCompressed(std::move(compressed), state.size(), 123));
	EXPECT_EQ(1U, buffer.size());
	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(state, output);
}

TEST(RewindBufferTest, AgeLimitUsesGuestTimeRatherThanSnapshotCount)
{
	RewindBuffer buffer;
	buffer.configure(1024, 20);
	const std::uint8_t old = 1, boundary = 2, recent = 3;
	ASSERT_TRUE(buffer.push(&old, 1, 100));
	ASSERT_TRUE(buffer.push(&boundary, 1, 200));
	ASSERT_TRUE(buffer.push(&recent, 1, 900));
	buffer.discardOlderThan(200);
	EXPECT_EQ(2U, buffer.size());
	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(recent, output[0]);
	ASSERT_TRUE(buffer.pop(output));
	EXPECT_EQ(boundary, output[0]);
	EXPECT_FALSE(buffer.pop(output));
	EXPECT_EQ(0U, buffer.memoryUsed());
}

TEST(RewindBufferTest, PopUsesNewestWhenCurrentFrameSnapshotIsStillCompressing)
{
	RewindBuffer buffer;
	buffer.configure(1024, 8);
	const std::vector<std::uint8_t> older = {1};
	const std::vector<std::uint8_t> newest = {2};

	ASSERT_TRUE(buffer.push(older.data(), older.size(), 100));
	const std::size_t olderBytes = buffer.memoryUsed();
	ASSERT_TRUE(buffer.push(newest.data(), newest.size(), 200));

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output, 300));
	EXPECT_EQ(newest, output);
	EXPECT_EQ(1U, buffer.size());
	EXPECT_EQ(olderBytes, buffer.memoryUsed());
}

TEST(RewindBufferTest, PopSkipsSnapshotAtCurrentTimestampAndUpdatesMemoryAccounting)
{
	RewindBuffer buffer;
	buffer.configure(1024, 8);
	const std::vector<std::uint8_t> oldest = {1};
	const std::vector<std::uint8_t> previous = {2};
	const std::vector<std::uint8_t> current = {3};

	ASSERT_TRUE(buffer.push(oldest.data(), oldest.size(), 100));
	const std::size_t oldestBytes = buffer.memoryUsed();
	ASSERT_TRUE(buffer.push(previous.data(), previous.size(), 150));
	ASSERT_TRUE(buffer.push(current.data(), current.size(), 200));

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output, 200));
	EXPECT_EQ(previous, output);
	EXPECT_EQ(1U, buffer.size());
	EXPECT_EQ(oldestBytes, buffer.memoryUsed());
}

TEST(RewindBufferTest, PopKeepsLoneSnapshotFallbackAtCurrentTimestamp)
{
	RewindBuffer buffer;
	buffer.configure(1024, 8);
	const std::vector<std::uint8_t> only = {1};
	ASSERT_TRUE(buffer.push(only.data(), only.size(), 100));

	std::vector<std::uint8_t> output;
	ASSERT_TRUE(buffer.pop(output, 100));
	EXPECT_EQ(only, output);
	EXPECT_EQ(0U, buffer.size());
	EXPECT_EQ(0U, buffer.memoryUsed());
}
