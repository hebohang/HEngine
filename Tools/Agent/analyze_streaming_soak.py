"""Analyze bounded camera traversal; separate payload, WDDM counters and frame timings."""
import argparse
import csv
import json
import re
from collections import Counter
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def percentiles(values):
    values = np.asarray(values, dtype=float)
    values = values[np.isfinite(values)]
    if not len(values):
        return {'samples': 0}
    return {'samples': len(values), 'minimum': float(values.min()), 'p50': float(np.median(values)), 'p95': float(np.quantile(values, .95)),
            'p99': float(np.quantile(values, .99)), 'maximum': float(values.max())}


def read_gpu_telemetry(root, inputs):
    if not inputs.get('nvidiaTelemetry'):
        return []
    zone = timezone(timedelta(minutes=inputs['localUtcOffsetMinutes']))
    fields = inputs['gpuTelemetryFields'].split(',')
    result = []
    with (root / 'gpu-telemetry.csv').open(newline='') as file:
        for raw in csv.reader(file):
            # The owned monitor is stopped only after Game has exited. Any
            # incomplete final monitor line is outside the measured phases.
            if len(raw) != len(fields):
                continue
            row = dict(zip(fields, (value.strip() for value in raw)))
            row['utcMs'] = datetime.strptime(row['timestamp'], '%Y/%m/%d %H:%M:%S.%f').replace(tzinfo=zone).timestamp() * 1000
            result.append(row)
    assert len(result) >= 10, 'Requested GPU hardware telemetry is incomplete'
    return result


def summarize_gpu_telemetry(rows, selected):
    if not rows:
        return {'available': False}
    first, last = float(selected[0]['cpuStartUtcMs']), float(selected[-1]['cpuStartUtcMs'])
    summary = {'available': True, 'scope': 'GPU-wide hardware counters, not per-process', 'devices': []}
    for uuid in sorted({r['uuid'] for r in rows}):
        device_rows = [r for r in rows if r['uuid'] == uuid]
        phase_rows = [r for r in device_rows if first <= r['utcMs'] <= last]
        assert len(phase_rows) >= 3, 'Hardware telemetry does not cover phase'
        device = {'uuid': uuid, 'samples': len(phase_rows), 'pstates': dict(Counter(r['pstate'] for r in phase_rows)),
                  'activeClockReasonMasks': dict(Counter(r['clocks_throttle_reasons.active'] for r in phase_rows))}
        for field in ['clocks.current.graphics', 'clocks.current.memory', 'utilization.gpu', 'utilization.memory',
                      'power.draw', 'temperature.gpu']:
            values = []
            for row in phase_rows:
                try:
                    values.append(float(row[field]))
                except ValueError:
                    pass  # Preserve unsupported fields as zero samples, not zero readings.
            device[field] = percentiles(values)
        device['nearSlowFrames'] = []
        for frame in sorted(selected, key=lambda f: float(f['cpuWallFrameMs']), reverse=True)[:10]:
            nearest = min(device_rows, key=lambda r: abs(r['utcMs'] - float(frame['cpuStartUtcMs'])))
            distance = abs(nearest['utcMs'] - float(frame['cpuStartUtcMs']))
            device['nearSlowFrames'].append({'frame': int(frame['frame']), 'wallMs': float(frame['cpuWallFrameMs']),
                                            'sampleDistanceMs': distance, 'hardwareSample': nearest if distance <= 1000 else None})
        summary['devices'].append(device)
    return summary


def read_cpu_telemetry(root, inputs):
    if not inputs.get('cpuTelemetry'):
        return []
    assert (root / 'cpu-monitor.stderr.log').stat().st_size == 0, 'CPU monitor failed'
    lines = (root / 'cpu-telemetry.jsonl').read_text(encoding='utf-8-sig').splitlines()
    rows = []
    for index, line in enumerate(lines):
        try:
            row = json.loads(line)
        except json.JSONDecodeError:
            # The owned monitor is stopped only after Game exit. An interrupted
            # final write is outside the phase endpoints; do not hide earlier errors.
            if index == len(lines) - 1:
                break
            raise
        row['utcMs'] = datetime.fromisoformat(row['utc'].replace('Z', '+00:00')).timestamp() * 1000
        rows.append(row)
    assert len(rows) >= 10, 'Requested CPU hardware telemetry is incomplete'
    return rows


def summarize_cpu_telemetry(rows, selected):
    if not rows:
        return {'available': False}
    first, last = float(selected[0]['cpuStartUtcMs']), float(selected[-1]['cpuStartUtcMs'])
    phase_rows = [r for r in rows if first <= r['utcMs'] <= last]
    assert len(phase_rows) >= 3, 'CPU telemetry does not cover phase'
    report = {'available': True, 'samples': len(phase_rows),
              'scope': 'Windows processor-wide PDH counters at one-second intervals, not per-process attribution',
              'timingLimit': 'A sample represents an interval; nearest timestamp does not establish hitch causality',
              'total': {}, 'nearSlowFrames': []}
    fields = ['actual frequency', 'processor frequency', '% processor performance', '% processor utility', 'performance limit flags']
    for field in fields:
        values = [float(v['value']) for row in phase_rows for v in row['counters']
                  if v['instance'].lower() == '_total' and v['counter'].lower() == field and v['status'] in (0, 1)]
        assert values and all(np.isfinite(values)), 'Missing/invalid CPU counter: ' + field
        report['total'][field] = (dict(Counter(str(int(v)) for v in values)) if field == 'performance limit flags'
                                  else percentiles(values))
    for frame in sorted(selected, key=lambda f: float(f['cpuWallFrameMs']), reverse=True)[:10]:
        nearest = min(rows, key=lambda r: abs(r['utcMs'] - float(frame['cpuStartUtcMs'])))
        distance = abs(nearest['utcMs'] - float(frame['cpuStartUtcMs']))
        report['nearSlowFrames'].append({'frame': int(frame['frame']), 'wallMs': float(frame['cpuWallFrameMs']),
                                        'sampleDistanceMs': distance, 'hardwareSample': nearest if distance <= 1500 else None})
    return report


def analyze(root):
    phases = read(root / 'phases.json')
    inputs = read(root / 'result.json')
    assert inputs['status'] == 'CAPTURED_AND_STOPPED' and inputs['shutdownVerified'], 'Successful process exit not verified'
    hardware_rows = read_gpu_telemetry(root, inputs)
    cpu_hardware_rows = read_cpu_telemetry(root, inputs)
    samples = [json.loads(line) for line in (root / 'telemetry.jsonl').read_text(encoding='utf-8-sig').splitlines()]
    with (root / 'frames.csv').open(encoding='utf-8-sig', newline='') as file:
        frames = list(csv.DictReader(file))
    with (root / 'frames.csv.memory.csv').open(encoding='utf-8-sig', newline='') as file:
        native_memory = list(csv.DictReader(file))
    assert frames and all(int(f['width']) == 1280 and int(f['height']) == 720 for f in frames)
    assert int(frames[-1]['frame']) >= phases[-1]['end']['renderFrame'], 'Truncated frame capture'
    assert len(frames) < 16384, 'Frame capture capacity reached'
    reports = []
    sheet = Image.new('RGB', (960, 300 * len(phases)))
    for index, phase in enumerate(phases):
        name = phase['name']
        streaming = phase['streaming']
        assert streaming['budgetBytes'] == phase['budgetMiB'] * 1048576
        assert streaming['failedUploads'] == 0 and not streaming['minimumOverrun']
        assert streaming['plannedBytes'] <= streaming['budgetBytes']
        # The planner targets budgetBytes, but that is not an allocation cap.
        # Demotions can admit scratch above max(budget, current residency) to
        # make progress after lowering a budget. Report overcommit explicitly;
        # do not misinterpret the promotion admission allowance as a global cap.
        over_target = max(0, streaming['residentPendingRetiringBytes'] - streaming['budgetBytes'])
        position = phase['camera']
        assert abs(((position[0] + 14) / 22) ** 2 + ((position[2] - 8) / 18) ** 2 - 1) < .001
        selected = [f for f in frames if phase['start']['renderFrame'] + 120 <= int(f['frame']) <= phase['end']['renderFrame']]
        assert len(selected) >= 400, (name, len(selected))
        assert all(int(f['internalWidth']) == 853 and int(f['internalHeight']) == 480 for f in selected)
        metrics = {key: percentiles([float(f[key]) for f in selected]) for key in
                   ['cpuWallFrameMs', 'gpuPassSumMs', 'gpuFrameSpanMs', 'cpuResourcePumpMs',
                    'cpuFramePreparationMs', 'cpuFrameSlotWaitMs', 'cpuPresentMs']}
        # Readbacks can appear in more than one CPU row. Preserve the per-frame
        # observation above and also report each valid GPU timing sample once.
        gpu_samples = {f['submissionTimingSampleId']: f for f in selected
                       if int(f['submissionTimingSampleId']) > 0 and float(f['gpuFrameSpanMs']) > 0}
        metrics['uniqueGpuSamples'] = len(gpu_samples)
        metrics['uniqueGpuPassSumMs'] = percentiles([float(f['gpuPassSumMs']) for f in gpu_samples.values()])
        metrics['uniqueGpuFrameSpanMs'] = percentiles([float(f['gpuFrameSpanMs']) for f in gpu_samples.values()])
        metrics['framesAbove33ms'] = sum(float(f['cpuWallFrameMs']) > 33.333 for f in selected)
        metrics['framesAbove50ms'] = sum(float(f['cpuWallFrameMs']) > 50 for f in selected)
        metrics['slowestFrames'] = [{key: row[key] for key in
                                    ['frame', 'cpuStartUtcMs', 'cpuWallFrameMs', 'gpuSample',
                                     'gpuPassSumMs', 'gpuFrameSpanMs', 'cpuThreadBusyMs',
                                     'cpuFramePreparationMs', 'cpuFrameSlotWaitMs', 'cpuResourcePumpMs']}
                                   for row in sorted(selected, key=lambda row: float(row['cpuWallFrameMs']), reverse=True)[:10]]
        memory = {}
        telemetry = [s for s in samples if s['phase'] == name and s['renderFrame'] >= phase['start']['renderFrame'] + 120]
        assert len(telemetry) >= 3, (name, len(telemetry))
        native_selected = [s for s in native_memory if int(s['available']) and
                           phase['start']['renderFrame'] + 120 <= int(s['frame']) <= phase['end']['renderFrame']]
        # RenderBackend::QueryMemoryBudget is optional (currently implemented
        # only by DX12). The mandatory WDDM process samples below are independent;
        # unavailable native queries must not become zero-usage measurements.
        native_report = {'available': False, 'scope': 'unavailable'}
        if native_selected:
            assert len(native_selected) >= 3, 'Insufficient available native GPU budget samples'
            native_scopes = {int(s['processScope']) for s in native_selected}
            assert len(native_scopes) == 1, 'Memory query scope changed within phase'
            native_report = {'available': True, 'scope': 'process' if native_scopes == {1} else 'device heap',
                             'localUsageBytes': percentiles([float(s['localUsageBytes']) for s in native_selected]),
                             'localBudgetBytes': percentiles([float(s['localBudgetBytes']) for s in native_selected]),
                             'nonLocalUsageBytes': percentiles([float(s['nonLocalUsageBytes']) for s in native_selected
                                                               if int(s['nonLocalAvailable'])])}
        for counter in ['dedicated usage', 'shared usage', 'total committed']:
            values = [sum(v['bytes'] for v in s['gpuCounters'] if v['path'].lower().endswith('\\' + counter)) for s in telemetry]
            assert all(v >= 0 for v in values)
            size = max(1, len(values) // 3)
            memory[counter] = {**percentiles(values), 'earlyThirdMedianBytes': float(np.median(values[:size])),
                               'lateThirdMedianBytes': float(np.median(values[-size:])),
                               'lateMinusEarlyBytes': float(np.median(values[-size:]) - np.median(values[:size]))}
        reports.append({'phase': name, 'budgetMiB': phase['budgetMiB'], 'elapsedSeconds': phase['elapsedSeconds'],
                        'simulationSeconds': phase['end']['simulationTimeSeconds'] - phase['start']['simulationTimeSeconds'],
                        'streamingEndpoint': streaming, 'frames': metrics, 'wddmProcessAllAdapters': memory,
                        'endpointOverTargetBytes': over_target,
                        'endpointAboveTargetPlus64MiB': over_target > 64 * 1048576,
                        'nativeMemoryQuery': native_report,
                        'gpuHardwareTelemetry': summarize_gpu_telemetry(hardware_rows, selected),
                        'cpuHardwareTelemetry': summarize_cpu_telemetry(cpu_hardware_rows, selected),
                        'processPrivateBytes': percentiles([s['processPrivateBytes'] for s in telemetry])})
        display = Image.open(root / (name + '.png')).convert('RGB')
        display.thumbnail((480, 270))
        sheet.paste(display, (0, index * 300 + 25))
        draw = ImageDraw.Draw(sheet)
        draw.text((5, index * 300 + 5), f'{name}: {phase["budgetMiB"]} MiB target', fill='white')
        draw.text((500, index * 300 + 45), f'CPU wall p95 {metrics["cpuWallFrameMs"]["p95"]:.2f} ms\n'
                  f'GPU pass sum p95 {metrics["gpuPassSumMs"]["p95"]:.2f} ms\n'
                  f'WDDM dedicated median {memory["dedicated usage"]["p50"] / 1048576:.1f} MiB\n'
                  f'Texture payload/reserved {streaming["residentPendingRetiringBytes"] / 1048576:.1f} MiB', fill='white')
    log = (root / 'runtime.log').read_text(encoding='utf-8', errors='replace')
    stderr = (root / 'stderr.log').read_text(encoding='utf-8', errors='replace')
    assert '[RenderStatistics] Completed frames=' in log, 'Statistics flush incomplete'
    assert not re.search(r'Validation Error|Device Removed|device.lost|ScriptException|\[TextureUpload\].*failed:', log + stderr, re.I)
    publication_count = len(re.findall(r'\[TextureStreaming\] published asset=', log))
    assert publication_count > 0
    for phase in phases:
        world = read(root / (phase['name'] + '.world.json'))['recordedRaster']
        assert world['upscaling']['active'] and not world['upscaling']['failure'] and not world['taaActive']
    pressure = next(r for r in reports if r['phase'] == 'pressure')
    baseline = next(r for r in reports if r['phase'] == 'baseline')
    recovery = next(r for r in reports if r['phase'] == 'recovery')
    assert pressure['streamingEndpoint']['residentPendingRetiringBytes'] < baseline['streamingEndpoint']['residentPendingRetiringBytes']
    assert recovery['streamingEndpoint']['residentPendingRetiringBytes'] > pressure['streamingEndpoint']['residentPendingRetiringBytes']
    report = {'api': inputs['api'], 'structuralChecks': 'PASS', 'sampledFrames': len(frames),
              'publicationCount': publication_count, 'phases': reports, 'visualReview': 'NOT_REVIEWED',
              'scope': 'VSync fixture, fixed simulation delta; diagnostic timing buffers and system counter polling enabled. WDDM process counters are separate from texture payload. No device-loss or universal no-leak claim.'}
    (root / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    sheet.thumbnail((1280, 1280))
    sheet.save(root / 'soak-preview.jpg', quality=85)
    print(json.dumps({'root': str(root), 'api': inputs['api'], 'structuralChecks': 'PASS', 'frames': len(frames),
                      'publicationCount': publication_count, 'phases': [{k: r[k] for k in ['phase', 'elapsedSeconds']} for r in reports]}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    for directory in parser.parse_args().directories:
        analyze(directory)
