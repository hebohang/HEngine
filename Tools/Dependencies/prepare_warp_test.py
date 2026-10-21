"""Stage Microsoft's hash-pinned WARP for an owned test EXE, never a shipping SDK."""
import argparse
import json
from pathlib import Path
import shutil
import struct

from prepare_arm64 import acquire


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--architecture', choices=('x64', 'arm64'), required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--target-directory', type=Path, required=True)
    args = parser.parse_args()
    # A narrow target guard prevents this testing-only binary from entering SDK roots.
    target = args.target_directory.resolve(strict=True)
    if target.name != 'HEngineFsrSmoke' or not (target / 'HEngineFsrSmoke.exe').is_file():
        raise ValueError('WARP can only be staged beside the owned HEngineFsrSmoke executable.')
    pin = json.loads(Path(__file__).with_name('warp-test.lock.json').read_text())
    source = acquire(pin, args.root.resolve())
    dll = source / f'build/native/bin/{args.architecture}/d3d10warp.dll'
    data = dll.read_bytes()
    offset = struct.unpack_from('<I', data, 60)[0]
    machine = struct.unpack_from('<H', data, offset + 4)[0]
    if data[:2] != b'MZ' or data[offset:offset+4] != b'PE\0\0' or machine != {'x64': 0x8664, 'arm64': 0xaa64}[args.architecture]:
        raise ValueError('WARP test dependency has the wrong native architecture.')
    shutil.copy2(dll, target / dll.name)
    shutil.copy2(source / 'LICENSE.TXT', target / 'WARP-LICENSE.txt')
    print(f'PASS: WARP {pin["version"]} {args.architecture}, testing only; archive SHA256={pin["sha256"]}')


if __name__ == '__main__':
    main()
