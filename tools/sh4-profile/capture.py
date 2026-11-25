#!/usr/bin/env python3
"""Replay a pinned hollybench gameplay scene with SH4 profiling modes.

Requires the local hollycast-benchmark skill package and its private frame probe.
The normal hollybench CLI intentionally sanitizes its environment, so this
adapter injects only the documented SH4 profiling variables at app launch.
"""
import argparse
import csv
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import statistics
import sys
import tempfile
import time
import types

sys.dont_write_bytecode = True


def load_runner(package):
    sys.path.insert(0, str(package))
    loader = importlib.machinery.SourceFileLoader("hollybench_cli", str(package / "hollybench"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    cli = importlib.util.module_from_spec(spec)
    loader.exec_module(cli)
    import gameplay
    return cli, gameplay


def run_app(argv, *, cwd, env, timeout, stdin, stdout, stderr, extra_env, metrics):
    """Match the gameplay runner's file-output subprocess.run contract."""
    env = {**env, **extra_env}
    load_before = os.getloadavg()
    before = time.monotonic_ns()
    child = subprocess.Popen(argv, cwd=cwd, env=env, stdin=stdin, stdout=stdout, stderr=stderr)
    deadline = time.monotonic() + timeout
    while True:
        pid, status, usage = os.wait4(child.pid, os.WNOHANG)
        if pid:
            break
        if time.monotonic() >= deadline:
            child.kill()
            _, status, usage = os.wait4(child.pid, 0)
            child.returncode = os.waitstatus_to_exitcode(status)
            raise subprocess.TimeoutExpired(argv, timeout)
        time.sleep(0.02)
    child.returncode = os.waitstatus_to_exitcode(status)
    metrics.update(
        process_wall_seconds=(time.monotonic_ns() - before) / 1e9,
        process_user_seconds=usage.ru_utime,
        process_system_seconds=usage.ru_stime,
        process_cpu_seconds=usage.ru_utime + usage.ru_stime,
        process_cpu_scope="whole replay, including BIOS boot and scene; app child only (wait4)",
        process_max_rss=usage.ru_maxrss,
        load_average_before=load_before,
        load_average_after=os.getloadavg(),
    )
    return subprocess.CompletedProcess(argv, child.returncode)




def validate_guest_timing(samples):
    baseline = None
    traces = []
    for sample in samples:
        with Path(sample["trace"]).open() as stream:
            cycles = [int(row["guest_cycle"]) for row in csv.DictReader(stream)]
        if baseline is None:
            baseline = cycles
        traces.append({
            "application": sample.get("application", "baseline"),
            "mode": sample["mode"], "frame_rows": len(cycles),
            "guest_cycle_sequence_sha256": hashlib.sha256(
                ",".join(map(str, cycles)).encode()).hexdigest(),
            "same_guest_cycle_sequence_as_first": cycles == baseline,
            "profile_start_cycle": sample["profiler_metadata"]["start_cycle"],
            "profile_end_cycle": sample["profiler_metadata"]["end_cycle"],
            "probe_start_cycle": sample["probe"]["start_guest_cycle"],
            "probe_terminal_cycle": sample["probe"]["guest_cycle"],
            "screenshot_sha256": sample["screenshot_sha256"],
        })
    return {
        "all_guest_frame_cycles_identical": all(t["same_guest_cycle_sequence_as_first"] for t in traces),
        "all_profile_boundaries_identical": len({(t["profile_start_cycle"], t["profile_end_cycle"]) for t in traces}) <= 1,
        "all_probe_boundaries_identical": len({(t["probe_start_cycle"], t["probe_terminal_cycle"]) for t in traces}) <= 1,
        "all_screenshots_identical": len({t["screenshot_sha256"] for t in traces}) <= 1,
        "traces": traces,
    }


def summarize(samples):
    summary = {}
    for mode in dict.fromkeys(s["mode"] for s in samples):
        group = [s for s in samples if s["mode"] == mode]
        item = {"count": len(group)}
        for key in ("scene_thread_cpu_seconds", "scene_process_cpu_seconds", "process_cpu_seconds", "process_wall_seconds"):
            values = [s[key] for s in group if key in s]
            if values:
                item[key] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        item["average_fps"] = [s["stats"]["average_fps"] for s in group]
        item["stutter_counts"] = [s["stats"]["stutter_count"] for s in group]
        summary[mode] = item
    if "off" in summary:
        for mode, item in summary.items():
            if mode == "off":
                continue
            item["change_vs_runtime_off_percent"] = {
                key: 100 * (value["median"] / summary["off"][key]["median"] - 1)
                for key, value in item.items()
                if isinstance(value, dict) and "median" in value and key in summary["off"]
            }
    return summary


def application_comparison(samples):
    groups = {label: summarize([s for s in samples if s["application"] == label])
              for label in dict.fromkeys(s["application"] for s in samples)}
    comparisons = {}
    if "baseline" in groups and "candidate" in groups:
        for mode in groups["baseline"].keys() & groups["candidate"].keys():
            baseline, candidate = groups["baseline"][mode], groups["candidate"][mode]
            comparisons[mode] = {
                key: {"baseline": value, "candidate": candidate[key],
                      "median_change_percent": 100 * (candidate[key]["median"] / value["median"] - 1),
                      "ranges_overlap": candidate[key]["min"] <= value["max"] and value["min"] <= candidate[key]["max"]}
                for key, value in baseline.items()
                if isinstance(value, dict) and "median" in value and key in candidate
            }
    return groups, comparisons


def capture_sequence(modes, sequence, has_candidate):
    allowed = {"off", "sample", "counts", "both"}
    if sequence is None:
        result = [("baseline", mode) for mode in modes.split(",")]
    else:
        result = []
        for item in sequence.split(","):
            if item.count(":") != 1:
                raise ValueError("--sequence requires application:mode entries")
            result.append(tuple(item.split(":")))
    for label, mode in result:
        if label not in ({"baseline", "candidate"} if has_candidate else {"baseline"}) or mode not in allowed:
            raise ValueError("capture accepts baseline/candidate labels and off,sample,counts,both modes")
    if has_candidate and sequence is None:
        raise ValueError("--candidate-app requires an explicit --sequence")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--candidate-app", type=Path, help="second app for an explicitly labeled comparison")
    parser.add_argument("--require-candidate-trace", action="store_true", help="require positive guarded-poll trace entries in every candidate capture")
    parser.add_argument("--sequence", help="ordered baseline:off,candidate:off,... entries; requires candidate app for candidate entries")
    parser.add_argument("--check-only", action="store_true", help="validate media, app provenance and sequence without running gameplay")
    parser.add_argument("--output", type=Path, required=True, help="parent for a new unique capture directory")
    parser.add_argument("--profile", default="metal-slug")
    parser.add_argument("--modes", default="off,both,both,off,off,both",
                        help="ordered comma-separated modes; alternate baseline/instrumented order")
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--hollybench-package", type=Path,
                        default=Path.home() / ".codex/skills/hollycast-benchmark")
    args = parser.parse_args()
    try:
        sequence = capture_sequence(args.modes, args.sequence, args.candidate_app is not None)
    except ValueError as exc:
        parser.error(str(exc))
    if args.require_candidate_trace and not args.candidate_app:
        parser.error("--require-candidate-trace requires --candidate-app")
    if not 1 <= args.timeout <= 7200:
        parser.error("--timeout must be 1..7200")
    cli, gameplay = load_runner(args.hollybench_package.expanduser().resolve(strict=True))
    folder = gameplay.resolve_profile(args.profile)
    profile = gameplay.checked_profile(folder)
    apps = {"baseline": gameplay.app_info(args.app, cli)}
    if args.candidate_app:
        apps["candidate"] = gameplay.app_info(args.candidate_app, cli)
    for binary, info in apps.values():
        if info.get("provenance", {}).get("build_type") != "Release":
            raise ValueError("capture requires adjacent probe-build.json proving a Release build")
    if len(apps) > 1:
        baseline, candidate = [apps[label][1]["provenance"] for label in ("baseline", "candidate")]
        for key in ("architecture", "compiler", "cmake_settings"):
            if baseline.get(key) != candidate.get(key):
                raise ValueError(f"baseline and candidate probe settings differ: {key}")
    # Explicitly retain JIT and the guarded candidate path in recorded settings.
    gameplay.INI = gameplay.INI.replace("[config]\n", "[config]\nDynarec.Enabled = yes\nDynarec.GuardedPollTrace = yes\n", 1)
    if args.check_only:
        print(json.dumps({"status": "ready", "profile": args.profile,
                          "applications": {label: {"binary": str(item[0]), "sha256": item[1]["sha256"]}
                                           for label, item in apps.items()},
                          "config": gameplay.INI,
                          "sequence": [{"application": label, "mode": mode} for label, mode in sequence]}, indent=2))
        return
    parent = args.output.expanduser().resolve()
    parent.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="sh4-", dir=parent))
    report = run / "capture.json"
    doc = dict(schema="hollycast-sh4-capture-v1", status="running", profile=profile,
               binary=apps["baseline"][1], binaries={label: item[1] for label, item in apps.items()},
               host=cli.host(), config=gameplay.INI, modes=[mode for _, mode in sequence],
               sequence=[{"application": label, "mode": mode} for label, mode in sequence], samples=[],
               require_candidate_trace=args.require_candidate_trace,
               load_average_before=os.getloadavg(), report=str(report))
    cli.stored(report, doc)
    actual_subprocess = gameplay.subprocess
    try:
        with cli.timing_slot(300):
            for index, (label, mode) in enumerate(sequence, 1):
                binary, info = apps[label]
                if cli.file_hash(binary) != info["sha256"]:
                    raise ValueError(f"app binary changed after capture preflight: {binary}")
                name = f"sample-{index:02d}-{label}-{mode}" if len(apps) > 1 else f"sample-{index:02d}-{mode}"
                directory = run / name
                profile_path = directory / "sh4-profile.json"
                metrics = {}
                extra_env = dict(HOLLYCAST_SH4_PROFILE=str(profile_path),
                                 HOLLYCAST_SH4_PROFILE_MODE=mode,
                                 HOLLYCAST_SH4_PROFILE_START=str(profile["start_cycle"]),
                                 HOLLYCAST_SH4_PROFILE_END=str(profile["end_cycle"]))
                def launch(argv, **kwargs):
                    return run_app(argv, **kwargs, extra_env=extra_env, metrics=metrics)
                # Avoid changing subprocess.run globally: gameplay owns this facade.
                gameplay.subprocess = types.SimpleNamespace(run=launch, DEVNULL=subprocess.DEVNULL,
                                                            STDOUT=subprocess.STDOUT)
                print(f"Capturing {index}/{len(sequence)}: {label}/{mode} -> {directory}", flush=True)
                sample = gameplay.sample(binary, profile, folder / "replay.input", directory, args.timeout, None)
                if cli.file_hash(binary) != info["sha256"]:
                    raise ValueError(f"app binary changed during gameplay capture: {binary}")
                sample.update(application=label, binary_sha256=info["sha256"],
                              mode=mode, sh4_profile=str(profile_path), **metrics)
                if not profile_path.is_file():
                    raise ValueError(f"profiler did not produce {profile_path}")
                captured = json.loads(profile_path.read_text())
                metadata = captured["metadata"]
                if not metadata.get("complete") or metadata.get("thread_cpu_ns", 0) <= 0:
                    raise ValueError(f"profiler did not complete scene CPU boundaries: {profile_path}")
                if label == "candidate" and args.require_candidate_trace:
                    if metadata.get("guarded_poll_trace_enabled") is not True or metadata.get("guarded_poll_trace_entries", 0) <= 0:
                        raise ValueError(f"guarded poll trace never entered in candidate capture: {profile_path}")
                if mode != "off" and not captured.get("blocks"):
                    raise ValueError(f"no executed JIT metadata; check Dynarec.Enabled: {profile_path}")
                if mode in {"counts", "both"} and not any(b.get("executions", 0) for b in captured["blocks"]):
                    raise ValueError(f"no executed JIT block counters: {profile_path}")
                if mode in {"sample", "both"} and not captured.get("samples"):
                    raise ValueError(f"no runnable CPU samples: {profile_path}")
                sample.update(scene_thread_cpu_seconds=metadata["thread_cpu_ns"] / 1e9,
                              profiler_metadata=metadata)
                if "process_cpu_ns" in metadata:
                    sample["scene_process_cpu_seconds"] = metadata["process_cpu_ns"] / 1e9
                doc["samples"].append(sample)
                cli.stored(report, doc)
            grouped, compared = application_comparison(doc["samples"])
            doc.update(status="ok", load_average_after=os.getloadavg(),
                       summary=grouped["baseline"] if len(apps) == 1 else grouped,
                       application_comparison=compared,
                       guest_timing_validation=validate_guest_timing(doc["samples"]))
    except Exception as exc:
        doc.update(status="error", error=str(exc), load_average_after=os.getloadavg())
        raise
    finally:
        gameplay.subprocess = actual_subprocess
        cli.stored(report, doc)
        print(report, flush=True)


if __name__ == "__main__":
    main()
