#!/usr/bin/env python3
"""Independent audit gate: arithmetic oracle plus nine real interleaved pairs."""
import json
import math
from pathlib import Path
import statistics
import sys

sys.dont_write_bytecode = True

from check_backend_bench import summarise


def check(directory):
    # Independently chosen arithmetic oracle: median ratios differ from best runs.
    fixture = {
        'jit': [{w: {'seconds': t} for w in ('ALU', 'RAM')} for t in (1, 2, 9)],
        'shil_vm': [{w: {'seconds': t} for w in ('ALU', 'RAM')} for t in (2, 8, 10)],
    }
    row = summarise(fixture)['ALU']
    assert row['vm_jit_ratio'] == 0.25
    assert row['paired_ratio_median'] == 0.5
    assert row['jit']['mad_percent'] == 50
    assert row['jit']['range_percent'] == 400
    assert not row['target_met']
    fixture['shil_vm'] = fixture['jit']
    assert summarise(fixture)['ALU']['target_met']

    summary = json.loads((directory / 'summary.json').read_text())
    for workload in ('ALU', 'RAM'):
        values = {}
        for backend in ('jit', 'shil_vm'):
            logs = sorted(directory.glob(f'{backend}-*.log'))
            assert len(logs) >= 9, 'need at least nine samples per backend'
            values[backend] = []
            for log in logs:
                rows = [json.loads(line[10:]) for line in log.read_text().splitlines()
                        if line.startswith('SH4_BENCH ')]
                row, = [r for r in rows if r['workload'] == workload]
                assert row['backend'] == backend
                assert row['iterations'] == row['r0'] == 40_000_000 and row['r1'] == 0
                assert row['code_hash'] == {'ALU': 4010934473, 'RAM': 1377643567}[workload]
                assert row['ram'] == (40_000_000 if workload == 'RAM' else 0)
                assert math.isfinite(row['seconds']) and row['seconds'] > 0
                values[backend].append(row['seconds'])
            assert sorted(values[backend]) == sorted(summary[workload][backend]['seconds'])
        ratio = statistics.median(values['jit']) / statistics.median(values['shil_vm'])
        assert math.isclose(ratio, summary[workload]['vm_jit_ratio'], rel_tol=1e-12)
        assert summary[workload]['target_met'] == (ratio >= 0.75)
    print('PASS: median/MAD oracle; >=9 real samples/backend; identical ALU/RAM results; ratios and 0.75 gate')


if __name__ == '__main__':
    if not __debug__:
        raise SystemExit('Python assertion checks must be enabled')
    check(Path(sys.argv[1]))
