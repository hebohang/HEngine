"""One-time authored light migration. Runtime serialization has no compatibility path."""
import argparse
import hashlib
import json
import math
import re
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DECODER = json.JSONDecoder()
COMPONENT = re.compile(r'"(PointLightComponent|SpotLightComponent|EnvironmentLightComponent|CameraComponent|PostProcessingComponent)"\s*:\s*(?=\{)')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def migrate(text, notes):
    replacements = []
    for match in COMPONENT.finditer(text):
        start = match.end()
        component, length = DECODER.raw_decode(text[start:])
        before = dict(component)
        kind = match.group(1)
        if kind in ('PointLightComponent', 'SpotLightComponent'):
            component.setdefault('IntensityUnits', 1)
            exponent = component.pop('FalloffExponent', 2.0)
            if exponent != 2.0:
                notes.append({'component': kind, 'removedFalloffExponent': exponent})
        else:
            if 'Exposure' in component:
                exposure = float(component.pop('Exposure'))
                ev = -math.log2(exposure) if exposure > 0 else 32.0
                if exposure <= 0 or ev < -24 or ev > 32:
                    notes.append({'component': kind, 'clampedExposure': exposure})
                component['ExposureEV100'] = max(-24.0, min(32.0, ev))
            component.setdefault('ExposureEV100', 14.0)
            component.setdefault('ExposureCompensation', 0.0)
            component.pop('bUsePhysicalExposure', None)
        if before != component:
            line = text[text.rfind('\n', 0, match.start())+1:match.start()]
            indent = line if line.isspace() else ''
            block = json.dumps(component, ensure_ascii=False, indent=4)
            block = block.replace('\n', '\n' + indent)
            replacements.append((start, start+length, block))
    for start, end, value in reversed(replacements):
        text = text[:start] + value + text[end:]
    pattern = r'"Rendering\.PostProcess\.Exposure"\s*:\s*([-+\d.eE]+)'
    text = re.sub(pattern, lambda m: '"Rendering.PostProcess.ExposureEV100": ' +
                  str(max(-24.0, min(32.0, -math.log2(float(m[1])) if float(m[1]) > 0 else 32.0))), text)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', required=True, type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    changes = []
    assets = ROOT / 'Project/Assets'
    paths = sorted(p for asset_root in (assets, ROOT / 'Engine/Content/Assets')
                   for p in asset_root.rglob('*') if p.suffix.lower() in ('.he', '.prefab', '.hprefab', '.json'))
    def save(path, source, changed, notes):
        rel = path.relative_to(ROOT)
        if source == changed: return
        changes.append({'path': rel.as_posix(), 'before': digest(source), 'after': digest(changed), 'notes': notes})
        if args.apply:
            backup = args.backup.resolve() / rel
            backup.parent.mkdir(parents=True, exist_ok=True)
            with backup.open('xb') as output: output.write(source)
            path.write_bytes(changed)
    for path in paths:
        source = path.read_bytes()
        text = source.decode('utf-8-sig')
        notes = []
        result = migrate(text, notes)
        if path.suffix.lower() == '.he':
            scene = json.loads(result)
            if scene and not any('EnvironmentLightComponent' in e for e in scene.values() if isinstance(e, dict)):
                # Persist the formerly inherited camera setting once in authored data.
                # New scenes use the physical daylight default without a runtime branch.
                entity_id = str(uuid.uuid5(uuid.NAMESPACE_URL, path.relative_to(ROOT).as_posix()+'/photometric-exposure'))
                entity = {entity_id: {
                    'TagComponent': {'Tag': 'Scene Exposure'},
                    'TransformComponent': {'ParentUuid': '', 'Translation': [0.0,0.0,0.0], 'Rotation': [0.0,0.0,0.0], 'Scale': [1.0,1.0,1.0]},
                    'EnvironmentLightComponent': {'HdrTexturePath': '', 'IBLIntensity': 0.0, 'ExposureEV100': -math.log2(0.8), 'ExposureCompensation': 0.0}}}
                end = result.rfind('}')
                result = result[:end].rstrip()+',\n'+json.dumps(entity,ensure_ascii=False,indent=4)[2:-2]+'\n}\n'
                notes.append('Authored inherited exposure as EV100')
        if result != text:
            newline = '\r\n' if b'\r\n' in source else '\n'
            result = result.replace('\r\n', '\n').replace('\n', newline)
            save(path, source, result.encode('utf-8'), notes)
    # Live trajectory manifests refer to the migrated scene, never to an old hash.
    if args.apply:
        for path in (assets/'RenderingBenchmarks/Trajectories').rglob('benchmark_input.json'):
            source = path.read_bytes()
            text = source.decode('utf-8-sig')
            data = json.loads(text)
            scene = ROOT / 'Project' / data['scene']
            expected = digest(scene.read_bytes())
            if data.get('sceneSha256') != expected:
                changed = re.sub(r'("sceneSha256"\s*:\s*")[^"]+', lambda m: m[1]+expected, text)
                save(path, source, changed.encode('utf-8'), ['Updated scene hash after migration'])
        args.backup.mkdir(parents=True, exist_ok=True)
        with (args.backup/'manifest.json').open('x', encoding='utf-8') as out:
            json.dump(changes, out, ensure_ascii=False, indent=2)
    print(json.dumps({'apply': args.apply, 'changedFiles': len(changes),
        'exceptions': [c for c in changes if c['notes']], 'backup': str(args.backup)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
