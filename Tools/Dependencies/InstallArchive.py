"""Extract a verified dependency archive with Windows long-path support."""
import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import zipfile


def native_path(value):
    value = os.path.abspath(value)
    if os.name == 'nt' and not value.startswith('\\\\?\\'):
        value = '\\\\?\\UNC\\' + value[2:] if value.startswith('\\\\') else '\\\\?\\' + value
    return Path(value)


def safe_name(value):
    name = value.replace('\\', '/')
    if name.startswith('/') or ':' in name or '..' in PurePosixPath(name).parts:
        raise ValueError('Unsafe dependency archive path: ' + value)
    return name


def install(archive, stage, destination, archive_root=''):
    stage = native_path(stage)
    destination = native_path(destination)
    if stage.exists():
        raise FileExistsError('Dependency extraction requires a fresh stage: ' + str(stage))
    with zipfile.ZipFile(native_path(archive)) as bundle:
        for entry in bundle.infolist():
            safe_name(entry.filename)
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError('Dependency archive contains a symbolic link.')
        root = safe_name(archive_root)
        bundle.extractall(stage)
    source = stage / root
    if not source.is_dir():
        raise ValueError('Dependency archive root is missing: ' + root)
    # Runtime installation intentionally merges into the separately pinned SDK.
    shutil.copytree(source, destination, dirs_exist_ok=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True)
    parser.add_argument('--stage', required=True)
    parser.add_argument('--destination', required=True)
    parser.add_argument('--archive-root', default='')
    args = parser.parse_args()
    install(args.archive, args.stage, args.destination, args.archive_root)
