#!/usr/bin/env python3
"""Build and run HEngine on Apple Silicon using repository-local dependencies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", choices=("editor", "game", "package"))
    parser.add_argument("action", nargs="?", choices=("build", "check", "open", "capture"), default="build")
    parser.add_argument("-Project", "--project", type=Path)
    parser.add_argument("-Configuration", "--configuration", choices=("Debug", "RelWithDebInfo", "Release"), default="RelWithDebInfo")
    parser.add_argument("-Parallel", "--parallel", type=int, default=2)
    parser.add_argument("--build-directory", type=Path)
    parser.add_argument("--cmake-arg", action="append", default=[])
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--capture-path", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--app-arg", action="append", default=[])
    args = parser.parse_args()
    if sys.platform != "darwin" or platform.machine() != "arm64":
        parser.error("This entry point requires an Apple Silicon macOS host")
    repo = Path(__file__).resolve().parents[2]
    project = (args.project or repo / "Engine/Templates/Blank").resolve()
    if project.is_file():
        project = project.parent
    descriptor = json.loads((project / "Project.heproject").read_text(encoding="utf-8-sig"))
    key = hashlib.sha256((descriptor["ProjectId"] + "\n" + str(project)).encode()).hexdigest()[:12]
    configuration = "Release" if args.host == "package" else args.configuration
    build = (args.build_directory or repo / f"out/agent/projects/{key}/macos-{args.host}-{configuration.lower()}").resolve()
    environment = os.environ.copy()
    dependencies = repo / "out/agent/dependencies/macos"
    settings_path = dependencies / "settings.json"
    settings = json.loads(settings_path.read_text()) if settings_path.is_file() else {}
    dotnet_root = Path(settings.get("dotnetRoot", str(dependencies / "dotnet")))
    path = []
    for directory in (dotnet_root, dependencies / "cmake/bin", dependencies / "ninja", dependencies / "bin"):
        if directory.is_dir():
            path.append(str(directory))
    path += [directory for directory in ("/opt/homebrew/bin", "/usr/local/bin") if Path(directory).is_dir()]
    environment["PATH"] = os.pathsep.join(path + [environment.get("PATH", "")])
    if (dotnet_root / "dotnet").is_file():
        environment["DOTNET_ROOT"] = str(dotnet_root)
    cmake = shutil.which("cmake", path=environment["PATH"])
    if not cmake:
        raise RuntimeError("CMake is required; use the documented macOS dependency setup")
    configure = [cmake, "-S", repo, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=" + configuration,
                 "-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", "-DHE_PROJECT_DIR=" + str(project),
                 "-DPACK_GAME=" + ("ON" if args.host == "package" else "OFF"),
                 "-DHE_BUILD_DEV_GAME=" + ("ON" if args.host == "game" else "OFF"),
                 "-DHE_PORTABLE_PLAYER=" + ("ON" if args.host in ("game", "package") and descriptor.get("RuntimeMode") == "Scene" else "OFF"),
                 "-DHE_PORTABLE_PLAYER_SOURCE_CONTENT=" + ("ON" if args.host == "game" and descriptor.get("RuntimeMode") == "Scene" else "OFF"),
                 "-DHE_ENABLE_GM=OFF", "-DHE_BUILD_TESTS=OFF", "-DHE_RENDER_DX12=OFF", "-DHE_RENDER_VULKAN=ON"]
    if (dotnet_root / "dotnet").is_file():
        configure.append("-DHE_DOTNET_EXECUTABLE=" + str(dotnet_root / "dotnet"))
    for variable, directory in (("HE_MOLTENVK_ROOT", "/opt/homebrew/opt/molten-vk"), ("HE_LIBCLANG_ROOT", "/opt/homebrew/opt/llvm")):
        if Path(directory).is_dir():
            configure.append("-D" + variable + "=" + directory)
    python = Path("/opt/homebrew/bin/python3.11")
    if python.is_file():
        configure += ["-DPython3_EXECUTABLE=" + str(python), "-DPython3_ROOT_DIR=/opt/homebrew/opt/python@3.11"]
        environment["HE_BUILD_PYTHON"] = str(python)
    dxc_candidates = list((dependencies / "VulkanSDK").glob("*/macOS/bin/dxc"))
    dxc_candidates += list((repo.parent / ".macos-dependencies/VulkanSDK").glob("*/macOS/bin/dxc"))
    if dxc_candidates:
        configure.append("-DHE_DXC_EXECUTABLE=" + str(sorted(dxc_candidates)[-1]))
    for key, variable in (("dxc", "HE_DXC_EXECUTABLE"), ("moltenvkRoot", "HE_MOLTENVK_ROOT"),
                          ("libclangRoot", "HE_LIBCLANG_ROOT"), ("python", "Python3_EXECUTABLE"),
                          ("pythonRoot", "Python3_ROOT_DIR")):
        if settings.get(key):
            configure.append("-D" + variable + "=" + settings[key])
    if settings.get("python"):
        environment["HE_BUILD_PYTHON"] = settings["python"]
    configure += args.cmake_arg
    run(configure, env=environment)
    target = {"editor": "HEngineEditor", "game": "HGame", "package": "PackageGame"}[args.host]
    run([cmake, "--build", build, "--config", configuration, "--target", target, "--parallel", args.parallel], env=environment)
    if args.host == "package" or args.action == "build":
        return
    executable = build / "bin" / ("HEngineEditor" if args.host == "editor" else descriptor["ExecutableName"])
    arguments = [executable, "--project=" + str(project), "--graphics-api=vulkan", "--strict-graphics-api"]
    if args.action != "open":
        arguments += ["--exit-after-frames=" + str(args.frames), "--no-cursor-capture"]
    if args.action == "capture":
        capture = (args.capture_path or build / "capture.png").resolve()
        arguments += ["--capture-frame=" + str(max(1, args.frames - 1)), "--capture-path=" + str(capture)]
    arguments += args.app_arg
    run(arguments, cwd=repo, env=environment, timeout=None if args.action == "open" else args.timeout)


if __name__ == "__main__":
    main()
