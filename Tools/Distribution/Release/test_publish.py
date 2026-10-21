import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from publish import digest, prepare_publication, require_tag_commit, validate_downloads, validate_existing_release


class PublishGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.commit = 'a' * 40
        for arch in ('x64', 'arm64'):
            directory = self.root / f'sdk-{arch}'
            directory.mkdir()
            path = directory / f'HEngine-1.2.3-Windows-{arch}.zip'
            capabilities = dict(architecture=arch, fsr=True, oidn=arch == 'x64', dx12=True, vulkan=True,
                                fsrDx12=True, fsrVulkan=arch == 'x64')
            receipt = dict(architecture=arch, version='1.2.3', sourceCommit=self.commit,
                           nativeCapabilities=capabilities, **{'pass': True},
                           passedChecks=['layout', 'native-architecture', 'fsr-native-dispatch', 'editor-startup', 'api-gameplay',
                                         'windows-export', 'android-export', 'web-export', 'wechat-export'],
                           limitations=['No physical device validation'],
                           workflowRun='https://github.com/example/engine/actions/runs/123',
                           commands=[{'stage': 'fixture-check', 'exitCode': 0}])
            with zipfile.ZipFile(path, 'w') as bundle:
                bundle.writestr(f'{path.stem}/HEngine.distribution.json', json.dumps(dict(Platform=f'Windows-{arch}', EngineVersion='1.2.3')))
                bundle.writestr(f'{path.stem}/HEngine.Runtime.json', json.dumps(capabilities))
                bundle.writestr(f'{path.stem}/Docs/Release/RELEASE_PROVENANCE.json', json.dumps(receipt))
            (directory / 'artifacts.json').write_text(json.dumps({'version': '1.2.3', 'artifacts': [
                dict(file=path.name, bytes=path.stat().st_size, sha256=digest(path))]}))
            (directory / 'result.json').write_text(json.dumps(receipt))

    def tearDown(self):
        self.temp.cleanup()

    def test_both_successful_sdk_receipts_required(self):
        assets, receipts = validate_downloads(self.root, ['x64', 'arm64'], '1.2.3', self.commit)
        self.assertEqual(len(assets), 2)
        self.assertEqual(len(receipts), 2)
        (self.root / 'sdk-arm64/result.json').unlink()
        with self.assertRaises(FileNotFoundError):
            validate_downloads(self.root, ['x64', 'arm64'], '1.2.3', self.commit)

    def test_corrupt_download_rejected_before_publication(self):
        path = self.root / 'sdk-x64/HEngine-1.2.3-Windows-x64.zip'
        with path.open('ab') as stream:
            stream.write(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'differs'):
            validate_downloads(self.root, ['x64'], '1.2.3', self.commit)

    def test_only_sdk_zips_published_with_complete_notes_and_unchanged_bytes(self):
        for selection in (['x64'], ['x64', 'arm64']):
            assets, receipts = validate_downloads(self.root, selection, '1.2.3', self.commit)
            before = {path: digest(path) for path, _ in assets}
            files, notes = prepare_publication(self.root / 'publication', 'example/engine', '1.2.3', self.commit, assets, receipts)
            self.assertEqual(files, [path for path, _ in assets])
            self.assertTrue(all(path.suffix == '.zip' for path in files))
            self.assertEqual(before, {path: digest(path) for path in files})
            body = notes.read_text()
            for path, entry in assets:
                self.assertIn(path.name, body)
                self.assertIn(entry['sha256'], body)
                self.assertIn(f'{entry["bytes"]:,}', body)
            for required in (self.commit, 'No physical device validation', 'actions/runs/123',
                             'fsr-native-dispatch', 'Docs/Release/RELEASE_PROVENANCE.json', 'Source code'):
                self.assertIn(required, body)

    def test_sdk_provenance_must_match_external_receipt(self):
        path = self.root / 'sdk-arm64/result.json'
        receipt = json.loads(path.read_text())
        receipt['commands'] = [{'stage': 'different-validation'}]
        path.write_text(json.dumps(receipt))
        with self.assertRaisesRegex(ValueError, 'provenance differs'):
            validate_downloads(self.root, ['arm64'], '1.2.3', self.commit)

    def test_source_or_architecture_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, 'different source'):
            validate_downloads(self.root, ['arm64'], '1.2.3', 'b' * 40)
        with self.assertRaises(ValueError):
            validate_downloads(self.root, ['arm64', 'arm64'], '1.2.3', self.commit)

    def test_missing_runtime_gate_rejected(self):
        path = self.root / 'sdk-arm64/result.json'
        data = json.loads(path.read_text())
        data['passedChecks'].remove('editor-startup')
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'missing required'):
            validate_downloads(self.root, ['arm64'], '1.2.3', self.commit)

    def test_published_release_and_unrelated_draft_never_overwritten(self):
        marker = '<!-- source -->'
        validate_existing_release([], marker)
        validate_existing_release([dict(draft=True, body=marker)], marker)
        for release in (dict(draft=False, body=marker), dict(draft=True, body='another build')):
            with self.assertRaisesRegex(ValueError, 'never overwrites'):
                validate_existing_release([release], marker)

    def test_missing_fsr_execution_gate_rejected(self):
        path = self.root / 'sdk-arm64/result.json'
        data = json.loads(path.read_text())
        data['passedChecks'].remove('fsr-native-dispatch')
        path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'missing required'):
            validate_downloads(self.root, ['arm64'], '1.2.3', self.commit)

    def test_arm64_optional_feature_contract_cannot_be_misreported(self):
        directory = self.root / 'sdk-arm64'
        path = directory / 'HEngine-1.2.3-Windows-arm64.zip'
        receipt_path = directory / 'result.json'
        for field in ('oidn', 'fsrVulkan'):
            receipt = json.loads(receipt_path.read_text())
            receipt['nativeCapabilities'][field] = True
            with zipfile.ZipFile(path, 'w') as bundle:
                bundle.writestr(f'{path.stem}/HEngine.distribution.json', json.dumps(dict(Platform='Windows-arm64', EngineVersion='1.2.3')))
                bundle.writestr(f'{path.stem}/HEngine.Runtime.json', json.dumps(receipt['nativeCapabilities']))
            receipt_path.write_text(json.dumps(receipt))
            (directory / 'artifacts.json').write_text(json.dumps({'version': '1.2.3', 'artifacts': [
                dict(file=path.name, bytes=path.stat().st_size, sha256=digest(path))]}))
            with self.assertRaisesRegex(ValueError, 'unsupported'):
                validate_downloads(self.root, ['arm64'], '1.2.3', self.commit)
            receipt['nativeCapabilities'][field] = False
            receipt_path.write_text(json.dumps(receipt))

    def test_tag_mismatch_cannot_be_force_updated(self):
        with patch('publish.api', return_value={'object': {'type': 'commit', 'sha': 'b' * 40}}):
            with self.assertRaisesRegex(ValueError, 'never moved'):
                require_tag_commit('example/engine', 'v1.2.3', self.commit)
        with patch('publish.api', side_effect=[{'object': {'type': 'tag', 'sha': 'c' * 40}},
                                              {'object': {'type': 'commit', 'sha': self.commit}}]):
            self.assertTrue(require_tag_commit('example/engine', 'v1.2.3', self.commit))


if __name__ == '__main__':
    unittest.main()
