#!/usr/bin/env python3
"""Pool sample-only SH4 CPU weights with matched counted gameplay replays."""
import argparse
import datetime
import collections
import hashlib
import importlib.util
import json
from pathlib import Path
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture_report",type=Path,help="capture.py capture.json")
parser.add_argument("--output",type=Path,required=True,help="pooled result JSON")
parser.add_argument("--pair",nargs=2,type=Path,action="append",metavar=("SAMPLE_PROFILE","COUNT_PROFILE"),help="explicit pair; repeat for each sample-only replay (default: nearest counted run in capture order)")
args=parser.parse_args()
REPORT=args.capture_report.resolve(strict=True)
RUN=REPORT.parent
capture=json.loads(REPORT.read_text())
if capture.get('status')!='ok':
    parser.error('capture report must be complete and successful')
def profile_path(sample):
    original=Path(sample['sh4_profile'])
    return original if original.is_file() else RUN/original.parent.name/original.name
if args.pair:
    PAIRS=[(sample.resolve(strict=True),counted.resolve(strict=True)) for sample,counted in args.pair]
else:
    samples=list(enumerate(capture['samples']))
    counted=[(index,sample) for index,sample in samples if sample['mode'] in ('counts','both')]
    sampled=[(index,sample) for index,sample in samples if sample['mode']=='sample']
    if not counted or not sampled:
        parser.error('need sample-only and counted runs, or explicit --pair inputs')
    PAIRS=[(profile_path(sample),profile_path(min(counted,key=lambda item:(abs(item[0]-index),item[0]))[1])) for index,sample in sampled]
spec=importlib.util.spec_from_file_location('analyzer', ROOT/'tools/sh4-profile/analyze.py')
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def relative(path):
    try:return str(path.relative_to(ROOT))
    except ValueError:return str(path)
def range_of(values):
    return {'min':min(values),'max':max(values),'median':statistics.median(values)} if values else None

def identity(row):
    return (row['kind'],row['header_pc'],tuple(sorted(a.code_signature(b) for b in row['blocks'])))

def add_maps(target, source):
    for key,value in source.items():target[key]+=value

reports=[];provenance=[]
for index,(sp,cp) in enumerate(PAIRS,1):
    sc=json.loads(sp.read_text());cc=json.loads(cp.read_text())
    combined=a.combine_captures(sc,cc);r=a.analyze(combined)
    reports.append(r)
    (RUN/f'pair-{index:02d}-loops.json').write_text(json.dumps(r,indent=2)+'\n')
    (RUN/f'pair-{index:02d}-loops.md').write_text(a.markdown(r))
    exits=collections.Counter()
    for edge in cc['edges']:
        if edge.get('kind')!='call_continuation_estimate':exits[edge['from']]+=edge['count']
    mismatch=[{'pc':hex(b['pc']),'entries':b['executions'],'exits':exits[b['id']]} for b in cc['blocks'] if b['executions']!=exits[b['id']]]
    signatures={b['id']:a.code_signature(b) for b in cc['blocks']}
    canonical_blocks=collections.Counter()
    canonical_edges=collections.Counter()
    for b in cc['blocks']:
        if b['executions']:canonical_blocks[signatures[b['id']]]+=b['executions']
    for edge in cc['edges']:canonical_edges[(signatures[edge['from']],edge['to_pc'],edge.get('kind','branch'))]+=edge['count']
    block_digest=hashlib.sha256(json.dumps(sorted(canonical_blocks.items())).encode()).hexdigest()
    edge_digest=hashlib.sha256(json.dumps(sorted(canonical_edges.items())).encode()).hexdigest()
    provenance.append({'sample_capture':relative(sp),'sample_sha256':sha(sp),'count_capture':relative(cp),'count_sha256':sha(cp),
                       'sample_metadata':sc['metadata'],'count_metadata':cc['metadata'],
                       'sample_accounting':r['accounting'],'canonical_block_count_sha256':block_digest,'canonical_edge_count_sha256':edge_digest,'boundary_match':r['metadata']['capture_boundary_match'],
                       'unmatched_sample_code_samples':r['metadata']['unmatched_sample_capture_cpu_samples'],
                       'count_invariants':{'generation_count':len(cc['blocks']),'executed_generations':sum(b['executions']>0 for b in cc['blocks']),
                                           'entry_total':sum(b['executions'] for b in cc['blocks']),'exit_total':sum(exits.values()),
                                           'blocks_with_entry_exit_mismatch':mismatch}})

pooled=collections.Counter()
for r in reports:
    for key,value in r['accounting'].items():
        if key!='mapped_coverage_percent':pooled[key]+=value
pooled['mapped_coverage_percent']=100*pooled['mapped_cpu_samples']/pooled['total_cpu_samples']
regions={}
for run_index,r in enumerate(reports):
    by_id={row['id']:row for row in r['loops']}
    for row in r['loops']:
        key=identity(row)
        group=regions.setdefault(key,{'kind':row['kind'],'representative_pc':row['header_pc'],'rows':{},'cpu_samples':0,'cpu_categories':collections.Counter(),'native_leaves':collections.Counter()})
        group['rows'][run_index]=row;group['cpu_samples']+=row['exclusive_cpu_samples']
        add_maps(group['cpu_categories'],row['exclusive_cpu_sample_categories']);add_maps(group['native_leaves'],row['exclusive_native_leaf_samples'])

all_symbols=set()
for group in regions.values():
    all_symbols.update(group['native_leaves'])
    for category in group['cpu_categories']:
        if category.count('/')>1:all_symbols.add(category.split('/',2)[2])
all_symbols.update(key for r in reports for key in r['samples'].get('unmapped_native_leaf_samples',{}))
ordered=sorted(all_symbols)
demangled=subprocess.run(['c++filt'],input='\n'.join(ordered)+'\n',text=True,capture_output=True,check=True).stdout.splitlines()
symbols=dict(zip(ordered,demangled))

def normalize_categories(categories):
    out=collections.Counter()
    for category,weight in categories.items():
        pieces=category.split('/',2)
        if len(pieces)==3:
            pieces[2]=symbols.get(pieces[2],pieces[2])
            if pieces[2].isdigit() or pieces[2].startswith('0x'):pieces[2]='unresolved native target (ASLR-dependent)'
        out['/'.join(pieces)]+=weight
    return dict(out)

def summarize(group, include_code=False):
    rows=group['rows'];first=next(iter(rows.values()))
    out={'kind':group['kind'],'representative_pc':group['representative_pc'],'header_role':first['header_role'],
         'pooled_exclusive_cpu_samples':group['cpu_samples'],
         'pooled_exclusive_cpu_percent_estimate':100*group['cpu_samples']/pooled['total_cpu_samples'],
         'per_run_exclusive_cpu_percent_estimate':[rows[i]['exclusive_cpu_percent'] if i in rows else 0 for i in range(len(reports))],
         'per_run_exclusive_cpu_ms_estimate':[rows[i]['exclusive_cpu_ms_estimate'] if i in rows else 0 for i in range(len(reports))],
         'per_run_process_cpu_percent_estimate':[rows[i]['exclusive_process_cpu_percent_estimate'] if i in rows else 0 for i in range(len(reports))],
         'pooled_process_cpu_percent_estimate':100*sum(row['exclusive_cpu_ms_estimate'] for row in rows.values())/sum(report['metadata']['sample_capture_metadata']['process_cpu_ns']/1e6 for report in reports),
         'matched_runs':len(rows),'block_pcs':[hex(a.number(b['pc'])) for b in sorted(first['blocks'],key=lambda b:a.number(b['pc']))],
         'cpu_sample_categories':normalize_categories(group['cpu_categories']),
         'native_leaf_samples':{symbols.get(k,k):v for k,v in group['native_leaves'].most_common()},
         'header_entries':range_of([r['header_executions'] for r in rows.values() if r['header_executions'] is not None]),
         'backedge_traversals':range_of([r['backedge_traversals'] for r in rows.values()]),
         'backedge_count_complete':all(r['backedge_count_complete'] for r in rows.values()),
         'cycle_edge_traversals_by_guest_pc':[],
         'completed_two_edge_cycles_upper_bound_estimate':range_of([r['completed_two_edge_cycles_upper_bound_estimate'] for r in rows.values() if r['completed_two_edge_cycles_upper_bound_estimate'] is not None]),
         'fallback_upper_bound_counts_per_run':[r['fallback']['producer_counts_or_estimates'] if (r:=rows.get(i)) else None for i in range(len(reports))],
         'sync_upper_bound_counts_per_run':[r['sync']['producer_counts_or_estimates'] if (r:=rows.get(i)) else None for i in range(len(reports))]}
    for edge in first['cycle_edge_traversals']:
        blocks={str(b['id']):b for b in first['blocks']}
        source=hex(a.number(blocks[edge['from']]['pc']));target=hex(a.number(blocks[edge['to']]['pc']))
        counts=[]
        for r in rows.values():
            byid={str(b['id']):b for b in r['blocks']}
            counts.extend(e['count'] for e in r['cycle_edge_traversals'] if hex(a.number(byid[e['from']]['pc']))==source and hex(a.number(byid[e['to']]['pc']))==target)
        out['cycle_edge_traversals_by_guest_pc'].append({'from':source,'to':target,'count':range_of(counts)})
    if include_code:
        out['blocks']=[]
        for block in sorted(first['blocks'],key=lambda b:a.number(b['pc'])):
            matches=[b for r in rows.values() for b in r['blocks'] if a.code_signature(b)==a.code_signature(block)]
            out['blocks'].append({'pc':hex(a.number(block['pc'])),'entries':range_of([b['executions'] for b in matches]),
                                  'pooled_cpu_samples':sum(b['cpu_samples'] for b in matches),
                                  'disassembly':block['disassembly'],'guest_pc_samples':dict(collections.Counter({hex(a.number(pc)):sum(b.get('guest_pc_samples',{}).get(pc,0) for b in matches) for b in matches for pc in b.get('guest_pc_samples',{})}))})
    return out

eligible=sorted([g for g in regions.values() if g['kind'] in ('natural','two_block_cycle')],key=lambda g:(-g['cpu_samples'],g['representative_pc']))
top=[summarize(g,True) for g in eligible[:10]]
for rank,row in enumerate(top,1):row['rank']=rank
residual=[summarize(g) for g in sorted(regions.values(),key=lambda g:-g['cpu_samples']) if g['kind'] not in ('natural','two_block_cycle') and g['cpu_samples']][:10]
unmapped=collections.Counter()
for r in reports:add_maps(unmapped,r['samples'].get('unmapped_native_leaf_samples',{}))
output={'schema':'hollycast-sh4-loop-results-v1','date':datetime.datetime.now(datetime.timezone.utc).date().isoformat(),'capture_report':relative(REPORT),
        'status':capture['status'],'profile':capture['profile'],'binary':capture['binary'],'host':capture['host'],
        'profiling_source_sha256':{str(path.relative_to(ROOT)):sha(path) for path in [ROOT/'core/profiler/sh4_loop_profiler.cpp',ROOT/'core/profiler/sh4_loop_profiler.h',ROOT/'core/rec-ARM64/rec_arm64.cpp',ROOT/'core/hw/sh4/dyna/decoder.cpp',ROOT/'tools/sh4-profile/analyze.py',Path(__file__).resolve()]},
        'method':f'{len(PAIRS)} sample-only replays supply CPU weights; matched counted replays supply CFG/exact block and branch counts. Join by guest PC plus instruction addresses/opcodes. Rank pooled exclusive innermost natural-loop or measured two-block-cycle samples. CPU milliseconds are each run thread-CPU clock scaled by its sample share.',
        'provenance':provenance,'pooled_accounting':dict(pooled),'top10_loops':top,'residual_cyclic_regions':residual,
        'unmapped_native_leaf_samples':{symbols.get(k,k):v for k,v in unmapped.most_common()},'overhead_summary':capture.get('summary',{}),
        'validation':{'screenshot_hashes':sorted(set(s['screenshot_sha256'] for s in capture['samples'])),'reference_hash':capture['profile']['reference_sha256'],
                      'all_real_bios':all(s['probe']['real_bios_loaded'] for s in capture['samples']),
                      'counted_block_counts_identical':len(set(p['canonical_block_count_sha256'] for p in provenance))==1,
                      'counted_edge_counts_identical':len(set(p['canonical_edge_count_sha256'] for p in provenance))==1,
                      'modes':capture['modes'],'complete_samples':len(capture['samples'])}}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(output,indent=2)+'\n')
print(json.dumps({'top10':[{k:r[k] for k in ('rank','kind','representative_pc','pooled_exclusive_cpu_samples','pooled_exclusive_cpu_percent_estimate')} for r in top],'accounting':dict(pooled),'provenance':[{'boundaries':p['boundary_match'],'unmatched':p['unmatched_sample_code_samples'],'counter_mismatches':len(p['count_invariants']['blocks_with_entry_exit_mismatch'])} for p in provenance]},indent=2))
