"""Restore the official Bistro environment in a separate benchmark derivative.

Does not execute the downloaded pyscene or alter geometry/material/light data.
Original failed lighting inputs and their measured evidence remain unchanged.
"""
import hashlib
import json
import shutil
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PROJECT = ROOT / 'Examples/RenderingTests'
SOURCE = PROJECT / 'SourceAssets/RenderingBenchmarks/Bistro/extracted/Bistro_v5_2'
OLD = PROJECT / 'Assets/RenderingBenchmarks/Trajectories/BistroExterior2048BCGPUMaskFixed'
NEW = OLD.with_name('BistroExterior2048BCGPUMaskFixedEnvironment')
HDR = PROJECT / 'Assets/RenderingBenchmarks/Environments/san_giuseppe_bridge_4k.hdr'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def main():
    if NEW.exists():
        raise RuntimeError('Refusing to replace an existing corrected benchmark.')
    source_hdr = SOURCE / HDR.name
    recipe = (SOURCE / 'BistroExterior.pyscene').read_text(encoding='utf-8-sig')
    if 'EnvMap("san_giuseppe_bridge_4k.hdr")' not in recipe or 'sceneBuilder.envMap.intensity = 10' not in recipe:
        raise RuntimeError('Official source environment declaration changed; re-audit it.')
    scene = json.loads((OLD / 'Benchmark.he').read_text(encoding='utf-8'))
    if any('EnvironmentLightComponent' in entity for entity in scene.values()):
        raise RuntimeError('Original scene already has an environment; re-audit the defect.')
    HDR.parent.mkdir(parents=True, exist_ok=True)
    if HDR.exists() and sha(HDR) != sha(source_hdr):
        raise RuntimeError('Destination environment differs from the official source.')
    if not HDR.exists():
        shutil.copyfile(source_hdr, HDR)
    NEW.mkdir(parents=True)
    shutil.copyfile(OLD / 'trajectory.json', NEW / 'trajectory.json')
    old_tag = 'BenchmarkCamera:' + (OLD / 'trajectory.json').relative_to(PROJECT).as_posix()
    new_tag = 'BenchmarkCamera:' + (NEW / 'trajectory.json').relative_to(PROJECT).as_posix()
    camera_count = 0
    for entity in scene.values():
        if entity.get('TagComponent', {}).get('Tag') == old_tag:
            entity['TagComponent']['Tag'] = new_tag
            camera_count += 1
    if camera_count != 1:
        raise RuntimeError('Expected one benchmark camera tag.')
    environment_id = str(uuid.uuid5(uuid.NAMESPACE_URL, 'HEngine/ORCA/Bistro/official-environment'))
    if environment_id in scene:
        raise RuntimeError('Environment identity collides with source geometry.')
    scene[environment_id] = {
        'TagComponent': {'Tag': 'ORCA Official Environment (intensity 10)'},
        'TransformComponent': {'ParentUuid': '', 'Translation': [0, 0, 0],
                               'Rotation': [0, 0, 0], 'Scale': [1, 1, 1]},
        'EnvironmentLightComponent': {'HdrTexturePath': HDR.relative_to(PROJECT).as_posix(),
                                      'IBLIntensity': 10.0, 'ExposureEV100': 1.6896598793878495, 'ExposureCompensation': 0.0},
    }
    write(NEW / 'Benchmark.he', scene)
    manifest = json.loads((OLD / 'benchmark_input.json').read_text(encoding='utf-8'))
    for kind, name in [('scene', 'Benchmark.he'), ('trajectory', 'trajectory.json')]:
        manifest[kind] = (NEW / name).relative_to(PROJECT).as_posix()
        manifest[kind + 'Sha256'] = sha(NEW / name)
    manifest['environment'] = {'path': HDR.relative_to(PROJECT).as_posix(), 'sha256': sha(HDR),
                               'iblIntensity': 10.0, 'exposureEV100': 1.6896598793878495, 'exposureCompensation': 0.0}
    write(NEW / 'benchmark_input.json', manifest)
    original = json.loads((OLD / 'Benchmark.he').read_text(encoding='utf-8'))
    restored = json.loads(json.dumps(scene))
    del restored[environment_id]
    for entity in restored.values():
        if entity.get('TagComponent', {}).get('Tag') == new_tag:
            entity['TagComponent']['Tag'] = old_tag
    if restored != original or sha(OLD / 'trajectory.json') != sha(NEW / 'trajectory.json'):
        raise RuntimeError('Correction altered original entities or camera trajectory.')
    evidence = {'evidenceType': 'unit-test', 'status': 'PASS',
                'command': 'python Tools/Assets/RenderingBenchmarks/restore_bistro_environment.py',
                'sourceSceneSha256': sha(OLD / 'Benchmark.he'), 'correctedSceneSha256': sha(NEW / 'Benchmark.he'),
                'sourcePysceneSha256': sha(SOURCE / 'BistroExterior.pyscene'),
                'hdrSourceSha256': sha(source_hdr), 'hdrAssetSha256': sha(HDR),
                'hdrBytes': HDR.stat().st_size, 'sourceEntitiesPreserved': len(original),
                'originalGeometryMaterialsLightsAndTransformsUnchanged': True,
                'cameraTrajectoryBytesUnchanged': True, 'runtimeVisualStatus': 'NOT_VERIFIED',
                'license': 'Official ORCA Bistro package CC BY 4.0; original notices retained under Examples/RenderingTests/SourceAssets/RenderingBenchmarks/Bistro',
                'attribution': 'Amazon Lumberyard Bistro, Open Research Content Archive (ORCA), Amazon Lumberyard, July 2017',
                'interpretation': 'Restores declared lighting input; not a claim of cross-renderer photometric parity.'}
    write(NEW / 'environment-restoration.json', evidence)
    print(json.dumps(evidence))


if __name__ == '__main__':
    main()
