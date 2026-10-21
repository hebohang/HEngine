"""Check raw FSR particle inputs; keep pixel evidence separate from visual review."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def read(path):
    meta = json.loads(Path(str(path) + '.json').read_text(encoding='utf-8-sig'))
    values = np.fromfile(path, np.dtype(meta['dtype'])).reshape(meta['height'], meta['width'], meta['channels']).astype(np.float32)
    if meta['dtype'] == 'uint8':
        values /= 255
    assert np.isfinite(values).all(), path
    return values


def analyze(root):
    reports = []
    rows = []
    centers = {}
    names = ['empty'] + [f'{name}-{phase}' for name in ['blend', 'add', 'subtract', 'multiply', 'distortion']
                        for phase in ['peak', 'moved', 'ended']] + ['stopped']
    if (root / 'gpu-add-peak-upscale-color.raw').is_file():
        names += ['gpu-add-peak', 'gpu-add-zero', 'gpu-add-ended']
    for phase in names:
        color = read(root / f'{phase}-upscale-color.raw')
        opaque = read(root / f'{phase}-upscale-opaque.raw')
        reactive = read(root / f'{phase}-reactive.raw')[:, :, 0]
        composition = read(root / f'{phase}-composition.raw')[:, :, 0]
        alpha = np.clip(color[:, :, 3], 0, 1)
        visible = reactive > .02
        report = {'phase': phase, 'reactivePixels': int(visible.sum()),
                  'coveragePixels': int((alpha > .02).sum()), 'reactiveMax': float(reactive.max())}
        assert reactive.max() <= .902 and composition.max() <= 1, report
        if phase in ('empty', 'stopped') or phase.endswith('-ended'):
            assert reactive.max() == 0 and composition.max() == 0 and alpha.max() == 0, report
        else:
            assert visible.sum() > 100, report
            # AOVs are read back on separate render frames: reject gradient/edge
            # neighborhoods where jitter changes coverage or background sampling.
            rgb = np.maximum(color[:, :, :3], 0)
            base = np.maximum(opaque[:, :, :3], 0)
            difference = np.max(np.abs(rgb - base) / np.maximum(np.maximum(rgb, base), .01), axis=2)
            expected = np.maximum(np.clip(difference * .9, 0, 1), alpha * .9)
            stable = np.ones(alpha.shape, dtype=bool)
            for dy, dx in [(-3, 0), (3, 0), (0, -3), (0, 3)]:
                stable &= np.max(np.abs(np.roll(base, (dy, dx), (0, 1)) - base), axis=2) < .002
                stable &= np.abs(np.roll(expected, (dy, dx), (0, 1)) - expected) < .02
            stable[:3, :] = False; stable[-3:, :] = False
            stable[:, :3] = False; stable[:, -3:] = False
            selected = stable & visible
            assert selected.sum() > 100, report
            error = float(np.quantile(np.abs(expected[selected] - reactive[selected]), .95))
            report.update(stableResponsePixels=int(selected.sum()), responseErrorP95=error)
            assert error < .02, report
            assert np.quantile(np.abs(np.maximum(expected, alpha)[selected] - composition[selected]), .95) < .02, report
            name, stage = phase.rsplit('-', 1)
            if name in ('subtract', 'multiply'):
                assert alpha.max() == 0, report  # Verify the color-difference path itself.
            if name == 'gpu-add' and stage == 'zero':
                low_contrast = (alpha > .1) & (np.max(np.abs(rgb - base), axis=2) < .002)
                report['lowContrastCoveragePixels'] = int(low_contrast.sum())
                assert low_contrast.sum() > 100, report
            centers[phase] = float(np.nonzero(visible)[1].mean())
            if stage == 'moved':
                report['maskShiftPixels'] = centers[phase] - centers[name + '-peak']
                assert report['maskShiftPixels'] > 50, report
            if stage == 'peak':
                with Image.open(root / f'{phase}.png') as frame:
                    display = frame.convert('RGB').resize((384, 216))
                row = Image.new('RGB', (1152, 236))
                row.paste(display, (0, 20))
                for index, values in enumerate([reactive, composition], 1):
                    row.paste(Image.fromarray(np.uint8(values * 255)).convert('RGB').resize((384, 216)), (384 * index, 20))
                ImageDraw.Draw(row).text((5, 3), name + ' | scene / reactive / composition', fill='white')
                rows.append(row)
        reports.append(report)
    sheet = Image.new('RGB', (1152, 236 * len(rows)))
    for index, row in enumerate(rows):
        sheet.paste(row, (0, index * 236))
    sheet.thumbnail((1280, 1280))
    sheet.save(root / 'masks-preview.jpg', quality=85)
    result = {'pixelChecks': 'PASS', 'visualReview': 'NOT_REVIEWED', 'phases': reports}
    (root / 'analysis.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'root': str(root), 'pixelChecks': 'PASS', 'phases': len(reports)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    for directory in parser.parse_args().directories:
        analyze(directory)
