#!/usr/bin/env bash
# Run the headless boot benchmark several times and report the best result per
# build. Each process replays the same frames from reset, so repeating the
# process is what averages out host noise - repeating windows inside one
# process would just measure different parts of the boot sequence.
#
#   tests/bench-boot.sh /path/to/game.cdi [runs] [build-dir ...]
set -euo pipefail

disc=${1:?usage: bench-boot.sh <disc> [runs] [build-dir ...]}
runs=${2:-3}
shift 2 || shift 1
builds=("$@")
if [ ${#builds[@]} -eq 0 ]; then
    builds=(build-jitless-tests build-jit-tests)
fi

for build in "${builds[@]}"; do
    bin="$build/RelWithDebInfo/Hollycast.app/Contents/MacOS/Hollycast"
    if [ ! -x "$bin" ]; then
        echo "skip $build (no binary)"
        continue
    fi
    best=""
    for _ in $(seq "$runs"); do
        line=$(HOLLYCAST_BENCH_DISC="$disc" "$bin" \
            --gtest_also_run_disabled_tests --gtest_filter='GameBootBench.*' 2>/dev/null \
            | grep fps || true)
        [ -z "$line" ] && continue
        fps=$(echo "$line" | awk '{print $3}')
        if [ -z "$best" ] || awk "BEGIN{exit !($fps > $best)}"; then
            best=$fps
            bestline=$line
        fi
    done
    if [ -n "$best" ]; then
        printf '%-22s %s\n' "$build" "$bestline"
    else
        echo "$build: no result"
    fi
done
