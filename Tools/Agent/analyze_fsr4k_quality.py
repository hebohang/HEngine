"""Inspect 4K sequence integrity and crop changes; metrics are not a flicker verdict."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from analyze_render_preset import read_json


def analyze(root):
    assert read_json(root / 'result.json')['state'] == 'success'
    sequences = read_json(root / 'sequences.json')
    rois = {'foliage': (1680, 0, 2280, 450), 'roof': (390, 210, 990, 660),
            'hair': (1240, 650, 1840, 1100)}
    report = {'scope': 'Consecutive fixed-step captures, not performance timings. '
              'ROI deltas include animation and camera motion; visual review is required.', 'sequences': []}
    for sequence in sequences:
        arrays = {key: [] for key in rois}
        sheet = Image.new('RGB', (1200, 705))
        draw = ImageDraw.Draw(sheet)
        previous = None
        frames = sequence['sequence']['frames']
        assert len(frames) == 16
        for index, frame in enumerate(frames):
            if previous:
                assert frame['renderFrame'] == previous['renderFrame'] + 1
                assert frame['simulationStep'] == previous['simulationStep'] + 1
            previous = frame
            world = read_json(Path(frame['path'] + '.world.json'))
            upscale = world['recordedRaster']['upscaling']
            assert upscale['active'] and not upscale['failure'] and upscale['provider'] == '3.1.4'
            assert upscale['quality'] == sequence['quality'] and upscale['outputSize'] == [3840, 2160]
            expected = [2258, 1270] if sequence['quality'] == 'Balanced' else [1920, 1080]
            assert upscale['renderSize'] == expected
            with Image.open(frame['path']) as image:
                assert image.size == (3840, 2160)
                for row, (key, box) in enumerate(rois.items()):
                    crop = image.crop(box).convert('RGB')
                    arrays[key].append(np.asarray(crop, dtype=np.float32))
                    if index in (0, 5, 10, 15):
                        column = (0, 5, 10, 15).index(index)
                        crop.thumbnail((300, 225))
                        sheet.paste(crop, (column * 300, row * 235 + 10))
                        draw.text((column * 300, row * 235), f'{key} frame {index}', fill='white')
        metrics = {}
        for key, values in arrays.items():
            delta = np.abs(np.diff(np.stack(values), axis=0))
            metrics[key] = {'meanAdjacentDelta255': float(delta.mean()),
                            'p99AdjacentDelta255': float(np.percentile(delta, 99)),
                            'fractionOver20': float((delta.mean(axis=3) > 20).mean())}
        name = sequence['quality'] + '-' + sequence['phase']
        sheet.save(root / (name + '-crops.jpg'), quality=90)
        report['sequences'].append({'name': name, 'frames': len(frames), 'metrics': metrics})
        print(name, json.dumps(metrics))
    (root / 'analysis.json').write_text(json.dumps(report, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    analyze(parser.parse_args().root)
