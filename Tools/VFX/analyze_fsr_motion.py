"""Inspect repeated motion/identity captures; NativeAA is a reference, not ground truth."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def document(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def raw(root, name):
    path = root / name
    meta = document(Path(str(path) + '.json'))
    data = np.fromfile(path, np.dtype(meta['dtype'])).reshape(meta['height'], meta['width'], meta['channels'])
    assert np.isfinite(data).all(), name
    return data


def raster(root, name):
    result = document(root / (name + '.world.json'))['recordedRaster']
    assert result['valid'] and result['upscaling']['active'] and not result['taaActive']
    return result


def summary(values):
    values = np.concatenate(values)
    return dict(samples=int(values.size), median=float(np.median(values)), p95=float(np.quantile(values, .95)), maximum=float(values.max()), fractionAbove2Pixels=float(np.mean(values > 2)))


def analyze(root, fixture):
    config = document(fixture or root / 'fixture.json')
    stages = ['quality-display', 'quality-motion', 'quality-ids', 'native-display', 'native-ids']
    for endpoint in ['before', 'after']:
        actors = [document(root / f'{stage}-{endpoint}.json') for stage in stages]
        signatures = [(a['currentPaletteHash'], a['clothSolvers'][0]['positionsHash']) for a in actors]
        assert len(set(signatures)) == 1, 'Unaligned replay poses'
    histories = {stage: document(root / f'{stage}-history.json')['records'] for stage in stages}
    for stage, records in histories.items():
        assert len(records) == 24 and all(r['valid'] and r['contractValid'] for r in records)
        assert records[0]['historyReset'] and not any(r['historyReset'] for r in records[1:])
        assert all(current['previousSubmissionId'] == previous['submission'] for previous, current in zip(records, records[1:])), 'Broken submitted history chain'
        assert [r['phase'] for r in records] == ['settle'] * 8 + ['capture'] * 16
        steps = [r['simulationStep'] for r in records[8:]]
        assert np.all(np.diff(steps) == 1), 'Nonconsecutive simulation'
        peer = 'quality-display' if stage.startswith('quality') else 'native-display'
        assert [r['jitterPixels'] for r in records] == [r['jitterPixels'] for r in histories[peer]]
        for frame in range(16):
            extension = 'png' if stage.endswith('display') else 'raw'
            state = raster(root, f'{stage}-{frame}.{extension}')['upscaling']
            quality = 'Quality' if stage.startswith('quality') else 'NativeAA'
            assert state['quality'] == quality and state['sharpness'] == 0 and not state['failure']
            assert state['outputSize'] == [1280, 720]
            assert state['renderSize'] == ([853, 480] if quality == 'Quality' else [1280, 720])
    actor_id = document(root / 'quality-ids-before.json')['objectGpuIndex']
    native_actor = document(root / 'native-ids-before.json')['objectGpuIndex']
    groups = config['motionGroups']
    samples = {name: {'motion': [], 'reprojection': [], 'zeroMotion': []} for name in groups}
    pixel_counts = {name: 0 for name in groups}
    comparisons, rows = [], []
    previous_ids = previous_native = None
    for frame in range(16):
        ids = raw(root, f'quality-ids-{frame}.raw')
        motion = raw(root, f'quality-motion-{frame}.raw').astype(np.float32)
        r = raster(root, f'quality-motion-{frame}.raw')
        ir = raster(root, f'quality-ids-{frame}.raw')
        assert r['currentJitterPixels'] == ir['currentJitterPixels']
        height, width = ids.shape[:2]
        actor = ids[:, :, 0] == actor_id
        primitive = ids[:, :, 1] & np.uint32(0x7fffffff)
        vectors = motion[:, :, :2] * [width, height]
        jitter = np.array(r['previousJitterPixels']) - r['currentJitterPixels']
        speed = np.linalg.norm(vectors - jitter, axis=2)
        footprints = {}
        if previous_ids is not None:
            py, px = np.nonzero(previous_ids[:, :, 0] == actor_id)
            previous_keys = previous_ids[py, px, 1] & np.uint32(0x7fffffff)
            order = np.argsort(previous_keys)
            boundaries = np.flatnonzero(np.diff(previous_keys[order])) + 1
            for indices in np.split(order, boundaries):
                if len(indices):
                    footprints[int(previous_keys[indices[0]])] = np.stack([px[indices], py[indices]], axis=1)
        for name, sections in groups.items():
            selected = np.zeros(actor.shape, bool)
            for section in sections:
                s = config['sections'][section]
                selected |= (primitive >= s['firstPrimitive']) & (primitive < s['firstPrimitive'] + s['primitiveCount'])
            selected &= actor
            assert selected.sum() > 100, (frame, name, selected.sum())
            assert np.all(motion[selected, 3] > 0), (frame, name, 'invalid previous depth')
            pixel_counts[name] += int(selected.sum())
            samples[name]['motion'].append(speed[selected])
            if previous_ids is None:
                continue
            # Compare against the previous raster footprint of the SAME triangle.
            # Visibility changes and subpixel triangles may have no previous sample.
            # Distance to a sampled footprint has up to ~sqrt(.5) px quantization.
            ys, xs = np.nonzero(selected)
            keys = primitive[ys, xs]
            for key in np.unique(keys):
                old_points = footprints.get(int(key))
                if old_points is None:
                    continue
                selected_key = keys == key
                points = np.stack([xs[selected_key], ys[selected_key]], axis=1)
                predicted = points + vectors[ys[selected_key], xs[selected_key]]
                for label, coordinates in [('reprojection', predicted), ('zeroMotion', points + jitter)]:
                    distances = np.linalg.norm(coordinates[:, None, :] - old_points[None, :, :], axis=2).min(axis=1)
                    samples[name][label].append(distances)
        native = raw(root, f'native-ids-{frame}.raw')
        q = np.array(Image.open(root / f'quality-display-{frame}.png').convert('RGB'), dtype=np.float32) / 255
        n = np.array(Image.open(root / f'native-display-{frame}.png').convert('RGB'), dtype=np.float32) / 255
        error = np.abs(q - n).mean(axis=2)
        if previous_native is not None:
            # Pixels newly exposed by the actor. Remove a 1px current silhouette
            # band to reduce raster jitter/coverage differences between resolutions.
            current_actor = native[:, :, 0] == native_actor
            expanded = current_actor.copy()
            for dy in [-1, 0, 1]:
                for dx in [-1, 0, 1]:
                    expanded |= np.roll(current_actor, (dy, dx), (0, 1))
            exposed = (previous_native[:, :, 0] == native_actor) & ~expanded
            assert exposed.sum() > 20, (frame, 'no disocclusion')
            comparisons.append({'frame': frame, 'exposedPixels': int(exposed.sum()),
                                'meanDisplayDifference': float(error[exposed].mean()),
                                'p95DisplayDifference': float(np.quantile(error[exposed], .95))})
        if frame in [0, 5, 10, 15]:
            row = Image.new('RGB', (1200, 360))
            for index, (label, values) in enumerate([('Quality', q), ('NativeAA reference', n), ('absolute difference x4', np.minimum(error[:, :, None] * 4, 1).repeat(3, axis=2))]):
                crop = Image.fromarray(np.uint8(values * 255)).crop((260, 75, 930, 715))
                crop.thumbnail((400, 335))
                row.paste(crop, (index * 400, 25))
                ImageDraw.Draw(row).text((index * 400 + 5, 5), f'{frame}: {label}', fill='white')
            rows.append(row)
        previous_ids, previous_native = ids, native
    measurements = {name: {key: summary(values) for key, values in entries.items()} for name, entries in samples.items()}
    paused = raw(root, 'paused-motion.raw').astype(np.float32)
    pr = raster(root, 'paused-motion.raw')
    jitter = np.array(pr['previousJitterPixels']) - pr['currentJitterPixels']
    paused_error = np.linalg.norm(paused[:, :, :2] * [pr['width'], pr['height']] - jitter, axis=2)[paused[:, :, 3] > 0]
    checks = []
    for name, values in measurements.items():
        checks.append((name + ': adequate corresponding coverage', values['reprojection']['samples'] > 1000))
        checks.append((name + ': motion exercised', values['motion']['median'] > 2))
        checks.append((name + ': footprint median < .8 px', values['reprojection']['median'] < .8))
        checks.append((name + ': footprint p95 < 1.5 px', values['reprojection']['p95'] < 1.5))
        checks.append((name + ': better than zero motion', values['reprojection']['p95'] < values['zeroMotion']['p95'] * .2))
    checks.append(('paused motion < .01 px', paused_error.max() < .01))
    report = {'api': document(root / 'result.json')['api'], 'replayChecks': 'PASS',
              'motionChecks': 'PASS' if all(passed for _, passed in checks) else 'FAIL',
              'checks': {name: bool(passed) for name, passed in checks},
              'sections': measurements, 'visiblePixels': pixel_counts,
              'pausedMotionPixels': summary([paused_error]), 'disocclusionReference': comparisons,
              'visualReview': 'NOT_REVIEWED', 'referenceIsGroundTruth': False}
    (root / 'motion-analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    sheet = Image.new('RGB', (1200, 360 * len(rows)))
    for index, row in enumerate(rows):
        sheet.paste(row, (0, index * 360))
    sheet.thumbnail((1280, 1280))
    sheet.save(root / 'motion-preview.jpg', quality=85)
    print(json.dumps({'root': str(root), 'api': report['api'], 'motionChecks': report['motionChecks'],
                      'sections': measurements, 'pausedMaximum': float(paused_error.max()),
                      'maximumDisocclusionMeanDifference': max(x['meanDisplayDifference'] for x in comparisons)}))
    assert report['motionChecks'] == 'PASS', 'See motion-analysis.json for failed checks'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, help='Defaults to the snapshot in each evidence directory')
    parser.add_argument('directories', nargs='+', type=Path)
    args = parser.parse_args()
    for directory in args.directories:
        analyze(directory, args.fixture)
