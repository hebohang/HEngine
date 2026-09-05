#!/usr/bin/env python3

import argparse
import os
import subprocess
import sys


def run(args, cwd=None):
    print("+ " + " ".join(args))
    subprocess.check_call(args, cwd=cwd)


def is_dawn_checkout(path):
    return os.path.isfile(os.path.join(path, "CMakeLists.txt")) and os.path.isdir(
        os.path.join(path, "src", "dawn")
    )


def main():
    parser = argparse.ArgumentParser(
        description="Fetch Dawn for the LLGI WebGPU backend."
    )
    parser.add_argument(
        "-d",
        "--directory",
        default=os.path.join("thirdparty", "dawn"),
        help="Dawn checkout directory. Default: thirdparty/dawn",
    )
    parser.add_argument(
        "--repository",
        default="https://dawn.googlesource.com/dawn",
        help="Dawn git repository URL.",
    )
    parser.add_argument(
        "--revision",
        default="main",
        help="Dawn branch, tag, or commit to checkout. Default: main",
    )
    parser.add_argument(
        "--skip-dependencies",
        action="store_true",
        help="Only clone/update Dawn; do not run Dawn dependency fetch.",
    )
    parser.add_argument(
        "--shallow-dependencies",
        action="store_true",
        help="Allow Dawn's dependency fetch script to use shallow clones. By default, dependencies are fetched with --no-shallow for broader Git compatibility.",
    )
    args = parser.parse_args()

    dawn_dir = os.path.abspath(args.directory)

    if os.path.exists(dawn_dir):
        if not is_dawn_checkout(dawn_dir):
            print(
                f"error: {dawn_dir} exists but does not look like a Dawn checkout",
                file=sys.stderr,
            )
            return 1
        run(["git", "fetch", "--tags", "origin"], cwd=dawn_dir)
    else:
        parent = os.path.dirname(dawn_dir)
        if parent:
            os.makedirs(parent, exist_ok=True)
        run(["git", "clone", args.repository, dawn_dir])

    run(["git", "checkout", args.revision], cwd=dawn_dir)

    if not args.skip_dependencies:
        dependency_script = os.path.join(dawn_dir, "tools", "fetch_dawn_dependencies.py")
        if not os.path.isfile(dependency_script):
            print(
                f"error: dependency script was not found: {dependency_script}",
                file=sys.stderr,
            )
            return 1

        dependency_args = [sys.executable, dependency_script]
        if not args.shallow_dependencies:
            dependency_args.append("--no-shallow")
        run(dependency_args, cwd=dawn_dir)

    print(f"Dawn is ready: {dawn_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
