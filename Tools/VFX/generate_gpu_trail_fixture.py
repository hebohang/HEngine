"""Compile a finite GPU trail fixture with official Effekseer 1.80.6 (no engine run)."""
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import authoring

ROOT = authoring.ROOT
NAME = 'GpuTrailPause16'


def main():
    recipe = json.loads((authoring.RECIPES / 'environment-ember.recipe.json').read_text())
    recipe['id'] = 'gpu-trail-pause-16'
    recipe['compile'].update(color=[255, 255, 255, 255], particles=16,
                             attack_s=0, sustain_s=0.25, decay_s=2, seed=1806)
    project = ET.fromstring(authoring.source(recipe))
    node = project.find('Root/Children/Node')
    expected = {
        'Basic/EmitCount/Value': 16, 'Basic/EmitPerFrame': 1,
        'Basic/LifeTime/Center': 120, 'Basic/LifeTime/Min': 120, 'Basic/LifeTime/Max': 120,
        'EmitShape/Shape': 0, 'Velocity/Spread': 20,
        'Velocity/InitialSpeed/Center': 0.04, 'Velocity/InitialSpeed/Min': 0.04,
        'Velocity/InitialSpeed/Max': 0.04, 'RenderShape/Shape': 2,
        'RenderShape/TrailLength': 16, 'RenderShape/ShapeSize': 0.04,
        'RenderColor/ColorInheritType': 3, 'RenderColor/FadeIn': 1, 'RenderColor/FadeOut': 1}
    gpu = node.find('GpuParticles')
    for path, value in expected.items():
        authoring.set_xml(gpu, path, value)
    for key in ('Center', 'Min', 'Max'):
        authoring.set_xml(node, 'CommonValues/Life/' + key, 180)
    authoring.set_xml(project, 'EndFrame', 180)
    payload = b'<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(project, encoding='utf-8')
    dependencies = {'Texture/spark.png': authoring.textures()['Texture/spark.png']}
    editor = ROOT / 'Engine/Tools/Effekseer/bin/Effekseer.exe'
    core = editor.with_name('EffekseerCore.dll')
    with tempfile.TemporaryDirectory(prefix='he-gpu-trail-') as folder:
        stage = Path(folder)
        source = stage / (NAME + '.efkproj')
        binary = stage / (NAME + '.efkefc')
        source.write_bytes(payload)
        for name, data in dependencies.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        authoring.toon.compile_effect(editor, source, binary)
        decoded = stage / 'roundtrip.xml'
        subprocess.run(['pwsh', '-NoProfile', '-File', str(authoring.RECIPES / 'InspectCompiled.ps1'),
                        '-InputFile', str(binary), '-OutputFile', str(decoded), '-CoreAssembly', str(core)], check=True)
        actual = ET.parse(decoded).getroot().find('Root/Children/Node')
        assert actual.findtext('GpuParticles/Enabled') == 'True'
        assert actual.findtext('DrawingValues/Type') == '0'  # No CPU renderer.
        for path, value in expected.items():
            # Official XML omits default values; 1.80.6 RenderShapeParams
            # TrailLength default is 16 (verified through the locked CLR type).
            default = '16' if path == 'RenderShape/TrailLength' else '0'
            assert abs(float(actual.findtext('GpuParticles/' + path, default)) - value) < 1e-5, path
        data = binary.read_bytes()
        runtime = dict(authoring.stress.parse_chunks(data))[b'BIN_']
        # Exact GPU-enabled/basic and trail type/data/size encodings from the
        # SDK GpuParticlesParameter reader, in the official compiler's BIN_.
        basic = struct.pack('<iiifff', 1, 16, 1, 0, 120, 120)
        shape = struct.pack('<BIf', 2, 16, 0.04)
        assert runtime.count(basic) == 1 and runtime.count(shape) == 1
        assert runtime.index(shape) > runtime.index(basic)
        license_data = (ROOT / authoring.stress.SOURCE_LICENSE).read_bytes().replace(b'\r\n', b'\n')
        assert authoring.sha(license_data) == authoring.stress.EXPECTED_LICENSE_SHA256
        report = {'schema_version': 1, 'fixture': NAME, 'compiler': 'official Effekseer 1.80.6',
                  'simulation': 'gpu', 'cpu_renderer': False, 'edit_parameters': expected,
                  'runtime_bin_basic_offset': runtime.index(basic), 'runtime_bin_trail_offset': runtime.index(shape),
                  'allocator_expected': {'reservedParticles': 256, 'reservedTrails': 4096, 'allocatedEmitters': 1},
                  'tool_sha256': {p.name: authoring.sha(p.read_bytes()) for p in (editor, core)},
                  'source_sha256': authoring.sha(payload), 'binary_sha256': authoring.sha(data),
                  'validation': {'official_compile': 'PASS', 'compiled_EDIT': 'PASS',
                                 'compiled_BIN_signatures': 'PASS', 'DX12': 'NOT_RUN', 'Vulkan': 'NOT_RUN'}}
        provenance = authoring.encoded(report)
        entries = {NAME + '.efkefc': data, 'PROVENANCE.json': provenance,
                   'LICENSE-CC0.md': license_data, **dependencies}
        package = authoring.toon.deterministic_zip(entries)
        output = ROOT / 'Project/SourceAssets/VFXAuthoring/GpuTrailPause16'
        output.mkdir(parents=True, exist_ok=True)
        for name, blob in {NAME + '.efkproj': payload, NAME + '.efkefc': data,
                           'PROVENANCE.json': provenance, 'roundtrip.xml': decoded.read_bytes(),
                           'LICENSE-CC0.md': license_data, **dependencies}.items():
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(blob)
        target = ROOT / 'Project/Assets/Effects/Stress' / (NAME + '.efkpkg')
        target.write_bytes(package)
        print(json.dumps({'package': str(target), 'sha256': authoring.sha(package), **report['validation']}))


if __name__ == '__main__':
    main()
