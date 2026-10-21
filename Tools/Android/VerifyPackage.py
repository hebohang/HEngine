"""Check the actual Android archive, native ABI and 16 KB ELF alignment."""
import hashlib
import argparse
import json
import struct
from pathlib import Path
from zipfile import ZipFile


def verify_elf(data, name):
    if len(data) < 64 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError(f"{name}: expected little-endian ELF64")
    if struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError(f"{name}: expected AArch64")
    offset = struct.unpack_from("<Q", data, 32)[0]
    stride, count = struct.unpack_from("<HH", data, 54)
    if stride < 56 or not count or offset + stride * count > len(data):
        raise ValueError(f"{name}: invalid ELF program header table")
    loads = 0
    for index in range(count):
        kind, flags, file_offset, address, _, file_size, memory_size, alignment = struct.unpack_from("<IIQQQQQQ", data, offset + index * stride)
        if kind != 1:
            continue
        loads += 1
        if alignment < 16384 or alignment & (alignment - 1) or file_offset % 16384 != address % 16384:
            raise ValueError(f"{name}: PT_LOAD is not 16 KB compatible")
        if file_size > memory_size or file_offset + file_size > len(data):
            raise ValueError(f"{name}: invalid PT_LOAD bounds")
    if not loads:
        raise ValueError(f"{name}: no loadable segments")


def verify(path, expected_native_sha256=None, expected_player_fingerprint=None):
    prefix = "base/" if path.suffix == ".aab" else ""
    with ZipFile(path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            raise ValueError("Duplicate archive entries")
        for required in ("assets/content/Game.pak", "assets/content/Game.manifest.json", "assets/content/android-content.json", "lib/arm64-v8a/libhegame.so"):
            if prefix + required not in names:
                raise ValueError("Android package is incomplete: " + required)
        libraries = [name for name in names if name.startswith(prefix + "lib/") and name.endswith(".so")]
        if expected_native_sha256:
            native_hash = hashlib.sha256(archive.read(prefix + "lib/arm64-v8a/libhegame.so")).hexdigest()
            if native_hash != expected_native_sha256:
                raise ValueError("Android native player differs from the verified template/input")
        if expected_player_fingerprint:
            manifest = json.loads(archive.read(prefix + "assets/content/Game.manifest.json"))
            if manifest.get('Player', {}).get('RuntimeFingerprint') != expected_player_fingerprint:
                raise ValueError("Android content targets a different native player")
        for name in libraries:
            if not name.startswith(prefix + "lib/arm64-v8a/"):
                raise ValueError("Unexpected Android ABI: " + name)
            verify_elf(archive.read(name), name)
        content = json.loads(archive.read(prefix + "assets/content/android-content.json"))
        if content.get("schema") != 1 or not content.get("files"):
            raise ValueError("Invalid Android content manifest")
        entries = content["files"]
        paths = [file["path"] for file in entries]
        expected = {name.removeprefix(prefix + "assets/content/") for name in names
                    if name.startswith(prefix + "assets/content/") and not name.endswith("/")
                    and name != prefix + "assets/content/android-content.json"}
        if len(paths) != len(set(paths)) or set(paths) != expected:
            raise ValueError("Content manifest must cover every payload exactly once")
        if any(path.startswith(("/", "\\")) or "\\" in path or ":" in path
               or any(part in ("", ".", "..") for part in path.split("/")) for path in paths):
            raise ValueError("Invalid content path")
        identity = hashlib.sha256("".join(f"{file['path']}:{file['sha256']}\n" for file in entries).encode()).hexdigest()
        if content.get("identity") != identity:
            raise ValueError("Content identity mismatch")
        for file in entries:
            name = prefix + "assets/content/" + file["path"]
            digest = hashlib.sha256()
            with archive.open(name) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            if digest.hexdigest() != file["sha256"]:
                raise ValueError("Content hash mismatch: " + name)
        if path.suffix == ".aab" and not any(name.startswith("META-INF/") and name.endswith(".SF") for name in names):
            raise ValueError("Unsigned Android app bundle")
    print(f"PASS: Android ARM64 package, {len(libraries)} native libraries, 16 KB ELF segments, content hashes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('path', type=Path)
    parser.add_argument('--elf', action='store_true')
    parser.add_argument('--native-sha256')
    parser.add_argument('--runtime-fingerprint')
    args = parser.parse_args()
    if args.elf:
        path = args.path
        verify_elf(path.read_bytes(), str(path))
        print("PASS: AArch64 ELF, 16 KB load segments")
    else:
        verify(args.path, args.native_sha256, args.runtime_fingerprint)
