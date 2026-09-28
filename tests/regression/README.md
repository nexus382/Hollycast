<!-- Portions Copyright 2026 The Hollycast Authors -->

# Performance overlay regression checks

These focused checks compile the current overlay sampling and GPU query
allocation functions with deterministic clock and device adapters.
Generated C++ stays outside the checkout.
They require Python 3, a C++17 compiler, and the Vulkan-Headers submodule for the
device failure checks. They do not require an emulator build or a connected GPU.

Run from the repository root on Linux:

```sh
hollycast_checks="$(mktemp -d)"
python3 tests/regression/performance_overlay.py "$hollycast_checks"
python3 tests/regression/failure_paths.py "$hollycast_checks"

g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -Icore \
  "$hollycast_checks/performance_overlay.cpp" -o "$hollycast_checks/overlay"
g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined \
  -Icore/deps/Vulkan-Headers/include \
  "$hollycast_checks/failure_paths.cpp" -o "$hollycast_checks/failures"

"$hollycast_checks/overlay"
"$hollycast_checks/failures"
```

Keep assertions enabled; do not pass `-DNDEBUG`.

Coverage includes lifecycle sampling, hidden collectors, GPU sample generations,
per-VBlank timing validity, viewport reporting and partial GPU query allocation.

The device checks inject allocation failures. They do not exercise a real
graphics driver or an emulated game. Platform builds and gameplay checks remain
necessary before release.
