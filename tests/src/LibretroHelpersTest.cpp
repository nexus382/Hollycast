// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>

#include "../../shell/libretro/libretro_helpers.h"

TEST(LibretroHelpersTest, PendingTimingChangeInvokesFrontendUpdateOnce)
{
	PendingFps pending;
	double currentFps = 0.0;
	int frontendUpdates = 0;
	auto updateFrontend = [&frontendUpdates](double) { ++frontendUpdates; };

	pending.publish(50.0);
	EXPECT_TRUE(updatePendingFps(pending, currentFps, updateFrontend));
	EXPECT_DOUBLE_EQ(currentFps, 50.0);
	EXPECT_EQ(frontendUpdates, 1);
	EXPECT_FALSE(updatePendingFps(pending, currentFps, updateFrontend));
	EXPECT_EQ(frontendUpdates, 1);

	// Timing changes are delivered even when no geometry change is involved.
	pending.publish(59.94);
	EXPECT_TRUE(updatePendingFps(pending, currentFps, updateFrontend));
	EXPECT_DOUBLE_EQ(currentFps, 59.94);
	EXPECT_EQ(frontendUpdates, 2);
}

TEST(LibretroHelpersTest, ResetDropsTimingQueuedByPreviousGame)
{
	PendingFps pending;
	double currentFps = 50.0;
	pending.publish(59.94);
	resetPendingFps(pending, currentFps);
	int frontendUpdates = 0;
	EXPECT_FALSE(updatePendingFps(pending, currentFps, [&frontendUpdates](double) { ++frontendUpdates; }));
	EXPECT_DOUBLE_EQ(currentFps, 0.0);
	EXPECT_EQ(frontendUpdates, 0);
}

TEST(LibretroHelpersTest, PendingFpsCoalescesToLatestTiming)
{
	PendingFps pending;
	double currentFps = 0.0;
	pending.publish(50.0);
	pending.publish(59.94);
	EXPECT_TRUE(updatePendingFps(pending, currentFps, [](double) {}));
	EXPECT_DOUBLE_EQ(currentFps, 59.94);
}

TEST(LibretroHelpersTest, SerializePaddingIsZeroedWithoutChangingPayload)
{
	unsigned char buffer[] = { 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc };
	clearSerializePadding(buffer, sizeof(buffer), 3);

	EXPECT_EQ(buffer[0], 0x12);
	EXPECT_EQ(buffer[1], 0x34);
	EXPECT_EQ(buffer[2], 0x56);
	EXPECT_EQ(buffer[3], 0);
	EXPECT_EQ(buffer[4], 0);
	EXPECT_EQ(buffer[5], 0);
}

TEST(LibretroHelpersTest, SerializePaddingHandlesExactFitAndNullBuffer)
{
	unsigned char buffer[] = { 0x12, 0x34, 0x56 };
	clearSerializePadding(buffer, sizeof(buffer), sizeof(buffer));
	clearSerializePadding(nullptr, 4, 1);
	EXPECT_EQ(buffer[0], 0x12);
	EXPECT_EQ(buffer[1], 0x34);
	EXPECT_EQ(buffer[2], 0x56);
}
