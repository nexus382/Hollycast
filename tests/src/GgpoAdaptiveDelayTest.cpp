// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "network/ggpo_adaptive_delay.h"

namespace
{
using ggpo::AdaptiveDelayPolicy;
using ggpo::AdaptiveDelayWindow;
using namespace std::chrono_literals;
using TimePoint = AdaptiveDelayPolicy::Clock::time_point;

TEST(GgpoAdaptiveDelayWindow, RollsStallsAcrossQuietIntervals)
{
	AdaptiveDelayWindow window;
	TimePoint start;
	EXPECT_FALSE(window.observe(start, {}, false, false));
	EXPECT_FALSE(window.observe(start + 100ms, 90ms, true, true));
	EXPECT_FALSE(window.observe(start + 200ms, 30ms, false, true));
	EXPECT_FALSE(window.observe(start + 1s, {}, false, false));
	EXPECT_FALSE(window.observe(start + 1700ms, 100ms, true, true));
	EXPECT_FALSE(window.observe(start + 2s, {}, false, false));
	EXPECT_FALSE(window.observe(start + 3s, {}, false, false));
	EXPECT_FALSE(window.observe(start + 3500ms, {}, false, true));
	EXPECT_FALSE(window.observe(start + 4s, {}, false, false));

	auto report = window.observe(start + 5s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_EQ(220, report->stallMs);
	EXPECT_EQ(2, report->episodes);
	EXPECT_FALSE(report->quiet);

	report = window.observe(start + 6s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_EQ(100, report->stallMs);
	EXPECT_EQ(1, report->episodes);
	EXPECT_FALSE(report->quiet);

	report = window.observe(start + 7s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_EQ(0, report->stallMs);
	EXPECT_EQ(0, report->episodes);
	EXPECT_FALSE(report->quiet);

	report = window.observe(start + 8s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_FALSE(report->quiet);
	report = window.observe(start + 9s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_TRUE(report->quiet);
}

TEST(GgpoAdaptiveDelayWindow, DiscardsPartialAndCompletedBucketsAfterGap)
{
	AdaptiveDelayWindow window;
	TimePoint start;
	EXPECT_FALSE(window.observe(start, 100ms, true, true));
	for (int second = 1; second < 5; ++second)
		EXPECT_FALSE(window.observe(start + second * 1s, {}, false, false));
	ASSERT_TRUE(window.observe(start + 5s, {}, false, false));
	EXPECT_FALSE(window.observe(start + 5500ms, 300ms, true, true));

	EXPECT_FALSE(window.observe(start + 7s, 25ms, true, true));
	for (int second = 8; second < 12; ++second)
		EXPECT_FALSE(window.observe(start + second * 1s, {}, false, false));
	const auto report = window.observe(start + 12s, {}, false, false);
	ASSERT_TRUE(report);
	EXPECT_EQ(25, report->stallMs);
	EXPECT_EQ(1, report->episodes);
	EXPECT_FALSE(report->quiet);
}

TEST(GgpoAdaptiveDelay, RequiresSustainedPressureAcrossSeparateStalls)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	EXPECT_EQ(0, policy.observe(start, 800, 1, false, 2));
	EXPECT_EQ(0, policy.observe(start + 1s, 199, 2, false, 2));
	EXPECT_EQ(1, policy.observe(start + 2s, 200, 2, false, 2));
	EXPECT_EQ(1, policy.observe(start + 3s, 200, 2, false, 2));
}

TEST(GgpoAdaptiveDelay, RevertsUnhelpfulIncreaseAndWaitsAfterUndo)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	ASSERT_EQ(1, policy.observe(start, 400, 2, false, 2));
	policy.applied(start, 1, 400);

	EXPECT_EQ(0, policy.observe(start + 6s, 400, 2, false, 3));
	EXPECT_EQ(-1, policy.observe(start + 7s, 350, 1, false, 3));
	EXPECT_EQ(-1, policy.observe(start + 8s, 350, 1, false, 3));
	policy.applied(start + 8s, -1, 350);

	EXPECT_EQ(0, policy.observe(start + 37s, 400, 2, false, 2));
	EXPECT_EQ(1, policy.observe(start + 38s, 400, 2, false, 2));
}

TEST(GgpoAdaptiveDelay, KeepsHelpfulIncrease)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	ASSERT_EQ(1, policy.observe(start, 400, 2, false, 2));
	policy.applied(start, 1, 400);

	EXPECT_EQ(0, policy.observe(start + 7s, 300, 2, false, 3));
	EXPECT_EQ(1, policy.observe(start + 8s, 220, 2, false, 3));
	policy.applied(start + 8s, 1, 220);

	EXPECT_EQ(0, policy.observe(start + 15s, 199, 2, false, 4));
	EXPECT_EQ(0, policy.observe(start + 16s, 199, 2, false, 4));
}

TEST(GgpoAdaptiveDelay, RestoresDelayWhenPressureReturnsAfterReduction)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	for (int second = 0; second < 30; ++second)
		EXPECT_EQ(0, policy.observe(start + second * 1s, 0, 0, true, 3));
	ASSERT_EQ(-1, policy.observe(start + 30s, 0, 0, true, 3));
	policy.applied(start + 30s, -1, 0);

	EXPECT_EQ(0, policy.observe(start + 34s, 0, 0, true, 2));
	ASSERT_EQ(1, policy.observe(start + 38s, 240, 2, false, 2));
	EXPECT_EQ(1, policy.observe(start + 39s, 240, 2, false, 2));
	policy.applied(start + 39s, 1, 240);

	for (int second = 40; second < 99; ++second)
		EXPECT_EQ(0, policy.observe(start + second * 1s, 0, 0, true, 3));
	EXPECT_EQ(-1, policy.observe(start + 99s, 0, 0, true, 3));
}

TEST(GgpoAdaptiveDelay, StaleFeedbackRestartsQuietPeriod)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	for (int second = 0; second <= 29; ++second)
		EXPECT_EQ(0, policy.observe(start + second * 1s, 0, 0, true, 1));
	for (int second = 33; second < 63; second += 3)
		EXPECT_EQ(0, policy.observe(start + second * 1s, 0, 0, true, 1));
	EXPECT_EQ(-1, policy.observe(start + 63s, 0, 0, true, 1));
}

TEST(GgpoAdaptiveDelay, RespectsDelayBounds)
{
	AdaptiveDelayPolicy policy;
	TimePoint start;
	EXPECT_EQ(0, policy.observe(start, 300, 2, false, AdaptiveDelayPolicy::MaxDelay));
	for (int second = 1; second <= 32; ++second)
		EXPECT_EQ(0, policy.observe(start + second * 1s, 0, 0, true, 0));
}
}
