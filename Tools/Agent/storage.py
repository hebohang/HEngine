"""hectl storage: logical byte accounting and conservative, recorded-run retention.

Only registered artifacts and hectl's own retired cache units can be collected.
Windows locks use the same deny-sharing handles as the PowerShell workflows.
"""
import argparse
import contextlib
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import sys
import time
import uuid

DEFAULTS = dict(successCount=5, failureDays=7, budgetBytes=5 * 1024**3,
                wechatBackups=1, autoIntervalSeconds=300, autoEnabled=True)
ROOTS = ('out/agent', 'out/build', 'out/package', 'out/pkg', 'dist')


def absolute(path):
    return Path(os.path.abspath(str(path)))


def filesystem_path(path):
    """Use Windows extended-length paths for filesystem calls, not boundary checks."""
    path = Path(path)
    if os.name == 'nt' and path.is_absolute():
        value = str(path)
        if value.startswith('\\\\?\\'):
            return path
        if value.startswith('\\\\'):
            return Path('\\\\?\\UNC\\' + value[2:])
        return Path('\\\\?\\' + value)
    return path


def inside(path, root):
    return path != root and root in path.parents


def safe(path, roots):
    path = absolute(path)
    if not any(inside(path, absolute(root)) for root in roots):
        raise ValueError('Outside managed roots or root itself: ' + str(path))
    for parent in [path] + list(path.parents):
        try:
            info = filesystem_path(parent).lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse point refused: ' + str(parent))
    return path


def read(path):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('Oversized metadata: ' + str(path))
    return json.loads(path.read_text(encoding='utf-8-sig'))


def owner_alive(pid, started):
    if os.name != 'nt' or not pid:
        return None
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.OpenProcess(0x1000, False, pid)
    if not handle:
        return None if ctypes.get_last_error() == 5 else False
    try:
        times = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in times)):
            return None
        created = times[0].dwLowDateTime | (times[0].dwHighDateTime << 32)
        return created == started and not (times[1].dwLowDateTime or times[1].dwHighDateTime)
    finally:
        kernel.CloseHandle(handle)


def atomic(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + '.' + uuid.uuid4().hex + '.tmp')
    try:
        temp.write_text(json.dumps(value, indent=2), encoding='utf-8')
        os.replace(str(temp), str(path))
    finally:
        if temp.exists():
            temp.unlink()


@contextlib.contextmanager
def directory_guard(path):
    """Prevent Windows directory rename/junction substitution while using child paths."""
    if os.name != 'nt':
        yield
        return
    from ctypes import wintypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel.CreateFileW(str(filesystem_path(path)), 0x80, 3, None, 3, 0x02000000 | 0x00200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), 'Cannot guard directory: ' + str(path))
    try:
        yield
    finally:
        kernel.CloseHandle(handle)


@contextlib.contextmanager
def exclusive(path, create=False, allow_delete=False):
    """Hold an existing workflow lock until deletion completes (never probe/release)."""
    if os.name == 'nt':
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                      ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        handle = kernel.CreateFileW(str(path), 0xC0000000, 4 if allow_delete else 0, None, 4 if create else 3, 0x80, None)
        if handle == ctypes.c_void_p(-1).value:
            raise OSError(ctypes.get_last_error(), 'Lock unavailable: ' + str(path))
        try:
            yield
        finally:
            kernel.CloseHandle(handle)
    else:
        import fcntl
        with open(path, 'a+b' if create else 'r+b') as stream:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
            yield


@contextlib.contextmanager
def mutex(root, wait_ms=0):
    # Named mutex has no filesystem side effects, including during WhatIf.
    if os.name == 'nt':
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateMutexW.argtypes = [ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR]
        kernel.CreateMutexW.restype = wintypes.HANDLE
        for name in ('WaitForSingleObject', 'ReleaseMutex', 'CloseHandle'):
            getattr(kernel, name).argtypes = ([wintypes.HANDLE, wintypes.DWORD]
                                            if name == 'WaitForSingleObject' else [wintypes.HANDLE])
        name = 'Local\\HEStorage_' + hashlib.sha256(str(root).lower().encode()).hexdigest()[:24]
        handle = kernel.CreateMutexW(None, False, name)
        if not handle:
            raise OSError('Cannot create storage mutex')
        acquired = kernel.WaitForSingleObject(handle, wait_ms) in (0, 0x80)
        try:
            if not acquired:
                raise OSError('Storage collector/registration is busy')
            yield
        finally:
            if acquired:
                kernel.ReleaseMutex(handle)
            kernel.CloseHandle(handle)
    else:
        # Non-Windows support is only for isolated contract fixtures.
        import tempfile
        lock = Path(tempfile.gettempdir()) / ('he-storage-' + hashlib.sha256(str(root).encode()).hexdigest())
        with exclusive(lock, create=True):
            yield


def tree(path, errors):
    """Never traverse a link, even when it points within the workspace."""
    try:
        info = filesystem_path(path).lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise ValueError('Reparse point refused: ' + str(path))
        if stat.S_ISDIR(info.st_mode):
            for entry in filesystem_path(path).iterdir():
                yield from tree(path / entry.name, errors)
        else:
            yield path, info.st_size
    except (OSError, ValueError) as error:
        errors.append(dict(path=str(path), error=str(error)))


def measure(path):
    errors = []
    if not path.exists():
        return 0, errors
    size = sum(size for _, size in tree(path, errors))
    return size, errors


def category(path, root):
    rel = path.relative_to(root).as_posix()
    parts = rel.split('/')
    if rel.startswith(('Project/Assets', 'Project/SourceAssets', 'Engine/Content')):
        return 'persistent-resources'
    if '.he-content' in parts:
        return 'content-state-recovery'
    if 'downloads' in parts:
        return 'download-cache'
    if any(p in ('tools', 'validation-deps') for p in parts):
        return 'toolchain'
    if 'deps' in parts:
        return 'sdk-dependencies'
    if any(p.startswith('CookInput-') or '.staging-' in p for p in parts):
        return 'package-intermediate-unknown'
    if rel.startswith('dist/') or 'wechat-export' in parts:
        return 'published-output'
    if any('.previous' in p for p in parts):
        return 'recovery-or-backup-unknown'
    if any(p in ('sessions', 'logs', 'captures', 'profiles', 'benchmarks', 'rendering-evidence', 'content') for p in parts):
        return 'historical-evidence-unknown'
    if 'codegen' in parts or any(p.startswith(('game-', 'editor-', 'tests-', 'wechat-native-', 'wechat-wasm-')) for p in parts) or rel.startswith(('out/build/', 'out/pkg/', 'out/package/build/')):
        return 'build-cache'
    return 'unknown'


class Storage:
    def __init__(self, root, policy=None):
        self.root = absolute(root)
        self.roots = [self.root / p for p in ROOTS]
        self.home = self.root / 'out/agent/storage'
        self.records = self.home / 'runs'
        self.policy = dict(DEFAULTS)
        config = self.root / 'Tools/Agent/storage-policy.json'
        if config.exists():
            self.policy.update(read(config))
        if policy:
            self.policy.update(policy)
        for key, value in self.policy.items():
            if key == 'autoEnabled' and isinstance(value, bool):
                continue
            if key not in DEFAULTS or not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise ValueError('Invalid retention setting: ' + key)
            if key == 'autoEnabled':
                raise ValueError('Invalid retention setting: ' + key)

    def record_path(self, ident):
        if len(ident) != 32 or any(c not in '0123456789abcdef' for c in ident):
            raise ValueError('Invalid record id')
        return safe(self.records / (ident + '.json'), self.roots)

    def start(self, project, workflow, paths, locks=(), parent='', kind='evidence', ident=None, project_id='', revision='', outer=False, owner_pid=0, owner_started=0, dependencies=()):
        ident = ident or uuid.uuid4().hex
        with mutex(self.root, wait_ms=30000):
            record = self.record_path(ident)
            if record.exists():
                raise ValueError('Run id already exists')
            external_paths = []
            try:
                artifacts = [str(safe(p, self.roots)) for p in paths if p]
                for artifact in artifacts:
                    self.validate_artifact(Path(artifact), kind)
                    if category(Path(artifact), self.root) in ('download-cache', 'toolchain', 'sdk-dependencies', 'build-cache', 'published-output', 'content-state-recovery'):
                        raise ValueError('Protected artifact category: ' + artifact)
            except ValueError:
                if not outer or kind != 'evidence':
                    raise
                # An external/custom output is report-only, but still needs an outer
                # lifecycle barrier so its child sessions survive subsequent analysis.
                external_paths = [str(absolute(p)) for p in paths if p]
                receipt = safe(self.root / ('out/agent/logs/workflow-guards/' + ident), self.roots)
                receipt.mkdir(parents=True)
                atomic(receipt / 'references.json', dict(paths=external_paths, policy='external/custom output preserved'))
                artifacts = [str(receipt)]
            if not artifacts:
                raise ValueError('No managed artifacts')
            if parent:
                parent_record = read(self.record_path(parent))
                if parent_record['status'] != 'running':
                    raise ValueError('Outer workflow is no longer running')
                if all(any(Path(a) == Path(p) or inside(Path(a), Path(p)) for p in parent_record['paths']) for a in artifacts):
                    return None  # The outer immutable directory already owns these nested artifacts.
            if any(a == b or inside(Path(a), Path(b)) or inside(Path(b), Path(a))
                   for i, a in enumerate(artifacts) for b in artifacts[i+1:]):
                raise ValueError('Overlapping paths within one record')
            # SDK, fixed caches, final exports and transaction material cannot be registered as evidence.
            for artifact in artifacts:
                label = category(Path(artifact), self.root)
                if label in ('download-cache', 'toolchain', 'sdk-dependencies', 'build-cache', 'published-output', 'content-state-recovery'):
                    raise ValueError('Protected artifact category: ' + artifact)
                self.validate_artifact(Path(artifact), kind)
            value = dict(schema=1, id=ident, project=project, workflow=workflow, kind=kind,
                         status='running', started=time.time(), ended=None, paths=artifacts,
                         locks=[str(safe(p, self.roots)) for p in locks], parent=parent,
                         pinned=False, recovery=False, projectId=project_id, revision=revision, externalPaths=external_paths,
                         dependencies=[str(absolute(p)) for p in dependencies if p],
                         ownerPid=owner_pid, ownerStarted=owner_started)
            atomic(record, value)
        return ident

    def bind_owner(self, ident, pid, started):
        with mutex(self.root, wait_ms=30000):
            path = self.record_path(ident)
            record = read(path)
            if record['status'] != 'running':
                raise ValueError('Cannot bind a completed run')
            record.update(ownerPid=pid, ownerStarted=started)
            atomic(path, record)

    @contextlib.contextmanager
    def workflow_scope(self, project, workflow, paths):
        """Python workflow equivalent of the PowerShell outer lifecycle bridge."""
        previous_parent = os.environ.get('HE_STORAGE_PARENT')
        ident = None
        state = dict(status='unknown')
        try:
            ident = self.start(project, workflow, paths, parent=previous_parent or '', outer=True)
            if ident:
                os.environ['HE_STORAGE_PARENT'] = ident
        except (OSError, ValueError) as error:
            print('storage: workflow registration deferred: ' + str(error), file=sys.stderr)
        try:
            yield state
        finally:
            try:
                if ident:
                    self.finish(ident, state['status'])
            except (OSError, ValueError) as error:
                print('storage: workflow completion deferred: ' + str(error), file=sys.stderr)
            finally:
                if previous_parent is None:
                    os.environ.pop('HE_STORAGE_PARENT', None)
                else:
                    os.environ['HE_STORAGE_PARENT'] = previous_parent
            if ident and not previous_parent:
                self.auto()

    def finish(self, ident, status):
        if status not in ('success', 'failed', 'unknown'):
            raise ValueError('Invalid terminal state')
        with mutex(self.root, wait_ms=30000):
            path = self.record_path(ident)
            value = read(path)
            if value['status'] in ('success', 'failed') and status == 'unknown':
                return  # Repeated stop cannot erase a previously observed terminal outcome.
            value.update(status=status, ended=time.time())
            atomic(path, value)

    def load(self):
        records, errors = [], []
        if self.records.exists():
            safe(self.records, self.roots)
            for path in self.records.glob('*.json'):
                try:
                    safe(path, self.roots)
                    value = read(path)
                    if value['schema'] != 1 or self.record_path(value['id']) != path:
                        raise ValueError('Invalid ownership record')
                    if value['status'] not in ('running', 'unknown', 'success', 'failed') or not isinstance(value['paths'], list) or not value['paths']:
                        raise ValueError('Invalid run state/artifacts')
                    for key in ('project', 'workflow', 'kind', 'parent', 'projectId'):
                        if not isinstance(value[key], str):
                            raise ValueError('Invalid ownership field: ' + key)
                    if not isinstance(value['locks'], list) or not isinstance(value['pinned'], bool) or not isinstance(value['recovery'], bool):
                        raise ValueError('Invalid protection metadata')
                    if not isinstance(value.get('dependencies', []), list) or any(not isinstance(p, str) for p in value.get('dependencies', [])):
                        raise ValueError('Invalid evidence dependencies')
                    if not isinstance(value['started'], (int, float)) or not math.isfinite(value['started']):
                        raise ValueError('Invalid start time')
                    if value['ended'] is not None and (not isinstance(value['ended'], (int, float)) or not math.isfinite(value['ended'])):
                        raise ValueError('Invalid end time')
                    for artifact in value['paths']:
                        self.validate_artifact(safe(artifact, self.roots), value['kind'])
                    records.append(value)
                except (OSError, ValueError, KeyError, TypeError) as error:
                    errors.append(dict(path=str(path), error=str(error)))
        return records, errors

    def related_locks(self, record):
        leases, internal = [], []
        for lock in record['locks']:
            path = safe(lock, self.roots)
            if path.name in ('.agent-execution.lock', '.agent-build.lock', '.package-build.lock'):
                lease = self.cache_lease(path.parent)
                if lease.exists():
                    leases.append(str(lease))
                    if not path.exists():
                        # Explicit cache retirement keeps this outside lease. The old
                        # internal lock may be gone, but new writers still cannot start.
                        continue
            internal.append(lock)
        # Match writer acquisition order, with the non-deletable cache lease first.
        return sorted(set(leases)) + sorted(set(internal))

    def plan(self, kind='', project='', now=None, held_locks=frozenset()):
        now = time.time() if now is None else now
        records, errors = self.load()
        by_id = {r['id']: r for r in records}
        # The old pairwise test is purely lexical. Index equal paths and their
        # ancestors once per snapshot; retain all filesystem safety scans below.
        artifact_owners = {}
        for record in records:
            for artifact in record['paths']:
                artifact_owners.setdefault(Path(artifact), set()).add(record['id'])
        overlapping_owners = set()
        for artifact, owners in artifact_owners.items():
            related = set(owners)
            for ancestor in artifact.parents:
                related.update(artifact_owners.get(ancestor, ()))
            if len(related) > 1:
                overlapping_owners.update(related)
        recent = set()
        groups = {}
        for r in records:
            if r['status'] == 'success':
                groups.setdefault((os.path.normcase(r['project']), r['workflow'], r['kind']), []).append(r)
        for group, runs in groups.items():
            count = self.policy['wechatBackups'] if group[2] == 'wechat-backup' else self.policy['successCount']
            if group[2] in ('package-intermediate', 'package-staging'):
                count = 0  # Verified publication makes the input copy immediately disposable.
            recent.update(r['id'] for r in sorted(runs, key=lambda r: r['ended'] or 0, reverse=True)[:int(count)])
        items = []
        for r in records:
            reason = 'expired completed evidence'
            observed_status = r['status']
            if r['status'] == 'running' and r.get('ownerPid') and owner_alive(r['ownerPid'], r.get('ownerStarted', 0)) is not True:
                observed_status = 'unknown'
            pinned = r['pinned'] or any((Path(p) / '.he-keep').exists() if Path(p).is_dir() else Path(p + '.he-keep').exists() for p in r['paths'])
            if pinned:
                reason = 'user pinned'
            elif r['recovery']:
                reason = 'recovery transaction'
            elif r['status'] not in ('success', 'failed') or not r['ended']:
                reason = 'owner exited; terminal state unknown' if observed_status == 'unknown' else 'running or unknown state'
            elif r['parent'] and (r['parent'] not in by_id or by_id[r['parent']]['status'] not in ('success', 'failed')):
                reason = 'outer workflow incomplete or unknown'
            elif r['id'] in recent:
                reason = 'recent successful evidence'
            elif r['status'] == 'failed' and now - r['ended'] < self.policy['failureDays'] * 86400:
                reason = 'failure diagnostic window'
            sizes, safety_flags = [], set()
            for artifact in r['paths']:
                scan_errors, size = [], 0
                if Path(artifact).exists():
                    for file, length in tree(Path(artifact), scan_errors):
                        size += length
                        if file.name == '.he-keep':
                            safety_flags.add('user pinned')
                        if file.name == 'pending.json' or '.journal' in file.name:
                            safety_flags.add('recovery transaction')
                sizes.append((size, scan_errors))
            if safety_flags:
                reason = 'user pinned' if 'user pinned' in safety_flags else 'recovery transaction'
                pinned = pinned or 'user pinned' in safety_flags
            scan_errors = [e for _, es in sizes for e in es]
            errors.extend(scan_errors)
            if scan_errors:
                reason = 'incomplete scan'
            # Overlap is never ownership: do not allow one record to erase another run.
            if r['id'] in overlapping_owners:
                reason = 'overlapping artifact ownership'
            if reason == 'expired completed evidence':
                try:
                    with contextlib.ExitStack() as stack:
                        for lock in self.related_locks(r):
                            if lock not in held_locks:
                                stack.enter_context(exclusive(safe(lock, self.roots)))
                        for artifact in r['paths']:
                            self.check_special(r, Path(artifact))
                except (OSError, ValueError, KeyError) as error:
                    reason = 'locked or recovery protected: ' + str(error)
            eligible = reason == 'expired completed evidence'
            items.append(dict(id=r['id'], project=r['project'], workflow=r['workflow'], kind=r['kind'],
                              paths=r['paths'], status=observed_status, started=r['started'], ended=r['ended'],
                              ownerPid=r.get('ownerPid', 0), revision=r.get('revision', ''),
                              externalPaths=r.get('externalPaths', []),
                              budgetExcluded=pinned or r['kind'] == 'wechat-backup',
                              bytes=sum(s for s, _ in sizes), disposition='remove' if eligible else 'keep', reason=reason))
        # Build lexical dependency edges once per plan snapshot. All safety scans,
        # locks and the fresh under-lock plan in collect remain unchanged.
        items_by_id = {item['id']: item for item in items}
        owners_by_path = {}
        for index, item in enumerate(items):
            for artifact in item['paths']:
                owners_by_path.setdefault(Path(artifact), set()).add(index)
        consumers_by_owner = [[] for _ in items]
        for consumer in items:
            owner_indices = set()
            for dependency in by_id[consumer['id']].get('dependencies', []):
                path = Path(dependency)
                for ancestor in (path, *path.parents):
                    owner_indices.update(owners_by_path.get(ancestor, ()))
            for index in owner_indices:
                if items[index]['id'] != consumer['id']:
                    consumers_by_owner[index].append(consumer)
        # Preserve the original item/consumer iteration and reason precedence,
        # including cycles, budget propagation and disposable package exceptions.
        for _ in range(len(items)):
            changed = False
            for index, item in enumerate(items):
                parent = items_by_id.get(by_id[item['id']]['parent'])
                if parent and parent['budgetExcluded'] and not item['budgetExcluded']:
                    item['budgetExcluded'] = True
                    changed = True
                disposable_input = item['kind'] in ('package-intermediate', 'package-staging') and item['status'] == 'success'
                if parent and parent['disposition'] == 'keep' and item['disposition'] == 'remove' and not disposable_input:
                    item.update(disposition='keep', reason='outer workflow retained: ' + parent['reason'])
                    changed = True
                for consumer in consumers_by_owner[index]:
                    if consumer['disposition'] != 'keep':
                        continue
                    if item['disposition'] == 'remove':
                        item.update(disposition='keep', reason='referenced by retained run ' + consumer['id'])
                        changed = True
                    if consumer['budgetExcluded'] and not item['budgetExcluded']:
                        item['budgetExcluded'] = True
                        changed = True
            if not changed:
                break
        for item in items:
            item['policyProtected'] = item['disposition'] == 'keep'
            if (kind and item['kind'] != kind or project and os.path.normcase(item['project']) != os.path.normcase(project)) and item['disposition'] == 'remove':
                item.update(disposition='keep', reason='outside filter')
        history = [i for i in items if not i['budgetExcluded'] and i['status'] in ('success', 'failed')]
        kept = sum(i['bytes'] for i in history if i['policyProtected'])
        return dict(schema=1, sizeKind='logical-file-bytes; physical allocation/release not measured',
                    accountingScope='registered artifacts; unknown inventory is report-only',
                    policy=self.policy, items=items, errors=errors, scanComplete=not errors,
                    reclaimableBytes=sum(i['bytes'] for i in items if i['disposition'] == 'remove'),
                    preservedBytes=sum(i['bytes'] for i in items if i['disposition'] == 'keep'),
                    historyBytes=sum(i['bytes'] for i in history), protectedOverBudgetBytes=max(0, kept-self.policy['budgetBytes']),
                    budgetAdvice='Protected evidence is never evicted. Archive/unpin explicitly or raise the budget.' if kept > self.policy['budgetBytes'] else '')

    def inventory(self):
        groups, errors, root_totals, extensions = {}, [], {}, {}
        roots = self.roots + [self.root / p for p in ('Project/Assets', 'Project/SourceAssets', 'Project/.he-content', 'Project/Saved', 'Engine/Content')]
        for root in roots:
            if not os.path.lexists(root):
                continue
            try:
                safe(root / '_boundary_check', [self.root])
            except ValueError as error:
                errors.append(dict(path=str(root), error=str(error)))
                continue
            for path, size in tree(root, errors):
                root_totals[str(root.relative_to(self.root))] = root_totals.get(str(root.relative_to(self.root)), 0) + size
                extension = path.suffix.lower() or '(none)'
                extensions[extension] = extensions.get(extension, 0) + size
                rel = path.relative_to(self.root).parts
                # Show project namespaces and target/config units separately.
                depth = 5 if len(rel) > 4 and rel[:3] == ('out', 'agent', 'projects') else 3
                for index, part in enumerate(rel[:-1]):
                    if part.startswith('CookInput-') or '.staging-' in part or part in ('downloads', 'deps', 'tools'):
                        depth = index + 1
                group = '/'.join(rel[:min(depth, len(rel)-1)])
                label = category(path, self.root)
                groups[(group, label)] = groups.get((group, label), 0) + size
        rows = [dict(path=p, category=c, bytes=n, disposition='report-only',
                     reason='Inventory is not deletion authority; only terminal owned records enter retention') for (p, c), n in groups.items()]
        totals = {}
        for row in rows:
            totals[row['category']] = totals.get(row['category'], 0) + row['bytes']
        return dict(sizeKind='logical-file-bytes', categories=totals, roots=root_totals, extensions=extensions,
                    directories=sorted(rows, key=lambda r: r['bytes'], reverse=True), errors=errors, scanComplete=not errors)

    def cache_summary(self):
        """Compact, read-only inventory of isolated hectl CMake build directories."""
        parents = [self.root / 'out/agent']
        projects = self.root / 'out/agent/projects'
        if projects.is_dir():
            parents.extend(p for p in projects.iterdir()
                           if p.is_dir() and re.fullmatch(r'[0-9a-f]{16}', p.name))
        rows, errors = [], []
        for parent in parents:
            if not parent.is_dir():
                continue
            safe(parent / '_boundary_check', [self.root])
            for path in parent.iterdir():
                if not path.is_dir() or not re.fullmatch(r'(game|editor|tests)(?:-[a-z0-9-]+)?-(debug|relwithdebinfo|release)', path.name):
                    continue
                scan_errors = []
                try:
                    safe(path, self.roots)
                    files = list(tree(path, scan_errors))
                except (OSError, ValueError) as error:
                    files = []
                    scan_errors.append(dict(path=str(path), error=str(error)))
                bytes_used = sum(size for _, size in files)
                newest = 0
                for file, _ in files:
                    try:
                        newest = max(newest, filesystem_path(file).stat().st_mtime)
                    except OSError as error:
                        scan_errors.append(dict(path=str(file), error=str(error)))
                errors.extend(scan_errors)
                relative = path.relative_to(self.root).as_posix()
                rows.append(dict(path=relative, bytes=bytes_used,
                                 lastWriteUtc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(newest)) if newest else None,
                                 state='incomplete-scan' if scan_errors else
                                       'cache' if (path / 'CMakeCache.txt').is_file() else 'no-cmake-cache',
                                 defaultHost=relative in ('out/agent/game-relwithdebinfo',
                                                          'out/agent/editor-relwithdebinfo')))
        rows.sort(key=lambda row: row['bytes'], reverse=True)
        return dict(sizeKind='logical-file-bytes; deletion requires clean cache -WhatIf',
                    caches=rows, totalBytes=sum(row['bytes'] for row in rows),
                    errors=errors, scanComplete=not errors)

    def collect(self, what_if=False, kind='', project='', before_delete=None, retired=False):
        with mutex(self.root):
            report = self.plan(kind, project)
            report.update(deletedBytes=0, deleted=[], skipped=[], failed=[])
            if retired and not kind and not project:
                self.collect_retired(report, what_if)
            if what_if:
                return report
            for item in report['items']:
                if item['disposition'] != 'remove':
                    continue
                try:
                    if before_delete:
                        before_delete(item)
                    with contextlib.ExitStack() as stack:
                        record = read(self.record_path(item['id']))
                        for parent in sorted({p for a in record['paths'] for p in Path(a).parents if p.is_dir()}, key=lambda p: len(p.parts)):
                            stack.enter_context(directory_guard(parent))
                            safe(parent / '_boundary_check', [self.root]) if parent == self.root or inside(parent, self.root) else None
                        related_locks = self.related_locks(record)
                        for lock in related_locks:
                            stack.enter_context(exclusive(safe(lock, self.roots)))
                        # Re-read under the held writer locks and collector mutex.
                        # Locks are already held here; avoid reopening deny-sharing handles.
                        fresh = next(i for i in self.plan(kind, project, held_locks=set(related_locks))['items'] if i['id'] == item['id'])
                        if fresh['disposition'] != 'remove':
                            report['skipped'].append(dict(id=item['id'], reason=fresh['reason']))
                            continue
                        for artifact in record['paths']:
                            target = safe(artifact, self.roots)
                            self.check_special(record, target)
                            errors = []
                            list(tree(target, errors)) if target.exists() else None
                            if errors:
                                raise ValueError(str(errors))
                        for artifact in record['paths']:
                            self.remove(safe(artifact, self.roots), report)
                        # Keep a tiny parent tombstone until all children have been collected.
                        records, _ = self.load()
                        if not any(r['parent'] == item['id'] for r in records):
                            self.record_path(item['id']).unlink()
                        report['deleted'].append(item['id'])
                except (OSError, ValueError, KeyError, StopIteration) as error:
                    report['failed'].append(dict(id=item['id'], error=str(error)))
            # Retire parent tombstones after children; do not accumulate a second history in metadata.
            remaining, _ = self.load()
            for record in remaining:
                if record['status'] in ('success', 'failed') and not record['pinned'] and not record['recovery'] and not any(Path(p).exists() or Path(p + '.he-keep').exists() for p in record['paths']) and not any(r['parent'] == record['id'] for r in remaining):
                    try:
                        self.record_path(record['id']).unlink()
                    except OSError as error:
                        report['failed'].append(dict(id=record['id'], error=str(error), phase='metadata-retirement'))
            safe(self.home / 'last-clean.json', self.roots)
            report['outcome'] = 'partial' if report['failed'] or report['errors'] else 'complete'
            try:
                atomic(self.home / 'last-clean.json', report)
            except OSError as error:
                report['failed'].append(dict(path=str(self.home / 'last-clean.json'), error=str(error), phase='report-write'))
                report['outcome'] = 'partial'
            return report

    def check_special(self, record, target):
        self.validate_artifact(target, record['kind'])
        if not target.exists():
            return
        if record['kind'] == 'package-intermediate':
            expected = 'HEngine CookInput ' + record['projectId'] + ' v1'
            if not target.name.startswith('CookInput-') or (target / '.he-cook-input').read_text().strip() != expected:
                raise ValueError('CookInput ownership mismatch')
        if record['kind'] == 'package-staging':
            if '.staging-' not in target.name or (target / '.he-package-root').read_text().strip() != 'HEngine ' + record['projectId'] + ' Package v1':
                raise ValueError('Package staging ownership mismatch')
            current = target.with_name(target.name.split('.staging-')[0])
            current_marker = safe(current / '.he-package-root', self.roots)
            if current.with_name(current.name + '.previous').exists() or not current_marker.is_file() or current_marker.read_text().strip() != 'HEngine ' + record['projectId'] + ' Package v1':
                raise ValueError('Package recovery material or missing current package')
        if record['kind'] in ('wechat-backup', 'wechat-staging'):
            marker = read(target / '.he-export.json')
            current = safe(marker['output'], self.roots)
            if current.with_name('.' + current.name + '.publish.json').exists():
                raise ValueError('Export publication recovery journal is active')
            suffix = '.previous-' if record['kind'] == 'wechat-backup' else '.staging-'
            if marker['project'] != record['project'] or not target.name.startswith(current.name + suffix):
                raise ValueError('Export ownership mismatch')
            live = read(current / '.he-export.json')
            if live['output'] != str(current) or live['project'] != marker['project'] or not (current / 'resource-manifest.json').is_file():
                raise ValueError('Current export unavailable; retain recovery backup')

    def validate_artifact(self, target, kind):
        rel = target.relative_to(self.root).parts
        if kind == 'evidence':
            buckets = ('sessions', 'logs', 'captures', 'profiles', 'benchmarks', 'rendering-evidence', 'content')
            if not any(p in buckets for p in rel[2:-1]):
                raise ValueError('Evidence must be below a history bucket, never the bucket itself: ' + str(target))
        elif kind == 'package-intermediate':
            if not re.fullmatch(r'CookInput-[0-9a-f]+', target.name) or rel[:2] not in (('out', 'package'), ('out', 'pkg')):
                raise ValueError('Invalid CookInput boundary')
        elif kind == 'package-staging':
            if rel[0] != 'dist' or '.staging-' not in target.name:
                raise ValueError('Invalid package staging boundary')
        elif kind in ('wechat-backup', 'wechat-staging'):
            suffix = '.previous-' if kind == 'wechat-backup' else '.staging-'
            if rel[:2] != ('out', 'agent') or suffix not in target.name:
                raise ValueError('Invalid export backup boundary')
        else:
            raise ValueError('Unknown artifact kind')

    def remove(self, path, report):
        if not filesystem_path(path).exists():
            return
        safe(path, self.roots)
        if filesystem_path(path).is_dir():
            with directory_guard(path):
                safe(path, self.roots)
                for child in filesystem_path(path).iterdir():
                    self.remove(path / child.name, report)
            filesystem_path(path).rmdir()
        else:
            size = filesystem_path(path).stat().st_size
            filesystem_path(path).unlink()
            report['deletedBytes'] += size

    def auto(self):
        if not self.policy['autoEnabled']:
            return
        try:
            last = self.home / 'last-clean.json'
            if last.exists() and time.time() - last.stat().st_mtime < self.policy['autoIntervalSeconds']:
                return
            result = self.collect()
            if result['failed'] or result['errors'] or result['protectedOverBudgetBytes']:
                print('storage: review ' + str(last), file=sys.stderr)
        except (OSError, ValueError) as error:
            print('storage: cleanup deferred: ' + str(error), file=sys.stderr)

    def cache_lease(self, cache):
        key = hashlib.sha256(str(absolute(cache)).lower().encode()).hexdigest()[:24]
        return safe(self.home / ('leases/' + key + '.lock'), self.roots)

    def check_cleanup_processes(self):
        """Fail closed before cold-cache cleanup; never stop a user's process."""
        if os.name != 'nt':
            return []
        import subprocess
        command = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $ErrorActionPreference='Stop'; Get-CimInstance Win32_Process | Select-Object ExecutablePath,Name | ConvertTo-Json -Compress"
        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command],
                                capture_output=True, text=True, encoding='utf-8', creationflags=0x08000000)
        if result.returncode:
            raise ValueError('Cannot verify build/runtime processes')
        processes = json.loads(result.stdout or '[]')
        processes = processes if isinstance(processes, list) else [processes]
        for process in processes:
            name, exe = process['Name'].lower(), process.get('ExecutablePath')
            if name in ('cmake.exe', 'ninja.exe', 'msbuild.exe', 'cl.exe', 'link.exe') or (
                exe and inside(absolute(exe), self.root / 'out')) or (
                not exe and re.fullmatch(r'(hgame|hengineeditor|starbloomsurvivor|game[0-9a-f]{8})\.exe', name)):
                raise ValueError('Build/runtime process active or unverifiable: ' + process['Name'])
        return processes

    def maximize(self, what_if=False, before_delete=None):
        """Explicit out-only reclamation. Never called by auto()."""
        out = self.root / 'out'
        report = dict(schema=1, sizeKind='logical-file-bytes', items=[], deleted=[], skipped=[], failed=[],
                      reclaimableBytes=0, deletedBytes=0, freeBytesBefore=shutil.disk_usage(self.root).free)
        original_policy = self.policy

        def skip(path, reason):
            report['skipped'].append(dict(path=str(path), reason=str(reason)))

        def children(parent):
            if not os.path.lexists(parent):
                return []
            try:
                safe(parent / '_boundary_check', [out])
                return list(parent.iterdir())
            except (OSError, ValueError) as error:
                skip(parent, error)
                return []

        def inspect(path):
            safe(path, [out])
            for parent in (path, *path.parents):
                if parent == out or inside(parent, out):
                    if (parent / '.he-keep').exists() or Path(str(parent) + '.he-keep').exists():
                        raise ValueError('user pinned')
            errors = []
            files = list(tree(path, errors))
            if errors:
                raise ValueError('incomplete scan: ' + str(errors[:1]))
            for file, _ in files:
                parts = file.relative_to(path).parts if path.is_dir() else (file.name,)
                if file.name.endswith('.he-keep'):
                    raise ValueError('user pinned')
                if file.name in ('pending.json', 'active.json', '.he-cook-input') or '.journal' in file.name or any(
                    '.previous' in part or '.staging-' in part or part.startswith('CookInput-') for part in parts):
                    raise ValueError('session or recovery material')
            return files

        def references(path, records):
            return any(p == path or inside(p, path) or inside(path, p)
                       for record in records for p in map(Path, record['paths'] + record.get('dependencies', [])))

        def remove(path, kind, records=(), locks=()):
            if not os.path.lexists(path):
                return True
            deleting = False
            try:
                with contextlib.ExitStack() as stack:
                    safe(path, [out])
                    for parent in reversed(path.parents):
                        stack.enter_context(directory_guard(parent))
                    for lock in locks:
                        stack.enter_context(exclusive(safe(lock, [out])))
                    if references(path, records):
                        raise ValueError('registered evidence or dependency')
                    files = inspect(path)
                    executable_names = {file.name.lower() for file, _ in files if file.suffix.lower() == '.exe'}
                    if any(not p.get('ExecutablePath') and p['Name'].lower() in executable_names for p in processes):
                        raise ValueError('cannot verify runtime executable ownership')
                    if kind == 'retired-cache' and any(any(part in ('sessions', 'captures', 'profiles', 'benchmarks')
                        for part in file.relative_to(path).parts) for file, _ in files):
                        raise ValueError('retired cache contains historical evidence')
                    # Download/installer locks are held through deletion, including WhatIf.
                    for file, _ in files:
                        if file.name.endswith('.lock') and str(file) not in locks:
                            stack.enter_context(exclusive(file, allow_delete=True))
                    size = sum(size for _, size in files)
                    report['items'].append(dict(path=str(path), kind=kind, bytes=size))
                    report['reclaimableBytes'] += size
                    if not what_if:
                        deleting = True
                        self.remove(path, report)
                        report['deleted'].append(str(path))
                    return True
            except (OSError, ValueError) as error:
                (report['failed'] if deleting else report['skipped']).append(dict(path=str(path), reason=str(error)))
                return False

        try:
            self.policy = dict(original_policy, successCount=0, failureDays=0)
            with mutex(self.root):
                safe(out / '_boundary_check', [self.root])
                processes = self.check_cleanup_processes() or []
                plan = self.plan()
                records, errors = self.load()
                if errors or plan['errors']:
                    raise ValueError('Storage metadata or safety scan errors; inspect clean status')
                by_id = {record['id']: record for record in records}
                # The mutex freezes cooperating metadata writers for this entire batch.
                # Keep ownership records intact; check each artifact again under its locks.
                for item in plan['items']:
                    if not item['bytes']:
                        continue
                    if item['disposition'] != 'remove' or item['kind'] != 'evidence' or not all(
                        inside(Path(path), out) for path in item['paths']):
                        for path in item['paths']:
                            skip(path, item['reason'])
                        continue
                    record = by_id[item['id']]
                    if before_delete:
                        before_delete(item)
                        fresh = next(i for i in self.plan()['items'] if i['id'] == item['id'])
                        if fresh['disposition'] != 'remove':
                            skip(item['paths'][0], fresh['reason'])
                            continue
                    if read(self.record_path(item['id'])) != record:
                        skip(item['paths'][0], 'ownership changed')
                        continue
                    try:
                        locks = self.related_locks(record)
                        for artifact in record['paths']:
                            remove(Path(artifact), 'completed-evidence', locks=locks)
                    except (OSError, ValueError) as error:
                        skip(item['paths'][0], error)

                # Exact cache parents only: no recursive name-based search through out.
                parents = [out / 'agent']
                parents.extend(p for p in children(out / 'agent/projects') if re.fullmatch(r'[0-9a-f]{16}', p.name))
                units = []
                for parent in parents:
                    units.extend(p for p in children(parent) if re.fullmatch(
                        r'(game|editor|tests)(?:-[a-z0-9-]+)?-(debug|relwithdebinfo|release)(?:\.retired-[0-9a-f]{32})?', p.name))
                units.extend(p for p in children(out / 'package') if re.fullmatch(r'build(?:\.retired-[0-9a-f]{32})?', p.name))
                units.extend(p for p in children(out / 'pkg') if re.fullmatch(r'[0-9a-f]{12}(?:\.retired-[0-9a-f]{32})?', p.name))
                android = out / 'agent/android-player-release'
                if android.exists():
                    units.append(android)
                parts = ('CMakeCache.txt', 'build.ninja', 'Engine', 'bin', 'deps', 'dependencies', 'GeneratedContent',
                         'GeneratedSource', 'generated', 'include', 'CMakeFiles', 'rmlui.lib', 'compile_commands.json',
                         '.ninja_deps', '.ninja_log', 'cmake_install.cmake', 'CPackConfig.cmake', 'CPackSourceConfig.cmake')
                for unit in units:
                    try:
                        safe(unit, [out])
                        processes = self.check_cleanup_processes() or []
                        base = unit.with_name(unit.name.split('.retired-')[0])
                        lease = self.cache_lease(base)
                        with contextlib.ExitStack() as stack:
                            for parent in reversed(unit.parents):
                                stack.enter_context(directory_guard(parent))
                            if lease.exists() or not what_if:
                                if not what_if:
                                    lease.parent.mkdir(parents=True, exist_ok=True)
                                stack.enter_context(exclusive(lease, create=not what_if))
                            if '.retired-' in unit.name:
                                remove(unit, 'retired-cache', records)
                                continue
                            stack.enter_context(directory_guard(unit))
                            for lock in unit.glob('*.lock'):
                                stack.enter_context(exclusive(safe(lock, [out])))
                            retry = self.home / 'partial-caches' / (lease.stem + '.json')
                            safe(retry, [out])
                            cache_file = unit / 'CMakeCache.txt'
                            owned = cache_file.is_file() and any(
                                line.startswith('CMAKE_HOME_DIRECTORY:INTERNAL=') and absolute(line.split('=', 1)[1]) == self.root
                                for line in cache_file.read_text(encoding='utf-8-sig').splitlines())
                            if not owned and not (retry.is_file() and read(retry).get('path') == str(unit)):
                                skip(unit, 'no verified CMake cache or partial-cleanup receipt')
                                continue
                            for recovery in (unit / 'codegen', unit / 'GeneratedSource'):
                                if recovery.exists():
                                    inspect(recovery)
                            if not what_if:
                                safe(retry, [out])
                                atomic(retry, dict(path=str(unit)))
                            # Invalidate configure state first. Never delete sessions to retire a cache.
                            for name in parts:
                                if not remove(unit / name, 'build-cache-part', records) and name in ('CMakeCache.txt', 'build.ninja'):
                                    break
                            if not what_if and not any((unit / name).exists() for name in parts):
                                retry.unlink()
                    except (OSError, ValueError) as error:
                        skip(unit, error)

                known = []
                windows = out / 'agent/dependencies/windows'
                known.extend((p, [windows / '.install.lock']) for p in children(windows) if p.name in ('archives', 'cmake', 'node', 'oidn'))
                fsr = out / 'agent/dependencies/fsr-3.1'
                known.extend((fsr / name, []) for name in ('sdk-1.1.4', 'FidelityFX-SDK-v1.1.4.zip'))
                known.append((out / 'agent/scripting/deps', []))
                for bucket in ('development-templates', 'export-templates'):
                    for path in children(out / 'agent' / bucket):
                        match = re.fullmatch(r'(Windows-x64|Android-arm64)(?:-r[0-9]+)?', path.name)
                        if match and (path / 'Template.json').is_file():
                            try:
                                safe(path / 'Template.json', [out])
                                manifest = read(path / 'Template.json')
                                if manifest.get('SchemaVersion') != 1 or manifest.get('Platform') != match.group(1) or not (
                                    isinstance(manifest.get('Files'), list) and manifest['Files']) or not re.fullmatch(
                                    r'[0-9a-f]{64}', str(manifest.get('RuntimeFingerprint', ''))):
                                    raise ValueError('unverified generated-template manifest')
                                known.append((path, []))
                            except (OSError, ValueError) as error:
                                skip(path, error)
                for path, locks in known:
                    if path.exists():
                        processes = self.check_cleanup_processes() or []
                    remove(path, 'download-or-generated-template', records, [str(p) for p in locks if p.exists()])
                for bucket in ('captures', '4k60'):
                    for path in children(out / 'agent' / bucket):
                        try:
                            safe(path, [out])
                            if not (path / 'result.json').is_file():
                                continue
                            result = read(path / 'result.json')
                            terminal = result.get('status') == 'PASS' or result.get('state') == 'success' or (
                                result.get('status') == 'CAPTURED_AND_STOPPED' and result.get('shutdownVerified') is True)
                            if terminal:
                                remove(path, 'completed-capture', records)
                            else:
                                skip(path, 'capture completion not confirmed')
                        except (OSError, ValueError) as error:
                            skip(path, error)
        except (OSError, ValueError) as error:
            report['failed'].append(dict(path=str(out), reason=str(error)))
        finally:
            self.policy = original_policy
        groups, inventory_errors = {}, []
        if os.path.lexists(out):
            for file, size in tree(out, inventory_errors):
                parts = file.relative_to(out).parts
                depth = 4 if parts[:2] == ('agent', 'projects') else 2
                key = '/'.join(parts[:min(depth, len(parts) - 1)])
                groups[key] = groups.get(key, 0) + size
        report['observedOutBytes'] = sum(groups.values())
        report['observedLargest'] = [dict(path='out/' + path, bytes=size) for path, size in
                                     sorted(groups.items(), key=lambda item: item[1], reverse=True)[:30]]
        report['inventoryErrors'] = inventory_errors
        report['freeBytesAfter'] = shutil.disk_usage(self.root).free
        report['physicalFreeIncreaseBytes'] = report['freeBytesAfter'] - report['freeBytesBefore']
        report['outcome'] = 'partial' if report['failed'] else 'complete'
        if not what_if:
            try:
                safe(self.home / 'last-maximize-clean.json', [out])
                atomic(self.home / 'last-maximize-clean.json', report)
            except (OSError, ValueError) as error:
                report['failed'].append(dict(path=str(self.home), reason=str(error)))
                report['outcome'] = 'partial'
        return report

    def retired_caches(self):
        """List only exact cache-unit siblings produced by clean_cache."""
        parents = [self.root / 'out/agent', self.root / 'out/package', self.root / 'out/pkg']
        projects = self.root / 'out/agent/projects'
        if projects.is_dir():
            parents.extend(p for p in projects.iterdir() if p.is_dir() and re.fullmatch(r'[0-9a-f]{16}', p.name))
        for parent in parents:
            if not parent.is_dir():
                continue
            safe(parent / '_boundary_check', [self.root])
            for path in parent.iterdir():
                match = re.fullmatch(r'(.+)\.retired-([0-9a-f]{32})', path.name)
                if not match or not path.is_dir():
                    continue
                base = parent / match.group(1)
                rel = base.relative_to(self.root).parts
                allowed = (rel[:2] == ('out', 'agent') and len(rel) in (3, 5)
                           and base.name.startswith(('game-', 'editor-', 'tests-'))
                           and (len(rel) == 3 or rel[2] == 'projects'))
                allowed = allowed or rel == ('out', 'package', 'build') or (rel[:2] == ('out', 'pkg') and len(rel) == 3)
                if allowed:
                    yield path, base

    def collect_retired(self, report, what_if):
        """Retry interrupted cache deletion during explicit stale cleanup."""
        import subprocess
        report['retired'] = []
        for path, base in self.retired_caches():
            item = dict(path=str(path), bytes=0, disposition='keep')
            report['retired'].append(item)
            try:
                safe(path, self.roots)
                errors = []
                files = list(tree(path, errors))
                if errors:
                    raise ValueError('Retired cache scan incomplete: ' + str(errors[:3]))
                item['bytes'] = sum(size for _, size in files)
                if (path.with_name(path.name + '.he-keep')).exists() or any(
                    file.name == '.he-keep' or file.name.endswith('.he-keep') or
                    'journal' in file.name or 'pending' in file.name or
                    any(part in ('sessions', 'captures', 'profiles', 'benchmarks') for part in file.relative_to(path).parts)
                    for file, _ in files):
                    raise ValueError('Retired cache contains pinned, session, or recovery material')
                records, errors = self.load()
                if errors or any(inside(Path(a), path) for record in records for a in record['paths']):
                    raise ValueError('Retired cache contains registered evidence or metadata errors')
                item['disposition'] = 'remove'
                report['reclaimableBytes'] += item['bytes']
                if what_if:
                    continue
                with contextlib.ExitStack() as stack:
                    lease = self.cache_lease(base)
                    lease.parent.mkdir(parents=True, exist_ok=True)
                    stack.enter_context(exclusive(lease, create=True))
                    for parent in reversed(path.parents):
                        stack.enter_context(directory_guard(parent))
                    # A live executable or build process may still hold this partial cache.
                    if os.name == 'nt':
                        command = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $ErrorActionPreference='Stop'; Get-CimInstance Win32_Process | Select-Object ExecutablePath,Name | ConvertTo-Json -Compress"
                        result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command], capture_output=True, text=True, encoding='utf-8', creationflags=0x08000000)
                        if result.returncode:
                            raise ValueError('Cannot verify cache process ownership')
                        processes = json.loads(result.stdout or '[]')
                        executable_names = {file.name.lower() for file, _ in files if file.suffix.lower() == '.exe'}
                        for process in processes:
                            exe = process.get('ExecutablePath')
                            if not exe and process['Name'].lower() in executable_names:
                                raise ValueError('Cannot verify runtime process path: ' + process['Name'])
                            if process['Name'].lower() in ('cmake.exe', 'ninja.exe', 'msbuild.exe', 'cl.exe') or (exe and inside(absolute(exe), path)):
                                raise ValueError('Build/runtime process active: ' + process['Name'])
                    safe(path, self.roots)
                    self.remove(path, report)
                report['deleted'].append(str(path))
            except (OSError, ValueError) as error:
                item['disposition'] = 'keep'
                item['reason'] = str(error)
                destination = report['skipped'] if str(error).startswith('Retired cache contains') else report['failed']
                destination.append(dict(path=str(path), reason=str(error), phase='retired-cache'))

    def clean_retired(self, what_if=False):
        with mutex(self.root):
            report = dict(schema=1, sizeKind='logical-file-bytes', reclaimableBytes=0,
                          deletedBytes=0, deleted=[], skipped=[], failed=[])
            self.collect_retired(report, what_if)
            report['outcome'] = 'partial' if report['failed'] else 'complete'
            if not what_if:
                atomic(self.home / 'last-retired-clean.json', report)
            return report

    def clean_cache(self, path, what_if=False):
        """Explicit whole-unit retirement, restricted to hectl's cooperating caches."""
        import subprocess
        cache = safe(path, [self.root / 'out/agent', self.root / 'out/package', self.root / 'out/pkg'])
        rel = cache.relative_to(self.root).parts
        allowed = (rel[:2] == ('out', 'agent') and len(rel) in (3, 5)
                   and cache.name.startswith(('game-', 'editor-', 'tests-'))
                   and (len(rel) == 3 or rel[2] == 'projects'))
        allowed = allowed or rel == ('out', 'package', 'build') or (rel[:2] == ('out', 'pkg') and len(rel) == 3)
        if not allowed or not (cache / 'CMakeCache.txt').is_file():
            raise ValueError('Not an isolated hectl CMake cache unit: ' + str(cache))
        with mutex(self.root), contextlib.ExitStack() as stack:
            unit_locks = stack.enter_context(contextlib.ExitStack())
            lease = self.cache_lease(cache)
            if lease.exists():
                stack.enter_context(exclusive(lease))
            elif not what_if:
                lease.parent.mkdir(parents=True, exist_ok=True)
                stack.enter_context(exclusive(lease, create=True))
            errors = []
            files = list(tree(cache, errors))
            if errors:
                raise ValueError('Cache scan incomplete: ' + str(errors))
            for file, _ in files:
                if any(part in ('sessions', 'captures', 'profiles', 'benchmarks') for part in file.relative_to(cache).parts):
                    raise ValueError('Cache contains historical evidence; inspect it separately: ' + str(file))
                if file.name == '.he-keep' or 'journal' in file.name or 'pending' in file.name or file.name.startswith('.he-cook-input') or file.name == 'active.json':
                    raise ValueError('Cache contains pinned/session/recovery/intermediate material: ' + str(file))
                if file.name.endswith('.lock'):
                    unit_locks.enter_context(exclusive(safe(file, self.roots), allow_delete=True))
            records, errors = self.load()
            if errors or any(inside(Path(a), cache) for r in records for a in r['paths']):
                raise ValueError('Cache contains registered evidence; recycle it separately first')
            if os.name == 'nt':
                # A user-owned open process may outlive the startup lease. Fail closed on query errors.
                command = "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); $ErrorActionPreference='Stop'; Get-CimInstance Win32_Process | Select-Object ExecutablePath,Name | ConvertTo-Json -Compress"
                result = subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', command], capture_output=True, text=True, encoding='utf-8', creationflags=0x08000000)
                if result.returncode:
                    raise ValueError('Cannot verify cache process ownership')
                processes = json.loads(result.stdout or '[]')
                executable_names = {file.name.lower() for file, _ in files if file.suffix.lower() == '.exe'}
                for process in processes:
                    exe = process.get('ExecutablePath')
                    if not exe and process['Name'].lower() in executable_names:
                        raise ValueError('Cannot verify runtime process path: ' + process['Name'])
                    if process['Name'].lower() in ('cmake.exe', 'ninja.exe', 'msbuild.exe', 'cl.exe') or (exe and inside(absolute(exe), cache)):
                        raise ValueError('Build/runtime process active: ' + process['Name'])
            report = dict(schema=1, sizeKind='logical-file-bytes', paths=[str(cache)], reclaimableBytes=sum(n for _, n in files), deletedBytes=0, failed=[])
            if not what_if:
                try:
                    for parent in reversed(cache.parents):
                        stack.enter_context(directory_guard(parent))
                    retired = safe(cache.with_name(cache.name + '.retired-' + uuid.uuid4().hex), self.roots)
                    # Windows cannot rename a directory with open descendant handles.
                    # The outside lease remains held across retirement and deletion; every
                    # hectl build/start/managed-codegen writer takes that lease first.
                    unit_locks.close()
                    cache.rename(retired)
                    report['retiredPath'] = str(retired)
                    self.remove(retired, report)
                except (OSError, ValueError) as error:
                    report['failed'].append(dict(path=str(cache), error=str(error)))
                try:
                    atomic(self.home / 'last-cache-clean.json', report)
                except OSError as error:
                    report['failed'].append(dict(path=str(self.home / 'last-cache-clean.json'), error=str(error), phase='report-write'))
            return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['status', 'preview', 'stale', 'cache', 'caches', 'retired', 'maximize', 'start', 'owner', 'finish', 'auto', 'lease-path'])
    parser.add_argument('--root', default=str(Path(__file__).resolve().parents[2]))
    parser.add_argument('--request')
    parser.add_argument('--id')
    parser.add_argument('--cache-path')
    parser.add_argument('--state')
    parser.add_argument('--pid', type=int)
    parser.add_argument('--started', type=int)
    parser.add_argument('--kind', default='')
    parser.add_argument('--project', default='')
    parser.add_argument('--what-if', action='store_true')
    parser.add_argument('--report')
    parser.add_argument('--success-count', type=int)
    parser.add_argument('--failure-days', type=int)
    parser.add_argument('--budget-bytes', type=int)
    args = parser.parse_args()
    policy = {key: val for key, val in [('successCount', args.success_count), ('failureDays', args.failure_days), ('budgetBytes', args.budget_bytes)] if val is not None}
    storage = Storage(args.root, policy)
    if args.action == 'lease-path':
        print(storage.cache_lease(args.cache_path))
        return
    if args.action == 'maximize':
        if args.kind or args.project or args.cache_path or any(value is not None for value in
            (args.success_count, args.failure_days, args.budget_bytes)):
            parser.error('maximize is restricted to this workspace out; filters are not supported')
        result = storage.maximize(args.what_if)
    elif args.action == 'caches':
        result = storage.cache_summary()
    elif args.action == 'cache':
        result = storage.clean_cache(args.cache_path, args.what_if)
    elif args.action == 'retired':
        result = storage.clean_retired(args.what_if)
    elif args.action == 'start':
        result = dict(id=storage.start(**read(Path(args.request))))
    elif args.action == 'finish':
        storage.finish(args.id, args.state)
        result = dict(id=args.id, status=args.state)
    elif args.action == 'owner':
        storage.bind_owner(args.id, args.pid, args.started)
        result = dict(id=args.id)
    elif args.action == 'auto':
        storage.auto()
        return
    elif args.action == 'status':
        result = storage.plan(args.kind, args.project)
        result['inventory'] = storage.inventory()
        result['scanComplete'] = result['scanComplete'] and result['inventory']['scanComplete']
        result['recordedPreservedBytes'] = result['preservedBytes']
        result['observedTotalBytes'] = sum(result['inventory']['categories'].values())
        result['preservedBytes'] = max(0, result['observedTotalBytes'] - result['reclaimableBytes'])
        result['accountingScope'] = 'all inventoried roots; incomplete scans are lower bounds'
    else:
        result = storage.collect(args.what_if or args.action == 'preview', args.kind, args.project,
                                 retired=args.action in ('stale', 'preview'))
    if args.report and not args.what_if and args.action != 'preview':
        try:
            atomic(absolute(args.report), result)
        except OSError as error:
            result['reportWriteError'] = str(error)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
