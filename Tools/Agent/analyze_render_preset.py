"""Summarize timed preset windows; paused measurements are not gameplay acceptance."""
import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

import numpy as np
from PIL import Image


def read_json(path):
    data = path.read_bytes()
    return json.loads(data.decode('utf-16' if data.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'))


def stats(values):
    data = np.array(values, dtype=float)
    assert len(data) and np.isfinite(data).all()
    return dict(mean=float(data.mean()), p50=float(np.percentile(data, 50)),
                p95=float(np.percentile(data, 95)), p99=float(np.percentile(data, 99)), maximum=float(data.max()))


def analyze(root):
    result = read_json(root / 'result.json')
    assert result['status'] == 'CAPTURED_AND_STOPPED' and result['shutdownVerified']
    with (root / 'frames.csv').open(encoding='utf-8-sig', newline='') as stream:
        frames = list(csv.DictReader(stream))
    with (root / 'frames.csv.passes.csv').open(encoding='utf-8-sig', newline='') as stream:
        passes = list(csv.DictReader(stream))
    phases = read_json(root / 'phases.json')
    if isinstance(phases, dict):
        phases = [phases]
    report = dict(scope=('Realtime scripted gameplay' if result.get('gameplay') else 'Paused fixed-camera steady render') + '; temporal visual acceptance remains separate.', phases=[])
    for phase in phases:
        selected = [f for f in frames if phase['startUtcMs'] <= float(f['cpuStartUtcMs']) < phase['endUtcMs']]
        assert len(selected) > 60, phase['name']
        # A bounded CSV buffer can fill before the last requested window.
        # Never silently report a truncated window as a full-duration run.
        covered_start = float(selected[0]['cpuStartUtcMs'])
        covered_end = float(selected[-1]['cpuStartUtcMs']) + float(selected[-1]['cpuWallFrameMs'])
        assert covered_start - phase['startUtcMs'] < 1000, (phase['name'], 'missing start')
        assert phase['endUtcMs'] - covered_end < 1000, (phase['name'], 'missing end')
        assert all((int(f['width']), int(f['height'])) == (3840, 2160) for f in selected)
        all_wall = stats([float(f['cpuWallFrameMs']) for f in selected])
        loading_frames = sum(f['runtimeLoading'] != '0' for f in selected)
        # Normal retry shows the loading screen. Report it separately rather
        # than allowing its cheap frames to inflate active gameplay throughput.
        selected = [f for f in selected if f['runtimeLoading'] == '0']
        assert len(selected) > 60, phase['name']
        metrics = {k: stats([float(f[k]) for f in selected]) for k in (
            'cpuWallFrameMs', 'cpuFrameSlotWaitMs', 'cpuPresentMs', 'cpuFramePreparationMs',
            'cpuGraphRecordMs', 'cpuResourcePumpMs', 'cpuPreUpdateMs', 'updateMs', 'cpuRuntimeRenderMs')}
        unique = {f['gpuSample']: f for f in selected if int(f['gpuSample']) > 0 and float(f['gpuFrameSpanMs']) > 0}
        assert len(unique) > 30
        metrics['gpuFrameSpanMs'] = stats([float(f['gpuFrameSpanMs']) for f in unique.values()])
        by_pass = defaultdict(list)
        for row in passes:
            if row['gpuSample'] in unique:
                by_pass[row['passName']].append(float(row['milliseconds']))
        top = sorted([(name, stats(values)) for name, values in by_pass.items()], key=lambda x: -x[1]['mean'])[:12]
        wall = metrics['cpuWallFrameMs']
        time_slices = []
        for offset in range(0, int(phase['endUtcMs'] - phase['startUtcMs']), 30000):
            interval = [f for f in selected if phase['startUtcMs'] + offset <= float(f['cpuStartUtcMs'])
                        < phase['startUtcMs'] + offset + 30000]
            if len(interval) < 60:
                continue
            timing = stats([float(f['cpuWallFrameMs']) for f in interval])
            time_slices.append(dict(offsetSeconds=offset / 1000, frames=len(interval),
                                    meanFps=1000 / timing['mean'], wall=timing))
        summary = dict(name=phase['name'], frames=len(selected), meanFps=1000 / wall['mean'],
                       coveredWindowMs=covered_end-covered_start,
                       timeSlices=time_slices,
                       excludedLoadingFrames=loading_frames, allFramesWall=all_wall,
                       fractionOver16_67ms=sum(float(f['cpuWallFrameMs']) > 1000 / 60 for f in selected) / len(selected),
                       internalSizes=sorted({(int(f['internalWidth']), int(f['internalHeight'])) for f in selected}),
                       metrics=metrics, topGpuPasses=top)
        report['phases'].append(summary)
        im = Image.open(root / (phase['name'] + '.png'))
        im.thumbnail((1280, 1280))
        im.convert('RGB').save(root / (phase['name'] + '-preview.jpg'), quality=88)
        print(json.dumps(dict(name=summary['name'], fps=summary['meanFps'], wall=wall,
                              gpu=metrics['gpuFrameSpanMs'], topGpu=top[:5], internal=summary['internalSizes'])))
    (root / 'analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    analyze(parser.parse_args().root)
