// Portions Copyright 2026 The Hollycast Authors
#include "gtest/gtest.h"
#include "util/periodic_thread.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

class PeriodicThreadTest : public ::testing::Test
{
};

TEST_F(PeriodicThreadTest, Basic)
{
	std::atomic<int> counter = 0;
	std::mutex mutex;
	std::condition_variable advanced;
	PeriodicThread thread = PeriodicThread("Test", [&]() {
		std::lock_guard<std::mutex> lock(mutex);
		counter++;
		advanced.notify_one();
	});
	thread.setPeriod(10);
	thread.start();
	bool started, repeated;
	{
		std::unique_lock<std::mutex> lock(mutex);
		// Worker startup and timer resolution can exceed 15 ms on a loaded
		// host. Observe actual progress instead of racing a fixed sleep.
		started = advanced.wait_for(lock, std::chrono::seconds(1), [&] { return counter > 0; });
		const int previous = counter;
		repeated = advanced.wait_for(lock, std::chrono::seconds(1), [&] { return counter > previous; });
	}
	thread.stop();
	const int copy = counter;
	std::this_thread::sleep_for(std::chrono::milliseconds(15));
	ASSERT_TRUE(started);
	ASSERT_TRUE(repeated);
	ASSERT_EQ(copy, counter);
}
