// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace ggpo
{

class AdaptiveDelayWindow
{
public:
	using Clock = std::chrono::steady_clock;
	static constexpr auto SamplePeriod = std::chrono::seconds(1);
	static constexpr int WindowMs = 5000;

	struct Report
	{
		int stallMs;
		int episodes;
		bool quiet;
	};

	std::optional<Report> observe(Clock::time_point now, Clock::duration waited, bool newStall, bool nearLimit)
	{
		if (!sampleEnd || now >= *sampleEnd + SamplePeriod)
		{
			// A skipped bucket leaves a gap in the five-second history.
			*this = {};
			sampleEnd = now + SamplePeriod;
		}

		current.stalled += waited;
		current.episodes += newStall;
		current.nearLimit |= nearLimit;
		if (now < *sampleEnd)
			return std::nullopt;

		samples[nextSample++] = current;
		if (nextSample == samples.size())
		{
			nextSample = 0;
			full = true;
		}
		current = {};
		*sampleEnd += SamplePeriod;
		if (!full)
			return std::nullopt;

		Clock::duration stalled{};
		Report report{0, 0, true};
		for (const Sample& sample : samples)
		{
			stalled += sample.stalled;
			report.episodes += sample.episodes;
			report.quiet &= !sample.nearLimit;
		}
		const auto stallMs = std::chrono::duration_cast<std::chrono::milliseconds>(stalled);
		report.stallMs = static_cast<int>(std::min(stallMs, std::chrono::milliseconds(WindowMs)).count());
		report.quiet &= stalled == Clock::duration{} && report.episodes == 0;
		return report;
	}

private:
	struct Sample
	{
		Clock::duration stalled{};
		int episodes = 0;
		bool nearLimit = false;
	};

	static constexpr auto SampleCount = std::chrono::milliseconds(WindowMs) / SamplePeriod;
	std::array<Sample, SampleCount> samples{};
	Sample current;
	std::size_t nextSample = 0;
	bool full = false;
	std::optional<Clock::time_point> sampleEnd;
};

class AdaptiveDelayPolicy
{
public:
	using Clock = std::chrono::steady_clock;
	static constexpr int MaxDelay = 8;

	int observe(Clock::time_point now, int stallMs, int episodes, bool quiet, int localDelay)
	{
		using namespace std::chrono_literals;
		constexpr int PressureMs = 200;
		const bool pressure = stallMs >= PressureMs && episodes >= 2;
		if (!quiet || !lastFeedback || now - *lastFeedback > 3s)
			quietSince.reset();
		lastFeedback = now;
		if (quiet && !quietSince)
			quietSince = now;

		switch (trial)
		{
		case Trial::Increase:
			// Allow transit time and a fresh five-second measurement window.
			if (now - trialStart < 7s)
				return 0;
			// A lower episode count can still hide one long stall.
			if (stallMs >= PressureMs && stallMs * 4LL > baselineStallMs * 3LL)
				return localDelay > 0 ? -1 : 0;
			trial = Trial::None;
			return 0;
		case Trial::Decrease:
			if (now - trialStart <= 8s)
			{
				if (pressure && localDelay < MaxDelay)
				{
					trial = Trial::RestoreDecrease;
					return 1;
				}
				return 0;
			}
			trial = Trial::None;
			break;
		case Trial::RestoreDecrease:
			return localDelay < MaxDelay ? 1 : 0;
		case Trial::None:
			break;
		}

		if (pressure && localDelay < MaxDelay && now >= increaseAfter)
			return 1;
		if (quietSince && localDelay > 0 && now >= decreaseAfter && now - *quietSince >= 30s)
			return -1;
		return 0;
	}

	void applied(Clock::time_point now, int change, int stallMs)
	{
		using namespace std::chrono_literals;
		quietSince.reset();
		if (change > 0)
		{
			if (trial == Trial::RestoreDecrease)
			{
				decreaseAfter = now + 60s;
				trial = Trial::None;
			}
			else
			{
				trial = Trial::Increase;
				trialStart = now;
				baselineStallMs = stallMs;
			}
		}
		else if (trial == Trial::Increase)
		{
			increaseAfter = now + 30s;
			trial = Trial::None;
		}
		else
		{
			trial = Trial::Decrease;
			trialStart = now;
		}
	}

private:
	enum class Trial { None, Increase, Decrease, RestoreDecrease };

	Trial trial = Trial::None;
	Clock::time_point trialStart;
	int baselineStallMs = 0;
	Clock::time_point increaseAfter = Clock::time_point::min();
	Clock::time_point decreaseAfter = Clock::time_point::min();
	std::optional<Clock::time_point> lastFeedback;
	std::optional<Clock::time_point> quietSince;
};

}
