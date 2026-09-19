#!/usr/bin/env python3
"""Run and validate the same dump-free ALU/RAM benchmark in ARM64 JIT and VM builds."""
import argparse
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET

FILTER = 'Sh4BackendThroughputTest.*'
TESTS = {'AluLoopHostSpeed', 'RamLoopHostSpeed'}
CODE_HASHES = {'ALU': 4010934473, 'RAM': 1377643567}
ITERS = 40_000_000
WARMUP = 64


def run(binary, backend, directory, round_id):
    report = directory / f'{backend}-{round_id}.xml'
    report.unlink(missing_ok=True)
    env = os.environ.copy()
    for name in ('HOLLYCAST_AOT_HOT_DUMP', 'HOLLYCAST_BENCH_DISC', 'HOLLYCAST_BENCH_STATE',
                 'GTEST_FILTER', 'GTEST_TOTAL_SHARDS', 'GTEST_SHARD_INDEX', 'GTEST_REPEAT',
                 'GTEST_SHUFFLE', 'GTEST_FAIL_FAST', 'GTEST_BREAK_ON_FAILURE'):
        env.pop(name, None)
    command = [str(Path(binary).resolve()), '--gtest_filter=' + FILTER,
               '--gtest_color=no', '--gtest_output=xml:' + str(report)]
    result = subprocess.run(command, cwd=directory, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
    (directory / f'{backend}-{round_id}.log').write_text(result.stdout)
    assert result.returncode == 0, result.stdout
    root = ET.parse(report).getroot()
    cases = root.findall('.//testcase')
    assert len(cases) == 2, f'{backend}: expected 2 tests, got {len(cases)}'
    assert {case.attrib['name'] for case in cases} == set(TESTS)
    for case in cases:
        assert case.attrib['classname'] == 'Sh4BackendThroughputTest'
        assert case.attrib.get('status') == 'run' and case.attrib.get('result') == 'completed'
        assert case.find('failure') is None and case.find('skipped') is None
    rows = [json.loads(line.removeprefix('SH4_BENCH ')) for line in result.stdout.splitlines()
            if line.startswith('SH4_BENCH ')]
    assert len(rows) == 2 and {row['workload'] for row in rows} == {'ALU', 'RAM'}
    for row in rows:
        assert row['backend'] == backend and row['iterations'] == ITERS and row['warmup'] == WARMUP
        assert row['ops_per_iteration'] == (3 if row['workload'] == 'ALU' else 5)
        assert row['code_hash'] == CODE_HASHES[row['workload']]
        assert row['r0'] == ITERS and row['r1'] == 0
        assert row['ram'] == (ITERS if row['workload'] == 'RAM' else 0)
        assert row['pc'] == (0xAC000008 if row['workload'] == 'ALU' else 0xAC00000C)
        assert row['seconds'] > 0 and math.isfinite(row['seconds'])
        expected = ITERS * row['ops_per_iteration'] / row['seconds'] / 1e6
        assert math.isclose(row['mops'], expected, rel_tol=1e-6)
        print('SH4_BENCH ' + json.dumps(row, sort_keys=True), flush=True)
    return {row['workload']: row for row in rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('jit')
    parser.add_argument('vm')
    parser.add_argument('--rounds', type=int, default=1)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Python assertion checks must be enabled')
    assert args.rounds > 0, 'rounds must be positive'
    directory = args.output.resolve() if args.output else Path(tempfile.mkdtemp(prefix='sh4-bench-'))
    directory.mkdir(parents=True, exist_ok=True)
    results = {'jit': [], 'shil_vm': []}
    for index in range(args.rounds):
        order = [('jit', args.jit), ('shil_vm', args.vm)]
        if index % 2:
            order.reverse()
        current = {}
        for backend, binary in order:
            current[backend] = run(binary, backend, directory, index)
            results[backend].append(current[backend])
        for workload in ('ALU', 'RAM'):
            a, b = current['jit'][workload], current['shil_vm'][workload]
            for key in set(a) - {'backend', 'seconds', 'mops'}:
                assert a[key] == b[key], (workload, key, a[key], b[key])
    for workload in ('ALU', 'RAM'):
        jit = max(rows[workload]['mops'] for rows in results['jit'])
        vm = max(rows[workload]['mops'] for rows in results['shil_vm'])
        print(f'{workload}: JIT {jit:.2f} Mops/s  SHIL VM {vm:.2f} Mops/s  VM/JIT {100 * vm / jit:.2f}%')
    print(f'PASS: identical {FILTER}; 2 passing tests/build/round; matching ALU/RAM work and results; logs {directory}')


if __name__ == '__main__':
    main()
