"""Summarize a Validate-Lighting run; timings describe this run, not a benchmark."""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def summarize(path):
    with path.open(newline='', encoding='utf-8-sig') as stream:
        rows = [row for row in csv.DictReader(stream) if int(row['frame']) >= 120
                and row['runtimeLoading'] == '0' and row['pendingTextureUploads'] == '0'
                and int(row['gpuSample']) > int(row['gpuWarmupCutoffSample'])]
    if not rows: raise ValueError('No settled timing samples in ' + str(path))
    result = {'samples': len(rows)}
    for name in ('cpuWallFrameMs', 'gpuFrameSpanMs', 'gpuPassSumMs'):
        values = sorted(float(row[name]) for row in rows)
        if not all(math.isfinite(value) and value >= 0 for value in values):
            raise ValueError('Invalid timing data: ' + name)
        result[name] = {'median': statistics.median(values),
                        'p95': values[math.ceil(len(values)*.95)-1]}
    result['resolution'] = {key: sorted({int(row[key]) for row in rows})
                           for key in ('width','height','internalWidth','internalHeight')}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = {case: summarize(args.directory / (case + '.csv')) for case in ('source','baked')}
    worlds = [json.loads((args.directory / (case+'.png.world.json')).read_text(encoding='utf-8-sig'))
              for case in ('source','baked')]
    result['matchingCaptureCamera'] = all(abs(a-b) < 1e-4 for a,b in
        zip(worlds[0]['cameraPosition'],worlds[1]['cameraPosition']))
    result['matchingCaptureSimulationFrame'] = worlds[0]['applicationSimulationFrame'] == worlds[1]['applicationSimulationFrame']
    result['scope'] = 'One sequential replay pair; excludes first 120 frames, loading and GPU warmup. Not a controlled benchmark.'
    (args.directory / 'timing-summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))


if __name__ == '__main__': main()
