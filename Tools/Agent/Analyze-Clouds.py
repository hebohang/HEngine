"""Summarize one Benchmark-Clouds output; join asynchronous GPU samples by ID."""
import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def summarize(values, budget):
    values = sorted(values)
    if not values:
        return None
    return dict(n=len(values), mean=statistics.mean(values),
                p95=values[math.ceil(len(values) * .95) - 1],
                p99=values[math.ceil(len(values) * .99) - 1], maximum=values[-1],
                budget=budget, overBudgetRatio=sum(v > budget for v in values) / len(values))


def analyze(root):
    with (root / 'frames.csv').open(encoding='utf-8-sig') as stream:
        frames = list(csv.DictReader(stream))
    cutoff = max(int(row['gpuWarmupCutoffSample']) for row in frames)
    samples = {int(row['gpuSample']): row for row in frames if int(row['gpuSample']) > cutoff}
    passes = {}
    with (root / 'frames.csv.passes.csv').open(encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            sample = int(row['gpuSample'])
            if sample in samples:
                passes.setdefault(sample, {})[row['passName']] = float(row['milliseconds'])
    cloud_names = sorted({name for row in passes.values() for name in row if name.startswith('Cloud')})
    cloud = [sum(value for name, value in row.items() if name.startswith('Cloud')) for row in passes.values()]
    result = dict(gpuSampleCutoff=cutoff, cloud=summarize(cloud, 1.5),
                  cloudP95TargetMs=2.0, cloudOver2MsRatio=sum(v > 2 for v in cloud) / len(cloud),
                  passes={name: summarize([row.get(name, 0) for row in passes.values()], 1.5) for name in cloud_names},
                  scenePassSum=summarize([float(row['gpuPassSumMs']) for row in samples.values()], 16.667),
                  frameGpuSpan=summarize([float(row['gpuFrameSpanMs']) for row in samples.values()], 16.667),
                  cpuCloudRecord=summarize([float(row['cpuCloudRecordMs']) for row in frames
                                            if 'cpuCloudRecordMs' in row], .2),
                  cpuWallFrame=summarize([float(row['cpuWallFrameMs']) for row in frames], 16.667))
    result['input'] = json.loads((root / 'input.json').read_text(encoding='utf-8-sig'))
    if result['cloud']['n'] < 600:
        raise ValueError('Fewer than 600 unique post-warmup GPU samples')
    (root / 'analysis.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps({key: result[key] for key in ['cloud', 'cpuCloudRecord']}, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    analyze(parser.parse_args().directory)
