"""Transactional project asset moves. Uses the existing content workflow's I/O.

Reference adapters are deliberately separate from serializers: ordinary text is
never searched/replaced. Unknown references and source-code literals block a move.
Imported immutable resources remain owned by content_sync.
"""
import argparse
import json
import posixpath
import re
from pathlib import Path
import sys
import uuid

from content_sync import Failure, atomic, confined, digest, encode, read_json, lock as content_lock

JSON_FORMATS = {'.he', '.hprefab', '.hmesh', '.hmat', '.htex', '.hanim', '.hskel', '.hanimctrl', '.hui', '.hbt', '.json'}
TEXT_FORMATS = {'.cs', '.py', '.js', '.rml', '.rcss', '.hlsl', '.hlsli', '.txt'}
# Explicit resource fields used by each authoring format. Unknown JSON members
# remain byte-for-byte semantically preserved; no serializer defaults are added.
COMPONENT_REFS = {
    'MeshComponent': {'Path', 'meshResPath', 'MaterialOverrides'},
    'SpriteRendererComponent': {'Texture'},
    'AnimatorComponent': {'animResPath', 'controllerPath'},
    'SoundComponent': {'Path'}, 'BehaviorTreeComponent': {'btResPath'},
    'EnvironmentLightComponent': {'HdrTexturePath'},
    'Rigidbody3DComponent': {'CollisionAssetPath'},
    'PrefabInstanceComponent': {'TemplatePath'},
}
RESOURCE_FIELDS = {
    '.hmesh': {'uri', 'materialResPath', 'skeletonPath', 'skeletonResPath', 'meshBinPath'},
    '.hmat': {'textureSourcePath', 'AssetPath', 'TexturePath'},
    '.htex': {'textureSourcePath'},
    '.hanimctrl': {'clipPath', 'targetSkeletonPath'},
    '.hanim': {'meshResPath', 'targetSkeletonPath'},
    '.hui': {'FontPath', 'BackgroundImage', 'ImagePath', 'RmlPath', 'RcssPath', 'GeneratedRmlPath', 'GeneratedRcssPath'},
}
EMBEDDED_JSON = {'Fields', 'BaselineJson', 'TemplateDocumentJson'}
DISPLAY_FIELDS = {'Tag', 'name', 'Name', 'Text', 'Description', 'Label'}
LIMIT = 32 * 1024 * 1024


def normalize(value):
    value = str(value).replace('\\', '/')
    if not value.startswith('Assets/') or any(p in ('', '.', '..') for p in value.split('/')):
        raise Failure('invalid_asset_path', 'Use a project-relative Assets path: ' + value)
    reserved = {'CON', 'PRN', 'AUX', 'NUL'} | {f'{p}{i}' for p in ('COM', 'LPT') for i in range(1, 10)}
    if any(ord(c) < 32 or c in ':<>"|?*' for c in value) or any(
            part.endswith((' ', '.')) or part.split('.')[0].upper() in reserved for part in value.split('/')):
        raise Failure('invalid_asset_path', 'Unsafe asset path: ' + value)
    return value


def relocate(value, source, target):
    canonical = value.replace('\\', '/')
    if canonical.casefold() == source.casefold():
        return target
    if canonical.casefold().startswith(source.casefold() + '/'):
        return target + canonical[len(source):]
    return value


def is_reference(extension, parents, key, parent):
    if len(parents) >= 1 and key in COMPONENT_REFS.get(parents[-1], set()):
        return True
    if key in RESOURCE_FIELDS.get(extension, set()):
        return True
    if extension == '.hprefab' and 'nested' in parents and key == 'path':
        return True
    if key == 'value' and isinstance(parent, dict) and (parent.get('kind') in ('resource', 'Resource') or parent.get('type') == 'resource'):
        return True
    # Typed ScriptFieldValue's stable resource reference representation.
    if key == 'id' and isinstance(parent, dict) and parent.get('kind') in ('resource', 'Resource'):
        return True
    if extension == '.json' and key in ('StartupScene', 'MainScene', 'Scene', 'IconPath', 'DefaultScene', 'Icon'):
        return True
    if extension == '.json' and 'Cook' in parents and key in ('RootFiles', 'IncludeDirectories'):
        return True
    if extension == '.json' and 'ResourceWarmup' in parents and 'Roots' in parents and key == 'Path':
        return True
    return False


def rewrite_reference(value, source, target, owner):
    canonical = value.replace('\\', '/')
    if canonical.casefold().startswith('assets/'):
        return relocate(value, source, target)
    # Built-in resource IDs (white, None, etc.) and external URLs are not paths.
    if ':' in canonical or canonical.startswith(('/', '#')) or not ('/' in canonical or Path(canonical).suffix):
        return value
    resolved = posixpath.normpath(posixpath.join(posixpath.dirname(owner), canonical))
    destination = relocate(resolved, source, target)
    new_owner = relocate(owner, source, target)
    if destination == resolved and new_owner == owner:
        return value
    if not resolved.casefold().startswith('assets/'):
        raise Failure('relative_reference_escape', owner + ': relative resource escapes Assets: ' + value)
    return posixpath.relpath(destination, posixpath.dirname(new_owner))


def rewrite(value, extension, source, target, owner, parents=(), key='', reference=False, locations=None):
    locations = locations if locations is not None else []
    if isinstance(value, str):
        if key in EMBEDDED_JSON and value.strip().startswith(('{', '[')):
            nested = json.loads(value)
            changed = rewrite(nested, extension, source, target, owner, parents, locations=locations)
            return json.dumps(changed, ensure_ascii=False, separators=(',', ':')) if changed != nested else value
        changed = rewrite_reference(value, source, target, owner) if reference else relocate(value, source, target)
        if changed == value or key in DISPLAY_FIELDS:
            return value
        location = '/' + '/'.join(parents + (key,))
        if not reference:
            raise Failure('unclassified_reference', f'{owner}{location}: reference requires an explicit format adapter')
        locations.append(location)
        return changed
    if isinstance(value, list):
        return [rewrite(item, extension, source, target, owner, parents + (key,), str(i), reference, locations)
                for i, item in enumerate(value)]
    if isinstance(value, dict):
        return {child: rewrite(item, extension, source, target, owner, parents + ((key,) if key else ()), child,
                               is_reference(extension, parents + ((key,) if key else ()), child, value), locations)
                for child, item in value.items()}
    return value


def inventory(root):
    result = []
    for folder in ('Assets', 'Config', 'Scripts', 'Shaders'):
        directory = root / folder
        if not directory.exists():
            continue
        for path in sorted(directory.rglob('*')):
            if path.is_symlink():
                raise Failure('asset_symlink', 'Asset operations do not follow symbolic links: ' + str(path))
            if path.is_file() and not any(p in ('bin', 'obj', 'Prebuilt', 'CACHES') for p in path.relative_to(directory).parts):
                confined(root, path.relative_to(root).as_posix())
                result.append(path)
    return result


def rewrite_ui(text, source, target, owner):
    # Explicit RML attributes and CSS URL syntax; preserve unrelated bytes/text.
    pattern = r'''(?P<prefix>\b(?:src|href)\s*=\s*["']|\burl\(\s*["']?)(?P<path>[^"'\s<>\)]+)'''
    references = []
    def replace(match):
        old = match.group('path')
        new = rewrite_reference(old, source, target, owner)
        if old != new:
            references.append(dict(owner=owner, pointer=f'byte:{match.start("path")}'))
        return match.group('prefix') + new
    changed = re.sub(pattern, replace, text, flags=re.IGNORECASE)
    if source.casefold() in changed.replace('\\', '/').casefold():
        raise Failure('unclassified_reference', owner + ': UI reference requires an explicit syntax adapter')
    return changed, references


def plan(root, source, target):
    root = Path(root).resolve()
    source, target = normalize(source), normalize(target)
    src, dst = confined(root, source), confined(root, target)
    if source.casefold() == target.casefold() or target.casefold().startswith(source.casefold() + '/'):
        raise Failure('invalid_destination', 'Destination must differ and cannot be inside the source.')
    if not src.exists() or dst.exists() or not dst.parent.is_dir():
        raise Failure('invalid_destination', 'Source must exist, destination must be new and its parent must exist.')
    if any(p.casefold() == 'assets/imported' or p.casefold().startswith('assets/imported/') for p in (source, target)):
        raise Failure('import_owned', 'Imported immutable resources are managed by hectl content sync.')
    edits, fingerprints, references = {}, {}, []
    total = 0
    for path in inventory(root):
        relative = path.relative_to(root).as_posix()
        extension = path.suffix.lower()
        if extension not in JSON_FORMATS | TEXT_FORMATS:
            continue
        if path.stat().st_size > LIMIT:
            raise Failure('asset_size_limit', 'Authoring document exceeds 32 MiB: ' + relative)
        data = path.read_bytes()
        total += len(data)
        if total > 256 * 1024 * 1024:
            raise Failure('asset_scan_limit', 'Authoring documents exceed the 256 MiB transaction budget.')
        fingerprints[relative] = digest(data)
        text = data.decode('utf-8-sig')
        if extension in ('.rml', '.rcss'):
            changed, locations = rewrite_ui(data.decode('utf-8'), source, target, relative)
            if changed != data.decode('utf-8'):
                edits[relative] = dict(before=data.decode('utf-8'), after=changed)
                references.extend(locations)
            continue
        if extension in TEXT_FORMATS:
            if source.casefold() in text.replace('\\\\', '/').replace('\\', '/').casefold():
                raise Failure('source_reference', 'Source/text reference must be migrated explicitly before moving: ' + relative)
            continue
        try:
            value = json.loads(text)
        except ValueError as error:
            raise Failure('invalid_document', f'{relative}: {error}') from error
        locations = []
        changed = rewrite(value, extension, source, target, relative, locations=locations)
        if changed != value:
            edits[relative] = dict(before=data.decode('utf-8'), after=encode(changed).decode('utf-8'))
            references.extend(dict(owner=relative, pointer=p) for p in locations)
    catalog_path = root / '.he-assets/catalog.json'
    catalog = read_json(catalog_path) if catalog_path.exists() else dict(version=1, assets={})
    if catalog.get('version') != 1 or not isinstance(catalog.get('assets'), dict):
        raise Failure('catalog_version', 'Unsupported authoring asset identity catalog.')
    moved = [p for p in inventory(root) if p == src or src in p.parents]
    for path in moved:
        relative = path.relative_to(root).as_posix()
        catalog['assets'].setdefault(relative, str(uuid.uuid4()))
    catalog['assets'] = {relocate(k, source, target): v for k, v in catalog['assets'].items()}
    edits['.he-assets/catalog.json'] = dict(before=catalog_path.read_bytes().decode('utf-8') if catalog_path.exists() else None,
                                           after=encode(catalog).decode('utf-8'))
    source_files = {p.relative_to(root).as_posix(): dict(size=p.stat().st_size, mtime=p.stat().st_mtime_ns) for p in moved}
    return dict(version=1, root=str(root), source=source, target=target, references=references,
                fingerprints=fingerprints, sourceFiles=source_files, edits=edits)


def lock(root):
    # Reuse the kernel-backed content writer lock. Process exit releases it;
    # interrupted operations remain recoverable without deleting stale locks.
    return content_lock(root / '.he-assets/operation.lock')


def restore(root, journal):
    source, target = journal['source'], journal['target']
    src, dst = confined(root, source), confined(root, target)
    moved = dst.exists() and not src.exists()
    if src.exists() == dst.exists():
        raise Failure('recovery_conflict', 'Cannot identify the asset move state; keep the journal and inspect both paths.')
    # Validate everything before restoring anything. Never overwrite later edits.
    for relative, edit in journal['edits'].items():
        path = confined(root, relocate(relative, source, target) if moved else relative)
        current = path.read_bytes().decode('utf-8') if path.exists() else None
        if current not in (edit['before'], edit['after']):
            raise Failure('recovery_conflict', 'A document was edited after this operation: ' + relative)
    for relative, edit in journal['edits'].items():
        path = confined(root, relocate(relative, source, target) if moved else relative)
        if edit['before'] is None:
            path.unlink(missing_ok=True)
        else:
            atomic(path, edit['before'].encode('utf-8'))
    if moved:
        dst.rename(src)


def apply(transaction):
    root = Path(transaction['root']).resolve()
    journal_path = root / '.he-assets/pending.json'
    with lock(root):
        if journal_path.exists():
            raise Failure('recovery_required', 'Recover .he-assets/pending.json before another move.')
        # A new plan rescans added/deleted documents and unknown references too.
        fresh = plan(root, transaction['source'], transaction['target'])
        if fresh['fingerprints'] != transaction['fingerprints'] or fresh['sourceFiles'] != transaction['sourceFiles']:
            raise Failure('asset_conflict', 'Project inputs changed since the move preview. Plan again.')
        for relative, edit in transaction['edits'].items():
            path = confined(root, relative)
            current = path.read_bytes().decode('utf-8') if path.exists() else None
            if current != edit['before']:
                raise Failure('asset_conflict', 'Document changed since preview: ' + relative)
        atomic(journal_path, encode(transaction))
        try:
            confined(root, transaction['source']).rename(confined(root, transaction['target']))
            for relative, edit in transaction['edits'].items():
                path = confined(root, relocate(relative, transaction['source'], transaction['target']))
                atomic(path, edit['after'].encode('utf-8'))
        except Exception:
            restore(root, transaction)
            journal_path.unlink()
            raise
        journal_path.unlink()
    return dict(ok=True, source=transaction['source'], target=transaction['target'], references=transaction['references'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('operation', choices=('plan', 'move', 'recover'))
    parser.add_argument('--project', required=True)
    parser.add_argument('--source')
    parser.add_argument('--target')
    args = parser.parse_args()
    try:
        root = Path(args.project).resolve()
        if not (root / 'Project.heproject').is_file():
            raise Failure('invalid_project', 'Expected a project containing Project.heproject.')
        if args.operation == 'recover':
            with lock(root):
                journal = read_json(root / '.he-assets/pending.json')
                restore(root, journal)
                (root / '.he-assets/pending.json').unlink()
            result = dict(ok=True, recovered=True)
        else:
            transaction = plan(root, args.source, args.target)
            result = apply(transaction) if args.operation == 'move' else dict(ok=True, references=transaction['references'],
                source=transaction['source'], target=transaction['target'], affectedDocuments=len(transaction['edits']) - 1)
        print(json.dumps(result, ensure_ascii=False))
        return 0
    except (Failure, OSError, ValueError) as error:
        print(json.dumps(dict(ok=False, error=error.diagnostic if isinstance(error, Failure) else str(error)), ensure_ascii=False))
        return 3


if __name__ == '__main__':
    sys.exit(main())
