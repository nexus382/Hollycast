<!-- Portions Copyright 2026 The Hollycast Authors -->

# Performance overlay regression checks

These focused checks compile the current overlay sampling, render dispatch,
GL/GL4 render scopes, framebuffer paths, and GPU query functions with deterministic
clock and device adapters. BSD checks use the production sampling functions with
syscall adapters for FreeBSD, NetBSD, and OpenBSD.
Generated C++ stays outside the checkout.
They require Python 3, a C++17 compiler, and the Vulkan-Headers submodule for the
device failure checks. They do not require an emulator build or a connected GPU.

Run from the repository root on Linux:

```sh
hollycast_checks="$(mktemp -d)"
python3 tests/regression/performance_overlay.py "$hollycast_checks"
python3 tests/regression/failure_paths.py "$hollycast_checks"
python3 tests/regression/framebuffer_paths.py "$hollycast_checks"
python3 tests/regression/bsd_sampling.py "$hollycast_checks"

g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -Icore \
  "$hollycast_checks/performance_overlay.cpp" -o "$hollycast_checks/overlay"
g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined \
  -Icore/deps/Vulkan-Headers/include \
  "$hollycast_checks/failure_paths.cpp" -o "$hollycast_checks/failures"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined \
  -Icore/deps/Vulkan-Headers/include \
  "$hollycast_checks/framebuffer_paths.cpp" -o "$hollycast_checks/framebuffers"
for platform in freebsd netbsd openbsd; do
  g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Icore \
    "$hollycast_checks/${platform}_sampling.cpp" -o "$hollycast_checks/$platform"
  "$hollycast_checks/$platform"
done

"$hollycast_checks/overlay"
"$hollycast_checks/failures"
"$hollycast_checks/framebuffers"
```

Keep assertions enabled; do not pass `-DNDEBUG`.

Coverage includes lifecycle sampling, hidden collectors, GPU sample generations,
per-VBlank timing validity, viewport reporting, partial GPU query allocation,
not-ready timestamp retention, frame-time graph independence, failed thread CPU
reads versus valid zero work, GL/GL4 query closure on render exceptions, SH4 cleanup
on non-threaded errors, and Vulkan query ownership through teardown reset. It also
covers GPU graph sampling gaps, framebuffer dimension reporting and upload failures,
BSD CPU normalization, partial/failed sysctl reads, and declining resident memory.

The device checks inject allocation and render failures. They do not exercise a real
graphics driver or an emulated game. BSD adapters do not check native BSD headers
or kernel behavior. Platform builds and gameplay checks remain necessary before
release.

## macOS UI font coverage

After building the macOS arm64 Debug application with Xcode, run:

```sh
python3 tests/regression/gui_fonts.py <xcode-build-dir> <temporary-output-dir>
```

This links the production font loader and checks that the built-in primary and
CJK fonts cover the language picker and translated strings for each UI language,
without relying on installed CJK fonts. It also rasterizes each tested glyph.
It does not replace visual checks of the running application.

## OpenBSD resource embedding

Run `python3 tests/regression/cmrc_openbsd.py <temporary-output-dir>` with
CMake and a Clang supporting the x86-64 OpenBSD target. It checks the generated
ELF payload and pointer relocations, unusual paths, cache invalidation after a
same-size change, and fallback generation at the size threshold. This is a
cross-target check and does not replace an OpenBSD build.
