// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <chrono>
#include <cstdint>

namespace ggpo
{
class ResumeBarrier
{
public:
	using Clock = std::chrono::steady_clock;
	static constexpr std::uint8_t Ready = 1;
	static constexpr std::uint8_t Finished = 2;

	struct Status
	{
		std::uint32_t generation = 0;
		std::uint32_t acknowledged = 0;
		std::uint8_t flags = 0;
	};

	void begin()
	{
		++localGeneration;
		resetCountdown();
	}

	bool receive(Status status, Clock::time_point now)
	{
		if ((status.flags & ~(Ready | Finished)) != 0 || status.flags == Finished
				|| status.acknowledged > localGeneration || status.acknowledged < peer.acknowledged
				|| status.generation < peer.generation)
			return false;
		const bool newGeneration = status.generation > peer.generation;
		if (!newGeneration && status.acknowledged == peer.acknowledged
				&& (status.flags & peer.flags) != peer.flags)
			return false;
		if (newGeneration)
			resetCountdown();
		else
			expireCountdown(now);
		peer = status;
		lastFeedback = now;
		return true;
	}

	void update(Clock::time_point now, bool localReady)
	{
		if (!pending)
			return;
		if (!localReady)
		{
			// A new generation prevents delayed Ready or Finished packets from the old countdown.
			if ((localFlags & Ready) != 0)
				begin();
			return;
		}
		expireCountdown(now);
		localFlags |= Ready;
		if ((peer.flags & Ready) == 0 || peer.acknowledged != localGeneration
				|| now - lastFeedback > FeedbackTimeout)
			return;
		if (end == Clock::time_point{})
			end = now + CountdownDuration;
		// Give the UI time to render zero before either peer can release the barrier.
		if (now < end + ZeroDisplayDuration)
			return;
		localFlags |= Finished;
		if ((peer.flags & Finished) != 0)
		{
			pending = false;
			end = {};
			sendUntil = now + SendGracePeriod;
		}
	}

	bool waiting() const { return pending; }
	Clock::time_point countdownEnd() const { return end; }
	bool shouldSend(Clock::time_point now) const { return pending || now < sendUntil; }
	Status status() const { return {localGeneration, peer.generation, localFlags}; }

private:
	void resetCountdown()
	{
		pending = true;
		localFlags = 0;
		end = {};
		sendUntil = {};
	}

	void expireCountdown(Clock::time_point now)
	{
		if (end != Clock::time_point{} && now - lastFeedback > FeedbackTimeout)
			begin();
	}

	static constexpr auto CountdownDuration = std::chrono::seconds(5);
	static constexpr auto ZeroDisplayDuration = std::chrono::milliseconds(100);
	static constexpr auto FeedbackTimeout = std::chrono::seconds(2);
	static constexpr auto SendGracePeriod = std::chrono::seconds(3);
	Status peer;
	std::uint32_t localGeneration = 0;
	std::uint8_t localFlags = 0;
	bool pending = false;
	Clock::time_point lastFeedback{};
	Clock::time_point end{};
	Clock::time_point sendUntil{};
};
}
