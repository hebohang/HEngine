"""Owned-session gameplay regression through real UI and input, without modifying project assets."""
import argparse
import json
import math
from pathlib import Path
import shutil
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Tools/VFX'))
from preview import Session, digest, write_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['DX12', 'Vulkan'], required=True)
    parser.add_argument('--language', choices=['CSharp', 'Python'], required=True)
    args = parser.parse_args()
    scene = 'Assets/Scenes/MainGameCSharp.he' if args.language == 'CSharp' else 'Assets/Scenes/MainGame.he'
    document = json.loads((ROOT / 'Project' / scene).read_text(encoding='utf-8-sig'))
    player = next(key for key, value in document.items() if value.get('TagComponent', {}).get('Tag') == 'Player')
    executable = ROOT / 'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe'
    output = ROOT / 'out/agent/survivor-production' / time.strftime(f'%Y%m%d-%H%M%S-{args.language}-{args.backend}')
    output.mkdir(parents=True, exist_ok=False)
    report = dict(status='FAIL', backend=args.backend, language=args.language, scene=scene,
                  executableSha256=digest(executable), evidenceType='runtime-manual', captures=[])
    startup = scene if args.language == 'CSharp' else 'Assets/Scenes/MainMenu.he'
    report['startupScene'] = startup
    session = Session(output, args.backend, report['executableSha256'], scene=startup)

    def step(frames=30):
        return session.call('runtime.step', {'frames': frames})

    def click(identifier):
        element = session.call('ui.element.rect', {'id': identifier})
        if not element['visible']:
            raise AssertionError('UI element is hidden: ' + identifier)
        x, y, width, height = element['rect']
        session.call('input.click', {'x': x + width / 2, 'y': y + height / 2, 'button': 'left'})
        step()

    def capture(name):
        result = session.call('capture.frame', {'filename': name + '.png'})
        destination = output / (name + '.png')
        shutil.copyfile(result['path'], destination)
        report['captures'].append(dict(path=str(destination), sha256=digest(destination)))

    def position():
        entity = session.call('world.get', {'uuid': player})
        if not entity['found']:
            raise AssertionError('Player entity disappeared')
        return entity['worldMatrix'][12:15]

    try:
        session.start()
        step(120)
        capture('menu')
        if args.language == 'CSharp':
            # Native Survivor hides the legacy scene Player and spawns its own
            # presentation entity from this prefab before gameplay begins.
            matches = []
            offset = 0
            for _ in range(30):
                page = session.call('world.list', {'offset': offset, 'limit': 100})
                matches.extend(entity['uuid'] for entity in page['entities']
                               if entity['name'] == 'SurvivorMenuPreview')
                if page['nextOffset'] is None:
                    break
                offset = page['nextOffset']
            else:
                raise AssertionError('Menu entity scan exceeded its bounded page budget')
            if len(matches) != 1:
                raise AssertionError(f'Expected one native player presentation entity, got {len(matches)}')
            player = matches[0]
        report['trackedPlayerUuid'] = player
        click('start_button')
        step(120)
        capture('started')
        before = position()
        session.call('input.set', {'held': ['w'], 'onStep': True})
        step(60)
        session.call('input.clear')
        after = position()
        distance = math.dist(before, after)
        if distance < 0.1:
            raise AssertionError(f'Gameplay movement did not respond: {before} -> {after}')
        report['movement'] = dict(before=before, after=after, distance=distance)
        capture('gameplay')
        report['gameplayWorld'] = session.call('world.summary')
        session.call('input.set', {'held': ['escape'], 'onStep': True})
        step(1)
        session.call('input.clear')
        step(30)
        click('pause_restart_button')
        step(120)
        report['restartedPosition'] = position()
        capture('restarted')
        report['restartedWorld'] = session.call('world.summary')
    finally:
        session.stop()
        write_json(output / 'report.json', report)
    logs = '\n'.join(path.read_text(encoding='utf-8-sig', errors='replace')
                     for path in output.glob('game.*.log'))
    if '[Script][kind=5]' in logs or 'Validation Error' in logs:
        raise AssertionError('Runtime logs contain a script fault or graphics validation error')
    report['status'] = 'PASS'
    report['visualReview'] = 'Captured images require separate visual review.'
    write_json(output / 'report.json', report)
    print('runtime-manual PASS: ' + str(output / 'report.json'))


if __name__ == '__main__':
    main()
