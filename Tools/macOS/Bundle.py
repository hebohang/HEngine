#!/usr/bin/env python3
"""Assemble and verify an app-local macOS bundle, without a package manager."""
import hashlib
import plistlib
import re
from pathlib import Path
import shutil
import subprocess


def run(arguments, **kwargs):
    print("+ " + " ".join(map(str, arguments)), flush=True)
    return subprocess.run(list(map(str, arguments)), check=True, **kwargs)


def output(arguments):
    return subprocess.check_output(list(map(str, arguments)), text=True)


def digest(path):
    with Path(path).open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def is_macho(path):
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as source:
        return source.read(4) in {b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"}


def dependencies(path):
    return [line.strip().split(" (", 1)[0] for line in output(["otool", "-L", path]).splitlines()[1:]]


def rpaths(path):
    lines = output(["otool", "-l", path]).splitlines()
    return [lines[i + 2].strip().split(" ", 1)[1].rsplit(" (offset", 1)[0]
            for i, line in enumerate(lines) if line.strip() == "cmd LC_RPATH"]


def system_library(name):
    return name.startswith(("/System/Library/", "/usr/lib/"))


def resolve_dependency(name, source, executable, inherited):
    def expand(value):
        return value.replace("@loader_path", str(source.parent)).replace("@executable_path", str(executable.parent))
    if name.startswith("@rpath/"):
        candidates = [Path(expand(path)) / name[len("@rpath/"):] for path in rpaths(source) + inherited]
    else:
        candidates = [Path(expand(name))]
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError(f"Cannot resolve {name} loaded by {source}; searched {candidates}")


def bundle_native_dependencies(source, destination, executable, frameworks, copied):
    # Resolve against original load commands before modifying copied binaries.
    original_rpaths = rpaths(executable)
    queue = [(source.resolve(), destination)]
    while queue:
        original, bundled = queue.pop(0)
        for dependency in dependencies(original):
            if system_library(dependency):
                continue
            if dependency in (str(original), "@rpath/" + original.name):
                continue  # dylib's own LC_ID_DYLIB
            resolved = resolve_dependency(dependency, original, executable, original_rpaths)
            if resolved == original:
                continue
            name = resolved.name
            target = frameworks / name
            if name in copied and copied[name] != resolved and digest(copied[name]) != digest(resolved):
                raise RuntimeError(f"Conflicting dylib names: {copied[name]} and {resolved}")
            if name not in copied:
                copied[name] = resolved
                shutil.copy2(resolved, target)
                target.chmod(0o755)
                run(["install_name_tool", "-id", "@rpath/" + name, target])
                queue.append((resolved, target))
            run(["install_name_tool", "-change", dependency, "@executable_path/../Frameworks/" + name, bundled])
        for path in set(rpaths(bundled)):
            run(["install_name_tool", "-delete_rpath", path, bundled])


def bundle_executable(source, app, executable_name, display_name, identifier, native_sources=None):
    app.mkdir(parents=True, exist_ok=False)
    macos = app / "Contents/MacOS"
    frameworks = app / "Contents/Frameworks"
    resources = app / "Contents/Resources"
    for directory in (macos, frameworks, resources):
        directory.mkdir(parents=True)
    executable = macos / executable_name
    shutil.copy2(source, executable)
    executable.chmod(0o755)
    bundle_native_dependencies(source, executable, source, frameworks,
                               native_sources if native_sources is not None else {})
    info = {"CFBundleExecutable": executable_name, "CFBundleName": display_name,
            "CFBundleDisplayName": display_name, "CFBundleIdentifier": identifier,
            "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "1.1.0",
            "CFBundleVersion": "1.1.0", "LSMinimumSystemVersion": "13.0",
            "NSHighResolutionCapable": True, "NSSupportsAutomaticGraphicsSwitching": True}
    with (app / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump(info, stream)
    return executable, resources


def sign_and_verify(app):
    # Sign inner code first, preserving the .NET archive's relative layout.
    native = [path for path in app.rglob("*") if is_macho(path)]
    minimum_versions = [(13, 0)]
    for path in native:
        minimum_versions += [tuple(map(int, version.split(".")))
                             for version in re.findall(r"\bminos ([0-9.]+)", output(["otool", "-l", path]))]
    info_path = app / "Contents/Info.plist"
    with info_path.open("rb") as stream:
        info = plistlib.load(stream)
    info["LSMinimumSystemVersion"] = ".".join(map(str, max(minimum_versions)))
    with info_path.open("wb") as stream:
        plistlib.dump(info, stream)
    for path in sorted(native, key=lambda item: len(item.parts), reverse=True):
        if "arm64" not in output(["lipo", "-archs", path]).split():
            raise RuntimeError("Bundle contains non-arm64 code: " + str(path))
        if "Scripting/CSharp/Runtime/" in path.as_posix():
            # Preserve Microsoft-signed runtime bytes and their pinned inventory.
            run(["codesign", "--verify", "--strict", path])
        else:
            run(["codesign", "--force", "--sign", "-", path])
    run(["codesign", "--force", "--sign", "-", app])
    run(["codesign", "--verify", "--deep", "--strict", app])
    for path in native:
        for dependency in dependencies(path):
            if dependency.startswith("/") and not system_library(dependency):
                raise RuntimeError(f"Bundle retains an external native dependency: {path}: {dependency}")
            if dependency.startswith("@executable_path/"):
                resolved = app / "Contents/MacOS" / dependency[len("@executable_path/"):]
                if not resolved.is_file():
                    raise RuntimeError("Missing bundled dependency: " + str(resolved))
        for rpath in rpaths(path):
            if rpath.startswith("/") and not system_library(rpath):
                raise RuntimeError(f"Bundle retains external library search path: {path}: {rpath}")
