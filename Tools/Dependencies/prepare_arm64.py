"""Acquire pinned, free Windows ARM64 dependencies without editing vendor sources."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import sys
import urllib.request
from urllib.parse import urlparse
import zipfile


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def safe_name(name):
    name = name.replace('\\', '/')
    if not name or name.startswith('/') or ':' in name or '..' in name.split('/'):
        raise ValueError(f'Unsafe dependency member: {name}')
    return name


def acquire(package, root):
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,31}', package.get('name', '')):
        raise ValueError('Invalid dependency name.')
    url = urlparse(package.get('url', ''))
    if url.scheme != 'https' or url.hostname not in {'api.nuget.org', 'github.com', 'raw.githubusercontent.com'}:
        raise ValueError('Dependency must use an approved public HTTPS source.')
    if package.get('type') not in {'git', 'zip', 'file'}:
        raise ValueError('Unsupported dependency format.')
    if package['type'] == 'git':
        if not re.fullmatch(r'[0-9a-f]{40}', package.get('commit', '')):
            raise ValueError('Source dependency requires an immutable commit.')
        destination = root / (package['name'] + '-source')
        if not destination.exists():
            subprocess.run(['git', 'init', str(destination)], check=True, stdout=sys.stderr)
            subprocess.run(['git', '-C', str(destination), 'fetch', '--depth', '1', package['url'], package['commit']], check=True, stdout=sys.stderr)
            subprocess.run(['git', '-C', str(destination), 'checkout', '--detach', package['commit']], check=True, stdout=sys.stderr)
        actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', '-C', str(destination), 'status', '--porcelain'], text=True).strip()
        if actual != package['commit'] or dirty:
            raise ValueError(f'Dependency checkout differs from pin: {destination}')
        return destination
    suffix = '.zip' if package['type'] == 'zip' else '.def'
    if not re.fullmatch(r'[0-9a-f]{64}', package.get('sha256', '')):
        raise ValueError('Binary dependency requires a SHA256 pin.')
    archive = root / 'downloads' / (package['name'] + suffix)
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        partial = archive.with_suffix(archive.suffix + '.partial')
        request = urllib.request.Request(package['url'], headers={'User-Agent': 'HEngine-pinned-dependencies'})
        with urllib.request.urlopen(request, timeout=90) as source, partial.open('wb') as target:
            while block := source.read(1024 * 1024):
                target.write(block)
        if sha(partial) != package['sha256']:
            raise ValueError(f'Download checksum differs from pin: {package["name"]}')
        partial.rename(archive)
    if sha(archive) != package['sha256']:
        raise ValueError(f'Cached dependency checksum differs from pin: {archive}')
    if package['type'] == 'file':
        return archive
    destination = root / (package['name'] + '-' + package['sha256'][:12])
    inventory = destination / '.verified-files.json'
    if destination.exists():
        manifest = json.loads(inventory.read_text())
        if manifest['archiveSha256'] != package['sha256']:
            raise ValueError('Extracted dependency archive identity changed.')
        for name, expected in manifest['files'].items():
            path = destination / safe_name(name)
            if not path.resolve().is_relative_to(destination.resolve()) or sha(path) != expected:
                raise ValueError(f'Extracted dependency changed: {path}')
    else:
        destination.mkdir(parents=True)
        hashes = {}
        with zipfile.ZipFile(archive) as bundle:
            for entry in bundle.infolist():
                name = safe_name(entry.filename)
                if package.get('includePrefixes') and not any(name.startswith(prefix) for prefix in package['includePrefixes']):
                    continue
                if stat.S_ISLNK(entry.external_attr >> 16):
                    raise ValueError('Dependency archive contains a symbolic link.')
                target = destination / name
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                target.parent.mkdir(parents=True, exist_ok=True)
                with bundle.open(entry) as source, target.open('wb') as output:
                    while block := source.read(1024 * 1024):
                        output.write(block)
                hashes[name] = sha(target)
        inventory.write_text(json.dumps({'archiveSha256': package['sha256'], 'files': hashes}))
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True, type=Path)
    args = parser.parse_args()
    root = args.root.resolve()
    lock = json.loads(Path(__file__).with_name('windows-arm64.lock.json').read_text(encoding='utf-8-sig'))
    if lock['schema'] != 1 or lock['architecture'] != 'arm64':
        raise ValueError('Unsupported ARM64 dependency lock.')
    paths = {entry['name']: str(acquire(entry, root)) for entry in lock['packages']}
    (root / 'prepared.json').write_text(json.dumps(paths, indent=2))
    print(f'PASS: {len(paths)} pinned ARM64 dependency inputs; {root / "prepared.json"}')


if __name__ == '__main__':
    main()
