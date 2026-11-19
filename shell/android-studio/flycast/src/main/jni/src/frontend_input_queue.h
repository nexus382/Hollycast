// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

class FrontendInputQueue
{
public:
	void setActive(bool accepting)
	{
		std::vector<std::function<void()>> discarded;
		{
			const std::lock_guard<std::mutex> lock(mutex);
			active = accepting;
			if (!active)
				pending.swap(discarded);
		}
	}

	bool post(std::function<void()> action)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		if (!active)
			return false;
		pending.push_back(std::move(action));
		return true;
	}

	std::future<bool> postPause(std::function<bool()> pause)
	{
		auto task = std::make_shared<std::packaged_task<bool()>>(std::move(pause));
		auto result = task->get_future();
		if (!post([task] { (*task)(); }))
			return {};
		return result;
	}

	void drain()
	{
		std::vector<std::function<void()>> actions;
		{
			const std::lock_guard<std::mutex> lock(mutex);
			actions.swap(pending);
		}
		for (const auto& action : actions)
			action();
	}

private:
	std::mutex mutex;
	std::vector<std::function<void()>> pending;
	bool active = false;
};
