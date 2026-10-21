"""Owned, fixed-step VFX captures through the public hectl Agent contract.

The report records capture/command success separately from visual/performance
acceptance. Paused stepping is diagnostic, not a shipping performance benchmark.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'Tools/Agent'))
from storage import Storage


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


class Session:
    def __init__(self, output, backend, executable_hash, scene='Assets/Scenes/VFXAuthoringPreview.he', *, render_stats=True):
        self.output, self.backend, self.executable_hash = output, backend, executable_hash
        self.owner = uuid.uuid4().hex
        self.scene = scene
        self.render_stats = render_stats
        self.commands = []
        self.runtime_log_directory = None

    def run(self, arguments, label, parse=True):
        index = len(self.commands)
        prefix = self.output / f'{index:04d}-{label}'
        command = ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                   str(ROOT / 'Tools/Agent/hectl.ps1'), *arguments,
                   '-Configuration', 'RelWithDebInfo', '-AgentOwnerTag', self.owner,
                   '-TimeoutSeconds', '180']
        start = time.time()
        with prefix.with_suffix('.stdout.log').open('w', encoding='utf-8') as stdout, \
                prefix.with_suffix('.stderr.log').open('w', encoding='utf-8') as stderr:
            process = subprocess.run(command, cwd=ROOT, stdout=stdout, stderr=stderr,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
        self.commands.append(dict(command=command, exitCode=process.returncode,
                                  started=start, elapsed=time.time()-start,
                                  stdout=str(prefix.with_suffix('.stdout.log'))))
        write_json(self.output / 'commands.json', self.commands)
        if process.returncode:
            raise RuntimeError(f'{label} exited {process.returncode}; see {prefix}.stdout.log')
        if not parse:
            return None
        raw = prefix.with_suffix('.stdout.log').read_text(encoding='utf-8-sig', errors='replace')
        offset = raw.find('{')
        if offset < 0:
            raise RuntimeError(f'{label}: no JSON receipt')
        response = json.loads(raw[offset:])
        if response.get('ok') is False:
            raise RuntimeError(f'{label}: {response.get("error")}')
        return response.get('result', response)

    def call(self, method, params=None):
        request = self.output / f'request-{len(self.commands):04d}.json'
        write_json(request, params or {})
        return self.run(['agent', 'call', '-Method', method, '-RequestFile', str(request)],
                        method.replace('.', '-'))

    def start(self):
        args = ['agent', 'start', '-Parallel', '2', '-GraphicsApi', self.backend,
                '-StrictGraphicsApi', '-Resolution', '1280x720', '-FixedDeltaSeconds', '0.0166666667',
                '-StartupScene', self.scene, '-RenderValidation']
        if self.render_stats:
            args.append('-RenderStats')
        if self.executable_hash:
            args += ['-ExistingBuildSha256', self.executable_hash]
        self.run(args, 'start', parse=False)
        receipt = Path(self.commands[-1]['stdout']).read_text(encoding='utf-8-sig', errors='replace')
        match = re.search(r'^session: (.+)$', receipt, re.MULTILINE)
        if not match:
            raise RuntimeError('Agent start did not identify its owned session log directory')
        directory = Path(match.group(1).strip()).resolve().parent
        if not directory.is_relative_to((ROOT/'out/agent/sessions').resolve()):
            raise RuntimeError('Agent returned a session outside the expected evidence directory')
        self.runtime_log_directory = directory

    def stop(self):
        try:
            self.run(['agent', 'stop'], 'stop', parse=False)
        finally:
            if self.runtime_log_directory is not None:
                logs = {}
                for name in ('game.stdout.log', 'game.stderr.log'):
                    destination = self.output/name
                    shutil.copyfile(self.runtime_log_directory/name, destination)
                    logs[name] = dict(path=str(destination), sha256=digest(destination))
                write_json(self.output/'runtime-logs.json', logs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=['DX12', 'Vulkan'], required=True)
    parser.add_argument('--effect', required=True, help='Project-relative efkpkg')
    parser.add_argument('--scene', default='Assets/Scenes/VFXAuthoringPreview.he')
    parser.add_argument('--output', type=Path, required=True, help='New evidence directory')
    parser.add_argument('--frames', default='0,1,15,45,90,180,360')
    parser.add_argument('--seed', type=int, default=1806)
    parser.add_argument('--scale', type=float, default=1.0)
    parser.add_argument('--intensity', type=float, default=1.0, help='RGBA attenuation in [0,1], not HDR gain')
    parser.add_argument('--color', type=float, nargs=4, metavar=('R','G','B','A'))
    parser.add_argument('--existing-build-sha256', default='')
    parser.add_argument('--gpu-probe', action='store_true', help='Explicit unverified development probe')
    args = parser.parse_args()
    if not math.isfinite(args.scale) or not 0 < args.scale <= 1000:
        parser.error('scale must be finite in (0,1000]')
    if not math.isfinite(args.intensity) or not 0 <= args.intensity <= 1:
        parser.error('intensity must be finite in [0,1]')
    if args.color is not None and any(not math.isfinite(v) or not 0 <= v <= 1 for v in args.color):
        parser.error('color must contain four finite values in [0,1]')
    if not -(2**31) <= args.seed < 2**31:
        parser.error('seed must fit int32')
    frames = [int(value) for value in args.frames.split(',')]
    if not frames or frames != sorted(set(frames)) or frames[0] < 0 or frames[-1] > 36000:
        parser.error('frames must be unique ascending values in [0,36000]')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    session = Session(output, args.backend, args.existing_build_sha256, scene=args.scene)
    report = dict(schemaVersion=1, backend=args.backend, effect=args.effect, seed=args.seed,
                  scale=args.scale, fixedDeltaSeconds=1/60, captures=[],
                  intensity=args.intensity, color=args.color,
                  commandStatus='RUNNING', visualStatus='NOT_REVIEWED',
                  performanceStatus='NOT_BENCHMARKED', evidenceType='runtime-manual',
                  effectSha256=digest(ROOT / 'Project' / args.effect))
    report['sourceHead'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    report['sourceIdentityScope'] = ('working tree at capture; pinned binary is runtime-only evidence, '
        'not a build of later source edits' if args.existing_build_sha256 else 'working tree submitted to build')
    report['sourceStatus'] = subprocess.check_output(['git', 'status', '--porcelain', '-uno'], cwd=ROOT, text=True).splitlines()
    report['executableSha256'] = digest(ROOT/'out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe')
    report['scene'] = args.scene
    report['sceneSha256'] = digest(ROOT/'Project'/args.scene)
    previous_probe = os.environ.get('HE_VFX_GPU_PARTICLES')
    if args.gpu_probe:
        os.environ['HE_VFX_GPU_PARTICLES'] = '1'
    with Storage(ROOT).workflow_scope(str(ROOT/'Project'), 'vfx-preview', [output]) as run:
        try:
            session.start()
            report['session'] = session.call('session.status')
            report['capabilities'] = session.call('vfx.describe')
            report['emptyStats'] = session.call('vfx.stats')
            empty = session.call('capture.frame', dict(filename='before-play.png'))
            shutil.copyfile(empty['path'], output/'before-play.png')
            report['beforePlayCapture'] = empty
            report['play'] = session.call('vfx.play', dict(path=args.effect, seed=args.seed, scale=args.scale))
            handle = report['play']['handle']
            if args.color is not None:
                report['colorControl'] = session.call('vfx.control', dict(handle=handle, action='color', value=args.color))
                if report['colorControl'].get('accepted') is not True:
                    raise RuntimeError('This compiled effect does not accept runtime color')
            if args.intensity != 1.0:
                report['intensityControl'] = session.call('vfx.control', dict(handle=handle, action='parameter', name='intensity', value=args.intensity))
                if report['intensityControl'].get('accepted') is not True:
                    raise RuntimeError('This compiled effect does not accept runtime intensity')
            previous = 0
            for frame in frames:
                remaining = frame-previous
                while remaining:
                    count = min(remaining, 120)
                    session.call('runtime.step', dict(frames=count))
                    remaining -= count
                capture = session.call('capture.frame', dict(filename=f'frame-{frame:05d}.png'))
                source = Path(capture['path'])
                destination = output / source.name
                shutil.copyfile(source, destination)
                report['captures'].append(dict(simulationFrame=frame, receipt=capture,
                    path=str(destination), sha256=digest(destination), stats=session.call('vfx.stats'),
                    status=session.call('vfx.control', dict(handle=handle, action='status'))))
                write_json(output/'report.json', report)
                previous = frame
            report['stop'] = session.call('vfx.control', dict(handle=handle, action='stop'))
            session.call('runtime.step', dict(frames=30))
            report['afterStopStats'] = session.call('vfx.stats')
            capture = session.call('capture.frame', dict(filename='after-stop.png'))
            shutil.copyfile(capture['path'], output/'after-stop.png')
            report['afterStopCapture'] = capture
            report['commandStatus'] = 'PASS'
            run['status'] = 'success'
        except Exception as error:
            report['commandStatus'], report['error'] = 'FAIL', str(error)
            run['status'] = 'failed'
        finally:
            try:
                session.stop()
            except Exception as error:
                report['cleanupError'] = str(error)
                report['commandStatus'], run['status'] = 'FAIL', 'unknown'
            if previous_probe is None:
                os.environ.pop('HE_VFX_GPU_PARTICLES', None)
            else:
                os.environ['HE_VFX_GPU_PARTICLES'] = previous_probe
            write_json(output/'report.json', report)
    print(json.dumps(dict(status=report['commandStatus'], report=str(output/'report.json'))))
    return 0 if report['commandStatus'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
