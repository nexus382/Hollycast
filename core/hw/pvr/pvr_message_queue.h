// Portions Copyright 2026 The Hollycast Authors
#pragma once

#include "Renderer_if.h"
#include "profiler/fc_profiler.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

// The SH4 producer and renderer consumer share one mutex for queue state and
// wake predicates. No separate event mutex or remembered wake token is needed.
class PvrMessageQueue
{
public:
	enum MessageType { NoMessage = -1, Render, RenderFramebuffer, Present, Stop };
	struct Message
	{
		MessageType type = NoMessage;
		FramebufferInfo config{};
	};

	void enqueue(MessageType type, FramebufferInfo config = FramebufferInfo())
	{
		std::unique_lock<std::mutex> lock(mutex);
		const auto duplicate = [&] {
			return std::any_of(queue.begin(), queue.end(), [type](const Message& message) {
				return message.type == type;
			});
		};
		if (type != Present && duplicate())
		{
			if (type == Stop)
				return;
			const std::uint64_t generation = cancelGeneration;
			++waitingProducers;
			dequeued.wait(lock, [&] { return generation != cancelGeneration || !duplicate(); });
			--waitingProducers;
			// A reset/stop must not let a previously blocked producer reinsert
			// stale work after the consumer has been cancelled.
			if (generation != cancelGeneration)
				return;
		}
		// Keep the existing depth-four policy: only the oldest pending Present
		// may be discarded. Render, framebuffer readback and Stop stay in order.
		if (queue.size() >= 4)
		{
			const auto oldestPresent = std::find_if(queue.begin(), queue.end(), [](const Message& message) {
				return message.type == Present;
			});
			if (oldestPresent != queue.end())
				queue.erase(oldestPresent);
		}
		queue.push_back({type, config});
		const bool wakeConsumer = waitingConsumers != 0;
		lock.unlock();
		if (wakeConsumer)
			enqueued.notify_one();
	}

	Message dequeue(int timeoutMs = -1)
	{
		FC_PROFILE_SCOPE;
		std::unique_lock<std::mutex> lock(mutex);
		if (queue.empty())
		{
			if (timeoutMs == 0)
				return {};
			++waitingConsumers;
			bool ready = true;
			if (timeoutMs == -1)
				enqueued.wait(lock, [this] { return !queue.empty(); });
			else
				ready = enqueued.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return !queue.empty(); });
			--waitingConsumers;
			if (!ready)
				return {};
		}
		Message message = queue.front();
		queue.pop_front();
		const bool wakeProducers = waitingProducers != 0;
		lock.unlock();
		if (wakeProducers)
			dequeued.notify_all();
		return message;
	}

	void reset()
	{
		std::unique_lock<std::mutex> lock(mutex);
		queue.clear();
		++cancelGeneration;
		const bool wakeProducers = waitingProducers != 0;
		lock.unlock();
		if (wakeProducers)
			dequeued.notify_all();
	}

	void cancelEnqueue()
	{
		std::unique_lock<std::mutex> lock(mutex);
		queue.erase(std::remove_if(queue.begin(), queue.end(), [](const Message& message) {
			return message.type != Render;
		}), queue.end());
		++cancelGeneration;
		const bool wakeProducers = waitingProducers != 0;
		lock.unlock();
		if (wakeProducers)
			dequeued.notify_all();
	}

private:
	friend class PvrMessageQueueTest;
	std::mutex mutex;
	std::condition_variable enqueued, dequeued;
	std::deque<Message> queue;
	std::uint64_t cancelGeneration = 0;
	unsigned waitingProducers = 0, waitingConsumers = 0;
};
