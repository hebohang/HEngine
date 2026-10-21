"""Owned Editor/Game SSGI diagnostic captures. Never edits the authored scene/config.

Run with the bundled Python. Outputs are scene-linear PFM except explicit PNG
cases. Each capture has a renderer-produced immutable world snapshot and settings.
Performance must be measured separately without capture/readback.
"""
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def sha(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def prepare(output):
    relative = 'Assets/RenderingBenchmarks/Validation/SSGITransport/' + output.name
    assets = ROOT / 'Project' / relative
    assets.mkdir(parents=True, exist_ok=True)
    original = ROOT / 'Project/Assets/Scenes/PBRTest.he'
    pbr = json.loads(original.read_text())
    # A global volume is the existing independent exposure override. Preserve
    # hidden environment semantics in Editor; explicitly zero IBL for Game.
    for entity in pbr.values():
        entity.pop('PythonScriptComponent', None)
        if 'EnvironmentLightComponent' in entity:
            entity['EnvironmentLightComponent']['IBLIntensity'] = 0.0
    pbr['a6383972-2314-4f4b-b9cf-001000000001'] = {
        'TagComponent': {'Tag': 'Diagnostic fixed exposure'},
        'PostProcessingComponent': {'bEnabled': True, 'bGlobal': True,
            'bOverrideExposure': True, 'ExposureEV100': 14.0,
            'ExposureCompensation': 0.0, 'BlendWeight': 1.0}}
    save(assets / 'pbr.he', pbr)

    def entity(name, position, scale=(1, 1, 1), rotation=(0, 0, 0)):
        return {'TagComponent': {'Tag': name}, 'TransformComponent': {
            'ParentUuid': '', 'Translation': position, 'Scale': scale, 'Rotation': rotation}}

    fixture = {}
    for index, (name, position, scale, color) in enumerate([
            ('Receiver y=0', (2, -0.05, 0), (2, .05, 2), (.5, .5, .5, 1)),
            ('Sunlit x=0', (-.05, 1, 0), (.05, 1, 2), (.6, .2, .1, 1))]):
        material = json.loads((ROOT / 'Project/Assets/Material/RoughCeramic.hmat').read_text())
        material['SchemaVersion'] = 2
        material['Textures'] = {}
        material['VariantKeywords'] = []
        props = material['Properties']
        for key in ('bUseAlbedoMap', 'bUseAoMap', 'bUseEmissiveMap', 'bUseMetallicRoughnessMap', 'bUseNormalMap'):
            props[key] = False
        props['pbrMetallicRoughness'].update(baseColorFactor=color, metallicFactor=0., roughnessFactor=1.)
        material['Parameters']['g_BaseColorFactor']['Value'] = color
        material['Parameters']['g_MetallicFactor']['Value'] = 0.
        material['Parameters']['g_RoughnessFactor']['Value'] = 1.
        save(assets / f'{index}.hmat', material)
        obj = entity(name, position, scale)
        obj['MeshComponent'] = {'Path': 'Assets/Models/Cube/Cube.hmesh',
            'MaterialOverrides': [f'{relative}/{index}.hmat'], 'meshResPath': ''}
        fixture[f'a6383972-2314-4f4b-b9cf-00100000001{index}'] = obj
    camera = entity('Reference camera', (3, 3, 5), rotation=(-math.atan2(2, math.sqrt(34)), math.atan2(3, 5), 0))
    camera['CameraComponent'] = copy.deepcopy(next(x['CameraComponent'] for x in pbr.values() if 'CameraComponent' in x))
    fixture['a6383972-2314-4f4b-b9cf-001000000020'] = camera
    # DirectionalLight uses transformed local +Y, not camera local -Z.
    sun = entity('100000 lux along -X', (0, 4, 0), rotation=(0, 0, math.pi / 2))
    sun['DirectionalLightComponent'] = copy.deepcopy(next(x['DirectionalLightComponent'] for x in pbr.values() if 'DirectionalLightComponent' in x))
    fixture['a6383972-2314-4f4b-b9cf-001000000021'] = sun
    fixture['a6383972-2314-4f4b-b9cf-001000000001'] = copy.deepcopy(pbr['a6383972-2314-4f4b-b9cf-001000000001'])
    save(assets / 'fixture.he', fixture)
    save(output / 'inputs.json', {'sourceCommit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'pbrSource': str(original), 'pbrSha256': sha(original), 'relative': relative,
        'scenes': {'pbr': sha(assets / 'pbr.he'), 'fixture': sha(assets / 'fixture.he')},
        'fixtureMaterialSha256': {path.name: sha(path) for path in assets.glob('*.hmat')},
        'modifications': ['Remove camera script in copies', 'IBLIntensity=0 for Game parity; hidden state retained',
            'Independent global PostProcessingComponent fixed EV100=14'],
        'fixture': 'Cube bounds [-1,1]; receiver top y=0, sunlit face x=0, y=[0,2], z=[-2,2].'})
    return relative


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--host', choices=['editor', 'game'], default='editor')
    p.add_argument('--scene', choices=['pbr', 'fixture'], default='pbr')
    p.add_argument('--label', default='baseline')
    p.add_argument('--resolution', default='1920x1080')
    p.add_argument('--frames', type=int, default=240)
    p.add_argument('--views', default='source,raw,demod,temporal,filtered,remod,depth,normal,albedo,hits,composite')
    p.add_argument('--rays', type=int, default=1)
    p.add_argument('--steps', type=int, default=64)
    p.add_argument('--refinement', type=int, default=3)
    p.add_argument('--bounces', type=int, default=1)
    p.add_argument('--lux', type=float, default=100000.)
    p.add_argument('--ev', type=float, default=14.)
    p.add_argument('--scale', type=float, default=.5)
    p.add_argument('--atrous', type=int, default=2)
    p.add_argument('--denoiser', choices=['None', 'SVGF'], default='SVGF')
    p.add_argument('--aa', choices=['None', 'FXAA', 'TAA'], default='None')
    p.add_argument('--distance', type=float, default=10.)
    p.add_argument('--performance', action='store_true')
    p.add_argument('--aspect', type=float)
    p.add_argument('--match-world', type=Path)
    p.add_argument('--prepare-only', action='store_true')
    p.add_argument('--occluder', action='store_true', help='Fixture only: add a 2 cm opaque vertical strip')
    p.add_argument('--motion', choices=['camera', 'board'], help='Fixture: fixed-tick continuous motion using the existing benchmark script')
    p.add_argument('--usage-settings', type=Path, help='Restore non-GI settings from an actual settings snapshot')
    p.add_argument('--format', choices=['pfm', 'png', 'raw'], default='pfm')
    p.add_argument('--api', choices=['DX12', 'Vulkan'], default='DX12')
    p.add_argument('--path', choices=['Deferred', 'Forward'], default='Deferred')
    args = p.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    relative = prepare(output) if not (output / 'inputs.json').exists() else json.loads((output / 'inputs.json').read_text())['relative']
    if args.prepare_only:
        print(relative)
        return
    executable = ROOT / f'out/agent/{args.host}-relwithdebinfo/bin' / ('HEngineEditor.exe' if args.host == 'editor' else 'StarbloomSurvivor.exe')
    binary_hash = sha(executable)
    scene = json.loads((ROOT / 'Project' / relative / (args.scene + '.he')).read_text())
    if args.occluder:
        if args.scene != 'fixture':
            raise ValueError('--occluder requires --scene fixture')
        blocker = copy.deepcopy(scene['a6383972-2314-4f4b-b9cf-001000000010'])
        blocker['TagComponent']['Tag'] = '2 cm opaque transport occluder'
        blocker['TransformComponent'].update(Translation=[.75,.55,0], Scale=[.01,.55,.4])
        black_material = json.loads((ROOT / 'Project' / relative / '0.hmat').read_text())
        black_material['Parameters']['g_BaseColorFactor']['Value'] = [0,0,0,1]
        # A black dielectric still has F0=.04. Black metal reduces that source
        # term, but Schlick's grazing F90 remains; measure its residual HDR.
        black_material['Parameters']['g_MetallicFactor']['Value'] = 1.
        black_material['Properties']['pbrMetallicRoughness']['baseColorFactor'] = [0,0,0,1]
        black_material['Properties']['pbrMetallicRoughness']['metallicFactor'] = 1.
        black_path = f'{relative}/{args.label}-occluder.hmat'
        save(ROOT / 'Project' / black_path, black_material)
        blocker['MeshComponent']['MaterialOverrides'] = [black_path]
        scene['a6383972-2314-4f4b-b9cf-001000000012'] = blocker
    for entity in scene.values():
        if 'DirectionalLightComponent' in entity:
            entity['DirectionalLightComponent']['Intensity'] = args.lux
        if 'PostProcessingComponent' in entity:
            entity['PostProcessingComponent']['ExposureEV100'] = args.ev
        if args.aspect and 'CameraComponent' in entity:
            entity['CameraComponent']['FixedAspectRatio'] = True
            entity['CameraComponent']['Camera']['mAspectRatio'] = args.aspect
    if args.match_world:
        import numpy as np
        reference = json.loads(args.match_world.read_text())
        transform = np.linalg.inv(np.array(reference['viewColumnMajor']).reshape(4, 4, order='F'))
        yaw = math.asin(float(np.clip(-transform[2, 0], -1., 1.)))
        if abs(math.cos(yaw)) < 1e-6:
            raise ValueError('Reference camera is singular in Euler coordinates; use an explicit nonsingular camera.')
        rotation = [math.atan2(transform[2, 1], transform[2, 2]), yaw,
            math.atan2(transform[1, 0], transform[0, 0])]
        projection = np.array(reference['projectionColumnMajor']).reshape(4, 4, order='F')
        for entity in scene.values():
            if entity.get('CameraComponent', {}).get('Primary'):
                entity['TransformComponent'].update(Translation=reference['cameraPosition'], Rotation=rotation)
                entity['CameraComponent']['FixedAspectRatio'] = True
                entity['CameraComponent']['Camera']['mAspectRatio'] = float(projection[1, 1] / projection[0, 0])
                entity['CameraComponent']['Camera']['mPerspectiveFOV'] = 2. * math.atan(1. / float(projection[1, 1]))
    if args.motion:
        if args.scene != 'fixture':
            raise ValueError('--motion requires the controlled fixture')
        key = 'a6383972-2314-4f4b-b9cf-001000000020' if args.motion == 'camera' else 'a6383972-2314-4f4b-b9cf-001000000011'
        moving = scene[key]
        trajectory = {'fixedDeltaSeconds': 1/60, 'periodFrames': 240,
            'origin': moving['TransformComponent']['Translation'], 'eulerRadians': moving['TransformComponent']['Rotation'],
            'right': [1,0,0] if args.motion == 'camera' else [0,0,1], 'forward': [0,0,-1],
            'lateralAmplitude': .25 if args.motion == 'camera' else 1.5,
            'forwardAmplitude': .1 if args.motion == 'camera' else 0.,
            'yawAmplitude': .04 if args.motion == 'camera' else 0.,
            'pitchAmplitude': .02 if args.motion == 'camera' else 0.}
        trajectory_path = f'{relative}/{args.label}-{args.motion}-trajectory.json'
        save(ROOT / 'Project' / trajectory_path, trajectory)
        moving['TagComponent']['Tag'] = f'BenchmarkCamera:{trajectory_path}'
        moving['PythonScriptComponent'] = {'Path': 'Scripts.RenderingBenchmarkCamera.py'}
    run_scene = f'{relative}/{args.label}-{args.host}-{args.scene}.he'
    save(ROOT / 'Project' / run_scene, scene)
    views = dict(composite=0, raw=3, albedo=4, demod=5, filtered=6, remod=7, normal=8,
        distance=10, history=11, confidence=12, coverage=13, variance=14, source=15, temporal=16, depth=17, hits=18, trace=19, hituv=20)
    settings = {'Rendering.GI.Technique': 'ScreenSpaceGI', 'Rendering.GI.Denoiser': args.denoiser,
        'Rendering.GI.RenderScale': args.scale, 'Rendering.GI.MaxBounces': args.bounces, 'Rendering.GI.AtrousIterations': args.atrous,
        'Rendering.GI.SVGF.OptimizedKernels': True, 'Rendering.GI.SSGI.RayCount': args.rays,
        'Rendering.GI.SSGI.StepCount': args.steps, 'Rendering.GI.SSGI.MaximumRayDistance': args.distance,
        'Rendering.GI.SSGI.BinaryRefinementSteps': args.refinement,
        'Rendering.GI.SSGI.Thickness': .1, 'Rendering.GI.SSGI.EdgeFade': .05,
        'Rendering.GI.SSGI.TemporalWeight': .97, 'Rendering.GI.SSGI.HiZStartMip': 2,
        'Rendering.AntiAliasing.Mode': args.aa, 'Rendering.AO.Enabled': False,
        'Rendering.Reflections.SSR.Enabled': False, 'Rendering.Reflections.Probes.Enabled': False,
        'Rendering.PostProcess.Bloom.Intensity': 0., 'Rendering.PostProcess.AutoExposure.Enabled': False,
        'Rendering.PostProcess.Contrast': 1., 'Rendering.PostProcess.Saturation': 1.,
        'Rendering.Presentation.VSync': False, 'Rendering.Debug.PBR.Mode': 0,
        'Rendering.Debug.GI.DirectOnly': False, 'Rendering.LOD.Enabled': False}
    # Contrast/saturation names are intentionally not guessed; project defaults
    # are neutral and GI diagnostics bypass color grading.
    settings.pop('Rendering.PostProcess.Contrast')
    settings.pop('Rendering.PostProcess.Saturation')
    if args.usage_settings:
        usage = json.loads(args.usage_settings.read_text())['effective']
        for key, value in usage.items():
            if key.startswith(('Rendering.AntiAliasing.', 'Rendering.AO.', 'Rendering.Reflections.', 'Rendering.PostProcess.')):
                settings[key] = value
    if args.denoiser != 'SVGF' and any(view in args.views.split(',') for view in ('temporal', 'filtered', 'demod')):
        raise ValueError('These illumination AOVs require SVGF; select raw with --denoiser None')
    manifest = []
    for name in args.views.split(','):
        if name != 'off' and name not in views:
            raise ValueError(f'Unknown diagnostic view: {name}')
        prefix = output / f'{args.label}-{args.host}-{args.scene}-{name}'
        if prefix.with_suffix('.log').exists():
            raise FileExistsError(prefix)
        settings['Rendering.Debug.GI.View'] = views.get(name, 0)
        settings['Rendering.GI.Technique'] = 'Off' if name == 'off' else 'ScreenSpaceGI'
        save(prefix.with_suffix('.json'), settings)
        command = [str(ROOT / 'hectl.bat'), args.host, 'check' if args.performance else 'capture', '-StartupScene', run_scene,
            '-RenderPath', args.path, '-GraphicsApi', args.api, '-StrictGraphicsApi',
            '-Resolution', args.resolution, '-Frames', str(args.frames), '-PerformanceWarmupFrames', '120',
            '-FixedDeltaSeconds', '0.016666666666666666', '-RenderSettingsPath', str(prefix.with_suffix('.json')),
            '-CapturePath', str(prefix.with_suffix('.' + args.format)), '-RenderStats',
            '-RenderStatsPath', str(prefix.with_suffix('.csv')), '-RenderValidation', '-TimeoutSeconds', '300',
            '-ExistingBuildSha256', binary_hash]
        if args.performance:
            index = command.index('-CapturePath')
            del command[index:index + 2]
            command.remove('-RenderValidation')
        shader_hashes = {str(path.relative_to(ROOT)): sha(path) for path in (
            ROOT / 'Engine/Content/Shaders/GlobalIllumination/ScreenSpaceGlobalIllumination.hlsl',
            ROOT / 'Engine/Content/Shaders/GlobalIllumination/RtgiSvgf.hlsl')}
        telemetry = None
        telemetry_log = None
        run_environment = os.environ.copy()
        if args.motion:
            motion_directory = prefix.with_suffix('.motion')
            motion_directory.mkdir()
            run_environment['HE_RENDER_BENCHMARK_OUTPUT'] = str(motion_directory)
        try:
            smi = Path('C:/Windows/System32/nvidia-smi.exe')
            if args.performance and smi.exists():
                telemetry_log = prefix.with_suffix('.gpu.csv').open('w', encoding='utf-8')
                telemetry = subprocess.Popen([str(smi), '--query-gpu=timestamp,name,driver_version,temperature.gpu,clocks.gr,clocks.mem,power.draw,memory.used',
                    '--format=csv', '--loop-ms=1000'], stdout=telemetry_log, stderr=subprocess.STDOUT,
                    creationflags=subprocess.CREATE_NO_WINDOW)
            with prefix.with_suffix('.log').open('w', encoding='utf-8') as log:
                result = subprocess.run(command, cwd=ROOT, env=run_environment, stdout=log, stderr=subprocess.STDOUT)
        finally:
            if telemetry is not None:
                telemetry.terminate()
                telemetry.wait(timeout=10)
            if telemetry_log is not None:
                telemetry_log.close()
        manifest.append({'command': command, 'exitCode': result.returncode, 'exeSha256': binary_hash,
            'evidenceType': 'runtime-manual', 'captureIsNotPerformanceMeasurement': not args.performance,
            'shaderHashes': shader_hashes, 'sceneSha256': sha(ROOT / 'Project' / run_scene)})
        manifest[-1]['overrideMaterialSha256'] = {
            material: sha(ROOT / 'Project' / material)
            for entity in scene.values() for material in entity.get('MeshComponent', {}).get('MaterialOverrides', [])
            if isinstance(material, str) and material and (ROOT / 'Project' / material).is_file()}
        if args.motion and result.returncode == 0:
            motion_evidence = json.loads((motion_directory / 'camera.runtime.json').read_text())
            if motion_evidence['status'] != 'PASS_FIXED_TICK_INPUT' or motion_evidence['ticks'] != args.frames:
                raise RuntimeError('Authored motion did not complete the exact requested fixed ticks')
            manifest[-1]['motionEvidence'] = motion_evidence
        save(output / f'{args.label}-{args.host}-{args.scene}-runs.json', manifest)
        print(name, 'PASS' if result.returncode == 0 else 'FAIL', flush=True)
        if result.returncode:
            raise RuntimeError(f'Capture failed; see {prefix}.log')


if __name__ == '__main__':
    main()
