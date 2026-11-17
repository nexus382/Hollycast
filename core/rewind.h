// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace rewind_state
{
enum class FrameAction { Run, Restore, Hold };

// Requests are refcounted per source so several inputs bound to Rewind on one
// device, or several devices, cannot release each other's hold. Matching a
// press with one release per bound input is the caller's responsibility;
// releaseSource() drops a source unconditionally for cleanup paths such as
// preset switches and device unregistration. These calls are thread-safe.
void setRequested(const void* source, bool held);
void releaseSource(const void* source);
void releaseRequests();
bool requested();
void requestStep();
bool canStep();
const char* unavailableReason();
// History counters are owned by the rendering thread; read them only there.
std::size_t snapshotCount();
std::size_t memoryUsed();

// Invalidation is thread-safe; history is released by poll() on the render thread.
void invalidate();
void poll();
FrameAction beforeFrame();
void afterFrame(bool restored);
}
