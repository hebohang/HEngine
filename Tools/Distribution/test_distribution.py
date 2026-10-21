"""Layout and archive contracts; fixtures are not engine or device validation."""
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'Scripting'))
from GenerateReference import generate
SPEC = importlib.util.spec_from_file_location('release_archive', HERE / 'Archive-Release.py')
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='HEngine release layout ')
        self.root = Path(self.temporary.name) / 'engine'
        contract = json.loads((HERE / 'RequiredFiles.json').read_text(encoding='utf-8'))
        for relative in contract['files']:
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture', encoding='utf-8')
        self.manifest = dict(SchemaVersion=1, EngineVersion='1.1.0', Platform='Windows-x64', Editor='HEngineEditor.exe')
        self.write_manifest()
        xml = self.root / 'Scripting/CSharp/HEngine.ScriptCore.xml'
        dll = xml.with_suffix('.dll')
        xml.write_text('<doc><members><member name="T:HEngine.Audio"><summary>Audio.</summary></member><member name="T:HEngine.GameSettings"><summary>Settings.</summary></member></members></doc>')
        api = self.root / 'Docs/Api'
        (api / 'api-index.json').unlink()
        (api / 'csharp-signatures.json').write_text(json.dumps({
            'schema': 1, 'assemblySha256': hashlib.sha256(dll.read_bytes()).hexdigest(),
            'xmlSha256': hashlib.sha256(xml.read_bytes()).hexdigest(),
            'declarations': {'T:HEngine.Audio': 'public static class Audio', 'T:HEngine.GameSettings': 'public static class GameSettings'}}))
        generate(xml, api)
        for directory in ('', 'ExportTemplates/Windows-x64/Player', 'DevelopmentTemplates/Windows-x64/Player'):
            root = self.root / directory
            root.mkdir(parents=True, exist_ok=True)
            (root / 'HEngine.Runtime.json').write_text(json.dumps(dict(schema=1, fsr=False, dx12=False, vulkan=True, oidn=False)))

    def tearDown(self):
        self.temporary.cleanup()

    def write_manifest(self):
        (self.root / 'HEngine.distribution.json').write_text(json.dumps(self.manifest), encoding='utf-8')

    def check(self):
        # PowerShell 7's module path is incompatible with Windows PowerShell
        # when inherited through Python; let the child use its own defaults.
        environment = {key: value for key, value in os.environ.items()
                       if key.upper() != 'PSMODULEPATH'}
        return subprocess.run(['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                               str(HERE / 'Test-Distribution.ps1'), '-Engine', str(self.root), '-Json'],
                              cwd=self.temporary.name, env=environment,
                              capture_output=True, text=True, timeout=30)

    def test_layout_without_project_or_native_toolchain(self):
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['evidence'], 'static-check')

    def test_missing_transitive_dependency_is_an_error(self):
        (self.root / 'Tools/Dependencies/Environment.ps1').unlink()
        (self.root / 'Tools/Content/content_sync.py').unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Tools/Dependencies/Environment.ps1', result.stdout)
        self.assertIn('Tools/Content/content_sync.py', result.stdout)

    def test_arm64_layout_requires_arm64_esbuild(self):
        self.manifest['Platform'] = 'Windows-arm64'
        self.write_manifest()
        for directory in ('ExportTemplates', 'DevelopmentTemplates'):
            (self.root / directory / 'Windows-x64').rename(self.root / directory / 'Windows-arm64')
        esbuild = self.root / 'Tools/Scripting/node_modules/@esbuild'
        (esbuild / 'win32-x64').rename(esbuild / 'win32-arm64')
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        (esbuild / 'win32-arm64/esbuild.exe').unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('@esbuild/win32-arm64/esbuild.exe', result.stdout)

    def test_missing_gameplay_documentation_is_an_error(self):
        for name in ('Docs/Api/api-index.json', 'Docs/ScriptingGuide.md', 'Examples/ScriptingBasics/Feedback.cs'):
            (self.root / name).unlink()
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Docs/Api/api-index.json', result.stdout)

    def test_reference_from_another_script_assembly_is_rejected(self):
        (self.root / 'Scripting/CSharp/HEngine.ScriptCore.dll').write_bytes(b'changed assembly')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('C# API reference does not match', result.stdout)

    def test_project_and_local_binding_are_rejected(self):
        (self.root / 'Project').mkdir()
        (self.root / 'HEngine.workspace.local.json').write_text('{}')
        self.assertNotEqual(self.check().returncode, 0)

    def test_invalid_manifest_is_rejected(self):
        self.manifest['Editor'] = '../Other.exe'
        self.write_manifest()
        self.assertNotEqual(self.check().returncode, 0)

    def test_native_source_is_not_part_of_binary_sdk(self):
        source = self.root / 'Engine/Source/Runtime/Platform/WeChat/Runtime.cpp'
        source.write_text('// fixture', encoding='utf-8')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Binary SDK contains native source', result.stdout)

    def test_platform_module_is_required(self):
        (self.root / 'Engine/Source/Runtime/Platform/WeChat/ManagedRuntime.cjs').unlink()
        self.assertNotEqual(self.check().returncode, 0)

    def test_dynamic_runtime_cannot_be_declared_without_payload(self):
        path = self.root / 'HEngine.Runtime.json'
        path.write_text(json.dumps(dict(schema=1, fsr=True, dx12=False, vulkan=True, oidn=False)))
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('amd_fidelityfx_vk.dll', result.stdout)

    def test_shipping_cannot_enable_offline_denoiser(self):
        path = self.root / 'ExportTemplates/Windows-x64/Player/HEngine.Runtime.json'
        path.write_text(json.dumps(dict(schema=1, fsr=False, dx12=True, vulkan=True, oidn=True)))
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('must not enter Shipping', result.stdout)

    def test_modified_vendor_runtime_is_rejected(self):
        (self.root / 'HEngine.Runtime.json').write_text(json.dumps(
            dict(schema=1, fsr=True, dx12=False, vulkan=True, oidn=False)))
        (self.root / 'amd_fidelityfx_vk.dll').write_bytes(b'wrong vendor binary')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checksum mismatch', result.stdout)

    def test_disabled_backend_cannot_leave_a_stale_runtime(self):
        (self.root / 'amd_fidelityfx_dx12.dll').write_bytes(b'stale backend')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Disabled FSR backend', result.stdout)

    def test_oidn_gpu_backend_cannot_leak_into_cpu_plugin(self):
        (self.root / 'HEngine.Runtime.json').write_text(json.dumps(
            dict(schema=1, fsr=False, dx12=False, vulkan=True, oidn=True)))
        plugin = self.root / 'Plugins/OfflineReferenceRenderer'
        plugin.mkdir(parents=True)
        (plugin / 'OpenImageDenoise_device_cuda.dll').write_bytes(b'unselected GPU backend')
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Unexpected OIDN backend', result.stdout)

    def test_engine_only_cli_does_not_require_game(self):
        release = Path(self.temporary.name)
        self.root.rename(release / 'HEngine-1.1.0-Windows-x64')
        result = subprocess.run([sys.executable, str(HERE / 'Archive-Release.py'),
                                 '--release', str(release), '--engine-only'],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        artifacts = json.loads((release / 'artifacts.json').read_text(encoding='utf-8'))
        self.assertEqual([item['file'] for item in artifacts['artifacts']],
                         ['HEngine-1.1.0-Windows-x64.zip'])

    def test_archive_does_not_traverse_excluded_caches(self):
        (self.root / 'out/deep-cache').mkdir(parents=True)
        original = ARCHIVE.os.scandir

        def scan(path):
            if 'out' in Path(path).parts:
                raise AssertionError('Excluded cache was traversed')
            return original(path)

        with patch.object(ARCHIVE.os, 'scandir', scan):
            ARCHIVE.archive(self.root, Path(self.temporary.name), 'pruned')

    def test_archive_preserves_tool_bin_and_excludes_local_state(self):
        (self.root / 'Saved').mkdir()
        (self.root / 'Saved/local.json').write_text('{}')
        (self.root / 'HEngine.workspace.local.json').write_text('{}')
        output = Path(self.temporary.name)
        ARCHIVE.archive(self.root, output, 'engine')
        with zipfile.ZipFile(output / 'engine.zip') as bundle:
            self.assertIn('engine/Tools/Scripting/node_modules/esbuild/bin/esbuild', bundle.namelist())
            self.assertNotIn('engine/Saved/local.json', bundle.namelist())
            self.assertNotIn('engine/HEngine.workspace.local.json', bundle.namelist())


if __name__ == '__main__':
    unittest.main()
