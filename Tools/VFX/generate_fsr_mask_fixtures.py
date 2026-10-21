"""Official-compiled particle blend diagnostics for FSR mask readbacks.

Writes isolated diagnostic assets; never modifies a project's existing effects.
Recipe v1 has no blend/material fields, so explicit XML overrides are recorded
separately and verified through the official compiler's EDIT roundtrip.
"""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile
import uuid
import xml.etree.ElementTree as ET

import authoring as a


def build(project):
    editor = a.ROOT / 'Engine/Tools/Effekseer/bin/Effekseer.exe'
    core = editor.with_name('EffekseerCore.dll')
    lock = json.loads((a.RECIPES / 'toolchain.lock.json').read_text())
    for path, expected in lock['files'].items():
        assert a.sha((a.ROOT / path).read_bytes()) == expected, 'Compiler identity changed'
    base = json.loads((a.RECIPES / 'hit-ember.recipe.json').read_text())
    relative = Path('Assets/Diagnostics/FsrParticles')
    assets = project / relative
    sources = project / 'SourceAssets/VFXAuthoring/FsrMaskValidation'
    assets.mkdir(parents=True, exist_ok=True)
    sources.mkdir(parents=True, exist_ok=True)
    reports = []
    for name, blend, distortion in [('blend', 1, False), ('add', 2, False),
                                    ('subtract', 3, False), ('multiply', 4, False),
                                    ('distortion', 1, True), ('gpu-add', 2, False)]:
        gpu = name == 'gpu-add'
        recipe = copy.deepcopy(base)
        recipe['id'] = 'fsr-mask-' + name
        recipe['compile'].update(color=[128, 192, 96, 192], radius_m=8,
                                 attack_s=.05, sustain_s=1, decay_s=.95, particles=1)
        if gpu:
            recipe['template'] = 'explosion'
            recipe['compile']['simulation'] = 'gpu'
            recipe['requirements']['fallback_recipe'] = 'fsr-mask-add'
        a.validate(recipe)
        document = ET.fromstring(a.source(recipe))
        node = document.find('Root/Children/Node')
        overrides = {'RendererCommonValues/AlphaBlend': blend,
                     'RendererCommonValues/ZTest': 'True',
                     'RendererCommonValues/ZWrite': 'False'}
        for axis in 'XYZ':
            for endpoint in ('Start', 'End'):
                for bound in ('Center', 'Min', 'Max'):
                    overrides[f'ScalingValues/Easing/{endpoint}/{axis}/{bound}'] = 2
            for endpoint in ('Center', 'Min', 'Max'):
                overrides[f'LocationValues/PVA/Velocity/{axis}/{endpoint}'] = 0
        if distortion:
            overrides.update({'RendererCommonValues/Material': 6,
                              'RendererCommonValues/DistortionIntensity': 1.25})
        if gpu:
            overrides = {'GpuParticles/RenderBasic/BlendType': 2,
                         'GpuParticles/RenderShape/ShapeSize': 2}
            for bound in ('Center', 'Min', 'Max'):
                overrides['GpuParticles/Velocity/InitialSpeed/' + bound] = 0
        for path, value in overrides.items():
            a.set_xml(node, path, value)
        payload = ET.tostring(document, encoding='utf-8', xml_declaration=True)
        dependencies = a.textures()
        with tempfile.TemporaryDirectory(prefix='he-fsr-mask-') as temporary:
            stage = Path(temporary)
            authored = stage / (recipe['id'] + '.efkproj')
            binary = authored.with_suffix('.efkefc')
            authored.write_bytes(payload)
            for path, data in dependencies.items():
                target = stage / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            a.toon.compile_effect(editor, authored, binary)
            decoded = stage / 'roundtrip.xml'
            subprocess.run(['pwsh', '-NoProfile', '-File', str(a.RECIPES / 'InspectCompiled.ps1'),
                            '-InputFile', str(binary), '-OutputFile', str(decoded),
                            '-CoreAssembly', str(core)], check=True, capture_output=True, timeout=30)
            actual = ET.parse(decoded).getroot().find('Root/Children/Node')
            for path, value in overrides.items():
                default = '1' if path.endswith('/AlphaBlend') else 'True' if path.endswith('/ZTest') else 'False' if path.endswith('/ZWrite') else '0'
                got = actual.findtext(path, default)
                assert (got == value if isinstance(value, str) else abs(float(got) - value) < 1e-6), (path, value, got)
            report = {'name': name, 'recipe': recipe, 'xmlOverrides': overrides,
                      'compiler': lock, 'officialCompile': 'PASS', 'EDITRoundtrip': 'PASS',
                      'sourceSha256': a.sha(payload), 'binarySha256': a.sha(binary.read_bytes()),
                      'runtimeValidation': 'NOT_RUN', 'main_effect': binary.name,
                      'license': 'Project-generated procedural diagnostic; no external artwork'}
            entries = {binary.name: binary.read_bytes(), 'PROVENANCE.json': a.encoded(report), **dependencies}
            package = a.toon.deterministic_zip(entries)
            (assets / (name + '.efkpkg')).write_bytes(package)
            folder = sources / name
            folder.mkdir(parents=True, exist_ok=True)
            for path, data in {**entries, authored.name: payload, 'roundtrip.xml': decoded.read_bytes(),
                               'recipe.json': a.encoded(recipe)}.items():
                target = folder / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            report['packageSha256'] = a.sha(package)
            reports.append(report)
    # A patterned emissive background makes refractive sampling visible.
    scene = {}
    for name, position, components in [
        ('Camera', [0, 0, 6], {'CameraComponent': {'Primary': True, 'bOverrideExposure': True,
          'ExposureEV100': 0, 'Camera': {'mProjectionType': 0, 'mPerspectiveFOV': .8,
          'mPerspectiveNear': .05, 'mPerspectiveFar': 100}}}),
        ('Environment', [0, 0, 0], {'EnvironmentLightComponent': {'IBLIntensity': 0,
          'ExposureEV100': 0, 'SkyColorTint': [0, 0, 0]}})]:
        scene[str(uuid.uuid5(uuid.NAMESPACE_URL, 'hengine-fsr-coverage/' + name))] = {
            'TagComponent': {'Tag': name}, 'TransformComponent': {'Translation': position,
            'Rotation': [0, 0, 0], 'Scale': [1, 1, 1], 'ParentUuid': ''}, **components}
    template = {'Type': 'Default', 'ShaderPath': 'Shaders/BasePBR.hlsl', 'Properties': {
        'name': 'Opaque', 'alphaMode': 'OPAQUE', 'doubleSided': False,
        'bUseAlbedoMap': False, 'bUseNormalMap': False, 'bUseMetallicRoughnessMap': False,
        'bUseAoMap': False, 'bUseEmissiveMap': False, 'emissiveFactor': [.2, .2, .2],
        'pbrMetallicRoughness': {'baseColorFactor': [0, 0, 0, 1], 'metallicFactor': 1, 'roughnessFactor': 1}}}
    for index in range(9):
        material = copy.deepcopy(template)
        material['Properties']['emissiveFactor'] = [.08, .25, .5] if index % 2 else [.6, .18, .07]
        material_path = relative / f'background-{index}.hmat'
        (project / material_path).write_bytes(a.encoded(material))
        scene[str(uuid.uuid5(uuid.NAMESPACE_URL, 'fsr-mask-background-' + str(index)))] = {
            'TagComponent': {'Tag': 'MaskBackground' + str(index)},
            'TransformComponent': {'Translation': [(index - 4) * .7, 0, 0], 'Rotation': [0, 0, 0], 'Scale': [.35, 8, .1]},
            'MeshComponent': {'Path': 'Assets/Models/Box/Box.hmesh', 'MaterialOverrides': [material_path.as_posix()]}}
    (assets / 'Particles.he').write_bytes(a.encoded(scene))
    (sources / 'report.json').write_bytes(a.encoded(reports))
    print(json.dumps({'officialCompile': 'PASS', 'EDITRoundtrip': 'PASS', 'effects': len(reports),
                      'assets': str(assets), 'source': str(sources)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, required=True)
    build(parser.parse_args().project.resolve())
