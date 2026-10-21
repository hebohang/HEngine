"""Report static-camera temporal variation; animations/lighting remain confounders."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument('directory', type=Path)
args = parser.parse_args()
receipt = json.loads((args.directory / 'static.json').read_text(encoding='utf-8-sig'))
frames = receipt['frames']
assert len(frames) == 16 and all(f['state'] == 'completed' for f in frames)
assert all(b['simulationStep'] == a['simulationStep'] + 1 for a, b in zip(frames, frames[1:]))
history = json.loads(Path(receipt['sequenceHistoryArtifact']['path']).read_text(encoding='utf-8-sig'))
captured = [r for r in history['records'] if r['phase'] == 'capture']
assert len(captured) == len(frames)
assert all(r['valid'] and r['contractValid'] and not r['historyReset'] for r in captured)
assert len({tuple(r['viewColumnMajor']) for r in captured}) == 1, 'Camera is not static'
rois = {'roof': (0.02, 0.12, 0.30, 0.30), 'ground': (0.53, 0.60, 0.98, 0.82),
        'cart': (0.06, 0.24, 0.28, 0.50), 'foliage': (0.44, 0.00, 0.70, 0.17)}
samples = {name: [] for name in rois}
for frame in frames:
    with Image.open(frame['path']) as image:
        width, height = image.size
        for name, bounds in rois.items():
            box = tuple(int(v * (width if i % 2 == 0 else height)) for i, v in enumerate(bounds))
            samples[name].append(np.array(image.crop(box).convert('RGB'), dtype=np.float32))
report = {'frames': len(frames), 'resolution': [width, height], 'rois': {},
          'camera_view_column_major': captured[0]['viewColumnMajor'],
          'unexpected_history_resets': 0,
          'limitation': 'Static-camera RGB8 variation, not image error; moving actors, foliage, particles and lighting may contribute. Moving sequence requires visual/guide inspection.'}
for name, images in samples.items():
    stack = np.stack(images)
    deviation = stack.std(axis=0).mean(axis=2)
    report['rois'][name] = {'bounds': rois[name], 'mean_std_rgb8': float(deviation.mean()),
                          'p95_std_rgb8': float(np.percentile(deviation, 95)),
                          'mean_frame_difference_rgb8': float(np.abs(np.diff(stack, axis=0)).mean()),
                          'mean_luminance_range_rgb8': float(np.ptp(stack.mean(axis=(1, 2, 3))))}
with Image.open(frames[0]['path']) as image:
    image = image.convert('RGB')
    image.thumbnail((1280, 1280))
    image.save(args.directory / 'preview.jpg', quality=85)
text = json.dumps(report, indent=2)
(args.directory / 'analysis.json').write_text(text, encoding='utf-8')
print(text)
