#!/usr/bin/env python3
"""Independent raw-log oracle for the integrated ARM64 synthetic comparison."""
import argparse
import json
import math
from pathlib import Path
import statistics
import xml.etree.ElementTree as ET


def check(directory, require_target=False):
    results = {}
    for backend in ('jit', 'shil_vm'):
        logs = sorted(directory.glob(f'{backend}-*.log'))
        assert len(logs) == 9, f'{backend}: expected nine runs'
        results[backend] = {'ALU': [], 'RAM': []}
        for log in logs:
            cases = ET.parse(log.with_suffix('.xml')).findall('.//testcase')
            assert len(cases) == 2
            assert {c.attrib['name'] for c in cases} == {'AluLoopHostSpeed', 'RamLoopHostSpeed'}
            for case in cases:
                assert case.attrib['classname'] == 'Sh4BackendThroughputTest'
                assert case.attrib['status'] == 'run' and case.attrib['result'] == 'completed'
                assert case.find('failure') is None and case.find('skipped') is None
            rows = [json.loads(line[10:]) for line in log.read_text().splitlines()
                    if line.startswith('SH4_BENCH ')]
            assert len(rows) == 2 and {r['workload'] for r in rows} == {'ALU', 'RAM'}
            for row in rows:
                ram = row['workload'] == 'RAM'
                # Independently reconstruct the byte hash from the guest words.
                words = ([0x6042, 0x7001, 0x2402, 0x4110, 0x8BFA] if ram
                         else [0x7001, 0x4110, 0x8BFC]) + [0x2502, 0xAFFE, 0x0009]
                digest = 2166136261
                for word in words:
                    for byte in word.to_bytes(2, 'little'):
                        digest = ((digest ^ byte) * 16777619) & 0xffffffff
                assert row['code_hash'] == digest
                assert row['backend'] == backend and row['warmup'] == 64
                assert row['iterations'] == row['r0'] == 40_000_000 and row['r1'] == 0
                assert row['ram'] == (40_000_000 if ram else 0)
                assert row['pc'] == 0xAC000000 + (12 if ram else 8)
                assert row['ops_per_iteration'] == (5 if ram else 3)
                assert math.isfinite(row['seconds']) and row['seconds'] > 0
                assert math.isclose(row['mops'] * row['seconds'], 40 * (5 if ram else 3), rel_tol=1e-6)
                results[backend][row['workload']].append(row['seconds'])
    summary = json.loads((directory / 'summary.json').read_text())
    passed = True
    for workload in ('ALU', 'RAM'):
        ratio = statistics.median(results['jit'][workload]) / statistics.median(results['shil_vm'][workload])
        assert math.isclose(ratio, summary[workload]['vm_jit_ratio'], rel_tol=1e-12)
        assert summary[workload]['target_met'] == (ratio >= 0.75)
        passed &= ratio >= 0.75
        print(f'{workload}: VM/JIT={ratio:.12f} ({100 * ratio:.6f}%), target_met={ratio >= 0.75}')
    print('PASS: nine raw-log/XML pairs; independent guest hashes, final results and median ratios')
    if require_target and not passed:
        raise SystemExit('FAIL: at least one workload is below 75% of JIT')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--require-target', action='store_true')
    args = parser.parse_args()
    if not __debug__:
        parser.error('assertions must be enabled')
    check(args.directory, args.require_target)
