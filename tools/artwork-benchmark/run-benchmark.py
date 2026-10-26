from pathlib import Path
import argparse
import datetime
import json
import os
import re
import statistics
import subprocess


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


root = Path(__file__).resolve().parents[2] / "build-artwork-benchmark"
parser = argparse.ArgumentParser(description="Compare native artwork scans with fresh state for each process.")
parser.add_argument("--before", type=Path, required=True, help="baseline benchmark executable")
parser.add_argument("--after", type=Path, required=True, help="fixed benchmark executable")
parser.add_argument("--library", type=Path, default=root / "library")
parser.add_argument("--output", type=Path, default=root / "runs", help="parent directory for uniquely labeled runs")
parser.add_argument("--count", type=positive_int, default=1000)
parser.add_argument("--pairs", type=positive_int, default=5)
parser.add_argument("--repeats", type=positive_int, default=10)
parser.add_argument("--label", default=datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
parser.add_argument("--interposer", type=Path, help="enable I/O counts using this macOS dylib; exclude these timings")
args = parser.parse_args()
if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", args.label):
    parser.error("--label must start with a letter or digit and contain only letters, digits, _, -, or .")
binaries = {"before": args.before.resolve(), "after": args.after.resolve()}
for side, binary in binaries.items():
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error(f"--{side} must be an executable file")
library = args.library.resolve()
for index in range(args.count):
    for extension in ("cue", "bin"):
        if not (library / f"game-{index:04d}.{extension}").is_file():
            parser.error(f"missing game-{index:04d}.{extension} in --library")
interposer = args.interposer.resolve() if args.interposer else None
if interposer and not interposer.is_file():
    parser.error("--interposer must be a macOS dylib file")
run_root = args.output.resolve() / args.label
run_root.mkdir(parents=True, exist_ok=False)
records = []
order = []
for pair in range(args.pairs):
    sides = ["before", "after"] if pair % 2 == 0 else ["after", "before"]
    for side in sides:
        stem = f"{pair + 1:02d}-{side}"
        state = run_root / f"{stem}-state"
        output = run_root / f"{stem}.json"
        env = dict(os.environ)
        env.update({
            "ARTWORK_BENCH_LIBRARY": str(library),
            "ARTWORK_BENCH_STATE": str(state),
            "ARTWORK_BENCH_OUTPUT": str(output),
            "ARTWORK_BENCH_COUNT": str(args.count),
            "ARTWORK_BENCH_REPEATS": str(args.repeats),
        })
        if interposer:
            env["DYLD_INSERT_LIBRARIES"] = str(interposer)
            env["ARTWORK_BENCH_REQUIRE_IO"] = "1"
        else:
            env.pop("DYLD_INSERT_LIBRARIES", None)
            env.pop("ARTWORK_BENCH_REQUIRE_IO", None)
        with (run_root / f"{stem}.log").open("w") as log:
            process = subprocess.run(
                [str(binaries[side]), "--gtest_filter=ArtworkBenchmark.Library"],
                env=env, cwd=run_root, stdout=log, stderr=subprocess.STDOUT, timeout=180,
            )
        if process.returncode != 0:
            raise RuntimeError(f"{stem} failed, see {run_root / (stem + '.log')}")
        result = json.loads(output.read_text())
        if (result["errors"] or result["database_records"] != args.count
                or result["games"] != args.count or result["repeat_passes"] != args.repeats
                or result["side"] != side or result["instrumented"] != bool(interposer)):
            raise RuntimeError(f"{stem} failed validation")
        expected_passes = {"first_scan": 1, "repeat_browsing": args.repeats, "persisted_restart": 1, "explicit_refresh": 1}
        if set(result["phases"]) != set(expected_passes):
            raise RuntimeError(f"{stem} missing benchmark phases")
        for phase, passes in expected_passes.items():
            measured = result["phases"][phase]
            if measured["errors"] or measured["passes"] != passes or measured["lookups"] != args.count * passes:
                raise RuntimeError(f"{stem} wrong work count in {phase}")
            if measured["disk_counter_status"] != [0, 0]:
                raise RuntimeError(f"{stem} physical disk counters unavailable")
        if interposer:
            if result.get("counter_control") != {"read_bytes": 16, "write_bytes": 16}:
                raise RuntimeError(f"{stem} counter self-test missing or failed")
            expected_scans = {
                "first_scan": args.count,
                "repeat_browsing": args.count * args.repeats if side == "before" else 0,
                "persisted_restart": args.count if side == "before" else 0,
                "explicit_refresh": args.count,
            }
            for phase, scans in expected_scans.items():
                if result["phases"][phase]["cue_opens"] != scans:
                    raise RuntimeError(f"{stem} wrong actual scan count in {phase}")
        records.append(result)
        order.append(stem)
        phase = result["phases"]["repeat_browsing"]
        opens = phase["cue_opens"] if interposer else "unmeasured"
        print(f'{stem}: repeat CPU {phase["cpu_ms"]:.3f} ms, elapsed {phase["elapsed_ms"]:.3f} ms, '
              f'cue opens {opens}, errors {result["errors"]}', flush=True)
summary = {"order": order, "instrumented": bool(interposer), "runs_per_side": args.pairs, "phases": {}}
logical_metrics = {"logical_read_bytes", "logical_write_bytes", "read_calls", "write_calls", "cue_opens", "bin_opens"}
for phase in records[0]["phases"]:
    summary["phases"][phase] = {}
    for side in binaries:
        same_side = [record["phases"][phase] for record in records if record["side"] == side]
        summary["phases"][phase][side] = {}
        for metric in same_side[0]:
            if metric == "disk_counter_status" or (not interposer and metric in logical_metrics):
                continue
            values = [record[metric] for record in same_side]
            summary["phases"][phase][side][metric] = {
                "median": statistics.median(values), "min": min(values), "max": max(values), "values": values,
            }
(run_root / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
print(f"Results {run_root / 'summary.json'}", flush=True)
