"""Inspect actual gameplay input/controller snapshots; emit bounded pose previews."""
import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def analyze(root):
    result = read(root / 'result.json')
    assert result['status'] == 'success', 'Game sequence or shutdown failed'
    samples = read(root / 'samples.json')
    assert len(samples) == 28
    expected = {1: 'ArmedIdle', 3: 'ArmedIdle', 4: 'ArmedRight', 5: 'ArmedForward',
                8: 'ArmedRun', 10: 'ArmedForward', 12: 'ArmedIdle', 17: 'ArmedRight',
                20: 'ArmedIdle', 24: 'ArmedIdle', 26: 'Idle', 28: 'ArmedIdle'}
    active_grips = []
    summaries = []
    try:
        font = ImageFont.truetype('C:/Windows/Fonts/consola.ttf', 12)
    except OSError:
        font = ImageFont.load_default()
    for first in range(0, len(samples), 8):
        page = Image.new('RGB', (1280, 860), '#161b22')
        draw = ImageDraw.Draw(page)
        for offset, sample in enumerate(samples[first:first + 8]):
            index = first + offset + 1
            actor = sample['actor']
            controller = actor['controller']
            parameters = controller['parameters']
            assert controller['loaded'] and controller['nonFiniteParameters'] == 0
            assert actor['currentPaletteCount'] > 0 and actor['nonFiniteComparisons'] == 0
            assert all(not isinstance(v, (int, float)) or math.isfinite(v) for v in parameters.values())
            if index in expected:
                assert controller['stateName'] == expected[index] and not controller['inTransition'], sample['name']
            contacts = actor['limbContacts']
            assert contacts and all('effectiveWeight' in c and 'targetBone' in c for c in contacts), 'Updated grip diagnostics required'
            for contact in contacts:
                if contact['effectiveWeight'] >= .95:
                    assert contact['boneResolved'] and contact['targetResolved'] and contact['finite']
                    active_grips.append({'sample': sample['name'], 'bone': contact['bone'], 'distance': contact['distance']})
            world = read(root / (sample['name'] + '.world.json'))['recordedRaster']
            assert world['upscaling']['active'] and not world['upscaling']['failure'] and not world['taaActive']
            assert world['upscaling']['renderSize'] == [853, 480] and world['upscaling']['outputSize'] == [1280, 720]
            summaries.append({'name': sample['name'], 'frame': sample['frame'], 'state': controller['stateName'],
                              'transition': controller['inTransition'], 'parameters': parameters,
                              'cameraFov': sample['cameraFov'], 'limbContacts': contacts})
            # Fixed crop from the verified third-person composition, retaining
            # the head, upper body, hands and lower-body pose visible in camera.
            pose = Image.open(root / (sample['name'] + '.png')).convert('RGB').crop((250, 40, 780, 720))
            pose.thumbnail((300, 385))
            x, y = (offset % 4) * 320, (offset // 4) * 430
            page.paste(pose, (x + 10, y + 42))
            label = (sample['name'] + '\n' + controller['stateName'] +
                     (' ->' if controller['inTransition'] else '') +
                     f' R={parameters.get("ReloadWeight", 0):.2f} G={parameters.get("GripWeight", 0):.2f}')
            draw.text((x + 5, y + 5), label, font=font, fill='white')
        page.save(root / f'poses-{first // 8 + 1}.jpg', quality=90)
    assert len(active_grips) >= 8, 'Insufficient full-weight support-hand observations'
    maximum_grip_error = max(c['distance'] for c in active_grips)
    report = {'api': result['api'], 'samples': len(samples), 'controllerAndFinitePoseChecks': 'PASS',
              'activeGripSamples': active_grips, 'maximumFullWeightGripErrorMeters': maximum_grip_error,
              'gripWithinFiveCentimeters': maximum_grip_error <= .05,
              'visualReview': 'NOT_REVIEWED', 'observations': summaries,
              'scope': 'Paused gameplay pose samples and configured support-hand target error; not continuous ghosting or gun-mesh contact ground truth'}
    (root / 'analysis.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['api', 'samples', 'controllerAndFinitePoseChecks',
                                           'maximumFullWeightGripErrorMeters', 'gripWithinFiveCentimeters']}))
    assert report['gripWithinFiveCentimeters'], 'Full-weight support-hand target error exceeds 5 cm'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', nargs='+', type=Path)
    for directory in parser.parse_args().directories:
        analyze(directory)
