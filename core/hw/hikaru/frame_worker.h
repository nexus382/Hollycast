// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

namespace hikaru::frontend::detail {

// Request-driven producer with one outstanding frame. Packet ownership and
// machine locking stay with the caller; this thread never invokes GUI work.
class FrameWorker {
public:
    FrameWorker();
    ~FrameWorker();
    FrameWorker(const FrameWorker&) = delete;
    FrameWorker& operator=(const FrameWorker&) = delete;

    // Requires idle, no unconsumed error, and a worker that has not shut down.
    void enqueue(std::function<void()> task);
    std::exception_ptr wait(); // Wait for completion and consume its error.
    void drain();             // Wait without consuming the error.
    // Finish accepted work before joining. Idempotent; construct a new worker
    // to restart. The worker task must not call shutdown or destroy its owner.
    void shutdown();

private:
    void run();
    std::mutex mutex, shutdownMutex;
    std::condition_variable changed;
    std::function<void()> pending;
    std::exception_ptr error;
    bool busy = false, stopping = false;
    std::thread thread;
};

} // namespace hikaru::frontend::detail
