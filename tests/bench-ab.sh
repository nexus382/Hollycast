#!/usr/bin/env bash
# Interleaved A/B benchmark for two prebuilt Hollycast test binaries.
#
# A single before/after pair is worthless on a shared machine: every earlier
# comparison on this codebase that was taken that way had to be redone. Runs
# here alternate A and B so that any drift in host load falls on both sides
# equally, and the best run of each is reported because noise only ever makes
# a run slower.
#
# Pass the same binary twice to measure the noise floor - no delta below that
# figure means anything.
#
#   tests/bench-ab.sh <binA> <binB> [rounds] [labelA] [labelB]
set -euo pipefail

binA=${1:?usage: bench-ab.sh <binA> <binB> [rounds] [labelA] [labelB]}
binB=${2:?usage: bench-ab.sh <binA> <binB> [rounds] [labelA] [labelB]}
rounds=${3:-6}
labelA=${4:-A}
labelB=${5:-B}

disc="/Users/thomasswift/Downloads/crazy_taxi_extracted/Crazy Taxi/Crazy Taxi.cdi"
slot=build-jitless-tests/RelWithDebInfo/Hollycast.app/Contents/MacOS/Hollycast

# Take the machine-wide benchmark lock before touching anything shared. The
# binary slot below is shared too, so a run that snapshotted or restored it
# while waiting would swap the binary out from under the holder and report a
# comparison between two binaries it never named.
lock=/tmp/hollycast-bench.lock
waited=0
while ! mkdir "$lock" 2>/dev/null; do
    # A worker killed mid-run would otherwise block every later benchmark, so
    # take the lock back once it is far older than any honest run.
    if [ -n "$(find "$lock" -maxdepth 0 -mmin +60 2>/dev/null)" ]; then
        echo "bench-ab: breaking stale lock $lock" >&2
        rm -rf "$lock"
        continue
    fi
    if [ "$waited" -eq 0 ]; then
        echo "bench-ab: another benchmark is running, waiting for $lock" >&2
    fi
    sleep 15
    waited=$((waited + 15))
done
trap 'rm -rf "$lock"' EXIT
if [ "$waited" -gt 0 ]; then
    echo "bench-ab: acquired lock after ${waited}s" >&2
fi

# Both binaries are run from the same app bundle, so they see identical
# resources and the bundle layout cannot itself become a variable.
saved=$(mktemp /tmp/hc-slot-XXXX)
cp "$slot" "$saved"
restore() { cp "$saved" "$slot"; rm -f "$saved"; rm -rf "$lock"; }
trap restore EXIT

run_one() {
    cp "$1" "$slot"
    HOLLYCAST_BENCH_DISC="$disc" "$slot" --gtest_also_run_disabled_tests \
        --gtest_filter='GameBootBench.DISABLED_FramePace' 2>/dev/null \
        | sed -nE 's/.*[[:space:]]([0-9.]+) fps.*/\1/p'
}

a_results=()
b_results=()
for i in $(seq "$rounds"); do
    # macOS ships bash 3.2, which has no negative array subscripts.
    a=$(run_one "$binA")
    b=$(run_one "$binB")
    if [ -z "$a" ] || [ -z "$b" ]; then
        echo "bench-ab: a run produced no fps line, aborting" >&2
        exit 1
    fi
    a_results+=("$a")
    b_results+=("$b")
    echo "round $i: $labelA $a  $labelB $b" >&2
done

summarise() {
    printf '%s\n' "${@:2}" | sort -rn | awk -v l="$1" '
        NR == 1 { best = $1 } { worst = $1 }
        END { printf "%-10s best %7.1f fps  worst %7.1f  spread %4.1f%%\n",
              l, best, worst, (best - worst) * 100 / best }'
}
summarise "$labelA" "${a_results[@]}"
summarise "$labelB" "${b_results[@]}"

bestA=$(printf '%s\n' "${a_results[@]}" | sort -rn | head -1)
bestB=$(printf '%s\n' "${b_results[@]}" | sort -rn | head -1)
awk -v a="$bestA" -v b="$bestB" -v la="$labelA" -v lb="$labelB" \
    'BEGIN { printf "%s vs %s: %+.1f%%\n", lb, la, (b - a) * 100 / a }'
