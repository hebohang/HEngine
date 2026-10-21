"""Owned export publication and recovery, using hectl's shared retention engine."""
import importlib.util
import hashlib
import json
from pathlib import Path
import sys
import uuid

spec = importlib.util.spec_from_file_location('he_storage', Path(__file__).resolve().parents[1] / 'Agent/storage.py')
storage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(storage)


def journal_path(output):
    return output.with_name('.' + output.name + '.publish.json')


def keep_failed_staging(manager, project, staging, lock):
    if not staging.exists():
        return
    try:
        if not any(str(staging) in r['paths'] for r in manager.load()[0]):
            ident = manager.start(str(project), 'wechat-export-staging', [staging], locks=[lock], kind='wechat-staging')
            manager.finish(ident, 'failed')
    except (OSError, ValueError) as error:
        print('storage: failed export staging retained without retirement: ' + str(error), file=sys.stderr)


def verify_staging(manager, staging, output, project):
    storage.safe(staging, manager.roots)
    marker = storage.read(staging / '.he-export.json')
    if marker['project'] != project or marker['output'] != str(output):
        raise ValueError('Export recovery staging ownership mismatch')
    errors = []
    list(storage.tree(staging, errors))
    if errors:
        raise ValueError('Unsafe export recovery staging')
    manifest = storage.read(staging / 'resource-manifest.json')
    for name, entry in manifest['files'].items():
        file = storage.safe(staging / name, manager.roots)
        if not storage.inside(file, staging):
            raise ValueError('Export recovery manifest escape')
        digest = hashlib.sha256()
        with file.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
        if digest.hexdigest() != entry['sha256'] or file.stat().st_size != entry['bytes']:
            raise ValueError('Export recovery staging hash mismatch')


def recover(manager, output):
    journal = journal_path(output)
    storage.safe(journal, manager.roots)
    if not journal.exists():
        return
    state = storage.read(journal)
    if state['output'] != str(output):
        raise ValueError('Export recovery ownership mismatch')
    backup = storage.safe(state['backup'], manager.roots)
    if backup.parent != output.parent or not backup.name.startswith(output.name + '.previous-'):
        raise ValueError('Export recovery backup boundary mismatch')
    if not output.exists() and backup.exists():
        # This exact rename was journalled while holding the export lock, including legacy outputs.
        backup.rename(output)
    if not output.exists() and state.get('staging'):
        staging = storage.safe(state['staging'], manager.roots)
        if staging.parent != output.parent or not staging.name.startswith(output.name + '.staging-'):
            raise ValueError('Export recovery staging boundary mismatch')
        verify_staging(manager, staging, output, state['project'])
        staging.rename(output)
    if not output.exists():
        raise ValueError('Export recovery has no current output; preserve journal and staging')
    journal.unlink()
    if state.get('staging'):
        staging = storage.safe(state['staging'], manager.roots)
        if staging.parent == output.parent and staging.name.startswith(output.name + '.staging-'):
            keep_failed_staging(manager, state['project'], staging, manager.root / 'out/agent/.wechat-export.lock')


def publish(manager, project, staging, output, lock):
    for path in (staging, output):
        storage.safe(path, manager.roots)
    marker = dict(schema=1, project=str(project), output=str(output), exportId=uuid.uuid4().hex)
    storage.atomic(staging / '.he-export.json', marker)
    backup = output.with_name(output.name + '.previous-' + uuid.uuid4().hex)
    storage.atomic(journal_path(output), dict(schema=1, project=str(project), output=str(output), backup=str(backup), staging=str(staging)))
    if output.exists():
        output.rename(backup)
    try:
        staging.rename(output)
    except BaseException:
        if backup.exists() and not output.exists():
            backup.rename(output)
        if output.exists():
            journal_path(output).unlink()
            keep_failed_staging(manager, project, staging, lock)
        raise
    journal_path(output).unlink()
    # Legacy backups have no provenance: preserve them. New backups are registered only
    # after the replacement is live and rollback no longer needs them.
    if (backup / '.he-export.json').is_file():
        old = storage.read(backup / '.he-export.json')
        if old['project'] == str(project) and old['output'] == str(output):
            try:
                ident = manager.start(str(project), 'wechat-export:' + str(output), [backup], locks=[lock], kind='wechat-backup')
                manager.finish(ident, 'success')
            except (OSError, ValueError) as error:
                print('storage: export backup registration deferred: ' + str(error), file=sys.stderr)


def prune(manager, project):
    try:
        manager.collect(kind='wechat-backup', project=str(project))
        manager.collect(kind='wechat-staging', project=str(project))
    except (OSError, ValueError) as error:
        print('storage: export cleanup deferred: ' + str(error), file=sys.stderr)
