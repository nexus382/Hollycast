// Portions Copyright 2026 The Hollycast Authors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>

namespace rewind_state
{
enum class FrameAction { Run, Restore, Hold };

#ifdef LIBRETRO
// Libretro frontends own rewind controls and state; shared engine hooks stay inert.
inline void invalidate() {}
inline void releaseRequests() {}
inline void setButtonInputAllowed(bool) {}
#else
// Disallowing buttons also clears pending held and step requests.
void setButtonInputAllowed(bool allowed);

// Requests are refcounted per source so several inputs bound to Rewind on one
// device, or several devices, cannot release each other's hold. Matching a
// press with one release per bound input is the caller's responsibility;
// releaseSource() drops a source unconditionally for cleanup paths such as
// preset switches and device unregistration. These calls are thread-safe.
void setRequested(const void* source, bool held);
void releaseSource(const void* source);
void releaseRequests();
bool requested();
bool requested(const void* source);
void requestStep();
bool canStep();
// Selects the layout while the guest is stopped. Never switches a running CPU thread.
bool requiresSharedThread();
const char* unavailableReason();
// History counters are owned by the rendering thread; read them only there.
std::size_t snapshotCount();
std::size_t memoryUsed();

// Invalidation is thread-safe; history is released by poll() on the render thread.
void invalidate();
void poll(bool loading = false);
FrameAction beforeFrame();
void afterFrame();
#endif
}
