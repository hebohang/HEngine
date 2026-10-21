"""Plan the sample split from a cooked closure and explicit learning entry points.

This tool never deletes or moves files. The JSON plan is reviewable before it is
applied, and only version-controlled project assets are candidates for relocation.
"""
import argparse
import json
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--manifest', type=Path, action='append', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    project = repo / 'Project'
    tracked = subprocess.check_output(['git', 'ls-files', '-z', 'Project/Assets'], cwd=repo).decode().split('\0')
    assets = {p.removeprefix('Project/') for p in tracked if p}
    keep = {'Assets/Scenes/MainMenu.he', 'Assets/Scenes/MainGame.he', 'Assets/Scenes/MainGameCSharp.he'}
    for path in args.manifest:
        manifest = json.loads(path.read_text(encoding='utf-8-sig'))
        keep.update(f['Path'] for f in manifest['Files'] if f['Path'].startswith('Assets/'))

    text_extensions = {'.json', '.he', '.hmat', '.hmesh', '.hprefab', '.hskel', '.hanim', '.hanimctrl', '.hui', '.hbt', '.rml', '.rcss', '.css', '.py', '.cs'}
    reference = re.compile(r'Assets[/\\][^\s\"\'<>\)\(\[\]{};,]+')

    def refs(path):
        if path.suffix.lower() not in text_extensions or not path.is_file():
            return set()
        text = path.read_text(encoding='utf-8-sig', errors='replace')
        return {p.replace('\\', '/').replace('//', '/') for p in reference.findall(text)} & assets

    for path in (project / 'Scripts').rglob('*'):
        if any(p in {'bin', 'obj', '__pycache__'} for p in path.parts):
            continue
        if path.name.startswith('Survivor') or 'Survivor' in path.parts:
            keep.update(refs(path))
    keep.update(refs(project / 'Config/SurvivorRuntimeAssets.json'))
    pending = list(keep)
    while pending:
        name = pending.pop()
        found = refs(project / name) - keep
        keep.update(found)
        pending.extend(found)
    # Authorship and licenses travel with both the game and the test assets.
    keep.update(p for p in assets if Path(p).name.lower().startswith(('license', 'copyright', 'ofl-')))
    keep &= assets
    move = assets - keep
    shared = set()
    pending = list(move)
    while pending:
        found = refs(project / pending.pop()) - move - shared
        shared.update(found)
        pending.extend(found)
    plan = {'schema': 1, 'keepAssets': sorted(keep), 'moveAssets': sorted(move), 'sharedTestAssets': sorted(shared)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(plan, indent=2), encoding='utf-8')
    for key, paths in list(plan.items())[1:]:
        size = sum((project / p).stat().st_size for p in paths)
        print(f'{key}: {len(paths)} files, {size / 1048576:.1f} MiB')


if __name__ == '__main__':
    main()
