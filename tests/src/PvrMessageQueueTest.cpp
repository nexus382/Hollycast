// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "hw/pvr/pvr_message_queue.h"
#include <atomic>
#include <future>
#include <thread>

class PvrMessageQueueTest : public ::testing::Test
{
protected:
	using Queue = PvrMessageQueue;
	using Clock = std::chrono::steady_clock;

	static bool waitForWaiters(Queue& queue, unsigned producers, unsigned consumers)
	{
		const auto deadline = Clock::now() + std::chrono::seconds(2);
		do {
			{
				std::lock_guard<std::mutex> lock(queue.mutex);
				if (queue.waitingProducers == producers && queue.waitingConsumers == consumers)
					return true;
			}
			std::this_thread::yield();
		} while (Clock::now() < deadline);
		return false;
	}

	static void notifyConsumer(Queue& queue)
	{
		queue.enqueued.notify_one();
	}

	static FramebufferInfo framebuffer(u32 address)
	{
		FramebufferInfo info{};
		info.fb_r_sof1 = address;
		return info;
	}
};

TEST_F(PvrMessageQueueTest, PreservesRenderFramebufferAndPresentOrder)
{
	Queue queue;
	queue.enqueue(Queue::Render);
	queue.enqueue(Queue::Present, framebuffer(1));
	queue.enqueue(Queue::RenderFramebuffer, framebuffer(0x123400));
	queue.enqueue(Queue::Present, framebuffer(2));
	EXPECT_EQ(queue.dequeue(0).type, Queue::Render);
	EXPECT_EQ(queue.dequeue(0).config.fb_r_sof1, 1u);
	const auto readback = queue.dequeue(0);
	EXPECT_EQ(readback.type, Queue::RenderFramebuffer);
	EXPECT_EQ(readback.config.fb_r_sof1, 0x123400u);
	EXPECT_EQ(queue.dequeue(0).config.fb_r_sof1, 2u);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, DropsOnlyOldestPresentAtDepthFour)
{
	Queue queue;
	queue.enqueue(Queue::Render);
	queue.enqueue(Queue::Present, framebuffer(0));
	queue.enqueue(Queue::RenderFramebuffer, framebuffer(0x100));
	queue.enqueue(Queue::Stop);
	for (u32 i = 1; i <= 100; ++i)
		queue.enqueue(Queue::Present, framebuffer(i));
	EXPECT_EQ(queue.dequeue(0).type, Queue::Render);
	EXPECT_EQ(queue.dequeue(0).type, Queue::RenderFramebuffer);
	EXPECT_EQ(queue.dequeue(0).type, Queue::Stop);
	const auto latest = queue.dequeue(0);
	EXPECT_EQ(latest.type, Queue::Present);
	EXPECT_EQ(latest.config.fb_r_sof1, 100u);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, CoalescesStopWithoutBlocking)
{
	Queue queue;
	queue.enqueue(Queue::Stop);
	queue.enqueue(Queue::Stop);
	EXPECT_EQ(queue.dequeue(0).type, Queue::Stop);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, DuplicateProducerResumesWhenMessageIsConsumed)
{
	Queue queue;
	queue.enqueue(Queue::RenderFramebuffer, framebuffer(1));
	auto producer = std::async(std::launch::async, [&] {
		queue.enqueue(Queue::RenderFramebuffer, framebuffer(2));
	});
	const bool blocked = waitForWaiters(queue, 1, 0);
	// Consume even after a failed expectation so the producer can always join.
	EXPECT_EQ(queue.dequeue(0).config.fb_r_sof1, 1u);
	producer.get();
	EXPECT_TRUE(blocked);
	const auto next = queue.dequeue(0);
	EXPECT_EQ(next.type, Queue::RenderFramebuffer);
	EXPECT_EQ(next.config.fb_r_sof1, 2u);
}

TEST_F(PvrMessageQueueTest, CancelAbortsBlockedReadbackBeforeStop)
{
	Queue queue;
	queue.enqueue(Queue::RenderFramebuffer);
	queue.enqueue(Queue::Present);
	auto producer = std::async(std::launch::async, [&] { queue.enqueue(Queue::RenderFramebuffer); });
	const bool blocked = waitForWaiters(queue, 1, 0);
	queue.cancelEnqueue();
	queue.enqueue(Queue::Stop);
	producer.get();
	EXPECT_TRUE(blocked);
	EXPECT_EQ(queue.dequeue(0).type, Queue::Stop);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, CancelRetainsRenderAndAbortsItsBlockedDuplicate)
{
	Queue queue;
	queue.enqueue(Queue::Render);
	auto producer = std::async(std::launch::async, [&] { queue.enqueue(Queue::Render); });
	const bool blocked = waitForWaiters(queue, 1, 0);
	queue.cancelEnqueue();
	queue.enqueue(Queue::Stop);
	if (!blocked)
		queue.dequeue(0);
	producer.get();
	ASSERT_TRUE(blocked);
	EXPECT_EQ(queue.dequeue(0).type, Queue::Render);
	EXPECT_EQ(queue.dequeue(0).type, Queue::Stop);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, ResetCancelsOldAdmissionAndAcceptsFreshWork)
{
	Queue queue;
	queue.enqueue(Queue::RenderFramebuffer);
	auto producer = std::async(std::launch::async, [&] { queue.enqueue(Queue::RenderFramebuffer); });
	const bool blocked = waitForWaiters(queue, 1, 0);
	queue.reset();
	producer.get();
	EXPECT_TRUE(blocked);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
	queue.enqueue(Queue::RenderFramebuffer, framebuffer(42));
	EXPECT_EQ(queue.dequeue(0).config.fb_r_sof1, 42u);
}

TEST_F(PvrMessageQueueTest, SleepingConsumerWakesForNewWork)
{
	Queue queue;
	auto consumer = std::async(std::launch::async, [&] { return queue.dequeue(); });
	const bool blocked = waitForWaiters(queue, 0, 1);
	queue.enqueue(Queue::RenderFramebuffer, framebuffer(42));
	const auto message = consumer.get();
	EXPECT_TRUE(blocked);
	EXPECT_EQ(message.type, Queue::RenderFramebuffer);
	EXPECT_EQ(message.config.fb_r_sof1, 42u);
}

TEST_F(PvrMessageQueueTest, SpuriousNotificationsDoNotProduceWorkOrRestartTimeout)
{
	Queue queue;
	const auto start = Clock::now();
	auto consumer = std::async(std::launch::async, [&] { return queue.dequeue(100); });
	const bool blocked = waitForWaiters(queue, 0, 1);
	while (consumer.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready
			&& Clock::now() - start < std::chrono::seconds(1))
	{
		notifyConsumer(queue);
		std::this_thread::yield();
	}
	EXPECT_EQ(consumer.get().type, Queue::NoMessage);
	const auto elapsed = Clock::now() - start;
	EXPECT_TRUE(blocked);
	EXPECT_GE(elapsed, std::chrono::milliseconds(90));
	EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST_F(PvrMessageQueueTest, MultipleBlockedProducersPreserveEveryRender)
{
	Queue queue;
	queue.enqueue(Queue::Render);
	std::future<void> producers[3];
	for (auto& producer : producers)
		producer = std::async(std::launch::async, [&] { queue.enqueue(Queue::Render); });
	const bool blocked = waitForWaiters(queue, 3, 0);
	for (unsigned i = 0; i < 4; ++i)
		EXPECT_EQ(queue.dequeue(2000).type, Queue::Render);
	for (auto& producer : producers)
		producer.get();
	EXPECT_TRUE(blocked);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}

TEST_F(PvrMessageQueueTest, ConcurrentFramesDoNotLoseWakeups)
{
	Queue queue;
	constexpr unsigned frames = 2000;
	std::atomic<unsigned> completed{0};
	std::atomic<bool> failed{false};
	auto consumer = std::async(std::launch::async, [&] {
		for (unsigned i = 0; i < frames; ++i)
		{
			const auto render = queue.dequeue(2000);
			if (render.type != Queue::Render || render.config.fb_r_sof1 != i)
			{
				failed = true;
				queue.reset();
				break;
			}
			++completed;
		}
	});
	for (unsigned i = 0; i < frames && !failed; ++i)
		queue.enqueue(Queue::Render, framebuffer(i));
	consumer.get();
	EXPECT_FALSE(failed);
	EXPECT_EQ(completed.load(), frames);
	EXPECT_EQ(queue.dequeue(0).type, Queue::NoMessage);
}
