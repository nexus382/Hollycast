// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hw/hikaru/frame_worker.h"
#include "gtest/gtest.h"
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>

#ifdef HIKARU_STANDALONE_HOST_TEST
// The standalone host harness has no platform shell; the full test target
// already supplies this hook through test_stubs.cpp.
void os_SetThreadName(const char *) {}
#endif

namespace {
using hikaru::frontend::detail::FrameWorker;
using namespace std::chrono_literals;

TEST(HikaruFrameWorkerTest, ProducerOverlapsHostDrawAndRejectsASecondTask)
{
    FrameWorker worker;
    std::promise<void> drawStarted, producerObservedDraw, release;
    auto drawStartedFuture = drawStarted.get_future();
    auto producerObservedDrawFuture = producerObservedDraw.get_future();
    auto releaseFuture = release.get_future();
    std::atomic<bool> drawActive{false};
    bool sawActiveDraw = false;
    std::thread::id producerThread;
    worker.enqueue([&] {
        drawStartedFuture.wait();
        producerThread = std::this_thread::get_id();
        sawActiveDraw = drawActive.load();
        producerObservedDraw.set_value();
        releaseFuture.wait();
    });
    const auto draw = [&] {
        drawActive = true;
        drawStarted.set_value();
        // Drawing N remains inside its callback until N+1 has executed on
        // the producer thread and observed this draw still in progress.
        EXPECT_EQ(producerObservedDrawFuture.wait_for(5s), std::future_status::ready);
        EXPECT_THROW(worker.enqueue([] {}), std::logic_error);
        drawActive = false;
        release.set_value();
    };
    draw();
    EXPECT_EQ(worker.wait(), nullptr);
    EXPECT_TRUE(sawActiveDraw);
    EXPECT_NE(producerThread, std::this_thread::get_id());
}

TEST(HikaruFrameWorkerTest, WaitBlocksUntilTheAcceptedTaskCompletes)
{
    FrameWorker worker;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    worker.enqueue([&] { entered.set_value(); releaseFuture.wait(); });
    const auto started = enteredFuture.wait_for(5s);
    auto waiter = std::async(std::launch::async, [&] { return worker.wait(); });
    EXPECT_EQ(waiter.wait_for(0ms), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(waiter.get(), nullptr);
    EXPECT_EQ(started, std::future_status::ready);
}

TEST(HikaruFrameWorkerTest, HandsOffAnyExceptionOnceThenAcceptsAnotherFrame)
{
    FrameWorker worker;
    worker.enqueue([] { throw 27; });
    worker.drain();
    EXPECT_THROW(worker.enqueue([] {}), std::logic_error);
    auto error = worker.wait();
    ASSERT_NE(error, nullptr);
    try {
        std::rethrow_exception(error);
        FAIL() << "Missing producer exception";
    } catch (int value) {
        EXPECT_EQ(value, 27);
    }
    EXPECT_EQ(worker.wait(), nullptr);
    bool completed = false;
    worker.enqueue([&] { completed = true; });
    EXPECT_EQ(worker.wait(), nullptr);
    EXPECT_TRUE(completed);
}

TEST(HikaruFrameWorkerTest, DrainPreservesTheProducerError)
{
    FrameWorker worker;
    worker.enqueue([] { throw std::runtime_error("frame failure"); });
    worker.drain();
    worker.drain();
    auto error = worker.wait();
    ASSERT_NE(error, nullptr);
    EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
    EXPECT_EQ(worker.wait(), nullptr);
}

TEST(HikaruFrameWorkerTest, ShutdownFinishesAcceptedWorkAndWakesWaiters)
{
    FrameWorker worker;
    std::promise<void> entered, release;
    auto enteredFuture = entered.get_future();
    auto releaseFuture = release.get_future();
    bool completed = false;
    worker.enqueue([&] {
        entered.set_value();
        releaseFuture.wait();
        completed = true;
    });
    const auto started = enteredFuture.wait_for(5s);
    auto stopping = std::async(std::launch::async, [&] { worker.shutdown(); });
    auto waiting = std::async(std::launch::async, [&] { return worker.wait(); });
    EXPECT_EQ(stopping.wait_for(0ms), std::future_status::timeout);
    release.set_value();
    stopping.get();
    EXPECT_EQ(waiting.get(), nullptr);
    EXPECT_TRUE(completed);
    EXPECT_EQ(started, std::future_status::ready);
    worker.shutdown();
    EXPECT_THROW(worker.enqueue([] {}), std::logic_error);
}

TEST(HikaruFrameWorkerTest, DestructorDrainsAnImmediatelyAcceptedTask)
{
    unsigned completed = 0;
    for (unsigned i = 0; i < 16; ++i) {
        FrameWorker worker;
        worker.enqueue([&] { ++completed; });
    }
    EXPECT_EQ(completed, 16u);
}

TEST(HikaruFrameWorkerTest, RepeatedConstructionWakeupAndIdleShutdown)
{
    unsigned completed = 0;
    for (unsigned i = 0; i < 16; ++i) {
        FrameWorker worker;
        EXPECT_EQ(worker.wait(), nullptr);
        EXPECT_THROW(worker.enqueue({}), std::invalid_argument);
        for (unsigned frame = 0; frame < 4; ++frame) {
            worker.enqueue([&] { ++completed; });
            EXPECT_EQ(worker.wait(), nullptr);
        }
        worker.shutdown();
        worker.shutdown();
        worker.drain();
        EXPECT_EQ(worker.wait(), nullptr);
    }
    EXPECT_EQ(completed, 64u);
}
} // namespace
