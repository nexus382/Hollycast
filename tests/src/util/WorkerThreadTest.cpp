#include "gtest/gtest.h"
#include "util/worker_thread.h"
#include <atomic>
#include <chrono>
#include <future>

class WorkerThreadTest : public ::testing::Test
{
};

TEST_F(WorkerThreadTest, RunsTasksBeforeAndAfterRestart)
{
	std::promise<void> first, restarted;
	WorkerThread worker{"Test"};
	worker.run([&] { first.set_value(); });
	ASSERT_EQ(std::future_status::ready, first.get_future().wait_for(std::chrono::seconds(2)));

	worker.stop();
	worker.run([&] { restarted.set_value(); });
	EXPECT_EQ(std::future_status::ready, restarted.get_future().wait_for(std::chrono::seconds(2)));
}

TEST_F(WorkerThreadTest, MultiThread)
{
	WorkerThread worker{"Test"};
	std::atomic<int> counter = 0;
	const auto& task = [&]() {
		++counter;
	};
	const auto& consumer = [&]() {
		for (int i = 0; i < 100; i++)
			worker.run(task);
	};
	std::future<void> futures[4];
	for (auto& f : futures)
		f = std::async(std::launch::async, consumer);
	for (auto& f : futures)
		f.get();
	worker.stop(); // force all tasks to be executed before stopping
	ASSERT_EQ(std::size(futures) * 100, counter);
}

// There's no guarantee that tasks submitted while the worker is being stopped will
// be executed. But it shouldn't crash.
TEST_F(WorkerThreadTest, StartStop)
{
	WorkerThread worker{"Test"};
	const auto& consumer = [&]() {
		for (int i = 0; i < 100; i++)
			worker.run([] {});
	};
	std::future<void> future = std::async(std::launch::async, consumer);
	std::future<void> future2 = std::async(std::launch::async, [&]() {
		for (int i = 0; i < 100; i++)
			worker.stop();
	});
	future.get();
	future2.get();
	worker.stop();
}

TEST_F(WorkerThreadTest, Future)
{
	WorkerThread worker{"Test"};
	const auto& task = [](u32 v) -> u32 {
		return v;
	};
	std::future<u32> f = worker.runFuture(task, 42);
	ASSERT_EQ(42, f.get());
}
