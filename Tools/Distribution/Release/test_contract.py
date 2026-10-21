import copy
import json
from pathlib import Path
import struct
import tempfile
import unittest

from contract import RUNNERS, audit_sdk, audit_workflow, free_context, pe_architecture, plan


def image(machine=0xAA64, managed=False, flags=1):
    data = bytearray(1024)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, machine, 1)
    optional_size = 224 if machine == 0x14C else 240
    struct.pack_into('<H', data, 148, optional_size)
    struct.pack_into('<H', data, 152, 0x10B if machine == 0x14C else 0x20B)
    if managed:
        directory = 96 if machine == 0x14C else 112
        struct.pack_into('<II', data, 152 + directory + 14 * 8, 4096, 72)
        struct.pack_into('<IIII', data, 152 + optional_size + 8, 512, 4096, 512, 512)
        struct.pack_into('<I', data, 528, flags)
    return data


class FreeReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.event = {'repository': {'private': False, 'visibility': 'public', 'full_name': 'example/engine'}}
        self.env = dict(GITHUB_EVENT_NAME='workflow_dispatch', GITHUB_REPOSITORY='example/engine',
                        HE_RUNNER_ENVIRONMENT='github-hosted', HE_RUNNER_LABEL=RUNNERS['arm64'],
                        RUNNER_OS='Windows', RUNNER_ARCH='ARM64')

    def tearDown(self):
        self.temp.cleanup()

    def test_public_manual_standard_runner_allowed(self):
        free_context(self.event, self.env, 'arm64')

    def test_workflow_prevents_allocation_on_private_repositories(self):
        workflow = (Path(__file__).resolve().parents[3] / '.github/workflows/release_windows.yml').read_text()
        audit_workflow(workflow)
        for changed in (workflow.replace('private == false', 'private == true', 1),
                        workflow.replace('runs-on: ubuntu-24.04', 'runs-on: paid-linux', 1),
                        workflow.replace('  workflow_dispatch:', '  push:', 1),
                        workflow.replace("visibility == 'public'", "visibility == 'public' || true", 1),
                        workflow.replace('matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}', 'matrix: ${{ inputs.matrix }}')):
            with self.assertRaises(ValueError):
                audit_workflow(changed)

    def test_private_and_missing_visibility_rejected(self):
        for changes in ({'private': True}, {'private': 'false'}, {'visibility': 'private'}, {'visibility': None}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                event = copy.deepcopy(self.event)
                event['repository'].update(changes)
                free_context(event, self.env, 'arm64')

    def test_nonmanual_events_rejected(self):
        for event in ('push', 'pull_request', 'schedule', 'workflow_run', 'pull_request_target'):
            with self.subTest(event=event), self.assertRaises(ValueError):
                free_context(self.event, dict(self.env, GITHUB_EVENT_NAME=event), 'arm64')

    def test_paid_custom_selfhosted_and_wrong_arch_rejected(self):
        for values in ({'HE_RUNNER_LABEL': 'windows-8-core'}, {'HE_RUNNER_LABEL': 'self-hosted'},
                       {'HE_RUNNER_ENVIRONMENT': 'self-hosted'}, {'RUNNER_ARCH': 'X64'},
                       {'RUNNER_OS': 'Linux'}, {'GITHUB_REPOSITORY': 'another/repository'}):
            with self.subTest(values=values), self.assertRaises(ValueError):
                free_context(self.event, dict(self.env, **values), 'arm64')

    def test_matrix_has_only_fixed_free_labels(self):
        (self.root / 'CMakeLists.txt').write_text('project(HEngine VERSION 1.2.3 LANGUAGES C CXX)')
        result = plan(self.root, 'both')
        self.assertEqual(result['version'], '1.2.3')
        self.assertEqual(result['architectures'], ['x64', 'arm64'])
        self.assertEqual([entry['runner'] for entry in result['matrix']['include']], list(RUNNERS.values()))
        for invalid in ('windows-latest', 'self-hosted', 'x64; exit 0', ''):
            with self.assertRaises(ValueError):
                plan(self.root, invalid)

    def test_architecture_specific_managed_images_are_not_anycpu(self):
        path = self.root / 'program.exe'
        for machine, managed, flags, expected in ((0xAA64, False, 0, 0xAA64),
                (0x8664, True, 1, 0x8664), (0x14C, True, 1, None),
                (0x14C, True, 3, 0x14C), (0x14C, True, 0x20001, 0x14C)):
            with self.subTest(machine=machine, flags=flags):
                path.write_bytes(image(machine, managed, flags))
                self.assertEqual(pe_architecture(path), expected)

    def test_malformed_binary_rejected(self):
        path = self.root / 'broken.dll'
        for payload in (b'', b'not a dll', image()[:160]):
            with self.subTest(size=len(payload)), self.assertRaises(ValueError):
                path.write_bytes(payload)
                pe_architecture(path)

    def test_arm_sdk_cannot_hide_x64_python_extension(self):
        descriptor = dict(Platform='Windows-arm64', EngineVersion='1.2.3')
        (self.root / 'HEngine.distribution.json').write_text(json.dumps(descriptor))
        for parent in ('ExportTemplates', 'DevelopmentTemplates'):
            directory = self.root / parent / 'Windows-arm64'
            directory.mkdir(parents=True)
            (directory / 'Template.json').write_text(json.dumps(descriptor))
        (self.root / 'HEngineEditor.exe').write_bytes(image())
        self.assertTrue(audit_sdk(self.root, 'arm64')['pass'])
        (self.root / 'python_extension.pyd').write_bytes(image(0x8664))
        with self.assertRaisesRegex(ValueError, 'python_extension.pyd'):
            audit_sdk(self.root, 'arm64')

    def test_testing_only_warp_cannot_enter_release(self):
        descriptor = dict(Platform='Windows-arm64', EngineVersion='1.2.3')
        (self.root / 'HEngine.distribution.json').write_text(json.dumps(descriptor))
        for parent in ('ExportTemplates', 'DevelopmentTemplates'):
            directory = self.root / parent / 'Windows-arm64'
            directory.mkdir(parents=True)
            (directory / 'Template.json').write_text(json.dumps(descriptor))
        (self.root / 'HEngineEditor.exe').write_bytes(image())
        (self.root / 'd3d10warp.dll').write_bytes(image())
        with self.assertRaisesRegex(ValueError, 'Test-only WARP'):
            audit_sdk(self.root, 'arm64')


if __name__ == '__main__':
    unittest.main()
