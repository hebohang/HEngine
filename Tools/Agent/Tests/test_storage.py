import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('storage', REPO / 'Tools/Agent/storage.py')
storage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(storage)
spec = importlib.util.spec_from_file_location('export_publish', REPO / 'Tools/WeChat/export_publish.py')
publication = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publication)


class Retention(unittest.TestCase):
    def test_cache_summary_is_compact_and_read_only(self):
        with tempfile.TemporaryDirectory(prefix='he-storage-summary-') as directory:
            root = Path(directory)
            current = root / 'out/agent/game-relwithdebinfo'
            old = root / 'out/agent/projects/0123456789abcdef/editor-debug'
            for cache in (current, old):
                cache.mkdir(parents=True)
                (cache / 'CMakeCache.txt').write_bytes(b'cache')
            (old / 'object.obj').write_bytes(b'x' * 100)
            unrelated = root / 'out/agent/editor-ui-output'
            unrelated.mkdir()
            (unrelated / 'note.txt').write_bytes(b'x' * 50)
            instance = storage.Storage(root)
            report = instance.cache_summary()
            self.assertEqual(report['totalBytes'], 110)
            self.assertEqual([row['bytes'] for row in report['caches']], [105, 5])
            self.assertTrue(report['caches'][1]['defaultHost'])
            self.assertTrue(all(row['state'] == 'cache' for row in report['caches']))
            self.assertFalse((root / 'out/agent/storage').exists())

    def make_cache(self, relative='out/agent/game-debug'):
        cache = self.root / relative
        (cache / 'Engine').mkdir(parents=True)
        (cache / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=' + str(self.root))
        (cache / 'build.ninja').write_text('generated')
        (cache / 'Engine/object.obj').write_bytes(b'object data')
        return cache

    def test_maximize_whatif_and_mixed_cache_preserve_pins_unknown_and_policy(self):
        cache = self.make_cache()
        session = cache / 'sessions/pinned'
        session.mkdir(parents=True)
        (session / '.he-keep').touch()
        (session / 'capture').write_bytes(b'keep')
        _, completed = self.run_record(0)
        _, unknown = self.run_record(1, 'unknown')
        unrelated = self.root / 'out/agent/unclassified'
        unrelated.mkdir()
        (unrelated / 'precious').write_bytes(b'keep')
        before, policy = self.snapshot(), dict(self.s.policy)
        with mock.patch.object(self.s, 'check_cleanup_processes'):
            preview = self.s.maximize(what_if=True)
            self.assertGreater(preview['reclaimableBytes'], 100)
            self.assertEqual(before, self.snapshot())
            report = self.s.maximize()
        self.assertFalse(report['failed'], report)
        self.assertFalse(completed.exists())
        self.assertFalse((cache / 'Engine').exists())
        self.assertFalse((cache / 'CMakeCache.txt').exists())
        self.assertTrue(session.exists())
        self.assertTrue(unknown.exists())
        self.assertTrue((unrelated / 'precious').exists())
        self.assertEqual(policy, self.s.policy)

    def test_maximize_rechecks_pin_and_processes(self):
        _, completed = self.run_record(0)
        with mock.patch.object(self.s, 'check_cleanup_processes', side_effect=ValueError('active process')):
            report = self.s.maximize()
        self.assertEqual(report['deletedBytes'], 0)
        self.assertTrue(report['failed'])
        def pin(item):
            (completed / '.he-keep').touch()
        with mock.patch.object(self.s, 'check_cleanup_processes'):
            report = self.s.maximize(before_delete=pin)
        self.assertEqual(report['deletedBytes'], 0)
        self.assertTrue(completed.exists())

    def test_maximize_preserves_dependency_and_root_pin(self):
        _, source = self.run_record(0)
        self.run_record(1, 'unknown', dependencies=[source])
        cache = self.make_cache()
        (cache / '.he-keep').touch()
        with mock.patch.object(self.s, 'check_cleanup_processes'):
            report = self.s.maximize()
        self.assertEqual(report['deletedBytes'], 0)
        self.assertTrue(source.exists())
        self.assertTrue((cache / 'Engine/object.obj').exists())

    def test_maximize_locked_cache_and_retry_partial_deletion(self):
        cache = self.make_cache()
        lease = self.s.cache_lease(cache)
        lease.parent.mkdir(parents=True)
        lease.touch()
        with mock.patch.object(self.s, 'check_cleanup_processes'):
            with storage.exclusive(lease):
                report = self.s.maximize()
            self.assertEqual(report['deletedBytes'], 0)
            self.assertTrue((cache / 'CMakeCache.txt').exists())
            original = self.s.remove
            def fail_engine(path, result):
                if path == cache / 'Engine':
                    raise OSError('simulated busy file')
                return original(path, result)
            with mock.patch.object(self.s, 'remove', side_effect=fail_engine):
                failed = self.s.maximize()
            self.assertTrue(failed['failed'])
            self.assertFalse((cache / 'CMakeCache.txt').exists())
            self.assertTrue((cache / 'Engine/object.obj').exists())
            retry = self.s.maximize()
        self.assertEqual(retry['deletedBytes'], len(b'object data'))
        self.assertFalse(retry['failed'], retry)
        self.assertFalse((cache / 'Engine').exists())
        self.assertFalse(list((self.s.home / 'partial-caches').glob('*.json')))

    def test_maximize_downloads_retired_and_recovery_boundaries(self):
        paths = ['out/agent/dependencies/fsr-3.1/sdk-1.1.4/runtime.dll',
                 'out/agent/game-debug.retired-' + 'a' * 32 + '/object.obj',
                 'out/agent/captures/finished/image.png']
        for rel in paths:
            path = self.root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'payload')
        (self.root / 'out/agent/captures/finished/result.json').write_text('{"status":"PASS"}')
        template = self.root / 'out/agent/export-templates/Windows-x64'
        template.mkdir(parents=True)
        (template / 'Template.json').write_text(json.dumps(dict(SchemaVersion=1, Platform='Windows-x64',
                                                              RuntimeFingerprint='a' * 64, Files=[{'Path':'Player/game.exe'}])))
        unknown_template = template.with_name('Windows-x64-r2')
        unknown_template.mkdir()
        (unknown_template / 'Template.json').write_text('{}')
        cache = self.make_cache()
        (cache / 'GeneratedSource').mkdir()
        (cache / 'GeneratedSource/pending.json').write_text('{}')
        history = self.root / ('out/agent/editor-debug.retired-' + 'b' * 32) / 'sessions/unknown'
        history.mkdir(parents=True)
        (history / 'data').write_bytes(b'keep')
        with mock.patch.object(self.s, 'check_cleanup_processes'):
            result = self.s.maximize()
        self.assertFalse(result['failed'], result)
        self.assertTrue(all(not (self.root / p).exists() for p in paths))
        self.assertFalse(template.exists())
        self.assertTrue(unknown_template.exists())
        self.assertTrue(history.exists())
        self.assertTrue((cache / 'Engine/object.obj').exists())

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_maximize_and_cache_inventory_skip_junction_continue_other_cache(self):
        cache = self.make_cache('out/agent/editor-debug')
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'precious').write_bytes(b'keep')
        link = self.root / 'out/agent/game-debug'
        result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(outside)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        try:
            summary = self.s.cache_summary()
            self.assertTrue(summary['errors'])
            self.assertEqual(len(summary['caches']), 2)
            with mock.patch.object(self.s, 'check_cleanup_processes'):
                report = self.s.maximize()
            self.assertFalse(report['failed'], report)
            self.assertFalse((cache / 'Engine').exists())
            self.assertEqual((outside / 'precious').read_bytes(), b'keep')
            self.assertTrue(link.exists())
        finally:
            link.rmdir()

    def test_auto_disabled_keeps_history_but_explicit_collect_works(self):
        with tempfile.TemporaryDirectory(prefix='he-storage-auto-') as directory:
            instance = storage.Storage(Path(directory), {'autoEnabled': False})
            with mock.patch.object(instance, 'collect', return_value={}) as collect:
                instance.auto()
                collect.assert_not_called()
                instance.collect()
                collect.assert_called_once()
            with self.assertRaises(ValueError):
                storage.Storage(Path(directory), {'autoEnabled': 0})

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='he-storage-test-')
        self.root = Path(self.temp.name)
        self.s = storage.Storage(self.root)

    def tearDown(self):
        if os.name == 'nt':
            shutil.rmtree(storage.filesystem_path(self.root), ignore_errors=True)
        self.temp.cleanup()

    def run_record(self, number, status='success', age=0, project='project-a', **kwargs):
        path = self.root / ('out/agent/projects/' + project + '/sessions/run-' + str(number))
        path.mkdir(parents=True)
        (path / 'data').write_bytes(b'x' * 100)
        ident = self.s.start(project, 'check', [path], **kwargs)
        if status != 'running':
            self.s.finish(ident, status)
            r = storage.read(self.s.record_path(ident))
            r['ended'] = time.time() - age * 86400 - number
            storage.atomic(self.s.record_path(ident), r)
        return ident, path

    def test_count_project_failure_priority_budget(self):
        for n in range(7):
            self.run_record(n)
        self.run_record(10, 'failed', age=6)
        self.run_record(11, 'failed', age=8)
        self.run_record(12, project='project-b')
        self.s.policy['budgetBytes'] = 1
        plan = self.s.plan()
        self.assertEqual(sum(i['disposition'] == 'remove' for i in plan['items']), 3)
        self.assertGreater(plan['protectedOverBudgetBytes'], 0)
        self.assertEqual(plan['reclaimableBytes'], 300)
        result = self.s.collect()
        self.assertEqual(result['deletedBytes'], 300)
        self.assertFalse(result['failed'])
        self.assertEqual(len(self.s.load()[0]), 7)

    def test_running_unknown_pin_sdk_cache_and_latest(self):
        self.s.policy['successCount'] = 0
        for n, state in enumerate(['running', 'unknown']):
            self.run_record(n, state)
        _, pinned = self.run_record(2)
        (pinned / '.he-keep').touch()
        for path in ['out/agent/wechat/deps/sdk', 'out/agent/game-debug', 'out/agent/wechat-export']:
            p = self.root / path
            p.mkdir(parents=True)
            (p / 'data').write_text('keep')
            with self.assertRaises(ValueError):
                self.s.start('p', 'check', [p])
        mystery = self.root / 'out/agent/unknown-history'
        mystery.mkdir()
        (mystery / 'data').write_text('keep')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertTrue(mystery.exists())
        self.assertTrue(pinned.exists())

    def snapshot(self):
        return {str(p.relative_to(self.root)): (p.read_bytes(), p.stat().st_mtime_ns)
                for p in self.root.rglob('*') if p.is_file()}

    def test_whatif_exactly_read_only(self):
        self.s.policy['successCount'] = 0
        self.run_record(0)
        before = self.snapshot()
        directories = sorted(str(p) for p in self.root.rglob('*'))
        self.assertEqual(self.s.collect(what_if=True)['reclaimableBytes'], 100)
        self.assertEqual(before, self.snapshot())
        self.assertEqual(directories, sorted(str(p) for p in self.root.rglob('*')))

    def test_escape_and_roots(self):
        for path in [self.root / 'out/agent', self.root / 'out/agent/../../Project/Assets', self.root / 'out/agent/projects']:
            with self.assertRaises(ValueError):
                self.s.start('p', 'x', [path])

    @unittest.skipUnless(os.name == 'nt', 'Windows junction contract')
    def test_junction_and_nested_junction(self):
        self.s.policy['successCount'] = 0
        _, path = self.run_record(0)
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'precious').write_text('keep')
        link = path / 'junction'
        result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(outside)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        try:
            self.assertEqual(self.s.collect()['deletedBytes'], 0)
            with self.assertRaises(ValueError):
                storage.safe(link / 'precious', self.s.roots)
            self.assertEqual((outside / 'precious').read_text(), 'keep')
        finally:
            link.rmdir()

    def test_active_lock_and_execution_revalidation(self):
        self.s.policy['successCount'] = 0
        lock = self.root / 'out/agent/execution.lock'
        lock.parent.mkdir(parents=True)
        lock.touch()
        ident, path = self.run_record(0, locks=[lock])
        with storage.exclusive(lock):
            self.assertEqual(self.s.collect()['deletedBytes'], 0)
        def restart(item):
            r = storage.read(self.s.record_path(ident))
            r['status'] = 'running'
            storage.atomic(self.s.record_path(ident), r)
        result = self.s.collect(before_delete=restart)
        self.assertEqual(result['deletedBytes'], 0)
        self.assertEqual(len(result['skipped']), 1)
        self.assertTrue(path.exists())

    def test_collector_mutex_other_process(self):
        with storage.mutex(self.root):
            code = "import sys; sys.path.insert(0, sys.argv[1]); import storage; storage.Storage(sys.argv[2]).collect()"
            child = subprocess.run([sys.executable, '-c', code, str(REPO / 'Tools/Agent'), str(self.root)], capture_output=True)
            self.assertNotEqual(child.returncode, 0)
            self.assertIn(b'busy', child.stderr)

    def test_partial_failure_counts_only_unlinked_files(self):
        self.s.policy['successCount'] = 0
        _, path = self.run_record(0)
        (path / 'z-blocked').write_bytes(b'y' * 55)
        unlink = Path.unlink
        def failing(p, *args, **kwargs):
            if p.name == 'z-blocked':
                raise PermissionError('fixture sharing violation')
            return unlink(p, *args, **kwargs)
        with mock.patch.object(Path, 'unlink', failing):
            result = self.s.collect()
        self.assertEqual(result['deletedBytes'], 100)
        self.assertEqual(len(result['failed']), 1)
        self.assertEqual((path / 'z-blocked').stat().st_size, 55)

    def test_outer_workflow_keeps_children_through_analysis(self):
        self.s.policy['successCount'] = 0
        parent, parent_path = self.run_record(0, 'running')
        _, child = self.run_record(1, parent=parent)
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.s.finish(parent, 'success')
        (parent_path / '.he-keep').touch()
        self.assertEqual(self.s.plan()['historyBytes'], 0)
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        (parent_path / '.he-keep').unlink()
        result = self.s.collect()
        self.assertFalse(result['failed'])
        self.assertFalse(child.exists())
        self.assertFalse(parent_path.exists())

    def test_nested_pin_recovery_malformed_and_changed_ownership(self):
        self.s.policy['successCount'] = 0
        ident, path = self.run_record(0)
        (path / 'nested').mkdir()
        (path / 'nested/.he-keep').touch()
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        (path / 'nested/.he-keep').unlink()
        (path / 'nested/pending.json').write_text('{}')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        (path / 'nested/pending.json').unlink()
        storage.atomic(self.s.record_path(ident), dict(schema=1, id=ident))
        plan = self.s.plan()
        self.assertFalse(plan['scanComplete'])
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertTrue((path / 'data').exists())

    def test_external_outer_barrier_and_nested_ownership(self):
        self.s.policy['successCount'] = 0
        external = self.root / 'Artifacts/baseline'
        external.mkdir(parents=True)
        (external / 'precious').write_text('keep')
        parent = self.s.start('p', 'external-validation', [external], outer=True)
        _, child = self.run_record(0, parent=parent)
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.s.finish(parent, 'success')
        self.s.collect()
        self.assertFalse(child.exists())
        self.assertTrue((external / 'precious').exists())
        parent, path = self.run_record(1, 'running')
        nested = path / 'nested'
        nested.mkdir()
        self.assertIsNone(self.s.start('p', 'inner', [nested], parent=parent))

    @unittest.skipUnless(os.name == 'nt', 'Windows PID identity')
    def test_dead_owner_is_unknown_never_success(self):
        ident, path = self.run_record(0, 'running', owner_pid=0x7fffffff, owner_started=1)
        item = next(i for i in self.s.plan()['items'] if i['id'] == ident)
        self.assertEqual(item['status'], 'unknown')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertTrue(path.exists())

    def test_retained_analysis_protects_original_trace(self):
        self.s.policy['successCount'] = 0
        _, source = self.run_record(0)
        consumer, output = self.run_record(1, 'running', dependencies=[source / 'data'])
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.s.finish(consumer, 'success')
        self.s.policy['successCount'] = 1
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertTrue(source.exists())
        self.s.policy['successCount'] = 0
        self.assertEqual(self.s.collect()['deletedBytes'], 200)
        self.assertFalse(source.exists())
        self.assertFalse(output.exists())

    def test_package_success_failure_and_recovery(self):
        lock = self.root / 'out/package/build/.package-build.lock'
        lock.parent.mkdir(parents=True)
        lock.touch()
        cook = lock.parent / 'Engine/Source/Game/CookInput-123abc'
        cook.mkdir(parents=True)
        (cook / '.he-cook-input').write_text('HEngine CookInput p v1\n')
        (cook / 'payload').write_bytes(b'x' * 123)
        parent, _ = self.run_record(0, 'running')
        ident = self.s.start('p-root', 'package', [cook], [lock], kind='package-intermediate', project_id='p', parent=parent)
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.s.finish(ident, 'failed')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.s.finish(ident, 'success')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)  # Outer publication not yet complete.
        self.s.finish(parent, 'success')
        with storage.exclusive(lock):
            self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertGreater(self.s.collect(kind='package-intermediate', project='p-root')['deletedBytes'], 123)
        self.assertFalse(cook.exists())
        stage = self.root / 'dist/Game.staging-123abc'
        stage.mkdir(parents=True)
        (stage / '.he-package-root').write_text('HEngine p Package v1\n')
        ident = self.s.start('p-root', 'package', [stage], [lock], kind='package-staging', project_id='p')
        self.s.finish(ident, 'failed')
        self.s.policy['failureDays'] = 0
        previous = self.root / 'dist/Game.previous'
        previous.mkdir()
        (previous / '.he-package-root').write_text('HEngine p Package v1\n')
        self.assertEqual(self.s.collect()['deletedBytes'], 0)
        self.assertTrue(previous.exists())
        self.assertTrue(stage.exists())

    def test_inventory_includes_project_history_and_cache_dimensions(self):
        self.run_record(0)
        for rel in ['out/build/Debug/data', 'out/pkg/123/CMakeCache.txt', 'out/agent/game-tracy-debug/CMakeCache.txt', 'out/agent/wechat/deps/sdk', 'dist/Game/data']:
            p = self.root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(b'123')
        report = self.s.inventory()
        self.assertTrue(report['scanComplete'])
        self.assertEqual(report['categories']['sdk-dependencies'], 3)
        self.assertTrue(any('projects/project-a/sessions' in r['path'] for r in report['directories']))

    def test_explicit_stale_retries_only_unprotected_retired_cache(self):
        parent = self.root / 'out/agent/projects/0123456789abcdef'
        retired = parent / ('game-relwithdebinfo.retired-' + 'a' * 32)
        pinned = parent / ('editor-relwithdebinfo.retired-' + 'b' * 32)
        unknown = parent / ('custom.retired-' + 'c' * 32)
        for path in (retired, pinned, unknown):
            path.mkdir(parents=True)
            (path / 'object.obj').write_bytes(b'cached')
        (pinned / '.he-keep').touch()
        query = subprocess.CompletedProcess([], 0, stdout='[]', stderr='')
        with mock.patch.object(subprocess, 'run', return_value=query):
            preview = self.s.collect(what_if=True, retired=True)
            self.assertTrue(retired.exists())
            self.assertEqual([i['path'] for i in preview['retired'] if i['disposition'] == 'remove'], [str(retired)])
            result = self.s.clean_retired()
        self.assertFalse(retired.exists())
        self.assertTrue(pinned.exists())
        self.assertTrue(unknown.exists())
        self.assertEqual(result['deletedBytes'], 6)
        self.assertIn(str(retired), result['deleted'])

    @unittest.skipUnless(os.name == 'nt', 'Windows PowerShell / cache lease contract')
    def test_cache_lease_whole_unit_and_reuse(self):
        cache = self.root / 'out/agent/game-debug'
        cache.mkdir(parents=True)
        (cache / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug')
        (cache / 'GeneratedSource').mkdir()
        (cache / 'GeneratedSource/manifest.json').write_text('{}')
        before = self.snapshot()
        # Limit process-query results to this fixture: real user builds are intentionally protected.
        query = subprocess.CompletedProcess([], 0, stdout='[]', stderr='')
        with mock.patch.object(subprocess, 'run', return_value=query):
            self.s.clean_cache(cache, what_if=True)
            self.assertEqual(self.snapshot(), before)
            lease = self.s.cache_lease(cache)
            lease.parent.mkdir(parents=True)
            lease.touch()
            with storage.exclusive(lease):
                with self.assertRaises(OSError):
                    self.s.clean_cache(cache)
            (cache / '.agent-build.lock').touch()
            result = self.s.clean_cache(cache)
            self.assertFalse(result['failed'], result)
            self.assertFalse(cache.exists())
            self.assertFalse(Path(result['retiredPath']).exists())
            # Same stable path can be recreated for the next isolated configure.
            cache.mkdir()
            (cache / 'CMakeCache.txt').write_text('new configuration')
            self.assertTrue(cache.is_dir())

    @unittest.skipUnless(os.name == 'nt', 'Windows extended-length cache path contract')
    def test_cache_removes_long_paths_without_skipping_files(self):
        cache = self.root / 'out/agent/game-debug'
        deep = cache / ('nested-' + 'a' * 80) / ('nested-' + 'b' * 80) / ('nested-' + 'c' * 80)
        storage.filesystem_path(deep).mkdir(parents=True)
        (cache / 'CMakeCache.txt').write_text('CMAKE_BUILD_TYPE:STRING=Debug')
        long_file = deep / 'long-path.obj'
        storage.filesystem_path(long_file).write_bytes(b'object data')
        self.assertGreater(len(str(long_file)), 260)
        self.assertEqual(storage.safe(long_file, self.s.roots), long_file)
        query = subprocess.CompletedProcess([], 0, stdout='[]', stderr='')
        with mock.patch.object(subprocess, 'run', return_value=query):
            report = self.s.clean_cache(cache)
        self.assertFalse(report['failed'], report)
        self.assertFalse(cache.exists())
        self.assertFalse(Path(report['retiredPath']).exists())
        self.assertEqual(report['deletedBytes'], report['reclaimableBytes'])

    def test_cmake_publish_and_exact_path_declaration(self):
        cmake = shutil.which('cmake')
        self.assertIsNotNone(cmake, 'CMake is required for the packaging contract')
        output = self.root / 'dist'
        project = self.root / 'Project'
        project.mkdir()
        def call(script, values):
            result = subprocess.run([cmake] + ['-D' + k + '=' + str(v) for k, v in values.items()] + ['-P', str(REPO / 'CMake' / script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for number in range(2):
            stage = output / ('Game.staging-' + format(number, '012x'))
            values = dict(PACKAGE_OUTPUT_ROOT=output, PACKAGE_DIR=output / 'Game', STAGING_DIR=stage,
                          PACKAGE_EXECUTABLE_NAME='Game', PACKAGE_PROJECT_ID='p', PROJECT_DIR=project,
                          PACKAGE_NONCE=format(number, '012x'))
            call('PrepareGamePackageStaging.cmake', values)
            (stage / 'Game.pak').write_text(str(number))
            call('PublishGamePackage.cmake', values)
            self.assertEqual((output / 'Game/Game.pak').read_text(), str(number))
            self.assertFalse(stage.exists())
            self.assertFalse((output / 'Game.previous').exists())
        # Execute the actual declaration added to Game CMake, without configuring the engine.
        source = (REPO / 'Engine/Source/Game/CMakeLists.txt').read_text(encoding='utf-8-sig')
        start = source.index('file(WRITE "${CMAKE_BINARY_DIR}/PackageStoragePaths.txt"')
        end = source.index('\nset(HE_GAME_COOK_REPORT', start)
        contract = self.root / 'paths.cmake'
        contract.write_text(source[start:end])
        result = subprocess.run([cmake, '-DCMAKE_BINARY_DIR=' + str(self.root), '-DHE_GAME_COOK_INPUT_DIR=' + str(self.root / 'CookInput-123'), '-DHE_GAME_PACKAGE_STAGING_DIR=' + str(output / 'Game.staging-123'), '-P', str(contract)], capture_output=True, cwd=self.root)
        self.assertEqual(result.returncode, 0, result.stderr)
        lines = (self.root / 'PackageStoragePaths.txt').read_text().splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(Path(lines[0]), self.root / 'CookInput-123')

    @unittest.skipUnless(os.name == 'nt', 'PowerShell 5.1 bridge')
    def test_powershell_bridge_lifecycle(self):
        # Point the bridge to the temporary repository; no real workflow/output is changed.
        folder = self.root / 'out/agent/sessions/ps-run'
        folder.mkdir(parents=True)
        (folder / 'evidence').write_text('evidence')
        script = self.root / 'bridge.ps1'
        script.write_text("""$ErrorActionPreference = 'Stop'
. $args[0]
$script:heStorageTool = $args[1]
$env:HE_STORAGE_PYTHON = $args[2]
$global:LASTEXITCODE = 37
$r = Start-HEStorageRun -Project fixture -Workflow ps-check -Paths @($args[3]) -Outer
if (-not $r -or $env:HE_STORAGE_PARENT -ne $r.Id) { throw 'registration failed' }
Set-HEStorageOwner -Run $r -Process (Get-Process -Id $PID)
$live = (Invoke-HEStorage @('preview')) | ConvertFrom-Json
if ($live.items[0].status -ne 'running') { throw 'live owner was not identified' }
Complete-HEStorageRun -Run $r -State success -NoAuto
if ($env:HE_STORAGE_PARENT -or $global:LASTEXITCODE -ne 37) { throw 'environment/exit code changed' }
""", encoding='utf-8-sig')
        # The CLI's default root is the location of storage.py, so copy only that stdlib tool.
        tool = self.root / 'Tools/Agent/storage.py'
        tool.parent.mkdir(parents=True)
        shutil.copy2(REPO / 'Tools/Agent/storage.py', tool)
        result = subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(script), str(REPO / 'Tools/Agent/Storage.ps1'), str(tool), sys.executable, str(folder)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.s.load()[0][0]['status'], 'success')

    def test_content_report_completion_preserves_persistent_state(self):
        spec = importlib.util.spec_from_file_location('content_sync', REPO / 'Tools/Content/content_sync.py')
        content = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(content)
        content.__file__ = str(self.root / 'Tools/Content/content_sync.py')
        tool = self.root / 'Tools/Agent/storage.py'
        tool.parent.mkdir(parents=True)
        shutil.copy2(REPO / 'Tools/Agent/storage.py', tool)
        project = self.root / 'Project'
        persistent = [project / '.he-content/source/state.json', project / '.he-content/source/pending.json', project / 'Assets/Imported/mesh.hmesh']
        for file in persistent:
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text('persistent')
        result = dict(ok=True, status='synchronized', diagnostics=[])
        argv = ['content_sync.py', 'plan', '--project', str(project)]
        with mock.patch.object(sys, 'argv', argv), mock.patch.object(content, 'synchronize', return_value=result.copy()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(content.main(), 0)
        records, errors = self.s.load()
        self.assertFalse(errors)
        self.assertEqual(records[0]['status'], 'success')
        default_report = Path(records[0]['paths'][0])
        self.assertTrue((default_report / 'report.json').exists())
        explicit = self.root / 'out/agent/content/explicit-report'
        with mock.patch.object(sys, 'argv', argv + ['--report', str(explicit)]), mock.patch.object(content, 'synchronize', return_value=result.copy()), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(content.main(), 0)
        self.s.policy['successCount'] = 0
        self.s.collect()
        self.assertFalse(default_report.exists())
        self.assertTrue((explicit / 'report.json').exists())
        for file in persistent:
            self.assertEqual(file.read_text(), 'persistent')


class ExportRetention(unittest.TestCase):
    setUp = Retention.setUp
    tearDown = Retention.tearDown
    def make_staging(self, output, number):
        staging = output.with_name(output.name + '.staging-' + str(number))
        staging.mkdir()
        (staging / 'resource-manifest.json').write_text('{}')
        (staging / 'data').write_text(str(number))
        return staging

    def test_multiple_exports_and_failed_rename_recover(self):
        output = self.root / 'out/agent/wechat-export'
        output.parent.mkdir(parents=True)
        lock = output.parent / '.wechat-export.lock'
        lock.touch()
        project = self.root / 'Project'
        for n in range(5):
            staging = self.make_staging(output, n)
            with storage.exclusive(lock):
                publication.publish(self.s, project, staging, output, lock)
            publication.prune(self.s, project)
        self.assertEqual(len(list(output.parent.glob('wechat-export.previous-*'))), 1)
        self.assertEqual((output / 'data').read_text(), '4')
        staging = self.make_staging(output, 6)
        rename = Path.rename
        def failure(p, dest):
            if p == staging:
                raise OSError('fixture publish failure')
            return rename(p, dest)
        with storage.exclusive(lock), mock.patch.object(Path, 'rename', failure):
            with self.assertRaises(OSError):
                publication.publish(self.s, project, staging, output, lock)
        self.assertEqual((output / 'data').read_text(), '4')
        with storage.exclusive(lock):
            publication.recover(self.s, output)
        self.assertFalse(publication.journal_path(output).exists())
        self.s.policy['failureDays'] = 0
        self.s.collect(kind='wechat-staging')
        self.assertFalse(staging.exists())
        self.assertEqual((output / 'data').read_text(), '4')

    def test_interrupted_export_only_backup_restored(self):
        output = self.root / 'out/agent/wechat-export'
        output.parent.mkdir(parents=True)
        backup = output.with_name(output.name + '.previous-123')
        backup.mkdir()
        (backup / 'data').write_text('old')
        storage.atomic(publication.journal_path(output), dict(output=str(output), backup=str(backup)))
        publication.recover(self.s, output)
        self.assertEqual((output / 'data').read_text(), 'old')

    def test_complete_export_pipeline_with_sdk_and_legacy_backup(self):
        spec = importlib.util.spec_from_file_location('export_wechat', REPO / 'Tools/WeChat/export_wechat.py')
        exporter = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(exporter)
        project = self.root / 'Project'
        shutil.copytree(REPO / 'Examples/RenderingTests/Assets/WeChatSample', project / 'Assets/WeChatSample')
        config = project / 'Config/WeChat.json'
        config.parent.mkdir()
        shutil.copy2(REPO / 'Project/Config/WeChat.json', config)
        files = {'out/agent/wechat-wasm-release/hegame.js': 'js',
                 'out/agent/wechat-wasm-release/hegame.wasm': 'wasm',
                 'out/agent/wechat/shaders/vertex.glsl': 'vertex',
                 'out/agent/wechat/shaders/fragment.glsl': 'fragment',
                 'out/agent/wechat/deps/sokol-LICENSE': 'license',
                 'out/agent/wechat/deps/emsdk/upstream/install/emscripten/LICENSE': 'sdk license',
                 'Engine/Source/ThirdParty/entt/LICENSE': 'license',
                 'Engine/Source/ThirdParty/glm/copying.txt': 'license',
                 'Engine/Source/ThirdParty/json/LICENSE.MIT': 'license',
                 'Engine/Source/ThirdParty/spdlog/LICENSE': 'license',
                 'Engine/Source/ThirdParty/crossguid/LICENSE': 'license',
                 'Engine/Source/ThirdParty/stb_image/stb_image.h': 'ALTERNATIVE A - MIT License\nfixture',
                 'Engine/Source/Runtime/Platform/WeChat/game.js': 'game',
                 'Engine/Source/Runtime/Platform/WeChat/host.js': 'host',
                 'Engine/Source/Runtime/Platform/WeChat/boot-diagnostics.js': 'diagnostics',
                 'Tools/WeChat/browser-harness.js': 'harness'}
        for relative, contents in files.items():
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        shaders = self.root / 'out/agent/wechat/shaders'
        binary = self.root / 'out/agent/wechat-wasm-release'
        storage.atomic(shaders / 'shader-manifest.json', dict(profile=dict(name=exporter.PROFILE), cacheKey='fixture', artifacts={s: dict(glsl=exporter.sha(shaders / (s + '.glsl'))) for s in ('vertex', 'fragment')}))
        storage.atomic(binary / 'build-receipt.json', dict(shaderCacheKey='fixture', inputs={}, artifacts={n: exporter.sha(binary / n) for n in ('hegame.js', 'hegame.wasm')}))
        output = self.root / 'out/agent/wechat-export'
        legacy = output.with_name(output.name + '.previous-unowned')
        legacy.mkdir()
        (legacy / 'precious').write_text('keep')
        for _ in range(4):
            exporter.export(self.root, project, config, output)
        backups = list(output.parent.glob('wechat-export.previous-*'))
        self.assertEqual(len(backups), 2)  # One confirmed backup plus the unowned legacy directory.
        self.assertTrue((legacy / 'precious').exists())
        self.assertTrue((self.root / 'out/agent/wechat/deps/emsdk/upstream/install/emscripten/LICENSE').exists())
        manifest = storage.read(output / 'resource-manifest.json')
        for name, file in manifest['files'].items():
            self.assertEqual(exporter.sha(output / name), file['sha256'])

    def test_first_publish_interruption_requires_verified_staging(self):
        output = self.root / 'out/agent/wechat-export'
        output.parent.mkdir(parents=True)
        lock = output.parent / '.wechat-export.lock'
        lock.touch()
        staging = self.make_staging(output, 0)
        storage.atomic(staging / 'resource-manifest.json', dict(files={'data': dict(bytes=1, sha256=publication.hashlib.sha256(b'0').hexdigest())}))
        with storage.exclusive(lock), mock.patch.object(Path, 'rename', side_effect=OSError('initial publish interrupted')):
            with self.assertRaises(OSError):
                publication.publish(self.s, self.root / 'Project', staging, output, lock)
        (staging / 'data').write_text('corrupt')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            publication.recover(self.s, output)
        self.assertTrue(publication.journal_path(output).exists())
        self.assertTrue(staging.exists())
        (staging / 'data').write_text('0')
        publication.recover(self.s, output)
        self.assertEqual((output / 'data').read_text(), '0')
        self.assertFalse(publication.journal_path(output).exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
