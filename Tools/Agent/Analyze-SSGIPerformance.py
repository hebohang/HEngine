"""Summarize HEngine GPU CSV timing and process GPU-memory observations.

python Tools/Agent/Analyze-SSGIPerformance.py RUN_DIRECTORY --output summary.json
Use --scope performance only for dedicated timing runs; diagnostic is default.
Select one CSV with --pattern NAME.csv. CPU timing columns are never combined
with GPU timings. Pass sums and frame spans describe different measurements.
"""
import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np


def read_csv(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(stream)
        return reader.fieldnames or [], list(reader)


def stats(values):
    a = np.asarray(values, dtype=np.float64)
    if not a.size:
        return {'samples': 0, 'mean': None, 'p50': None, 'p95': None, 'p99': None,
                'minimum': None, 'maximum': None}
    if not np.isfinite(a).all():
        raise ValueError('Non-finite timing or memory value')
    p50, p95, p99 = np.percentile(a, [50, 95, 99])
    return {'samples': len(a), 'mean': float(a.mean()), 'p50': float(p50),
            'p95': float(p95), 'p99': float(p99),
            'minimum': float(a.min()), 'maximum': float(a.max())}


def category(name):
    if name.startswith(('SSGIRayMarch', 'SSGITrace')):
        return 'trace'
    if name.startswith('DiffuseDemodulation'):
        return 'demodulation'
    if name.startswith('TemporalReprojection'):
        return 'temporal'
    if name.startswith(('HistoryFix', 'MomentsAndVariance', 'ATrousFilter')):
        return 'spatial'
    if name.startswith('BilateralUpsample'):
        return 'upsample'
    if name.startswith('DiffuseRemodulation'):
        return 'remodulation'
    if name.startswith(('GIComposite', 'GlobalIlluminationComposite')):
        return 'composite'
    if name.startswith('DepthPyramid'):
        return 'shared_depth_pyramid'
    if name.startswith('RasterSurfaceGuides'):
        return 'shared_surface_guides'
    return 'other'


def memory_observations(path, minimum_frame, maximum_frame):
    if not path.exists():
        return {'status': 'UNAVAILABLE'}
    _, rows = read_csv(path)
    process = [r for r in rows if r.get('available') == '1' and r.get('processScope') == '1']
    steady = [r for r in process if minimum_frame <= int(r['frame']) <= maximum_frame]
    result = {'status': 'AVAILABLE' if process else 'NO VALID PER-PROCESS SAMPLES',
              'observed_process_samples': len(process), 'discarded_unavailable_or_nonprocess': len(rows)-len(process),
              'scope': 'Process local/nonlocal GPU memory budget observations, not total adapter VRAM or CPU RAM.',
              'peak_scope': 'Maximum observed over this run, including loading; sparse queries can miss the actual peak.',
              'steady_scope': 'Memory sample frame falls within selected post-warmup CSV frame range; no convergence implied.',
              'steady_observation_frames': [int(r['frame']) for r in steady]}
    for key in ('localUsageBytes', 'localBudgetBytes', 'localReservationBytes', 'nonLocalUsageBytes'):
        available = lambda samples: [int(r[key]) for r in samples
                                    if key != 'nonLocalUsageBytes' or r.get('nonLocalAvailable') == '1']
        result[key] = {'whole_run': stats(available(process)), 'post_warmup_observed': stats(available(steady))}
    result['steady_sample_warning'] = 'Too few observations to establish a steady memory plateau' if len(steady) < 3 else None
    return result


def analyze(path, scope):
    fields, rows = read_csv(path)
    required = {'gpuSample', 'gpuWarmupCutoffSample', 'gpuFrameSpanMs', 'gpuPassSumMs', 'width', 'height', 'frame'}
    if not required.issubset(fields):
        raise ValueError(f'{path}: missing GPU main-table fields {sorted(required-set(fields))}')
    selected = {}
    discarded = {'before_warmup_or_zero_sample': 0, 'nonpositive_frame_span': 0, 'duplicate_gpu_sample': 0}
    conflicts = 0
    for row in rows:
        sample = int(row['gpuSample']); cutoff = int(row['gpuWarmupCutoffSample'])
        span = float(row['gpuFrameSpanMs']); total = float(row['gpuPassSumMs'])
        if not math.isfinite(span) or not math.isfinite(total) or total < 0:
            raise ValueError(f'{path}: invalid GPU timing at sample {sample}')
        if sample <= 0 or sample < cutoff:
            discarded['before_warmup_or_zero_sample'] += 1
            continue
        if span <= 0:
            discarded['nonpositive_frame_span'] += 1
            continue
        if sample in selected:
            discarded['duplicate_gpu_sample'] += 1
            conflicts += any(row[key] != selected[sample][key] for key in ('gpuFrameSpanMs', 'gpuPassSumMs', 'width', 'height'))
            continue
        selected[sample] = row
    if not selected:
        raise ValueError(f'{path}: no GPU samples survive warmup')
    dimensions = sorted({(int(r['width']), int(r['height'])) for r in selected.values()})
    if len(dimensions) != 1:
        raise ValueError(f'{path}: output dimensions changed during measurement')
    if conflicts:
        raise ValueError(f'{path}: {conflicts} inconsistent duplicate GPU sample rows')
    pass_path = Path(str(path)+'.passes.csv')
    samples = {s: {} for s in selected}
    duplicate_passes = 0
    if pass_path.exists():
        pass_fields, pass_rows = read_csv(pass_path)
        if not {'gpuSample', 'passName', 'milliseconds'}.issubset(pass_fields):
            raise ValueError(f'{pass_path}: invalid pass schema')
        for row in pass_rows:
            sample = int(row['gpuSample']); name = row['passName']
            if sample not in selected or name == 'FrameGpuSpan':
                continue
            value = float(row['milliseconds'])
            if not math.isfinite(value) or value < 0:
                raise ValueError(f'{pass_path}: invalid {name} time')
            if name in samples[sample]:
                if samples[sample][name] != value:
                    raise ValueError(f'{pass_path}: conflicting repeated pass {sample}/{name}')
                duplicate_passes += 1
                continue
            samples[sample][name] = value
    pass_names = sorted({name for entry in samples.values() for name in entry})
    details = {name: {'category': category(name), 'milliseconds': stats([entry[name] for entry in samples.values() if name in entry]),
                      'missing_samples': sum(name not in entry for entry in samples.values())} for name in pass_names}
    grouped = {}
    # If the graph changes, zero contributes only for an absent pass in a
    # present sample, not for an absent pass timing table.
    complete_samples = {s: entry for s, entry in samples.items() if entry}
    for group in sorted({category(name) for name in pass_names}):
        names = [n for n in pass_names if category(n) == group]
        grouped[group] = {'pass_names': names, 'milliseconds': stats([
            sum(entry.get(n, 0.) for n in names) for entry in complete_samples.values()])}
    exclusive_groups = {'trace', 'demodulation', 'temporal', 'spatial', 'upsample', 'remodulation', 'composite'}
    ids = sorted(selected)
    frames = [int(r['frame']) for r in selected.values()]
    result = {'path': str(path), 'scope': scope, 'formal_performance_evidence': scope == 'performance',
              'scope_warning': 'Caller-declared dedicated timing run; parser does not certify thermal stability or absence of readback.' if scope == 'performance'
                               else 'Diagnostic capture: do not use as dedicated performance evidence.',
              'policy': 'Unique positive gpuSample >= that row gpuWarmupCutoffSample; pass rows joined on exactly these sample IDs.',
              'resolution': list(dimensions[0]), 'gpu_sample_ids': ids, 'selected_gpu_samples': len(ids),
              'cutoffs': sorted({int(r['gpuWarmupCutoffSample']) for r in rows}),
              'selected_csv_frame_range': [min(frames), max(frames)], 'discarded_rows': discarded,
              'gpu_frame_span_ms': stats([float(r['gpuFrameSpanMs']) for r in selected.values()]),
              'gpu_reported_pass_sum_ms': stats([float(r['gpuPassSumMs']) for r in selected.values()]),
              'gpu_csv_pass_sum_ms': stats([sum(entry.values()) for entry in complete_samples.values()]),
              'ssgi_exclusive_pass_sum_ms': stats([sum(v for n,v in entry.items() if category(n) in exclusive_groups)
                                                   for entry in complete_samples.values()]),
              'pass_timing_missing_sample_ids': [s for s,entry in samples.items() if not entry],
              'duplicate_identical_pass_rows_ignored': duplicate_passes, 'categories': grouped, 'passes': details,
              'limitations': ['GPU frame span is a timestamp interval; pass sums are sums of timers and must not be added to frame span.',
                              'Shared depth/guides costs are reported separately and are not automatically all incremental SSGI costs.',
                              'No CPU timings are included. Off/On frame differences do not establish isolated SSGI cost.',
                              'Absent stages can be intentional (GI Off/full resolution) or missing data; inspect pass coverage.']}
    if 'transientReservedBytes' in fields:
        result['transient_reserved_bytes'] = {'selected_frames': stats([int(r['transientReservedBytes']) for r in selected.values()]),
                                               'all_main_csv_rows_observed_peak': max(int(r['transientReservedBytes']) for r in rows),
                                               'scope': 'Renderer transient pool reservation, not live allocation or total device memory.'}
    result['memory'] = memory_observations(Path(str(path)+'.memory.csv'), min(frames), max(frames))
    settings_path = Path(str(path)+'.settings.json')
    if settings_path.exists():
        snapshot = json.loads(settings_path.read_text(encoding='utf-8-sig'))
        result['settings'] = {key: snapshot[key] for key in ('effective','debug','capabilities','renderer') if key in snapshot}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--pattern', default='*.csv')
    parser.add_argument('--scope', choices=('diagnostic','performance'), default='diagnostic')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    paths, skipped = [], []
    for path in sorted(args.directory.glob(args.pattern)):
        if not path.is_file() or path.name.endswith(('.passes.csv','.memory.csv')):
            continue
        with path.open(newline='',encoding='utf-8-sig') as stream:
            fields = next(csv.reader(stream), [])
        if {'gpuSample','gpuFrameSpanMs','gpuPassSumMs'}.issubset(fields):
            paths.append(path)
        else:
            skipped.append(str(path))
    if not paths:
        parser.error('No main CSV files matched')
    report = {'schema': 'hengine.ssgi.gpu-performance-analysis.v1', 'scope': args.scope,
              'skipped_non_main_csv': skipped,
              'runs': [analyze(path,args.scope) for path in paths]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'output':str(args.output),'runs':len(report['runs']),
                      'selected_gpu_samples':[r['selected_gpu_samples'] for r in report['runs']],
                      'scope':args.scope}))


if __name__ == '__main__':
    main()
