#!/usr/bin/env python3
"""Acquire the SHA-512 pinned private .NET runtime for the native macOS host."""
import argparse
import hashlib
import fcntl
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile


def digest(path, algorithm="sha256"):
    with path.open("rb") as stream:
        result = hashlib.new(algorithm)
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
        return result.hexdigest()


def acquire(lock_path, cache):
    lock = json.loads(lock_path.read_text())
    if lock["schema"] != 1 or lock["rid"] != "osx-arm64" or lock["version"] != "10.0.10":
        raise RuntimeError("Unsupported macOS runtime lock")
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ("dotnet-runtime-" + lock["sha512"] + ".tar.gz")
    if not archive.exists():
        partial = archive.with_suffix(".partial")
        subprocess.run(["curl", "--fail", "--location", "--retry", "2", "--max-time", "300", lock["url"], "--output", str(partial)], check=True)
        if digest(partial, "sha512") != lock["sha512"]:
            raise RuntimeError("Downloaded .NET runtime differs from the lock")
        partial.replace(archive)
    if digest(archive, "sha512") != lock["sha512"]:
        raise RuntimeError("Cached .NET runtime is corrupt")
    root = cache / ("runtime-" + lock["sha512"][:16])
    version = lock["version"]
    required = {f"host/fxr/{version}/libhostfxr.dylib", f"shared/Microsoft.NETCore.App/{version}/libcoreclr.dylib", f"shared/Microsoft.NETCore.App/{version}/System.Private.CoreLib.dll", "LICENSE.txt", "ThirdPartyNotices.txt"}
    manifest_path = root / "runtime-manifest.json"
    if manifest_path.is_file():
        manifest = json.loads(manifest_path.read_text())
        if (manifest["archiveSha512"] == lock["sha512"] and required.issubset({f["path"] for f in manifest["files"]})
                and all((root / f["path"]).is_file() and digest(root / f["path"]) == f["sha256"] for f in manifest["files"])):
            return {"Root": str(root), "Manifest": manifest}
        raise RuntimeError("The private runtime cache was modified; choose a fresh cache")
    if root.exists():
        raise RuntimeError("Refusing to replace an incomplete runtime cache")
    with tempfile.TemporaryDirectory(prefix="runtime-stage-", dir=cache) as temporary:
        stage = Path(temporary)
        files = []
        seen = set()
        with tarfile.open(archive, "r:gz") as source:
            for entry in source:
                name = entry.name.removeprefix("./")
                path = PurePosixPath(name)
                if path.is_absolute() or ".." in path.parts:
                    raise RuntimeError("Unsafe runtime archive path: " + name)
                if entry.isdir():
                    continue
                if not (name.startswith(f"host/fxr/{version}/") or name.startswith(f"shared/Microsoft.NETCore.App/{version}/") or name in {"LICENSE.txt", "ThirdPartyNotices.txt"}):
                    continue
                if not entry.isfile() or name in seen:
                    raise RuntimeError("Unsupported runtime archive entry: " + name)
                seen.add(name)
                target = stage / path
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.extractfile(entry) as data, target.open("wb") as output:
                    shutil.copyfileobj(data, output)
                target.chmod(entry.mode & 0o755)
                files.append({"path": name, "sha256": digest(target)})
        if not required.issubset(seen):
            raise RuntimeError("Runtime archive is incomplete")
        manifest = {"version": version, "rid": lock["rid"], "source": lock["url"], "archiveSha512": lock["sha512"], "files": sorted(files, key=lambda f: f["path"])}
        (stage / "runtime-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Publish only a complete verified runtime; no existing tree is replaced.
        stage.rename(root)
    return {"Root": str(root), "Manifest": manifest}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    args.cache.mkdir(parents=True, exist_ok=True)
    with (args.cache / "runtime-acquisition.lock").open("a") as guard:
        fcntl.flock(guard, fcntl.LOCK_EX)
        print(json.dumps(acquire(args.lock.resolve(), args.cache.resolve())))
