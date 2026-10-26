# Compare repeated artwork scans

Build the same native workload against the baseline and the artwork cache fix on macOS. The workload generates no network requests. It checks metadata and persisted record counts after real disc reads.

Use CMake 3.24 or later, Ninja, Xcode command line tools, Python 3, and the dependencies required by the [project build instructions](../../README.md). Initialize the repository submodules in both source trees.

## Build both executables

Run these commands from the repository root. The overlay stays in the fixed checkout and applies to both sources. Generated tracks, binaries, state, and results stay under the ignored `build-artwork-benchmark` directory.

```sh
repo="$PWD"
bench_root="$repo/build-artwork-benchmark"
overlay="$repo/tools/artwork-benchmark/setup.cmake"
mkdir -p "$bench_root"
git worktree add --detach "$bench_root/before-source" c517c36618029fb65d495d0333211b469b147c7a
git -C "$bench_root/before-source" submodule update --init --recursive
git submodule update --init --recursive

cmake -S "$bench_root/before-source" -B "$bench_root/before-build" -G Ninja \
    -DCMAKE_PROJECT_INCLUDE="$overlay" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DENABLE_CTEST=ON -DUSE_VULKAN=OFF -DUSE_BREAKPAD=OFF \
    -DUSE_DISCORD=OFF -DUSE_OPENMP=OFF -DUSE_LUA=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake -S "$repo" -B "$bench_root/after-build" -G Ninja \
    -DCMAKE_PROJECT_INCLUDE="$overlay" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DENABLE_CTEST=ON -DUSE_VULKAN=OFF -DUSE_BREAKPAD=OFF \
    -DUSE_DISCORD=OFF -DUSE_OPENMP=OFF -DUSE_LUA=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER= -DCMAKE_CXX_COMPILER_LAUNCHER=
cmake --build "$bench_root/before-build" --target Hollycast
cmake --build "$bench_root/after-build" --target Hollycast

clang -std=c11 -O2 -dynamiclib tools/artwork-benchmark/io-counters.c \
    -o "$bench_root/io-counters.dylib"
```

On an Intel Mac, set both architecture flags to `x86_64`. The checked-in measurements use `arm64`.

The overlay enables Objective-C and Objective-C++, selects SDK zlib for Ninja, and links curl for the existing HTTP tests. It removes the app delegate from the test target so Google Test supplies `main`. It applies `-fno-access-control` only to `ArtworkBenchmark.cpp` so the workload can join the real worker futures and save the database after each batch. It selects the before or after assertions by detecting `OfflineArtState` in the target source tree.

## Generate tracks and run the comparison

```sh
python3 tools/artwork-benchmark/generate-fixture.py \
    --output "$bench_root/library" --count 1000 --track-mib 700

before="$bench_root/before-build/Hollycast.app/Contents/MacOS/Hollycast"
after="$bench_root/after-build/Hollycast.app/Contents/MacOS/Hollycast"
python3 tools/artwork-benchmark/run-benchmark.py \
    --before "$before" --after "$after" --library "$bench_root/library" \
    --output "$bench_root/runs" --count 1000 --pairs 5 --repeats 10 --label timing
python3 tools/artwork-benchmark/run-benchmark.py \
    --before "$before" --after "$after" --library "$bench_root/library" \
    --output "$bench_root/runs" --count 1000 --pairs 1 --repeats 10 --label io-counts \
    --interposer "$bench_root/io-counters.dylib"
```

Use a new `--label` for each comparison. The generator refuses an existing output directory. The runner refuses an existing run directory. Each process gets fresh state. Smaller `--count` and `--track-mib` values support local checks. Keep `--count` equal on both commands.

The runner prints repeat-phase CPU time, elapsed time, and validation errors. Each run directory contains per-process JSON, logs, state, and a `summary.json` with medians, ranges, and raw values. Without `--interposer`, logical I/O is unmeasured and omitted from the summary. With `--interposer`, a 16-byte read/write self-test and exact scan counts must pass. Keep instrumented timings separate from the timing comparison.

## Interpret the phases

The workload requests games in batches of 40 through `queueBoxart` and `startFetch`. It joins the production workers, saves the database, and validates the product ID, title, region, empty artwork, and idle completion. Online artwork downloading is disabled.

`first_scan` scans the generated library once. `repeat_browsing` makes ten further full passes by default. `persisted_restart` reconstructs the artwork service in the same process, then requests every game. `explicit_refresh` includes cache invalidation and one full rescan. Process startup, fixture generation, and final shutdown are outside these timed regions.

CPU time covers all process threads. Elapsed time uses a steady clock. macOS process disk counters report attributed bytes. The optional interposer counts bytes returned by `fread`, `fgets`, and `fwrite`, plus successful CUE and BIN opens. It does not count all filesystem operations or durable SSD writes.

Read the [measured results and limits](../../docs/performance/artwork-scan.md) and the compact [sample evidence](sample-results.json). Record the revisions, build flags, host, and background load when you publish another comparison.
