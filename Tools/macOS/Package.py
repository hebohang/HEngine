#!/usr/bin/env python3
"""Cook a Release player and publish a standalone Apple Silicon .app."""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tempfile
import uuid
import zipfile

from Bundle import bundle_executable, bundle_native_dependencies, is_macho, run, sign_and_verify, digest


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def owned_file(root, name):
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or "\\" in name:
        raise RuntimeError("Unsafe project-relative file: " + name)
    candidate = root / path
    if candidate.is_symlink() or not candidate.is_file() or not candidate.resolve().is_relative_to(root.resolve()):
        raise RuntimeError("Missing or redirected project file: " + str(candidate))
    return candidate


def verify_content(resources):
    manifest = read_json(resources / "Game.manifest.json")
    if manifest.get("ManifestVersion") != 1 or manifest.get("PakFile") != "Game.pak":
        raise RuntimeError("Unsupported package manifest")
    files = manifest["Files"]
    paths = {entry["Path"] for entry in files}
    if len(paths) != len(files) or len({path.casefold() for path in paths}) != len(paths):
        raise RuntimeError("Cooked content paths collide")
    forbidden = ("assets/devonly/", "scripts/devonly/", "shaders/devonly/", "config/devonly/")
    for entry in files:
        path = entry["Path"]
        if path.lower().startswith(forbidden) or path.lower() in {"config/userengine.json", "config/plugins.json"}:
            raise RuntimeError("Developer-only content entered Shipping: " + path)
        if PurePosixPath(path).is_absolute() or ".." in PurePosixPath(path).parts or "\\" in path:
            raise RuntimeError("Unsafe cooked content path: " + path)
        for dependency in entry.get("Dependencies", []):
            if dependency["Path"] not in paths:
                raise RuntimeError("Cooked dependency is absent: " + dependency["Path"])
    if not {"Config/DefaultEngine.json", "Config/BaseEngine.json"}.issubset(paths):
        raise RuntimeError("Cooked configuration is incomplete")
    if not any(path.startswith("Shaders/CACHES/SPIRV/") for path in paths):
        raise RuntimeError("Shipping requires SPIR-V shader artifacts")
    with zipfile.ZipFile(resources / "Game.pak") as archive:
        if set(archive.namelist()) != paths or len(archive.infolist()) != len(paths):
            raise RuntimeError("Game.pak and manifest file lists differ")
        for entry in files:
            if archive.getinfo(entry["Path"]).file_size != entry["StoredSize"]:
                raise RuntimeError("Cooked content size differs: " + entry["Path"])
        bad = archive.testzip()
        if bad:
            raise RuntimeError("Cooked archive failed CRC: " + bad)
    text = json.dumps(manifest).lower()
    for token in ("offlinereferencerenderer", "he.offline-reference-renderer", "referencepathtracer", "hardwareraytracingreference", "packagenever"):
        if token in text:
            raise RuntimeError("Forbidden Shipping dependency: " + token)
    return manifest


def set_player_identity(resources, project, contract_path, game_config):
    descriptor = read_json(project / "Project.heproject")
    contract = read_json(contract_path)
    if (contract["Configuration"] != "Release" or not contract["PackGame"] or contract["DevAgent"]
            or contract["Platform"] != "macOS-arm64" or not re.fullmatch("[0-9a-f]{64}", contract["RuntimeFingerprint"])):
        raise RuntimeError("Expected a Release macOS Shipping player contract")
    manifest = read_json(resources / "Game.manifest.json")
    if game_config not in {f["Path"] for f in manifest["Files"]}:
        raise RuntimeError("Player GameConfig is absent from cooked content")
    identity = {"SchemaVersion": 1, "EngineVersion": contract["EngineVersion"],
                "ProjectId": descriptor["ProjectId"], "DisplayName": descriptor["DisplayName"],
                "ExecutableName": descriptor["ExecutableName"], "GameConfig": game_config,
                "RuntimeFingerprint": contract["RuntimeFingerprint"]}
    if descriptor.get("LegacySavedDirectory"):
        identity["LegacySavedDirectory"] = descriptor["LegacySavedDirectory"]
    manifest["Player"] = identity
    (resources / "Game.manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def package(args):
    if args.output.is_symlink():
        raise RuntimeError("Output must not be a symbolic link")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    lock_name = ".he-macos-package-" + hashlib.sha256(str(output).encode()).hexdigest()[:16] + ".lock"
    with (output.parent / lock_name).open("a") as guard:
        try:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("Another build is publishing this macOS application") from error
        package_locked(args)


def package_locked(args):
    repo, project, build = args.engine.resolve(), args.project.resolve(), args.build.resolve()
    descriptor = read_json(project / "Project.heproject")
    name = descriptor["ExecutableName"]
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", name):
        raise RuntimeError("Unsafe executable name")
    identity = descriptor["ProjectId"]
    if args.output.is_symlink():
        raise RuntimeError("Output must not be a symbolic link")
    output = args.output.resolve()
    if output.suffix != ".app":
        raise RuntimeError("Output must be a regular .app path")
    output.parent.mkdir(parents=True, exist_ok=True)
    marker = "HEngine " + identity + " macOS Package v1\n"
    if output.exists() and (not (output / "Contents/Resources/.he-package-root").is_file() or
                            (output / "Contents/Resources/.he-package-root").read_text() != marker):
        raise RuntimeError("Refusing to replace an unowned application")
    work = build / ("macos-package-" + uuid.uuid4().hex)
    work.mkdir()
    app = output.parent / (name + ".staging-" + uuid.uuid4().hex + ".app")
    identifier = "org.hengine." + re.sub(r"[^A-Za-z0-9.-]", "-", identity)
    native_sources = {}
    executable, resources = bundle_executable(args.executable.resolve(), app, name, descriptor["DisplayName"], identifier, native_sources)
    cook = work / ("CookInput-" + uuid.uuid4().hex)
    run([args.cmake, "-DENGINE_CONTENT_DIR=" + str(repo / "Engine/Content"),
         "-DENGINE_GENERATED_CONTENT_DIR=" + str(args.generated.resolve()), "-DPROJECT_DIR=" + str(project),
         "-DCOOK_INPUT_DIR=" + str(cook), "-DCOOK_INPUT_PARENT=" + str(work), "-DPROJECT_ID=" + identity,
         "-DPYTHON_PLUGIN_ENABLED=" + ("ON" if args.python_config else "OFF"), "-P", repo / "CMake/PrepareCookInput.cmake"])
    owners = cook / "Shaders/CACHES/.artifact-owners"
    if owners.is_dir():
        shutil.rmtree(owners)  # Only the nonce-owned cook snapshot, never the build shader cache.
    arguments = [args.cooker, "--project", cook, "--config", args.game_config,
                 "--engine-config", repo / "Engine/Content/Config/EngineCook.json", "--python-plugin", "enabled" if args.python_config else "disabled",
                 "--csharp-plugin", "enabled" if args.script_dir else "disabled",
                 "--output", resources, "--report", work / "CookReport.json", "--run-id", uuid.uuid4().hex,
                 "--forbid-root", project, "--forbid-root", repo / "Engine/Content", "--forbid-root", args.generated]
    if args.script_dir:
        arguments += ["--csharp-metadata", args.script_dir / "GameScripts.metadata.json"]
    run([args.cooker, "--self-test"])
    run(arguments)
    if args.player_contract:
        set_player_identity(resources, project, args.player_contract, args.game_config)
    if args.script_dir:
        run([args.pwsh, "-NoProfile", "-NonInteractive", "-File", repo / "Tools/Scripting/StageRuntime.ps1",
             "-SourceDirectory", args.script_dir, "-DestinationDirectory", resources / "Scripting/CSharp"])
    if args.python_config:
        python_config = read_json(args.python_config)
        stdlib = Path(python_config["stdlib"]).resolve()
        if not (stdlib / "encodings/__init__.py").is_file():
            raise RuntimeError("Python stdlib is missing encodings")
        python_destination = resources / "Python/lib/python3.11"
        shutil.copytree(stdlib, python_destination,
                        ignore=shutil.ignore_patterns("site-packages", "__pycache__", "test", "tests", "idlelib", "tkinter", "ensurepip", "config-*"))
        for original in stdlib.rglob("*"):
            target = python_destination / original.relative_to(stdlib)
            if target.is_file() and is_macho(original):
                bundle_native_dependencies(original, target, args.executable.resolve(), app / "Contents/Frameworks", native_sources)
    package_config = read_json(owned_file(project, descriptor["PackageConfig"]))
    shutil.copy2(owned_file(project, package_config["ThirdPartyNotices"]), resources / "THIRD_PARTY_NOTICES.txt")
    licenses = resources / "Licenses"
    licenses.mkdir(exist_ok=True)
    license_names = set()
    def copy_license(source, name):
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*\.txt", name) or name.casefold() in license_names:
            raise RuntimeError("Unsafe or duplicate license output name: " + name)
        license_names.add(name.casefold())
        shutil.copy2(source, licenses / name)
    engine_licenses = read_json(Path(__file__).with_name("EngineLicenses.json"))
    for name, source in engine_licenses["common"].items():
        copy_license(owned_file(repo, source), name)
    if args.python_config:
        for name, source in engine_licenses["python"].items():
            copy_license(owned_file(repo, source), name)
    for item in package_config["Licenses"]:
        copy_license(owned_file(project, item["Source"]), item["Output"])
    if args.moltenvk_license:
        copy_license(args.moltenvk_license, "MoltenVK.txt")
    # Record licenses alongside native dependencies provided by Homebrew, when
    # used; this reads their installation and never modifies a package manager.
    for source in sorted(set(native_sources.values())):
        parts = source.parts
        if "Cellar" not in parts:
            continue
        index = parts.index("Cellar")
        root = Path(*parts[:index + 3])
        component = re.sub(r"[^A-Za-z0-9._-]", "-", parts[index + 1])
        for candidate in sorted(root.iterdir()):
            if candidate.is_file() and candidate.name.upper().startswith(("LICENSE", "COPYING")):
                name = "Native-" + component + "-" + candidate.name.replace(".", "-") + ".txt"
                if name.casefold() not in license_names:
                    copy_license(candidate, name)
    shutil.copy2(repo / "LICENSE", resources / "LICENSE.txt")
    runtime_contract = args.executable.resolve().parent / "HEngine.Runtime.json"
    runtime = read_json(runtime_contract)
    if runtime.get("dx12") or runtime.get("oidn") or runtime.get("fsr") or not runtime.get("vulkan"):
        raise RuntimeError("Shipping runtime contract is not the macOS Vulkan configuration")
    shutil.copy2(runtime_contract, resources / "HEngine.Runtime.json")
    (resources / ".he-package-root").write_text(marker)
    allowed_resources = {"Game.pak", "Game.manifest.json", "Scripting", "Python", "Licenses",
                         "LICENSE.txt", "THIRD_PARTY_NOTICES.txt", "HEngine.Runtime.json", ".he-package-root"}
    unexpected = {path.name for path in resources.iterdir()} - allowed_resources
    if unexpected:
        raise RuntimeError("Unexpected Shipping resources: " + str(sorted(unexpected)))
    manifest = verify_content(resources)
    sign_and_verify(app)
    # A fresh cwd and minimal PATH prove the runtime does not use the checkout or SDK.
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("DYLD_", "DOTNET_", "HE_", "PYTHON"))}
    environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin"
    check_id = uuid.uuid4().hex
    with (work / "package-check.log").open("w") as log:
        result = subprocess.run([str(executable), "--package-check", "--package-check-id=" + check_id,
                                 "--graphics-api=vulkan", "--strict-graphics-api"], cwd=tempfile.gettempdir(),
                                env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout)
    text = (work / "package-check.log").read_text(errors="replace")
    if result.returncode or any(marker not in text for marker in ("[Game] Ready", "[Graphics] Active backend: Vulkan", "[Automation] Completed frame limit: 2")):
        raise RuntimeError("Packaged runtime check failed; see " + str(work / "package-check.log"))
    receipt = {"schema": 1, "platform": "macOS-arm64", "configuration": "Release", "project": identity,
               "output": str(output), "contentFiles": len(manifest["Files"]), "packageCheck": "PASS",
               "log": str(work / "package-check.log"), "executableSha256": digest(executable)}
    # Publish only after cook, dependency, signing and real bounded runtime checks succeed.
    retired = output.with_name(output.name + ".retired-" + uuid.uuid4().hex)
    if output.exists():
        output.rename(retired)
    try:
        app.rename(output)
    except BaseException:
        if retired.exists():
            retired.rename(output)
        raise
    (work / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print("macOS Package PASS: " + str(output))
    print("Evidence: " + str(work / "result.json"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in ("engine", "project", "build", "executable", "cooker", "generated", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--game-config", required=True)
    parser.add_argument("--script-dir", type=Path)
    parser.add_argument("--python-config", type=Path)
    parser.add_argument("--player-contract", type=Path)
    parser.add_argument("--moltenvk-license", type=Path)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--pwsh", default="pwsh")
    parser.add_argument("--timeout", type=int, default=180)
    package(parser.parse_args())
