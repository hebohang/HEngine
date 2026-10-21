"""Configure-only FSR architecture/provenance tests; fixtures are not executable code."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


class FsrCmakeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='he-fsr-cmake-')
        self.root = Path(self.temp.name)
        self.repo = Path(__file__).resolve().parents[2]
        for directory in ('ffx-api/bin', 'ffx-api/include/ffx_api', 'sdk', 'fsr-build'):
            (self.root / directory).mkdir(parents=True)
        (self.root / 'ffx-api/include/ffx_api/ffx_api.h').write_text('// fixture')
        (self.root / 'sdk/LICENSE.txt').write_text('fixture')
        self.dll = self.root / 'ffx-api/bin/amd_fidelityfx_dx12.dll'
        data = bytearray(256)
        data[:2] = b'MZ'
        struct.pack_into('<I', data, 60, 128)
        data[128:132] = b'PE\0\0'
        struct.pack_into('<H', data, 132, 0xaa64)
        self.dll.write_bytes(data)
        pin = json.loads((self.repo / 'Tools/Dependencies/native-runtime.arm64.lock.json').read_text())
        self.receipt = dict(schema=1, architecture='arm64', backend='dx12',
                            sourceArchiveSha256=pin['fsrSourceArchiveSha256'],
                            runtimeSha256=hashlib.sha256(data).hexdigest())
        self.script = self.root / 'check.cmake'
        self.script.write_text(f'''
set(WIN32 TRUE)
set(CMAKE_SOURCE_DIR "{self.repo.as_posix()}")
set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID ARM64)
include("{self.repo.as_posix()}/CMake/WindowsArchitecture.cmake")
set(HE_WINDOWS_ARM64_ROOT "{self.root.as_posix()}")
set(HE_FSR31_ROOT "{self.root.as_posix()}")
set(HE_RENDER_DX12 ON)
set(HE_RENDER_VULKAN ON)
include("{self.repo.as_posix()}/CMake/FidelityFX.cmake")
if(NOT HE_FSR31_DX12 OR HE_FSR31_VULKAN OR NOT HE_RENDER_VULKAN)
  message(FATAL_ERROR "Incorrect ARM64 renderer/FSR backend split")
endif()
''')

    def tearDown(self):
        self.temp.cleanup()

    def configure(self):
        (self.root / 'fsr-build/HEngine.FsrBuild.json').write_text(json.dumps(self.receipt))
        return subprocess.run([shutil.which('cmake'), '-P', str(self.script)], capture_output=True, text=True)

    def test_dx12_fsr_keeps_vulkan_renderer_available(self):
        result = self.configure()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_modified_runtime_rejected(self):
        self.dll.write_bytes(self.dll.read_bytes() + b'modified')
        self.assertIn('receipt mismatch', self.configure().stderr)

    def test_wrong_source_pin_rejected(self):
        self.receipt['sourceArchiveSha256'] = '0' * 64
        self.assertNotEqual(self.configure().returncode, 0)

    def test_x64_runtime_rejected_even_with_matching_receipt(self):
        data = bytearray(self.dll.read_bytes())
        struct.pack_into('<H', data, 132, 0x8664)
        self.dll.write_bytes(data)
        self.receipt['runtimeSha256'] = hashlib.sha256(data).hexdigest()
        self.assertIn('not Windows arm64', self.configure().stderr)

    def test_incomplete_source_sdk_rejected(self):
        (self.root / 'ffx-api/include/ffx_api/ffx_api.h').unlink()
        self.assertIn('Incomplete ARM64', self.configure().stderr)


if __name__ == '__main__':
    unittest.main()
