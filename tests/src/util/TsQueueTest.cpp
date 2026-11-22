#include "gtest/gtest.h"
#include "util/tsqueue.h"
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

class TsQueueTest : public ::testing::Test
{
};

TEST_F(TsQueueTest, Basic)
{
	TsQueue<int> queue;
	ASSERT_TRUE(queue.empty());
	ASSERT_EQ(0, queue.size());
	queue.push(42);
	ASSERT_FALSE(queue.empty());
	ASSERT_EQ(1, queue.size());
	ASSERT_EQ(42, queue.pop());

	queue.push(1);
	queue.push(2);
	queue.push(3);
	ASSERT_FALSE(queue.empty());
	ASSERT_EQ(3, queue.size());
	ASSERT_EQ(1, queue.pop());
	ASSERT_EQ(2, queue.pop());
	ASSERT_EQ(3, queue.pop());
}

TEST_F(TsQueueTest, MultiThread)
{
	TsQueue<bool> queue;
	std::promise<void> started;
	auto ready = started.get_future();
	auto result = std::async(std::launch::async, [&]() {
		started.set_value();
		return queue.pop();
	});
	ready.wait();
	EXPECT_EQ(std::future_status::timeout, result.wait_for(std::chrono::milliseconds(50)));
	queue.push(true);
	EXPECT_TRUE(result.get());
}

TEST_F(TsQueueTest, CopiesLvalueOnPush)
{
	TsQueue<std::string> queue;
	std::string value = "pi";
	queue.push(value);
	EXPECT_EQ("pi", value);
	value.clear();
	EXPECT_EQ("pi", queue.pop());
}

TEST_F(TsQueueTest, TransfersMoveOnlyValue)
{
	TsQueue<std::unique_ptr<int>> queue;
	auto value = std::make_unique<int>(42);
	const auto *original = value.get();
	queue.push(std::move(value));
	EXPECT_EQ(nullptr, value);
	auto result = queue.pop();
	EXPECT_EQ(original, result.get());
}

TEST_F(TsQueueTest, Clear)
{
	TsQueue<const char*> q;
	q.push("a");
	q.push("b");
	q.clear();
	q.push("c");
	ASSERT_EQ(0, strcmp("c", q.pop()));
}
