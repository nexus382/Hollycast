<!-- Portions Copyright 2026 The Hollycast Authors -->
# Movable VMU regression tests

These standalone tests need CMake, a C++17 compiler, and Python 3. They do not
build the emulator or need game images. From a compiler-enabled terminal:

```sh
cmake -S tests/vmu -B build/vmu-tests
cmake --build build/vmu-tests --config Release
ctest --test-dir build/vmu-tests -C Release --output-on-failure -LE interactive
```

The capture tests compile the current SDL capture/transition/hotkey functions,
mouse-event filtering and dispatch, absolute-position tracking, and raw-input handling
against deterministic stubs. The generator fails if its source boundaries change;
it never uses a saved copy of the production logic. Four variants cover desktop
Windows, Libretro, UWP, and other platforms. Coverage includes failed automatic
recapture, retry throttling, pause/resume, explicit overrides, and arcade gating.
They also cover returning from native drags with no intervening SDL events,
suppressed SDL/absolute raw motion, normal relative raw motion, and dev's file-opening events.

The native Windows test includes the production window module with small
emulator/configuration stubs. It creates real Win32 windows, injects transient
API failures, checks bitmap pixels, and exercises placement, sizing, dragging,
opacity, focus, persistence, and cleanup.
It also checks moved first-drag cancellation, temporary work-area/monitor recovery,
and failed coordinate queries without changing the desktop's monitor configuration.
Run it from an unlocked desktop and
leave the mouse alone while it runs; it temporarily moves the pointer and focus:

```sh
ctest --test-dir build/vmu-tests -C Release --output-on-failure -L interactive
```

The test restores the pointer/focus and releases held buttons on failure. The
second-monitor drag is skipped when only one monitor is available. These tests
do not cover fullscreen input inside a running game.
