// Portions Copyright 2026 The Hollycast Authors
#include <gtest/gtest.h>
#include "network/ggpo_resume_barrier.h"

namespace
{
using ggpo::ResumeBarrier;
using namespace std::chrono_literals;

struct Peers
{
	ResumeBarrier first, second;
	ResumeBarrier::Clock::time_point now{};

	void poll(bool firstReady = true, bool secondReady = true)
	{
		const bool sendFirst = first.shouldSend(now);
		const auto firstStatus = first.status();
		if (second.shouldSend(now))
			first.receive(second.status(), now);
		if (sendFirst)
			second.receive(firstStatus, now);
		first.update(now, firstReady);
		second.update(now, secondReady);
	}

	void advance(std::chrono::seconds duration)
	{
		for (auto elapsed = 0s; elapsed < duration; elapsed += 1s)
		{
			now += 1s;
			poll();
		}
	}
};
}

TEST(GgpoResumeBarrierTest, OnePeerCanStartRecovery)
{
	Peers peers;
	EXPECT_FALSE(peers.first.shouldSend(peers.now));
	peers.first.begin();
	peers.poll();
	EXPECT_TRUE(peers.second.waiting());
	EXPECT_EQ(peers.second.status().generation, 0u);
	EXPECT_EQ(peers.first.countdownEnd(), ResumeBarrier::Clock::time_point{});
	peers.poll();
	EXPECT_EQ(peers.first.countdownEnd(), peers.now + 5s);
	EXPECT_EQ(peers.second.countdownEnd(), peers.now + 5s);
	peers.advance(5s);
	EXPECT_EQ(peers.first.countdownEnd(), peers.now);
	EXPECT_EQ(peers.second.countdownEnd(), peers.now);
	peers.now += 99ms;
	peers.poll();
	EXPECT_EQ(peers.first.status().flags, ResumeBarrier::Ready);
	EXPECT_EQ(peers.second.status().flags, ResumeBarrier::Ready);
	peers.now += 1ms;
	peers.poll();
	EXPECT_EQ(peers.first.status().flags, ResumeBarrier::Ready | ResumeBarrier::Finished);
	EXPECT_EQ(peers.second.status().flags, ResumeBarrier::Ready | ResumeBarrier::Finished);
	EXPECT_TRUE(peers.first.waiting());
	EXPECT_TRUE(peers.second.waiting());
	peers.poll();
	EXPECT_FALSE(peers.first.waiting());
	EXPECT_FALSE(peers.second.waiting());
	EXPECT_TRUE(peers.first.shouldSend(peers.now + 2s));
	EXPECT_FALSE(peers.first.shouldSend(peers.now + 3s));
}

TEST(GgpoResumeBarrierTest, IndependentInterruptionsMustAgreeOnTheGenerationPair)
{
	Peers peers;
	peers.first.begin();
	peers.second.begin();
	peers.poll();
	EXPECT_EQ(peers.first.countdownEnd(), ResumeBarrier::Clock::time_point{});
	EXPECT_EQ(peers.second.countdownEnd(), ResumeBarrier::Clock::time_point{});
	peers.poll();
	EXPECT_EQ(peers.first.status().acknowledged, 1u);
	EXPECT_EQ(peers.second.status().acknowledged, 1u);
	peers.advance(5s);
	peers.now += 100ms;
	peers.poll();
	peers.poll();
	EXPECT_FALSE(peers.first.waiting());
	EXPECT_FALSE(peers.second.waiting());
}

TEST(GgpoResumeBarrierTest, AnotherInterruptionInvalidatesTheOldCountdown)
{
	Peers peers;
	peers.first.begin();
	peers.poll();
	peers.poll();
	peers.advance(4s);
	const auto previousSecondStatus = peers.second.status();
	peers.poll(false);
	EXPECT_EQ(peers.first.countdownEnd(), ResumeBarrier::Clock::time_point{});
	EXPECT_EQ(peers.first.status().generation, 2u);
	peers.poll(false);
	EXPECT_EQ(peers.second.countdownEnd(), ResumeBarrier::Clock::time_point{});
	peers.poll();
	EXPECT_FALSE(peers.first.receive(previousSecondStatus, peers.now));
	peers.poll();
	EXPECT_EQ(peers.first.countdownEnd(), peers.now + 5s);
	EXPECT_EQ(peers.second.countdownEnd(), peers.now + 5s);
	peers.advance(1s);
	EXPECT_TRUE(peers.first.waiting());
	EXPECT_TRUE(peers.second.waiting());
}

TEST(GgpoResumeBarrierTest, MissingPeerFeedbackPreventsEarlyResume)
{
	Peers peers;
	peers.first.begin();
	peers.poll(false);
	peers.poll(false);
	EXPECT_EQ(peers.second.countdownEnd(), ResumeBarrier::Clock::time_point{});
	peers.poll();
	EXPECT_EQ(peers.first.countdownEnd(), peers.now + 5s);
	EXPECT_EQ(peers.second.countdownEnd(), ResumeBarrier::Clock::time_point{});
	const auto stalePeer = peers.second.status();
	peers.now += 5s;
	EXPECT_TRUE(peers.first.receive(stalePeer, peers.now));
	peers.first.update(peers.now, true);
	EXPECT_TRUE(peers.first.waiting());
	EXPECT_EQ(peers.first.countdownEnd(), ResumeBarrier::Clock::time_point{});
	EXPECT_EQ(peers.first.status().generation, 2u);
	EXPECT_EQ(peers.first.status().flags, ResumeBarrier::Ready);
}

TEST(GgpoResumeBarrierTest, RejectsInvalidAndRegressingStatus)
{
	ResumeBarrier barrier;
	barrier.begin();
	const ResumeBarrier::Clock::time_point now{};
	EXPECT_FALSE(barrier.receive({1, 2, ResumeBarrier::Ready}, now));
	EXPECT_FALSE(barrier.receive({1, 1, ResumeBarrier::Finished}, now));
	EXPECT_FALSE(barrier.receive({1, 1, 4}, now));
	EXPECT_TRUE(barrier.receive({1, 1, ResumeBarrier::Ready}, now));
	EXPECT_FALSE(barrier.receive({0, 1, ResumeBarrier::Ready}, now));
	EXPECT_FALSE(barrier.receive({1, 0, ResumeBarrier::Ready}, now));
	EXPECT_FALSE(barrier.receive({1, 1, 0}, now));
}
