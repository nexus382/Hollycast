// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "frame_worker.h"
#include "oslib/oslib.h"
#include <stdexcept>
#include <utility>

namespace hikaru::frontend::detail {

FrameWorker::FrameWorker() : thread(&FrameWorker::run, this) {}
FrameWorker::~FrameWorker() { shutdown(); }

void FrameWorker::enqueue(std::function<void()> task)
{
    if (!task) throw std::invalid_argument("Hikaru frame task is empty");
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping || busy || error)
            throw std::logic_error("Hikaru frame worker is not ready");
        pending = std::move(task);
        busy = true;
    }
    changed.notify_all();
}

std::exception_ptr FrameWorker::wait()
{
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [this] { return !busy; });
    return std::exchange(error, {});
}

void FrameWorker::drain()
{
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait(lock, [this] { return !busy; });
}

void FrameWorker::shutdown()
{
    // Serializes concurrent shutdown calls without holding the task mutex
    // while joining; the producer must acquire it to report completion.
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex);
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (thread.joinable()) thread.join();
}

void FrameWorker::run()
{
    ThreadName threadName("Hikaru-emu");
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        changed.wait(lock, [this] { return stopping || bool(pending); });
        if (!pending) return;
        auto task = std::move(pending);
        pending = {};
        lock.unlock();
        std::exception_ptr taskError;
        try { task(); }
        catch (...) { taskError = std::current_exception(); }
        // Release captured packet/lifetime references before making this
        // slot available or letting a waiting caller destroy its state.
        task = {};
        lock.lock();
        error = std::move(taskError);
        busy = false;
        changed.notify_all();
        if (stopping) return;
    }
}

} // namespace hikaru::frontend::detail
