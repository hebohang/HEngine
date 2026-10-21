"""Plan removal of untracked benchmark experiments; never delete files here.

Keep every tracked file and references from retained project assets/config/scripts.
Conservatively follow text references, including mesh buffer URIs, before planning.
The PowerShell entry point verifies hashes and path boundaries before deletion.
"""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

TEXT = {'.he', '.hmesh', '.hmat', '.hprefab', '.hanimctrl', '.hmodel', '.json',
        '.py', '.cs', '.cpp', '.h', '.ini', '.yaml', '.yml'}

def digest(path):
    sha = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            sha.update(block)
    return sha.hexdigest()

def references(path, repo):
    if path.suffix.lower() not in TEXT:
        return set()
    try:
        text = path.read_text(encoding='utf-8-sig')
    except UnicodeDecodeError:
        # Legacy ImGui layout labels contain mixed code-page bytes. Latin-1
        # preserves every byte and the ASCII benchmark resource paths verbatim.
        if path.suffix.lower() != '.ini': raise
        text = path.read_text(encoding='latin-1')
    # Decode JSON escapes before scanning; script literals may use forward slashes.
    try:
        value = json.loads(text)
        def strings(value):
            if isinstance(value, str): yield value
            elif isinstance(value, dict):
                for item in value.values(): yield from strings(item)
            elif isinstance(value, list):
                for item in value: yield from strings(item)
        chunks = list(strings(value))
    except ValueError:
        chunks = [text]
    result = set()
    for chunk in chunks:
        for name in re.findall(r'(?:Assets|Scripts)/[^\s"\'<>\x00]+', chunk.replace('\\', '/')):
            candidate = repo / 'Project' / name
            if candidate.is_file(): result.add(candidate.resolve())
        # Buffer references may be relative to the material/mesh rather than Project.
        if len(chunk) < 1024 and pathlib.PurePosixPath(chunk).suffix.lower() in {'.bin', '.hmesh', '.hmat', '.dds', '.png', '.he', '.json'}:
            for root in (path.parent, repo / 'Project'):
                try:
                    candidate = root / chunk
                    if candidate.is_file(): result.add(candidate.resolve())
                except OSError:
                    pass
    return result

def plan(repo):
    base = (repo / 'Project/Assets/RenderingBenchmarks').resolve()
    tracked = {repo / p for p in subprocess.check_output(
        ['git', 'ls-files', '-z'], cwd=repo).decode('utf-8').split('\0') if p}
    tracked = {p.resolve() for p in tracked}
    files = {p.resolve() for p in base.rglob('*') if p.is_file()}
    retained = files & tracked
    roots = set(retained)
    for directory in ('Assets', 'Config', 'Scripts', 'Source'):
        for path in (repo / 'Project' / directory).rglob('*'):
            if path.is_file() and base not in path.resolve().parents and path.suffix.lower() in TEXT:
                roots.add(path.resolve())
    for path in files:
        if any((parent / '.he-keep').exists() for parent in (path.parent, *path.parents)) or pathlib.Path(str(path) + '.he-keep').exists():
            retained.add(path); roots.add(path)
    seen = set()
    queue = list(roots)
    errors = []
    while queue:
        path = queue.pop()
        if path in seen: continue
        seen.add(path)
        try:
            refs = references(path, repo)
        except (OSError, UnicodeError) as error:
            errors.append({'path': str(path), 'error': str(error)})
            continue
        retained.update(refs & files)
        queue.extend(refs - seen)
    if errors:
        raise RuntimeError('Reference scan incomplete: ' + json.dumps(errors[:8]))
    def record(path):
        return {'path': path.relative_to(repo).as_posix(), 'bytes': path.stat().st_size, 'sha256': digest(path)}
    candidates = [record(p) for p in sorted(files - retained)]
    return {'schema': 1, 'root': str(base), 'scannedTextRoots': len(roots),
            'candidates': candidates, 'candidateBytes': sum(p['bytes'] for p in candidates),
            'retained': [record(p) for p in sorted(retained)],
            'protectedUntracked': [p.relative_to(repo).as_posix() for p in sorted(retained - tracked)],
            'scope': 'Only Project/Assets/RenderingBenchmarks; preserve all Git-tracked files and project references. SourceAssets untouched.'}

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[2]
    report = plan(repo)
    target = pathlib.Path(args.output)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps({'candidates': len(report['candidates']), 'bytes': report['candidateBytes'],
                      'retained': len(report['retained']), 'protectedUntracked': len(report['protectedUntracked']), 'report': str(target)}))
