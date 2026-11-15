#!/usr/bin/env python3
"""Summarize a completed paired guarded-trace replay experiment."""
import argparse
import collections
import csv
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import statistics


def load_analyzer():
    spec=importlib.util.spec_from_file_location('sh4_analyze',Path(__file__).with_name('analyze.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def summary(values):
    return {'median':statistics.median(values),'min':min(values),'max':max(values),'samples':values}


def normalized_counts(capture,analyzer):
    signatures={block['id']:analyzer.code_signature(block) for block in capture['blocks']}
    blocks,edges,exits=collections.Counter(),collections.Counter(),collections.Counter()
    for block in capture['blocks']:
        if block['executions']:blocks[signatures[block['id']]]+=block['executions']
    for edge in capture['edges']:
        edges[(signatures[edge['from']],edge['to_pc'],edge.get('kind','branch'))]+=edge['count']
        if edge.get('kind')!='call_continuation_estimate':exits[edge['from']]+=edge['count']
    digest=lambda counts:hashlib.sha256(json.dumps(sorted(counts.items())).encode()).hexdigest()
    return {'block_sha256':digest(blocks),'edge_sha256':digest(edges),
            'entries':sum(block['executions'] for block in capture['blocks']),
            'non_estimated_exits':sum(exits.values()),'executed_code_groups':len(blocks),
            'entry_exit_mismatches':[{'pc':hex(block['pc']),'entries':block['executions'],'exits':exits[block['id']]}
                                     for block in capture['blocks'] if block['executions']!=exits[block['id']]]}


def resolve_profile(report,sample):
    path=Path(sample['sh4_profile'])
    return path if path.is_file() else report.parent/path.parent.name/path.name


def timing_fingerprint(sample):
    with Path(sample['trace']).open() as source:
        cycles=[int(row['guest_cycle']) for row in csv.DictReader(source)]
    return {'frame_rows':len(cycles),'guest_frame_cycles_sha256':hashlib.sha256(','.join(map(str,cycles)).encode()).hexdigest(),
            'profile_bounds':[sample['profiler_metadata']['start_cycle'],sample['profiler_metadata']['end_cycle']],
            'probe_bounds':[sample['probe']['start_guest_cycle'],sample['probe']['guest_cycle']],
            'screenshot_sha256':sample['screenshot_sha256']}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture_report',type=Path)
    parser.add_argument('--candidate-counts',type=Path,required=True,help='successful counted candidate correctness smoke')
    parser.add_argument('--baseline-counts',type=Path,required=True,help='preserved counted baseline profile')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--test-log',type=Path,help='optional completed regression test log to fingerprint')
    parser.add_argument('--fixed-work',type=Path,help='optional disabled actual-JIT fixed-work benchmark JSON')
    parser.add_argument('--fixed-work-provenance',type=Path,help='optional benchmark binary/source fingerprint sidecar')
    parser.add_argument('--exploratory-comparison',type=Path,help='optional prior variant comparison, retained separately from final results')
    parser.add_argument('--exploratory-fixed-work',type=Path,help='optional prior variant fixed-work data')
    args=parser.parse_args()
    report=args.capture_report.resolve(strict=True)
    captured=json.loads(report.read_text())
    if captured.get('status')!='ok':parser.error('capture report is incomplete or failed')
    analyzer=load_analyzer()
    counted_candidate=json.loads(args.candidate_counts.read_text())
    counted_baseline=json.loads(args.baseline_counts.read_text())
    if not counted_candidate.get('metadata',{}).get('complete') or counted_candidate['metadata'].get('guarded_poll_trace_entries',0)<=0:
        parser.error('candidate counted smoke must be complete with positive guarded trace entries')
    count_checks={label:normalized_counts(value,analyzer) for label,value in [('baseline',counted_baseline),('candidate',counted_candidate)]}
    count_checks['block_counts_identical']=count_checks['baseline']['block_sha256']==count_checks['candidate']['block_sha256']
    count_checks['edge_counts_identical']=count_checks['baseline']['edge_sha256']==count_checks['candidate']['edge_sha256']
    metrics={}
    keys=['scene_thread_cpu_seconds','scene_process_cpu_seconds','process_cpu_seconds','process_wall_seconds','average_fps','p99_frame_ms','worst_frame_ms']
    for label in ('baseline','candidate'):
        samples=[sample for sample in captured['samples'] if sample['application']==label and sample['mode']=='off']
        if not samples:parser.error(f'no off-mode {label} samples')
        values={key:[] for key in keys}
        for sample in samples:
            for key in keys[:4]:values[key].append(sample[key])
            values['average_fps'].append(sample['stats']['average_fps'])
            values['p99_frame_ms'].append(sample['stats']['frame_time_ms']['p99'])
            values['worst_frame_ms'].append(sample['stats']['frame_time_ms']['worst'])
        metrics[label]={key:summary(value) for key,value in values.items()}
    changes={}
    for key in keys:
        base,candidate=metrics['baseline'][key],metrics['candidate'][key]
        changes[key]={'median_change_percent':100*(candidate['median']/base['median']-1),
                      'ranges_overlap':candidate['min']<=base['max'] and base['min']<=candidate['max']}
    off=[sample for sample in captured['samples'] if sample['mode']=='off']
    paired=[]
    if len(off)%2==0:
        for left,right in zip(off[::2],off[1::2]):
            if left['application']==right['application']:continue
            base,candidate=(left,right) if left['application']=='baseline' else (right,left)
            paired.append({'order':[left['application'],right['application']],
                           'thread_cpu_change_percent':100*(candidate['scene_thread_cpu_seconds']/base['scene_thread_cpu_seconds']-1),
                           'process_cpu_change_percent':100*(candidate['scene_process_cpu_seconds']/base['scene_process_cpu_seconds']-1)})
    runs=[];hot=[]
    for sample in captured['samples']:
        runs.append({'application':sample['application'],'mode':sample['mode'],'binary_sha256':sample['binary_sha256'],
                     'timing':timing_fingerprint(sample),'trace_entries':sample['profiler_metadata'].get('guarded_poll_trace_entries',0),
                     'scene_thread_cpu_seconds':sample['scene_thread_cpu_seconds'],'scene_process_cpu_seconds':sample['scene_process_cpu_seconds'],'whole_replay_cpu_seconds':sample['process_cpu_seconds'],
                     'average_fps':sample['stats']['average_fps'],'p99_frame_ms':sample['stats']['frame_time_ms']['p99'],
                     'worst_frame_ms':sample['stats']['frame_time_ms']['worst'],'stutter_count':sample['stats']['stutter_count'],
                     'all_intervals_over_threshold':sample['stats']['events'],'stutter_threshold_ms':sample['stats']['stutter_threshold_ms'],
                     'profile_metadata':sample['profiler_metadata']})
        if sample['application']=='candidate' and sample['mode']=='sample':
            path=resolve_profile(report,sample)
            raw=json.loads(path.read_text())
            analyzed=analyzer.analyze(analyzer.combine_captures(raw,counted_candidate))
            target=next(row for row in analyzed['loops'] if row['kind']=='two_block_cycle' and {analyzer.number(block['pc']) for block in row['blocks']}=={0x8c05b3a4,0x8c05b3d0})
            hot.append({'capture':str(path),'capture_sha256':sha(path),'accounting':analyzed['accounting'],
                        'sample_summary':analyzed['samples'],'boundary_match':analyzed['metadata']['capture_boundary_match'],
                        'unmatched_code_samples':analyzed['metadata']['unmatched_sample_capture_cpu_samples'],
                        'cycle':{key:target[key] for key in ['exclusive_cpu_samples','exclusive_cpu_percent','exclusive_cpu_ms_estimate','exclusive_process_cpu_percent_estimate','exclusive_cpu_sample_categories','exclusive_guest_pc_samples','cycle_edge_traversals']}})
    fingerprints=[json.dumps(run['timing'],sort_keys=True) for run in runs]
    pooled_hot=sum(run['cycle']['exclusive_cpu_samples'] for run in hot)
    pooled_samples=sum(run['accounting']['total_cpu_samples'] for run in hot)
    test_evidence=None
    if args.test_log:
        contents=args.test_log.read_text()
        passed=re.findall(r'\[\s*PASSED\s*\]\s+(\d+) tests',contents)
        test_evidence={'path':str(args.test_log.resolve()),'sha256':sha(args.test_log),'passed_tests':int(passed[-1]) if passed else None,'failure_summary_present':'FAILED TEST' in contents}
    fixed_work=None
    if args.fixed_work:
        benchmark=json.loads(args.fixed_work.read_text())
        groups={}
        for flag,label in [(False,'generic'),(True,'guarded')]:
            rows=[row for row in benchmark['samples'] if row['guarded_trace']==flag]
            groups[label]={'thread_cpu_seconds':summary([row['thread_cpu_seconds'] for row in rows]),'wall_seconds':summary([row['wall_seconds'] for row in rows])}
        fixed_work={'path':str(args.fixed_work.resolve()),'sha256':sha(args.fixed_work),'data':benchmark,'groups':groups,
                    'thread_cpu_median_change_percent':100*(groups['guarded']['thread_cpu_seconds']['median']/groups['generic']['thread_cpu_seconds']['median']-1),
                    'thread_cpu_ranges_overlap':groups['guarded']['thread_cpu_seconds']['min']<=groups['generic']['thread_cpu_seconds']['max'] and groups['generic']['thread_cpu_seconds']['min']<=groups['guarded']['thread_cpu_seconds']['max']}
        if args.fixed_work_provenance:fixed_work['provenance']=json.loads(args.fixed_work_provenance.read_text())
    exploratory={}
    for label,path in [('comparison',args.exploratory_comparison),('fixed_work',args.exploratory_fixed_work)]:
        if path:exploratory[label]={'path':str(path.resolve()),'sha256':sha(path),'data':json.loads(path.read_text())}
    artifact={'schema':'hollycast-guarded-poll-comparison-v1','capture_report':str(report),'capture_sha256':sha(report),
              'baseline_counts':{'path':str(args.baseline_counts.resolve()),'sha256':sha(args.baseline_counts)},
              'candidate_counts':{'path':str(args.candidate_counts.resolve()),'sha256':sha(args.candidate_counts)},
              'binaries':captured['binaries'],'host':captured['host'],'profile':captured['profile'],'sequence':captured['sequence'],'config':captured.get('config'),'capture_guest_timing_validation':captured.get('guest_timing_validation'),
              'off_mode_metrics':metrics,'off_mode_changes':changes,'adjacent_pair_changes':paired,'count_checks':count_checks,
              'regression_test_evidence':test_evidence,'fixed_work_benchmark':fixed_work,'exploratory_calibration':exploratory,
              'profiling_and_trace_source_sha256':{str(path):sha(path) for path in [Path(__file__).resolve(),Path(__file__).with_name('analyze.py').resolve(),Path(__file__).resolve().parents[2]/'core/rec-ARM64/rec_arm64.cpp',Path(__file__).resolve().parents[2]/'tests/src/GuardedPollTraceTest.cpp']},
              'runs':runs,'all_timing_fingerprints_identical':len(set(fingerprints))==1,
              'candidate_trace_entered_every_run':all(run['trace_entries']>0 for run in runs if run['application']=='candidate'),
              'candidate_sample_profiles':hot,
              'post_change_hot_cycle':{'exclusive_cpu_samples':pooled_hot,'total_thread_samples':pooled_samples,
                                       'thread_cpu_percent_estimate':100*pooled_hot/pooled_samples if pooled_samples else None,
                                       'per_run_thread_cpu_percent_estimate':[run['cycle']['exclusive_cpu_percent'] for run in hot],
                                       'per_run_cpu_ms_estimate':[run['cycle']['exclusive_cpu_ms_estimate'] for run in hot]},
              'limitations':['Off-mode clocks provide the paired optimization comparison; sample profiles are statistical attribution.',
                             'Fused native code in block B contains guest instructions from both A and B; compare the cycle union, not isolated native-block shares.',
                             'Manually emitted trace state commits/reloads do not pass allocator span hooks; allocator-span category changes do not measure total eliminated synchronization.',
                             'Three paired off-mode samples per application are not a formal regression-confidence guarantee.']}
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps(artifact,indent=2)+'\n')
    print(json.dumps({'off_mode_changes':changes,'post_change_hot_cycle':artifact['post_change_hot_cycle'],
                      'counts':count_checks,'timing_identical':artifact['all_timing_fingerprints_identical'],
                      'candidate_trace_entered_every_run':artifact['candidate_trace_entered_every_run']},indent=2))


if __name__=='__main__':main()
