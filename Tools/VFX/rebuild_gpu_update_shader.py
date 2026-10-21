"""Rebuild only Effekseer GPU update shaders; never build/run an engine target.

The retained Vulkan GLSL was recovered from the bundled 1.80.6 SPIR-V with
spirv-cross --vulkan-semantics --version 450, then patched alongside DX12 HLSL.
Validate all resource layouts against the checked-in ABI before publishing.
Requires fxc, glslangValidator, spirv-cross and spirv-val on PATH.
"""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'Engine/Source/ThirdParty/Effekseer/src'
ABI = Path(__file__).with_name('gpu_update_shader_abi.json')


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError('{}\n{}'.format(args, result.stdout + result.stderr))
    return result.stdout


def layout(reflection):
    types = reflection['types']

    def describe(type_id):
        if type_id not in types:
            return type_id
        value = types[type_id]
        return {'name': value.get('name'), 'members': [
            dict({k: v for k, v in m.items() if k != 'type'}, type=describe(m['type']))
            for m in value.get('members', [])]}

    return {'workgroup': reflection['entryPoints'][0]['workgroup_size'],
            'resources': {category: sorted([
                dict({k: v for k, v in r.items() if k != 'type'}, type=describe(r['type']))
                for r in reflection.get(category, [])], key=lambda r: (r['set'], r['binding']))
                for category in ('ubos', 'ssbos', 'textures')}}


def main():
    tools = {name: shutil.which(name) for name in ('fxc', 'glslangValidator', 'spirv-cross', 'spirv-val')}
    if not all(tools.values()):
        raise RuntimeError('Missing shader tools: {}'.format(tools))
    dx = SRC / 'EffekseerRendererDX12/EffekseerRendererDX12'
    vk = SRC / 'EffekseerRendererVulkan/EffekseerRendererVulkan'
    shader = 'gpu_particles_update_cs'
    with tempfile.TemporaryDirectory(prefix='he-vfx-shader-') as folder:
        stage = Path(folder)
        spv = stage / 'update.spv'
        run([tools['glslangValidator'], '-V', '-o', spv, vk / ('Shader/' + shader + '.fx.comp')])
        run([tools['spirv-val'], spv])
        reflection = json.loads(run([tools['spirv-cross'], spv, '--reflect']))
        actual = layout(reflection)
        if actual != json.loads(ABI.read_text()):
            raise RuntimeError('GPU update shader ABI changed; refusing to publish')
        dx_header = stage / 'dx.h'
        dx_binary = stage / 'dx.cso'
        run([tools['fxc'], '/nologo', '/Zpc', '/Tcs_5_0', '/Emain', '/Fh', dx_header,
             '/Fo', dx_binary, dx / ('Shader/' + shader + '.fx')])
        disassembly = run([tools['fxc'], '/dumpbin', dx_binary])
        reflected = disassembly[disassembly.index('// Buffer Definitions:'):disassembly.index('// Input signature:')]
        reflected = '\n'.join(line.rstrip() for line in reflected.splitlines()) + '\n'
        if reflected != ABI.with_name('gpu_update_shader_dx12_abi.txt').read_text():
            raise RuntimeError('DX12 GPU update shader ABI changed; refusing to publish')
        # ByteAddress particle/trail strides and fixed constant-buffer ABI are
        # retained in the HLSL. Keep fxc reflection/disassembly as full evidence.
        vk_header = stage / 'vk.h'
        run([tools['glslangValidator'], '-V', '--vn', shader, '-o', vk_header,
             vk / ('Shader/' + shader + '.fx.comp')])
        evidence = ROOT / 'out/agent/vfx-zero-delta-shaders'
        evidence.mkdir(parents=True, exist_ok=True)
        (evidence / 'vulkan.reflect.json').write_text(json.dumps(reflection, indent=2) + '\n')
        (evidence / 'dx12.disassembly.txt').write_text(disassembly)
        report = {'shader_compile': 'PASS', 'vulkan_validation': 'PASS', 'vulkan_abi': 'PASS', 'dx12_abi': 'PASS',
                  'engine_build': 'NOT_RUN', 'gpu_runtime': 'NOT_RUN', 'tools': tools, 'sha256': {}}
        for source, dest in ((dx_header, dx / ('ShaderHeader/' + shader + '.h')),
                             (vk_header, vk / ('ShaderHeader/' + shader + '.h'))):
            data = source.read_bytes()
            if dest.read_bytes() != data:
                dest.write_bytes(data)
            report['sha256'][str(dest.relative_to(ROOT))] = hashlib.sha256(data).hexdigest()
        (evidence / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
