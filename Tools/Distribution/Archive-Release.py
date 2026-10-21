"""Create clean, verified release archives without deleting local QA evidence."""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import uuid
import zipfile


def archive(source, output, label):
    source = source.resolve(strict=True)
    destination = output / (label + '.zip')
    if destination.exists():
        raise FileExistsError(destination)
    # SDKs and npm tools have executable bin/ directories. The assembled input
    # already excludes managed build output; never strip installed tools here.
    excluded = {'out', 'Saved', 'Dist', '.git', 'obj', '__pycache__', '.he-content'}
    count = 0
    staging = destination.with_name(destination.name + '.partial-' + uuid.uuid4().hex)
    with zipfile.ZipFile(staging, 'x', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as bundle:
        for directory, directories, files in os.walk(source, topdown=True, followlinks=False):
            directories[:] = sorted(name for name in directories if name not in excluded)
            for name in directories:
                path = Path(directory) / name
                if path.is_symlink() or getattr(path.lstat(), 'st_file_attributes', 0) & 0x400:
                    raise ValueError('Release input contains a linked directory: ' + str(path))
            for name in sorted(files):
                path = Path(directory) / name
                relative = path.relative_to(source)
                if path.is_symlink():
                    raise ValueError('Release input contains a symlink: ' + str(path))
                if path.suffix.lower() in {'.pdb', '.ilk', '.pyc', '.log', '.lock'}:
                    continue
                if path.name.startswith('HEngine.workspace.local.json') or relative.as_posix() == 'Config/EditorLayout.ini':
                    continue
                bundle.write(path, label + '/' + relative.as_posix())
                count += 1
    with zipfile.ZipFile(staging) as bundle:
        bad = bundle.testzip()
        if bad:
            raise ValueError('Archive CRC failed: ' + bad)
    with staging.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    staging.rename(destination)
    print(f'PASS: {destination.name}; files={count}; bytes={destination.stat().st_size}', flush=True)
    return {'file': destination.name, 'bytes': destination.stat().st_size, 'files': count, 'sha256': digest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--release', type=Path, required=True)
    parser.add_argument('--game', type=Path)
    parser.add_argument('--engine-only', action='store_true', help='Archive the binary SDK without any game project or sample.')
    parser.add_argument('--version', default='1.1.0')
    parser.add_argument('--architecture', choices=('x64', 'arm64'), default='x64')
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+', args.version):
        parser.error('--version must be a numeric major.minor.patch version')
    release = args.release.resolve(strict=True)
    if not args.engine_only and args.game is None:
        parser.error('--game is required unless --engine-only is selected')
    sdk_label = f'HEngine-{args.version}-Windows-{args.architecture}'
    targets = [(release / sdk_label, sdk_label)]
    if not args.engine_only:
        targets += [
        (release / f'StarbloomSurvivor-{args.version}', f'StarbloomSurvivor-{args.version}'),
        (args.game / 'Dist/Windows', f'StarbloomSurvivor-{args.version}-Windows-CSharp'),
        (args.game / 'Dist/WindowsPython', f'StarbloomSurvivor-{args.version}-Windows-Python'),
        (args.game / 'Dist/Web', f'StarbloomSurvivor-{args.version}-Web-CSharp'),
        ]
    artifacts = [archive(source, release, name) for source, name in targets]
    (release / 'artifacts.json').write_text(json.dumps({'version': args.version, 'artifacts': artifacts}, indent=2) + '\n', encoding='utf-8')
    (release / 'SHA256SUMS.txt').write_text(''.join(f"{item['sha256']}  {item['file']}\n" for item in artifacts), encoding='utf-8')


if __name__ == '__main__':
    main()
