#!/usr/bin/env bash
# Shared helpers for the benchmark scripts.

# macOS iCloud Drive's sync daemon periodically pins a performance core for
# minutes at a time. On this machine that took the noise floor - five runs of
# the same binary - from 4.4% to 16.4%, which is wider than most of the changes
# these scripts are used to measure, so a contaminated run can easily read as a
# regression or an improvement.
#
# Report it rather than silently producing bad numbers. Set HOLLYCAST_BENCH_QUIET=1
# to stop the daemon first; launchd restarts it idle afterwards, and it syncs
# nothing belonging to this repo.
bench_bird_cpu() {
    ps -Ao pcpu,comm | awk '/iCloudDriveCore.*bird/ { total += $1 } END { printf "%.0f", total + 0 }'
}

bench_check_quiet() {
    if [ "${HOLLYCAST_BENCH_QUIET:-0}" = "1" ] && [ "$(bench_bird_cpu)" -gt 5 ]; then
        killall bird 2>/dev/null || true
        sleep 3
    fi
    local busy
    busy=$(bench_bird_cpu)
    if [ "$busy" -gt 5 ]; then
        echo "bench: WARNING iCloud sync (bird) is at ${busy}% CPU - results will be noisy." >&2
        echo "bench: run 'killall bird' or set HOLLYCAST_BENCH_QUIET=1 to suppress it." >&2
    fi
}
