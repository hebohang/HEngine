"""Compare measured output/internal dimensions and frame costs without a second warmup cut.

Usage: python Tools/Agent/Analyze-RenderResolution.py run.csv [other.csv ...]
Older CSV files without internal dimensions are explicitly reported as unknown.
GPU samples are deduplicated and respect the writer's GPU warmup cutoff.
"""
import argparse
import collections
import csv
import json
import statistics
from pathlib import Path


def read_rows(path):
    with path.open(encoding='utf-8-sig', newline='') as stream:
        return list(csv.DictReader(stream))


def percentile(values, percent):
    ordered = sorted(values)
    index = (len(ordered) - 1) * percent / 100
    lower = int(index)
    return ordered[lower] + (ordered[min(lower + 1, len(ordered) - 1)] - ordered[lower]) * (index - lower)


def distribution(values):
    if not values:
        return None
    return dict(samples=len(values), median=statistics.median(values),
                p95=percentile(values, 95), p99=percentile(values, 99),
                maximum=max(values), over60FpsBudget=sum(value > 1000 / 60 for value in values))


def analyze(path):
    rows = read_rows(path)
    if not rows:
        raise ValueError('No measured frames: ' + str(path))
    gpu_rows = {row['gpuSample']: row for row in rows
                if int(row['gpuSample']) > int(row['gpuWarmupCutoffSample'])}
    dimensions = collections.Counter(
        (int(row['width']), int(row['height']),
         int(row['internalWidth']) if 'internalWidth' in row else None,
         int(row['internalHeight']) if 'internalHeight' in row else None) for row in rows)
    result = dict(csv=str(path), measuredFrames=len(rows),
                  firstFrame=int(rows[0]['frame']), lastFrame=int(rows[-1]['frame']),
                  loadingFrames=sum(int(row.get('runtimeLoading', 0)) for row in rows),
                  pendingTexturePeak=max(int(row.get('pendingTextureUploads', 0)) for row in rows),
                  dimensions=[dict(outputWidth=key[0], outputHeight=key[1],
                                   internalWidth=key[2], internalHeight=key[3], frames=count)
                              for key, count in sorted(dimensions.items())],
                  internalDimensionsRecorded='internalWidth' in rows[0],
                  transientPeakMiB=max(int(row['transientReservedBytes']) for row in rows) / 1048576,
                  timingsMs={})
    for name in ('cpuWallFrameMs', 'updateMs', 'renderSubmitPresentMs', 'cpuFramePreparationMs',
                 'cpuGraphRecordMs', 'cpuFrameSlotWaitMs', 'gpuFrameSpanMs'):
        selected = gpu_rows.values() if name.startswith('gpu') else rows
        result['timingsMs'][name] = distribution([float(row[name]) for row in selected])
    pass_path = Path(str(path) + '.passes.csv')
    if pass_path.exists():
        pass_samples = collections.defaultdict(dict)
        for row in read_rows(pass_path):
            if row['gpuSample'] in gpu_rows:
                pass_samples[row['passName']][row['gpuSample']] = float(row['milliseconds'])
        result['gpuPassesMs'] = {
            name: distribution(list(samples.values())) for name, samples in sorted(
                pass_samples.items(), key=lambda item: statistics.median(item[1].values()), reverse=True)}
    memory_path = Path(str(path) + '.memory.csv')
    if memory_path.exists():
        # Memory sampling includes startup: distinguish it from measured render frames.
        memory = [row for row in read_rows(memory_path) if row['available'] == '1']
        if memory:
            budgets = [int(row['localBudgetBytes']) for row in memory]
            result['memoryWholeSessionMiB'] = dict(
                localUsagePeak=max(int(row['localUsageBytes']) for row in memory) / 1048576,
                localBudgetMin=min(budgets) / 1048576, localBudgetMax=max(budgets) / 1048576,
                nonLocalUsagePeak=max(int(row['nonLocalUsageBytes']) for row in memory) / 1048576,
                samplesOverLocalBudget=sum(int(row['localUsageBytes']) > int(row['localBudgetBytes'])
                                           for row in memory))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('csv', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, help='Write the full comparison JSON to this file.')
    args = parser.parse_args()
    results = [analyze(path) for path in args.csv]
    encoded = json.dumps(results, indent=2) + '\n'
    if args.output:
        args.output.write_text(encoded, encoding='utf-8')
        print('Wrote ' + str(args.output))
    else:
        print(encoded)


if __name__ == '__main__':
    main()
