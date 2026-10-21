"""Local-only strict exporter for HEngine's initial WeChat capability profile."""
import hashlib
import json
import math
import pathlib
import shutil
import uuid
import importlib.util

_publish_spec = importlib.util.spec_from_file_location('export_publish', pathlib.Path(__file__).with_name('export_publish.py'))
publication = importlib.util.module_from_spec(_publish_spec)
_publish_spec.loader.exec_module(publication)

PROFILE = 'wechat-webgl2-sprite-v1'
FIELDS = {
    'TagComponent': {'Tag'},
    'TransformComponent': {'Translation', 'Rotation', 'Scale', 'ParentUuid'},
    'SpriteRendererComponent': {'Color', 'Texture', 'TilingFactor'},
    'NativeScriptComponent': {'Type', 'Properties'},
}

def read_json(path):
    def invalid(value): raise ValueError('Non-finite JSON value: ' + value)
    return json.loads(pathlib.Path(path).read_text(encoding='utf-8-sig'), parse_constant=invalid)

def resolve_asset(project, value):
    if not isinstance(value, str) or not value or '\\' in value or ':' in value or '..' in value.split('/'):
        raise ValueError('Invalid package asset path: ' + str(value))
    path = (project / value).resolve()
    path.relative_to(project.resolve())
    if not path.is_file(): raise ValueError('Resource does not exist: ' + value)
    return path

def validate(project, config):
    if config.get('schema') != 1 or config.get('shaderProfile') != PROFILE:
        raise ValueError('Unsupported WeChat schema or Shader profile; desktop shaders cannot be exported')
    if config.get('appid') and (len(config['appid']) != 18 or not config['appid'].startswith('wx') or
                               any(c not in '0123456789abcdef' for c in config['appid'][2:])):
        raise ValueError('AppID must be a configured wx + 16 hexadecimal digits, or empty pending configuration')
    if config.get('orientation') not in ('portrait', 'landscape') or not isinstance(config.get('storageKey'), str) or not config['storageKey']:
        raise ValueError('Invalid orientation or persistent storage namespace')
    scene = read_json(resolve_asset(project, config['scene']))
    if not isinstance(scene, dict) or not 1 <= len(scene) <= 8: raise ValueError('Sample profile requires 1..8 entities')
    scripts = 0
    for entity_id, entity in scene.items():
        uuid.UUID(entity_id)
        if not isinstance(entity, dict) or 'TagComponent' not in entity: raise ValueError('Entity requires TagComponent: ' + entity_id)
        for component, fields in entity.items():
            if component == 'PythonScriptComponent': raise ValueError('PythonScriptComponent cannot run on WeChat: ' + entity_id)
            if component not in FIELDS: raise ValueError('Unsupported WeChat component ' + component + ': ' + entity_id)
            if not isinstance(fields, dict) or set(fields) - FIELDS[component]:
                raise ValueError('Unsupported fields in ' + component + ': ' + entity_id)
        if not isinstance(entity['TagComponent'].get('Tag'), str): raise ValueError('Tag must be a string')
        transform = entity.get('TransformComponent', {})
        if transform.get('ParentUuid', ''): raise ValueError('Transform parenting is not supported by the sprite profile')
        for key in ('Translation', 'Rotation', 'Scale'):
            if key in transform and (not isinstance(transform[key], list) or len(transform[key]) != 3 or
                                     any(type(v) not in (int, float) or not math.isfinite(v) or abs(v) > 100 for v in transform[key])):
                raise ValueError('Invalid transform ' + key)
        sprite = entity.get('SpriteRendererComponent', {})
        if 'SpriteRendererComponent' in entity and sprite.get('Texture') != config['texture']:
            raise ValueError('Sprite texture must match the single-texture export profile')
        if sprite.get('TilingFactor', 1) != 1: raise ValueError('Sprite profile supports TilingFactor=1 only')
        if 'Color' in sprite and (not isinstance(sprite['Color'], list) or len(sprite['Color']) != 4 or
                                 any(type(v) not in (int, float) or not 0 <= v <= 1 for v in sprite['Color'])):
            raise ValueError('Invalid sprite color')
        if 'NativeScriptComponent' in entity:
            script = entity['NativeScriptComponent']; scripts += 1
            if script.get('Type') != 'TouchSample' or 'SpriteRendererComponent' not in entity:
                raise ValueError('Unknown native type or missing sprite: ' + str(script.get('Type')))
            properties = script.get('Properties', {})
            if not isinstance(properties, dict) or set(properties) - {'Speed', 'Score'}:
                raise ValueError('Unsupported TouchSample properties')
            if type(properties.get('Speed', 3)) not in (int, float) or not 0 <= properties.get('Speed', 3) <= 20:
                raise ValueError('Invalid TouchSample Speed')
            if type(properties.get('Score', 0)) != int or not 0 <= properties.get('Score', 0) <= 1000000:
                raise ValueError('Invalid TouchSample Score')
    if scripts != 1: raise ValueError('Sample requires exactly one native TouchSample controller')
    for key in ('texture', 'audio'): resolve_asset(project, config[key])
    return scene

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()

def export(root, project, config_path, output):
    manager = publication.storage.Storage(root)
    output = publication.storage.safe(output, [manager.root / 'out/agent'])
    # No export may replace a dependency/cache/history container.
    if publication.storage.category(output, manager.root) in ('download-cache', 'toolchain', 'sdk-dependencies', 'build-cache') or output.name in ('wechat', 'sessions', 'logs', 'storage', 'projects'):
        raise ValueError('Protected export destination')
    lock = manager.root / 'out/agent/.wechat-export.lock'
    publication.storage.safe(lock, manager.roots)
    lock.parent.mkdir(parents=True, exist_ok=True)
    with publication.storage.exclusive(lock, create=True):
        publication.recover(manager, output)
        result = _export_locked(root, project, config_path, output, manager, lock)
    publication.prune(manager, publication.storage.absolute(project))
    return result


def package_resources(project, config, staging):
    resources = []
    for key in ('scene', 'texture', 'audio'):
        # .he is not in WeChat's package whitelist. Carry the scene as opaque
        # bytes, not .json (which the packaging pipeline may normalize as text).
        # The CPU loader still consumes the original HE resource identity.
        package_path = config[key] + '.bin' if key == 'scene' else config[key]
        source = resolve_asset(project, config[key]); target = staging / package_path
        target.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, target)
        resources.append({'path': config[key], 'packagePath': package_path, 'bytes': source.stat().st_size, 'sha256': sha(source), 'type': key})
    return resources


def _export_locked(root, project, config_path, output, manager, lock):
    root, project, output = map(pathlib.Path, (root, project, output))
    output = publication.storage.absolute(output)
    project = publication.storage.absolute(project)
    # This route owns its local build artifacts only, never arbitrary user folders.
    output.relative_to((root / 'out/agent').resolve())
    if output == (root / 'out/agent').resolve(): raise ValueError('Output must be a child directory')
    config = read_json(config_path)
    validate(project, config)
    binary_root = root / 'out/agent/wechat-wasm-release'
    for name in ('hegame.js', 'hegame.wasm'):
        if not (binary_root / name).is_file(): raise ValueError('Missing Wasm build artifact: ' + name)
    shader_root = root / 'out/agent/wechat/shaders'
    shader = read_json(shader_root / 'shader-manifest.json')
    if shader['profile']['name'] != PROFILE: raise ValueError('Cross-platform Shader cache mismatch')
    receipt = read_json(binary_root / 'build-receipt.json')
    if receipt['shaderCacheKey'] != shader['cacheKey']: raise ValueError('Wasm/Shader receipt mismatch; rebuild with hectl wechat build')
    for name, expected in receipt['artifacts'].items():
        if sha(binary_root / name) != expected: raise ValueError('Wasm artifact changed after validation: ' + name)
    for name, expected in receipt['inputs'].items():
        source = pathlib.Path(name)
        if not source.is_file() or sha(source) != expected: raise ValueError('Build input changed; run hectl wechat build: ' + name)
    for stage in ('vertex', 'fragment'):
        if sha(shader_root / (stage + '.glsl')) != shader['artifacts'][stage]['glsl']:
            raise ValueError('Shader artifact hash mismatch: ' + stage)
    staging = output.parent / (output.name + '.staging-' + uuid.uuid4().hex)
    staging.mkdir()
    try:
        (staging / 'engine').mkdir(); (staging / 'shaders').mkdir()
        for name in ('hegame.js', 'hegame.wasm'): shutil.copy2(binary_root / name, staging / 'engine' / name)
        for name in ('game.js', 'host.js', 'boot-diagnostics.js'): shutil.copy2(root / 'Engine/Source/Runtime/Platform/WeChat' / name, staging / name)
        (staging / 'licenses').mkdir()
        notices = {'sokol.txt': root / 'out/agent/wechat/deps/sokol-LICENSE',
                   'emscripten.txt': root / 'out/agent/wechat/deps/emsdk/upstream/install/emscripten/LICENSE',
                   'entt.txt': root / 'Engine/Source/ThirdParty/entt/LICENSE',
                   'glm.txt': root / 'Engine/Source/ThirdParty/glm/copying.txt',
                   'json.txt': root / 'Engine/Source/ThirdParty/json/LICENSE.MIT',
                   'spdlog.txt': root / 'Engine/Source/ThirdParty/spdlog/LICENSE',
                   'crossguid.txt': root / 'Engine/Source/ThirdParty/crossguid/LICENSE'}
        for name, source in notices.items(): shutil.copy2(source, staging / 'licenses' / name)
        stb_source = (root / 'Engine/Source/ThirdParty/stb_image/stb_image.h').read_text(encoding='utf-8')
        license_start = stb_source.rfind('ALTERNATIVE A - MIT License')
        if license_start < 0: raise ValueError('stb_image license notice not found')
        (staging / 'licenses/stb_image.txt').write_text(stb_source[license_start:], encoding='utf-8')
        resources = package_resources(project, config, staging)
        for name in ('vertex.glsl', 'fragment.glsl'): shutil.copy2(shader_root / name, staging / 'shaders' / name)
        runtime_hashes = ''.join(sha(staging / name) for name in ('game.js', 'host.js', 'boot-diagnostics.js', 'engine/hegame.js', 'engine/hegame.wasm'))
        runtime_hashes += json.dumps(config, sort_keys=True, ensure_ascii=True) + json.dumps(resources, sort_keys=True)
        build_id = 'he-' + hashlib.sha256(runtime_hashes.encode('ascii')).hexdigest()[:12]
        host_config = {**config, 'buildId': build_id, 'resources': resources, 'shaderCacheKey': shader['cacheKey']}
        (staging / 'he-config.js').write_text('module.exports = ' + json.dumps(host_config, indent=2) + ';\n', encoding='utf-8')
        (staging / 'game.json').write_text(json.dumps({'deviceOrientation': config['orientation']}, indent=2), encoding='utf-8')
        project_config = {'appid': config['appid'], 'projectname': config['projectName'], 'compileType': 'game',
                          'miniprogramRoot': './', 'setting': {'es6': True, 'minified': False},
                          'packOptions': {'ignore': [{'type': 'file', 'value': 'browser-test.html'}, {'type': 'file', 'value': 'browser-harness.js'}]}}
        (staging / 'project.config.json').write_text(json.dumps(project_config, ensure_ascii=False, indent=2), encoding='utf-8')
        # Tiny diagnostics-only browser entrance; excluded from the WeChat package.
        shutil.copy2(root / 'Tools/WeChat/browser-harness.js', staging / 'browser-harness.js')
        (staging / 'browser-test.html').write_text('''<!doctype html><meta charset="utf-8"><title>HE WeChat auxiliary browser check</title>
<style>body{margin:0;background:#07101b;color:white}canvas{width:100vw;height:100vh;touch-action:none}#state{position:fixed;top:0;left:8px;pointer-events:none}</style>
<canvas id="game"></canvas><pre id="state">Auxiliary browser validation — not WeChat evidence</pre>
<script>var module={exports:{}};</script><script src="he-config.js"></script><script>var config=module.exports;</script>
<script src="boot-diagnostics.js"></script><script>var createBootDiagnostics=module.exports.createBootDiagnostics;</script>
<script src="host.js"></script><script>var createHost=module.exports.createHost;</script>
<script src="engine/hegame.js"></script><script>var createEngine=createHEngine;</script><script src="browser-harness.js"></script>''', encoding='utf-8')
        manifest = {'schema': 1, 'profile': PROFILE, 'shaderCacheKey': shader['cacheKey'], 'resources': resources,
                    'validation': {'wechatDevtools': 'unverified', 'android': 'unverified', 'ios': 'unverified'},
                    'files': {str(p.relative_to(staging)).replace('\\', '/'): {'bytes': p.stat().st_size, 'sha256': sha(p)} for p in staging.rglob('*') if p.is_file()}}
        manifest['totalBytesBeforeManifest'] = sum(item['bytes'] for item in manifest['files'].values())
        (staging / 'resource-manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
        publication.publish(manager, project, staging, output, lock)
        return {'output': str(output), 'bytes': manifest['totalBytesBeforeManifest'], 'appidConfigured': bool(config['appid'])}
    except BaseException:
        # A failed rollback leaves the journal and all recovery material intact.
        if staging.exists() and not publication.journal_path(output).exists() and not (staging / '.he-export.json').exists():
            shutil.rmtree(staging)
        raise

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(); parser.add_argument('action', choices=['validate', 'export'])
    parser.add_argument('--root', required=True); parser.add_argument('--project', required=True)
    parser.add_argument('--config', required=True); parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.action == 'validate':
        validate(pathlib.Path(args.project), read_json(args.config)); print('PASS strict WeChat export profile')
    else: print(json.dumps(export(args.root, args.project, args.config, args.output), indent=2))
