"""Windows host stage of HShaderCompiler's deliberately small WeChat profile.

No compiler ships in the game. No substitutions are applied to generated GLSL.
"""
import hashlib
import json
import pathlib
import re
import subprocess

PROFILE = {'name': 'wechat-webgl2-sprite-v1', 'glsl': 300, 'ubo': 'flatten-float4',
           'color': 'linear-rgba8', 'alpha': 'straight', 'depth': 'gl-minus-one-to-one',
           'precision': 'highp', 'threads': False, 'varyingNames': 'he_varying_LOCATION'}

def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()

def write_changed(path, contents):
    if not path.is_file() or path.read_text(encoding='utf-8') != contents:
        path.write_text(contents, encoding='utf-8')

def run(args, log):
    result = subprocess.run([str(a) for a in args], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = result.stdout.decode('utf-8', errors='replace')
    with log.open('a', encoding='utf-8') as stream:
        stream.write(json.dumps([str(a) for a in args]) + '\n' + output + '\n')
    if result.returncode:
        raise RuntimeError('Shader tool failed; full diagnostics: ' + str(log))
    return output

def compile_shader(root, output, dxc, cross):
    root, output, dxc, cross = map(pathlib.Path, (root, output, dxc, cross))
    output.mkdir(parents=True, exist_ok=True)
    log = output / 'compile.log'
    source = root / 'Engine/Shaders/WeChat/Sprite.hlsl'
    # This capability profile accepts exactly this offline shader, no desktop variants.
    # Include dependencies are recursively hashed and restricted to Engine/Shaders.
    dependencies = {}
    def visit(path):
        path = path.resolve()
        path.relative_to((root / 'Engine/Shaders').resolve())
        if str(path) in dependencies: return
        text = path.read_text(encoding='utf-8')
        dependencies[str(path)] = digest(path)
        for include in re.findall(r'^\s*#\s*include\s*["<]([^">]+)[">]', text, re.M):
            visit(path.parent / include)
    visit(source)
    options = ['-spirv', '-fspv-target-env=vulkan1.0', '-fvk-use-gl-layout', '-O3', '-Zpc']
    cross_options = ['--es', '--version', '300', '--flatten-ubo', '--no-420pack-extension']
    compiler_files = {str(p): digest(p) for p in (dxc, dxc.parent / 'dxcompiler.dll', cross)}
    identity = {'profile': PROFILE, 'dependencies': dependencies, 'tools': compiler_files,
                'dxcVersion': run([dxc, '--version'], log),
                'options': options, 'crossOptions': cross_options,
                'entries': {'vertex': ['VSMain', 'vs_6_0'], 'fragment': ['PSMain', 'ps_6_0']}}
    cache_key = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    artifacts = {}
    for stage, (entry, target) in identity['entries'].items():
        spirv = output / (stage + '.spv')
        glsl = output / (stage + '.glsl')
        run([dxc, source, '-E', entry, '-T', target, *options, '-Fo', spirv], log)
        reflection = json.loads(run([cross, spirv, '--reflect'], log))
        interface = 'outputs' if stage == 'vertex' else 'inputs'
        rename_options = []
        for varying in reflection.get(interface, []):
            location = varying['location']
            rename_options += ['--rename-interface-variable', 'out' if stage == 'vertex' else 'in', str(location), 'he_varying_' + str(location)]
        run([cross, spirv, *cross_options, *rename_options, '--output', glsl], log)
        text = glsl.read_text(encoding='utf-8')
        if not text.startswith('#version 300 es'):
            raise RuntimeError('SPIRV-Cross did not emit GLSL ES 3.00')
        forbidden = ('ssbos', 'storage_images', 'push_constants', 'subpass_inputs')
        if any(reflection.get(key) for key in forbidden):
            raise RuntimeError('Unsupported shader resource in WeChat profile: ' + stage)
        artifacts[stage] = {'spirv': digest(spirv), 'glsl': digest(glsl), 'reflection': reflection}
        (output / (stage + '.reflection.json')).write_text(json.dumps(reflection, indent=2), encoding='utf-8')
    vertex = artifacts['vertex']['reflection']
    fragment = artifacts['fragment']['reflection']
    stage_shape = lambda fields: sorted((field['location'], field['type']) for field in fields)
    if stage_shape(vertex.get('outputs', [])) != stage_shape(fragment.get('inputs', [])):
        raise RuntimeError('Vertex/fragment interface locations or types do not match')
    ubos = vertex.get('ubos', [])
    if len(ubos) != 1 or ubos[0]['block_size'] != 80:
        raise RuntimeError('Expected exactly an 80-byte vertex uniform block')
    members = vertex['types'][ubos[0]['type']]['members']
    if [(m['name'], m['offset']) for m in members] != [('Rows', 0), ('Tint', 64)]:
        raise RuntimeError('Unexpected final SPIR-V uniform offsets')
    if members[0].get('array_stride') != 16 or members[0].get('array') != [4]:
        raise RuntimeError('Unexpected matrix-row array layout')
    # These exact names are SPIRV-Cross output contracts, checked rather than rewritten.
    vs = (output / 'vertex.glsl').read_text()
    fs = (output / 'fragment.glsl').read_text()
    uniform = re.findall(r'uniform\s+vec4\s+(\w+)\[5\]', vs)
    sampler = re.findall(r'uniform\s+(?:highp\s+)?sampler2D\s+(\w+)', fs)
    attrs = re.findall(r'layout\(location = (\d+)\) in vec2 (\w+);', vs)
    if len(uniform) != 1 or len(sampler) != 1 or sorted(a[0] for a in attrs) != ['0', '1']:
        raise RuntimeError('GLSL binding shape is outside the supported profile')
    binding = {'uniform': uniform[0], 'uniformBytes': 80, 'uniformVec4Count': 5,
               'sampler': sampler[0], 'attributes': {n: int(i) for i, n in attrs}}
    manifest = {**identity, 'cacheKey': cache_key, 'artifacts': artifacts, 'bindings': binding}
    (output / 'shader-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    # Sokol descriptor data comes from final GLSL plus validated SPIR-V reflection.
    header = '#pragma once\nnamespace HEngine::WeChatShader {\n'
    for name, value in [('Vertex', vs), ('Fragment', fs), ('Uniform', uniform[0]), ('Sampler', sampler[0])]:
        header += 'inline constexpr char ' + name + '[] = R"HE_SHADER(' + value + ')HE_SHADER";\n'
    for i, name in attrs:
        header += f'inline constexpr char Attribute{i}[] = "{name}";\n'
    header += '}\n'
    write_changed(output / 'WeChatShader.h', header)
    return manifest

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    for name in ('root', 'output', 'dxc', 'cross'): parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    result = compile_shader(args.root, args.output, args.dxc, args.cross)
    print(json.dumps({'profile': PROFILE['name'], 'cacheKey': result['cacheKey'], 'bindings': result['bindings']}))
