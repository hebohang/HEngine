"""Measure static display-space noise; observations need not be adjacent frames."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def measure(directory):
    files = sorted(directory.glob('frame-*.png'))
    if len(files) < 2:
        raise ValueError(f'{directory}: at least two captures required')
    frames = np.stack([np.asarray(Image.open(p).convert('RGB'), dtype=np.float32)
                       for p in files])
    _, height, width, _ = frames.shape
    # Regions belong to the saved CurrentEditor camera, normalized for resolution.
    rois = {'shadow_floor': (820, 390, 970, 610),
            'sun_floor': (700, 350, 790, 600)}
    result = {'observations': len(files), 'resolution': [width, height], 'regions': {}}
    for name, (x0, y0, x1, y1) in rois.items():
        region = frames[:, round(y0 * height / 628):round(y1 * height / 628),
                        round(x0 * width / 1551):round(x1 * width / 1551)]
        deviation = region.std(axis=0)
        result['regions'][name] = {
            'mean_rgb8': float(region.mean()),
            'mean_temporal_std_rgb8': float(deviation.mean()),
            'p95_temporal_std_rgb8': float(np.percentile(deviation, 95)),
        }
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    report = {'metric': 'Standard deviation across static observations, RGB code values 0..255; not linear energy or consecutive-frame timing.',
              'runs': {str(p): measure(p) for p in args.directories}}
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, indent=2))
