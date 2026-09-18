#!/usr/bin/env bash
# Run the headless boot benchmark several times per build and report the best
# result plus the spread. Each process replays the same frames from reset, so
# repeating the process is what averages out host noise - repeating windows
# inside one process would just measure different parts of the boot sequence.
#
# Report the best, not the mean: every disturbance makes a run slower and none
# make it faster, so the fastest run is closest to the emulator's real speed.
# The spread is printed so that a comparison is not read as meaningful when it
# falls inside the run-to-run variation, which on a warm laptop is easily
# several percent.
#
#   tests/bench-boot.sh /path/to/game.cdi [runs] [build-dir ...]
set -euo pipefail

disc=${1:?usage: bench-boot.sh <disc> [runs] [build-dir ...]}
runs=${2:-5}
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
    results=()
    for _ in $(seq "$runs"); do
        line=$(HOLLYCAST_BENCH_DISC="$disc" "$bin" \
            --gtest_also_run_disabled_tests --gtest_filter='GameBootBench.*' 2>/dev/null \
            | grep -E '[0-9.]+ fps' || true)
        [ -z "$line" ] && continue
        # Match on the fps label, not a column index: the disc name contains
        # spaces, so field positions shift from one game to the next.
        results+=("$(sed -E 's/.*[[:space:]]([0-9.]+) fps.*/\1/' <<<"$line")")
    done
    if [ ${#results[@]} -eq 0 ]; then
        echo "$build: no result"
        continue
    fi
    printf '%s\n' "${results[@]}" | sort -rn | awk -v b="$build" -v n="${#results[@]}" '
        NR == 1 { best = $1 }
        { worst = $1 }
        END {
            printf "%-22s best %7.1f fps  %6.3f ms/frame  spread %4.1f%%  (%d runs)\n",
                b, best, 1000 / best, (best - worst) * 100 / best, n
        }'
done
