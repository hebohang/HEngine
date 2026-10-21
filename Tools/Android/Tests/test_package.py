"""Negative validation contracts for distributable Android archives."""
import importlib.util
import struct
import hashlib
import json
import contextlib
import io
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from zipfile import ZipFile

spec = importlib.util.spec_from_file_location("verify_android", Path(__file__).parents[1] / "VerifyPackage.py")
validator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(validator)


def elf(alignment=16384, machine=183):
    data = bytearray(120)
    data[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<H", data, 18, machine)
    struct.pack_into("<Q", data, 32, 64)
    struct.pack_into("<HH", data, 54, 56, 1)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0, 0, 0, 120, 120, alignment)
    return data


class PackageTests(unittest.TestCase):
    def test_arm64_16k(self):
        validator.verify_elf(elf(), "test.so")

    def test_4k_rejected(self):
        with self.assertRaisesRegex(ValueError, "16 KB"):
            validator.verify_elf(elf(4096), "test.so")

    def test_wrong_abi_rejected(self):
        with self.assertRaisesRegex(ValueError, "AArch64"):
            validator.verify_elf(elf(machine=62), "test.so")

    def test_truncated_rejected(self):
        with self.assertRaises(ValueError):
            validator.verify_elf(elf()[:100], "test.so")

    def test_invalid_segment_bounds_rejected(self):
        data = elf()
        struct.pack_into("<Q", data, 64 + 32, 121)
        with self.assertRaisesRegex(ValueError, "bounds"):
            validator.verify_elf(data, "test.so")

    def check_archive(self, corrupt=False, omit=False, native_hash=None, fingerprint=None):
        payloads = {"Game.pak": b"test cooked content", "Game.manifest.json": json.dumps({'Player': {'RuntimeFingerprint': 'a' * 64}}).encode()}
        entries = [{"path": name, "sha256": hashlib.sha256(data).hexdigest()} for name, data in payloads.items()]
        if omit: entries = entries[:1]
        identity = hashlib.sha256("".join(f"{file['path']}:{file['sha256']}\n" for file in entries).encode()).hexdigest()
        with TemporaryDirectory() as directory:
            path = Path(directory) / "test.apk"
            with ZipFile(path, "w") as archive:
                archive.writestr("lib/arm64-v8a/libhegame.so", elf())
                for name, data in payloads.items():
                    archive.writestr("assets/content/" + name, b"corrupt" if corrupt else data)
                archive.writestr("assets/content/android-content.json", json.dumps({"schema": 1, "identity": identity, "files": entries}))
            with contextlib.redirect_stdout(io.StringIO()): validator.verify(path, native_hash, fingerprint)

    def test_exact_precompiled_player(self):
        self.check_archive(native_hash=hashlib.sha256(elf()).hexdigest(), fingerprint='a' * 64)

    def test_replaced_precompiled_player(self):
        with self.assertRaisesRegex(ValueError, 'native player differs'):
            self.check_archive(native_hash='b' * 64)

    def test_other_player_content(self):
        with self.assertRaisesRegex(ValueError, 'different native player'):
            self.check_archive(fingerprint='b' * 64)

    def test_complete_archive(self):
        self.check_archive()

    def test_content_corruption_rejected(self):
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.check_archive(corrupt=True)

    def test_unverified_payload_rejected(self):
        with self.assertRaisesRegex(ValueError, "every payload"):
            self.check_archive(omit=True)

    def test_missing_payload_rejected(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "test.apk"
            with ZipFile(path, "w") as archive:
                archive.writestr("lib/arm64-v8a/libhegame.so", elf())
            with self.assertRaisesRegex(ValueError, "incomplete"):
                validator.verify(path)


if __name__ == "__main__":
    unittest.main()
