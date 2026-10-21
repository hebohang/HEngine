"""Publish only verified SDKs from this manual, public, standard-runner run."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

from contract import free_context, plan


def gh(*arguments, missing_ok=False):
    result = subprocess.run(['gh', *arguments], text=True, capture_output=True, check=False)
    if result.returncode:
        if missing_ok and 'HTTP 404' in result.stderr:
            return None
        raise RuntimeError(f'GitHub operation failed: {result.stderr.strip()}')
    return result.stdout


def api(endpoint, missing_ok=False):
    value = gh('api', endpoint, missing_ok=missing_ok)
    return None if value is None else json.loads(value)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate_downloads(root, architectures, version, commit):
    if not architectures or len(set(architectures)) != len(architectures) or any(a not in ('x64', 'arm64') for a in architectures):
        raise ValueError('Invalid selected architectures.')
    assets, receipts = [], []
    for arch in architectures:
        directory = root / f'sdk-{arch}'
        receipt = json.loads((directory / 'result.json').read_text(encoding='utf-8-sig'))
        if (receipt.get('pass') is not True or receipt.get('sourceCommit') != commit
                or receipt.get('version') != version or receipt.get('architecture') != arch):
            raise ValueError(f'{arch} receipt belongs to a different source/version or failed validation.')
        required = {'layout', 'native-architecture', 'fsr-native-dispatch', 'editor-startup', 'api-gameplay',
                    'windows-export', 'android-export', 'web-export', 'wechat-export'}
        if not required.issubset(receipt.get('passedChecks', [])):
            raise ValueError(f'{arch} is missing required validation gates.')
        manifest = json.loads((directory / 'artifacts.json').read_text(encoding='utf-8-sig'))
        expected_name = f'HEngine-{version}-Windows-{arch}.zip'
        entries = manifest.get('artifacts', [])
        if manifest.get('version') != version or len(entries) != 1 or entries[0].get('file') != expected_name:
            raise ValueError(f'{arch} archive manifest does not identify exactly one SDK.')
        path = directory / expected_name
        entry = entries[0]
        if path.stat().st_size != entry.get('bytes') or digest(path) != entry.get('sha256'):
            raise ValueError(f'{arch} downloaded archive differs from its verified build.')
        with zipfile.ZipFile(path) as bundle:
            descriptor = json.loads(bundle.read(f'{path.stem}/HEngine.distribution.json').decode('utf-8-sig'))
            if descriptor.get('Platform') != f'Windows-{arch}' or descriptor.get('EngineVersion') != version:
                raise ValueError(f'{arch} ZIP contains the wrong SDK.')
            capabilities = json.loads(bundle.read(f'{path.stem}/HEngine.Runtime.json').decode('utf-8-sig'))
            if (capabilities != receipt.get('nativeCapabilities') or capabilities.get('architecture') != arch
                    or any(not isinstance(capabilities.get(key), bool) for key in ('fsr', 'oidn', 'dx12', 'vulkan', 'fsrDx12', 'fsrVulkan'))):
                raise ValueError(f'{arch} receipt does not describe the shipped native capabilities.')
            if not capabilities['fsr'] or not capabilities['fsrDx12'] or not capabilities['dx12']:
                raise ValueError(f'{arch} release must include validated FSR DX12 support.')
            if arch == 'arm64' and (capabilities['oidn'] or capabilities['fsrVulkan']):
                raise ValueError('ARM64 release contains unsupported OIDN/FSR Vulkan capabilities.')
            embedded = json.loads(bundle.read(f'{path.stem}/Docs/Release/RELEASE_PROVENANCE.json').decode('utf-8-sig'))
            if embedded != receipt:
                raise ValueError(f'{arch} SDK provenance differs from the validated build receipt.')
        assets.append((path, entry))
        receipts.append(receipt)
    return assets, receipts


def require_tag_commit(repo, tag, commit):
    reference = api(f'repos/{repo}/git/ref/tags/{tag}', missing_ok=True)
    if reference is None:
        return False
    obj = reference['object']
    for _ in range(4):
        if obj['type'] != 'tag':
            break
        obj = api(f'repos/{repo}/git/tags/{obj["sha"]}')['object']
    if obj['type'] != 'commit' or obj['sha'] != commit:
        raise ValueError('Existing release tag points to different source; tags are never moved by CI.')
    return True


def validate_existing_release(existing, marker):
    if len(existing) > 1:
        raise ValueError('Ambiguous release identity.')
    if existing and (not existing[0]['draft'] or marker not in (existing[0].get('body') or '')):
        raise ValueError('Existing release was not created by this source; CI never overwrites published versions.')


def prepare_publication(output, repo, version, commit, assets, receipts):
    """Keep audit files in CI; the public attachment list contains only SDK ZIPs."""
    output.mkdir(parents=True, exist_ok=True)
    (output / 'RELEASE_PROVENANCE.json').write_text(json.dumps(
        {'version': version, 'sourceCommit': commit, 'validation': receipts}, indent=2) + '\n', encoding='utf-8')
    (output / 'artifacts.json').write_text(json.dumps(
        {'version': version, 'artifacts': [entry for _, entry in assets]}, indent=2) + '\n', encoding='utf-8')
    files = [path for path, _ in assets]
    (output / 'SHA256SUMS.txt').write_text(''.join(f'{digest(path)}  {path.name}\n' for path in files), encoding='utf-8')
    rows = '\n'.join(f'| `{path.name}` | {entry["bytes"]:,} | `{entry["sha256"]}` |' for path, entry in assets)
    details = '\n\n'.join(
        f'### Windows {r["architecture"]}\n\n'
        f'Workflow: {r.get("workflowRun", "not recorded")}\n\n'
        f'FSR DX12: {r["nativeCapabilities"]["fsrDx12"]}; '
        f'FSR Vulkan: {r["nativeCapabilities"]["fsrVulkan"]}; OIDN: {r["nativeCapabilities"]["oidn"]}.\n\n'
        'Passed checks: ' + ', '.join(r['passedChecks']) + '.\n\n'
        'Limitations:\n' + '\n'.join(f'- {limit}' for limit in r.get('limitations', [])) for r in receipts)
    (output / 'VALIDATION.md').write_text(details + '\n', encoding='utf-8')
    notes = output / 'RELEASE_NOTES.md'
    notes.write_text(
        f'<!-- HEngine release source: {commit} -->\n\nHEngine {version} Windows SDK\n\n'
        f'Source: [`{commit}`](https://github.com/{repo}/commit/{commit}).\n\n'
        '| SDK | Bytes | SHA-256 |\n| --- | ---: | --- |\n' + rows + '\n\n'
        'Download the SDK ZIP matching your Windows architecture. GitHub-generated Source code archives '
        'are optional for engine contributors; binary SDK users do not need them.\n\n'
        'Full per-SDK provenance and exact executed commands are included at '
        '`Docs/Release/RELEASE_PROVENANCE.json` inside each SDK ZIP.\n\n'
        f'[Build and validation procedure](https://github.com/{repo}/blob/{commit}/Docs/WindowsActionsRelease.md).\n\n'
        + details + '\n\nBuilt on standard GitHub-hosted runners in a public repository. '
        'Cloud validation does not establish physical GPU or mobile-device compatibility.\n', encoding='utf-8')
    return files, notes


def main():
    root = Path(__file__).resolve().parents[3]
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    free_context(event, os.environ)
    repo = os.environ['GITHUB_REPOSITORY']
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Invalid repository identity.')
    if api(f'repos/{repo}').get('private') is not False:
        raise ValueError('Repository is no longer public; refusing publication.')
    commit = os.environ['GITHUB_SHA']
    if not re.fullmatch(r'[0-9a-f]{40}', commit):
        raise ValueError('Expected immutable source commit.')
    version = os.environ['HE_RELEASE_VERSION']
    if plan(root, 'both')['version'] != version:
        raise ValueError('Version differs from checked-out engine source.')
    architectures = json.loads(os.environ['HE_ARCHITECTURES'])
    work = root / 'out/agent/release-ci'
    assets, receipts = validate_downloads(work / 'downloads', architectures, version, commit)
    tag = f'v{version}'
    marker = f'<!-- HEngine release source: {commit} -->'
    releases = json.loads(gh('api', '--paginate', '--slurp', f'repos/{repo}/releases'))
    existing = [r for page in releases for r in page if r['tag_name'] == tag]
    validate_existing_release(existing, marker)
    require_tag_commit(repo, tag, commit)
    output = work / 'publication'
    files, notes = prepare_publication(output, repo, version, commit, assets, receipts)
    if not existing:
        gh('release', 'create', tag, '--repo', repo, '--target', commit, '--draft',
           '--title', f'HEngine {version}', '--notes-file', str(notes))
    current = [r for page in json.loads(gh('api', '--paginate', '--slurp', f'repos/{repo}/releases'))
               for r in page if r['tag_name'] == tag]
    if len(current) != 1 or not current[0]['draft'] or marker not in (current[0].get('body') or ''):
        raise ValueError('Draft changed before upload.')
    release_id = current[0]['id']
    for path in files:
        remote = next((a for a in current[0]['assets'] if a['name'] == path.name), None)
        if remote:
            if remote['state'] != 'uploaded' or remote['size'] != path.stat().st_size or remote.get('digest') != 'sha256:' + digest(path):
                raise ValueError(f'Conflicting existing draft asset: {path.name}; no clobber allowed.')
        else:
            gh('release', 'upload', tag, str(path), '--repo', repo)
    remote = api(f'repos/{repo}/releases/{release_id}')
    if not remote['draft'] or marker not in (remote.get('body') or ''):
        raise ValueError('Draft changed before publication.')
    if {a['name'] for a in remote['assets']} != {p.name for p in files}:
        raise ValueError('Release asset set differs from selected architectures.')
    for path in files:
        item = next(a for a in remote['assets'] if a['name'] == path.name)
        if item['state'] != 'uploaded' or item['size'] != path.stat().st_size or item.get('digest') != 'sha256:' + digest(path):
            raise ValueError(f'Server asset checksum mismatch: {path.name}')
    require_tag_commit(repo, tag, commit)
    gh('release', 'edit', tag, '--repo', repo, '--notes-file', str(notes), '--draft=false', '--latest')
    result = api(f'repos/{repo}/releases/{release_id}')
    if result['draft']:
        raise ValueError('Release was not published.')
    if (result.get('body') or '').strip() != notes.read_text(encoding='utf-8').strip():
        raise ValueError('Published release notes do not match the verified checksums and validation summary.')
    if not require_tag_commit(repo, tag, commit):
        raise ValueError('Published release has no source tag.')
    print(result['html_url'])


if __name__ == '__main__':
    main()
