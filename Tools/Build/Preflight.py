"""Read-only export diagnostics for authored resources; cooker/IL audits remain authoritative."""
import argparse
import json
import posixpath
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Content'))
from asset_operations import EMBEDDED_JSON, JSON_FORMATS, is_reference

CAPABILITIES = json.loads(Path(__file__).with_name('Capabilities.json').read_text(encoding='utf-8-sig'))

def inspect(project, platform, profile=None, engine=None, precompiled_player=False):
    project = Path(project).resolve()
    engine = Path(engine or Path(__file__).resolve().parents[2]).resolve()
    profile = profile or {}
    diagnostics, visited, pending = [], set(), []

    def report(code, source, pointer, message, remedy, severity='error'):
        diagnostics.append(dict(severity=severity, code=code, source=source, pointer=pointer,
                                message=message, remedy=remedy))

    def resolve(name, source='', pointer='', owner=''):
        name = name.replace('\\', '/')
        if not name or name in ('None', 'none', 'white', 'black', 'normal'): return None
        if ':' in name or name.startswith('/'):
            report('invalid_path', source, pointer, name, 'Use a project-relative or engine-content-relative resource path.')
            return None
        if owner and not name.startswith(('Assets/', 'Shaders/', 'Config/')):
            name = posixpath.normpath(posixpath.join(posixpath.dirname(owner), name))
        if '..' in name.split('/'):
            report('path_escape', source, pointer, name, 'Keep dependencies within the project or engine content root.')
            return None
        for root in (project, engine / 'Engine/Content'):
            path = (root / name).resolve()
            if not path.is_relative_to(root):
                report('path_escape', source, pointer, name, 'Remove an escaping link or path.')
                return None
            if path.is_file(): return path, name
        report('missing_resource', source, pointer, name, 'Restore the resource, reimport it, or update this reference.')
        return None

    def read(path, source):
        try:
            if path.stat().st_size > 32 * 1024 * 1024: raise ValueError('JSON exceeds 32 MiB')
            return json.loads(path.read_text(encoding='utf-8-sig'))
        except (ValueError, OSError) as error:
            report('invalid_document', source, '', str(error), 'Repair the document before export.')
            return None

    descriptor = read(project / 'Project.heproject', 'Project.heproject')
    if not isinstance(descriptor, dict): return dict(schema=1, success=False, diagnostics=diagnostics, resources=0)
    if precompiled_player and descriptor.get('RuntimeMode') != 'Scene':
        report('native_project_requires_source', 'Project.heproject', '/RuntimeMode',
               'The standard precompiled player does not execute project C++ code.',
               'Use the source build, or explicitly adopt RuntimeMode=Scene after moving gameplay into supported scripts.')
    if platform not in ('Windows', 'Web', 'WeChat', 'Android', 'macOS'):
        report('unsupported_platform', 'Config/BuildProfiles.json', '/platform', platform, 'Select an installed export backend.')
    requirements = profile.get('requiredFeatures', [])
    if not isinstance(requirements, list) or any(not isinstance(item, str) for item in requirements):
        report('invalid_capabilities', 'Config/BuildProfiles.json', '/requiredFeatures',
               'requiredFeatures must be an array of feature identifiers.', 'Query hectl capabilities -Json.')
        requirements = []
    for feature in requirements:
        support = CAPABILITIES['features'].get(feature, {}).get('support', {}).get(platform, 'unclassified')
        if support not in ('supported', 'adapter'):
            report('unsupported_feature', 'Config/BuildProfiles.json', '/requiredFeatures',
                   f'{feature}: {support} on {platform}.', 'Use a supported feature or provide a matching custom runtime.')
        elif support == 'adapter':
            report('adapter_validation_required', 'Config/BuildProfiles.json', '/requiredFeatures',
                   f'{feature} requires a project adapter; the catalog does not certify it.',
                   'Run the complete platform export and exercise the actual game on the target.', 'info')
    if (project / '.he-assets/pending.json').exists():
        report('asset_recovery_required', '.he-assets/pending.json', '', 'An asset transaction is incomplete.', 'Run hectl asset recover before export.')
    platform_config = {}
    if platform == 'Android':
        android_name = profile.get('androidConfig', 'Config/Android.json')
        android_path = resolve(android_name, 'Config/BuildProfiles.json', '/androidConfig')
        platform_config = (read(*android_path) if android_path else {}) or {}
    game_name = profile.get('gameConfig', platform_config.get('gameConfig', descriptor.get('GameConfig', 'Config/Game.json')))
    game_path = resolve(game_name, 'Project.heproject', '/GameConfig') if (project / game_name).exists() or not profile.get('contentExporter') else None
    game = read(*game_path) if game_path else {}
    if isinstance(game, dict):
        roots = [game.get('Game', {}).get('DefaultScene', '')] + game.get('Cook', {}).get('RootFiles', [])
        for name in roots:
            found = resolve(name, game_name, '/Game/DefaultScene')
            if found: pending.append(found)
    plugin_name = profile.get('pluginConfig', platform_config.get('pluginConfig', 'Config/Plugins.json'))
    plugin_path = resolve(plugin_name, 'Config/BuildProfiles.json', '/pluginConfig') if (project / plugin_name).exists() else None
    plugins = read(*plugin_path) if plugin_path else {}
    shared = project / 'Config/SharedContent.json'
    fallbacks = {}
    if shared.exists():
        contract = read(shared, 'Config/SharedContent.json')
        if isinstance(contract, dict): fallbacks = contract.get('portableComponentFallbacks', {})
    portable_unsupported = set(CAPABILITIES['portableNativeComponents'])

    def walk(value, extension, source, parents=(), pointer='', inherited=False):
        if isinstance(value, dict):
            for key, child in value.items():
                location = pointer + '/' + key.replace('~', '~0').replace('/', '~1')
                if precompiled_player and key == 'NativeScriptComponent':
                    report('native_script_requires_source', source, location,
                           'The standard player has no project-defined native script registrations.',
                           'Use a supported C# script or build a custom native player from source.')
                if platform in ('Web', 'WeChat') and key in portable_unsupported and not fallbacks.get(key):
                    report('unsupported_component', source, location, f'{key} has no native {platform} implementation.',
                           'Use a supported component or declare and implement a SharedContent portable fallback.')
                if key == 'ScriptComponent' and isinstance(child, dict) and platform in ('Windows', 'Android', 'macOS'):
                    backend = child.get('Backend', '')
                    if not plugins.get(backend, {}).get('Game', False):
                        report('script_plugin_disabled', source, location + '/Backend', backend,
                               'Enable this script plugin for Game in the selected Plugins.json.')
                if key in EMBEDDED_JSON and isinstance(child, str) and child:
                    try: walk(json.loads(child), extension, source, parents + (key,), location)
                    except ValueError as error: report('embedded_json', source, location, str(error), 'Repair the serialized field data.')
                elif isinstance(child, str) and is_reference(extension, parents, key, value):
                    found = resolve(child, source, location, source)
                    if found: pending.append(found)
                else: walk(child, extension, source, parents + (key,), location, is_reference(extension, parents, key, value))
        elif isinstance(value, list):
            for index, child in enumerate(value):
                if inherited and isinstance(child, str):
                    found = resolve(child, source, pointer + '/' + str(index), source)
                    if found: pending.append(found)
                else: walk(child, extension, source, parents, pointer + '/' + str(index), inherited)

    while pending:
        path, name = pending.pop()
        if path in visited: continue
        visited.add(path)
        if len(visited) > 20000:
            report('resource_budget', name, '', 'Dependency graph exceeds 20000 files.', 'Split the export into bounded content groups.')
            break
        extension = path.suffix.lower()
        if extension in JSON_FORMATS:
            document = read(path, name)
            if extension == '.hui' and isinstance(document, dict):
                for field, suffix in (('GeneratedRmlPath', '.rml'), ('GeneratedRcssPath', '.rcss')):
                    expected = str(Path(name).with_suffix(suffix)).replace('\\', '/')
                    if document.get(field, '').replace('\\', '/').casefold() != expected.casefold():
                        report('ui_generated_path', name, '/' + field, 'UI generated sibling path is missing or mismatched.',
                               'Save/export this UI document in the UI editor before publishing.')
            if document is not None: walk(document, extension, name)
        elif extension in ('.rml', '.rcss'):
            if path.stat().st_size > 4 * 1024 * 1024:
                report('ui_budget', name, '', 'UI source exceeds 4 MiB.', 'Split the UI document.')
                continue
            text = path.read_text(encoding='utf-8-sig')
            for match in re.finditer(r'''(?:src|href)\s*=\s*["']([^"']+)["']|url\(\s*["']?([^)'"\s]+)''', text):
                reference = match.group(1) or match.group(2)
                if reference.startswith('#'): continue
                found = resolve(reference, name, f'offset:{match.start()}', name)
                if found: pending.append(found)
    if profile.get('contentExporter'):
        report('custom_exporter', 'Config/BuildProfiles.json', '/contentExporter',
               'Custom exporter output is checked by the platform export and managed IL audit stages.',
               'Run the complete publish check/build; source graph inspection is not device validation.', 'info')
    return dict(schema=1, platform=platform, success=not any(d['severity'] == 'error' for d in diagnostics),
                resources=len(visited), diagnostics=diagnostics)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--project', required=True)
    parser.add_argument('--profile', required=True)
    parser.add_argument('--output')
    parser.add_argument('--precompiled-player', action='store_true')
    args = parser.parse_args()
    project = Path(args.project).resolve()
    try:
        config = json.loads((project / 'Config/BuildProfiles.json').read_text(encoding='utf-8-sig'))
        profiles = [p for p in config['profiles'] if p['id'] == args.profile]
        if len(profiles) != 1: raise ValueError('Profile must exist and be unique.')
        result = inspect(project, profiles[0]['platform'], profiles[0], precompiled_player=args.precompiled_player)
    except (OSError, ValueError, KeyError, TypeError) as error:
        result = dict(schema=1, success=False, diagnostics=[dict(severity='error', code='invalid_profile', message=str(error))])
    if args.output:
        output = Path(args.output); output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result['success'] else 3


if __name__ == '__main__': sys.exit(main())
