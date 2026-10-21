"""Record the compiler's actual dependency graph after a successful build."""
import hashlib
import json
import pathlib
import subprocess
import sys

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def create(build, shader_root):
    build, shader_root = pathlib.Path(build).resolve(), pathlib.Path(shader_root).resolve()
    commands = json.loads((build / 'compile_commands.json').read_text())
    dependencies = set(pathlib.Path(item['file']).resolve() for item in commands)
    dependency_log = subprocess.check_output(['ninja', '-C', str(build), '-t', 'deps'], text=True, encoding='utf-8')
    for line in dependency_log.splitlines():
        if line.startswith('    '):
            dependency = pathlib.Path(line.strip())
            dependencies.add((dependency if dependency.is_absolute() else build / dependency).resolve())
    forbidden = ('/pybind11/', '/python', '/nvrhi/', '/vulkan/', '/directx/', '/windows.h')
    contaminated = [str(p) for p in dependencies if any(token in p.as_posix().lower() for token in forbidden)]
    if contaminated: raise RuntimeError('Forbidden Wasm compile dependency: ' + ', '.join(contaminated[:8]))
    shader = json.loads((shader_root / 'shader-manifest.json').read_text())
    receipt = {'schema': 1, 'shaderCacheKey': shader['cacheKey'], 'forbiddenDependencies': [],
               'inputs': {str(p): sha(p) for p in sorted(dependencies)},
               'artifacts': {name: sha(build / name) for name in ('hegame.js', 'hegame.wasm')}}
    (build / 'build-receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
    print(json.dumps({'status': 'PASS', 'dependencyFiles': len(dependencies), 'forbiddenDependencies': []}))

if __name__ == '__main__': create(sys.argv[1], sys.argv[2])
