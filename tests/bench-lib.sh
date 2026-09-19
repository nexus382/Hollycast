#!/usr/bin/env bash
# Shared helpers for the benchmark scripts.

# macOS iCloud Drive's sync daemon periodically pins a performance core for
# minutes at a time. On this machine that took the noise floor - five runs of
# the same binary - from 4.4% to 16.4%, which is wider than most of the changes
# these scripts are used to measure, so a contaminated run can easily read as a
# regression or an improvement.
#
# Report it rather than silently producing bad numbers. Set HOLLYCAST_BENCH_QUIET=1
# to stop the daemon first and refuse to benchmark until it stays down; launchd
# restarts it idle afterwards, and it syncs nothing belonging to this repo.

# Above this the daemon is doing real work, not idle bookkeeping.
bench_bird_busy_pct=5
# launchd restarts bird within seconds and it resumes the sync it was part way
# through, so a single kill followed by a single reading proves nothing: it has
# to read idle this many times in a row before a run may start.
bench_bird_quiet_samples=3
# Seconds between those readings. ps reports a decaying average, so a daemon
# that has just been restarted needs a moment before its figure means anything.
bench_bird_quiet_interval=2
# Stop killing and fail instead once waiting this long has not produced a quiet
# machine - something is driving the sync that this cannot wait out.
bench_bird_quiet_timeout=90

bench_bird_cpu() {
    ps -Ao pcpu,comm | awk '/iCloudDriveCore.*bird/ { total += $1 } END { printf "%.0f", total + 0 }'
}

# Without HOLLYCAST_BENCH_QUIET the noise is only reported. With it, the run is
# entitled to a clean floor, so the daemon is killed until it stays quiet and
# the run fails outright if it will not - benchmarking anyway would hand back
# numbers the flag says are trustworthy when they are not.
bench_check_quiet() {
    local busy
    if [ "${HOLLYCAST_BENCH_QUIET:-0}" != "1" ]; then
        busy=$(bench_bird_cpu)
        if [ "$busy" -gt "$bench_bird_busy_pct" ]; then
            echo "bench: WARNING iCloud sync (bird) is at ${busy}% CPU - results will be noisy." >&2
            echo "bench: run 'killall bird' or set HOLLYCAST_BENCH_QUIET=1 to suppress it." >&2
        fi
        return 0
    fi

    local quiet=0 waited=0 kills=0
    while :; do
        busy=$(bench_bird_cpu)
        if [ "$busy" -le "$bench_bird_busy_pct" ]; then
            quiet=$((quiet + 1))
            if [ "$quiet" -ge "$bench_bird_quiet_samples" ]; then
                if [ "$kills" -gt 0 ]; then
                    echo "bench: iCloud sync (bird) quiet after ${kills} kill(s), ${waited}s" >&2
                fi
                return 0
            fi
        else
            if [ "$waited" -ge "$bench_bird_quiet_timeout" ]; then
                echo "bench: iCloud sync (bird) is still at ${busy}% CPU after ${waited}s and ${kills} kill(s)." >&2
                echo "bench: HOLLYCAST_BENCH_QUIET=1 promises a quiet machine, so refusing to benchmark." >&2
                echo "bench: pause iCloud Drive from the menu bar and run this again." >&2
                exit 1
            fi
            if [ "$kills" -eq 0 ]; then
                echo "bench: iCloud sync (bird) is at ${busy}% CPU - stopping it and waiting for the machine to settle." >&2
            fi
            killall bird 2>/dev/null || true
            kills=$((kills + 1))
            quiet=0
        fi
        sleep "$bench_bird_quiet_interval"
        waited=$((waited + bench_bird_quiet_interval))
    done
}
