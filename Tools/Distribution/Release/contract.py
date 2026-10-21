"""Release inputs, free-runner guard and architecture checks (no network calls)."""
import argparse
import json
import os
from pathlib import Path
import re
import struct

RUNNERS = {"x64": "windows-2025-vs2026", "arm64": "windows-11-vs2026-arm"}
MACHINES = {"x64": 0x8664, "arm64": 0xAA64}


def audit_workflow(text):
    """Narrow policy lint for this workflow; actionlint validates the YAML syntax."""
    trigger = re.search(r'\non:\n(.*?)\npermissions:', text, re.S)
    if not trigger or re.findall(r'^  ([a-z_]+):', trigger[1], re.M) != ['workflow_dispatch']:
        raise ValueError('Release workflow must be manual-only.')
    jobs = dict(re.findall(r'^  ([a-z][a-z0-9_-]*):\n(.*?)(?=^  [a-z][a-z0-9_-]*:\n|\Z)',
                           text.split('\njobs:\n', 1)[1], re.M | re.S))
    if set(jobs) != {'plan', 'build', 'publish'}:
        raise ValueError('Unexpected release jobs require a new free-runner policy audit.')
    for name, body in jobs.items():
        condition = re.search(r'^    if: (.+)$', body, re.M)
        expected_condition = ("github.event_name == 'workflow_dispatch' && github.event.repository.private == false "
                              "&& github.event.repository.visibility == 'public'")
        if name == 'publish':
            expected_condition += " && inputs.publish && needs.build.result == 'success'"
        if not condition or condition[1].strip() != expected_condition:
            raise ValueError(f'{name} lacks a pre-allocation public/manual guard.')
        runner = re.search(r'^    runs-on: (.+)$', body, re.M)
        expected = '${{ matrix.runner }}' if name == 'build' else 'ubuntu-24.04'
        if not runner or runner[1] != expected:
            raise ValueError(f'{name} selects an unapproved runner.')
    if 'matrix: ${{ fromJSON(needs.plan.outputs.matrix) }}' not in jobs['build']:
        raise ValueError('Runner matrix must come from the constrained release planner.')
    if 'actions/cache@' in text:
        raise ValueError('Paid cache expansion is not part of this release workflow.')
    for action in re.findall(r'uses: (\S+)', text):
        if not re.fullmatch(r'actions/[a-z-]+@[0-9a-f]{40}', action):
            raise ValueError(f'Action must be official and commit-pinned: {action}')


def free_context(event, env, architecture=None):
    repository = event.get("repository", {})
    if (env.get("GITHUB_EVENT_NAME") != "workflow_dispatch"
            or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', env.get('GITHUB_REPOSITORY', ''))
            or repository.get("private") is not False
            or repository.get("visibility") != "public"
            or repository.get("full_name") != env.get("GITHUB_REPOSITORY")):
        raise ValueError("Release jobs require a manual dispatch in the current PUBLIC repository.")
    if env.get("HE_RUNNER_ENVIRONMENT") != "github-hosted":
        raise ValueError("Only standard GitHub-hosted runners are allowed; no self-hosted fallback.")
    if architecture:
        if (architecture not in RUNNERS or env.get("HE_RUNNER_LABEL") != RUNNERS[architecture]
                or env.get("RUNNER_OS") != "Windows"
                or env.get("RUNNER_ARCH", "").lower() != architecture):
            raise ValueError("Runner label/OS/architecture differs from the fixed free runner matrix.")


def plan(root, selection):
    if selection not in (*RUNNERS, "both"):
        raise ValueError("Choose x64, arm64 or both.")
    match = re.search(r"project\(HEngine VERSION (\d+\.\d+\.\d+) LANGUAGES", (root / "CMakeLists.txt").read_text())
    if not match:
        raise ValueError("Engine version is missing from CMakeLists.txt.")
    architectures = list(RUNNERS) if selection == "both" else [selection]
    return {"version": match[1], "architectures": architectures,
            "matrix": {"include": [{"arch": arch, "runner": RUNNERS[arch]} for arch in architectures]}}


def pe_architecture(path):
    """Return native PE machine, or None for architecture-neutral managed IL."""
    data = path.read_bytes()
    if len(data) < 64 or data[:2] != b"MZ":
        raise ValueError(f"Not a PE image: {path}")
    pe = struct.unpack_from("<I", data, 60)[0]
    if pe + 24 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"Invalid PE header: {path}")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if optional + optional_size > len(data) or optional_size < 2:
        raise ValueError(f"Truncated PE optional header: {path}")
    magic = struct.unpack_from("<H", data, optional)[0]
    directory = {0x10B: 96, 0x20B: 112}.get(magic)
    if directory is None:
        raise ValueError(f"Unsupported PE optional header: {path}")
    if optional_size >= directory + 15 * 8:
        clr_rva, clr_size = struct.unpack_from("<II", data, optional + directory + 14 * 8)
        if clr_rva and clr_size >= 20:
            table = optional + optional_size
            for index in range(sections):
                section = table + index * 40
                if section + 40 > len(data):
                    raise ValueError(f"Truncated PE sections: {path}")
                virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, section + 8)
                if rva <= clr_rva < rva + max(virtual_size, raw_size):
                    offset = raw + clr_rva - rva
                    if offset + 20 > len(data) or clr_rva - rva + 20 > raw_size:
                        raise ValueError(f"Invalid CLR header: {path}")
                    flags = struct.unpack_from("<I", data, offset + 16)[0]
                    if machine == 0x14C and magic == 0x10B and flags & 1 and not flags & (2 | 0x10 | 0x20000):
                        return None
                    break
    return machine


def audit_sdk(root, architecture):
    expected = f"Windows-{architecture}"
    descriptor = json.loads((root / "HEngine.distribution.json").read_text(encoding="utf-8-sig"))
    if descriptor.get("Platform") != expected:
        raise ValueError(f"SDK declares {descriptor.get('Platform')}, expected {expected}.")
    for directory in ("ExportTemplates", "DevelopmentTemplates"):
        manifest = json.loads((root / directory / expected / "Template.json").read_text(encoding="utf-8-sig"))
        if manifest.get("Platform") != expected or manifest.get("EngineVersion") != descriptor.get("EngineVersion"):
            raise ValueError(f"Mismatched {directory} identity.")
    count = 0
    for directory, children, files in os.walk(root, followlinks=False):
        children[:] = [name for name in children if name not in {"out", "Saved", "Dist", ".git", "obj", "__pycache__", ".he-content"}]
        for name in files:
            path = Path(directory) / name
            if name.lower() == 'd3d10warp.dll':
                raise ValueError('Test-only WARP must not enter a shipping SDK.')
            if path.suffix.lower() not in {".exe", ".dll", ".pyd"}:
                continue
            machine = pe_architecture(path)
            if machine is not None and machine != MACHINES[architecture]:
                raise ValueError(f"Wrong native machine 0x{machine:04x} in {path.relative_to(root)}; expected {expected}.")
            count += 1
    if count == 0:
        raise ValueError("SDK contains no PE images.")
    return {"evidence": "static-check", "pass": True, "platform": expected, "peImages": count}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("plan", "guard", "audit"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--architecture", choices=(*RUNNERS, "both"), default="both")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.action == "audit":
        if args.architecture == "both":
            parser.error("audit requires one architecture")
        result = audit_sdk(args.root, args.architecture)
    else:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8-sig"))
        free_context(event, os.environ, args.architecture if args.action == "guard" else None)
        result = plan(args.root, args.architecture)
        if args.action == "plan":
            with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
                for name in ("version", "matrix", "architectures"):
                    value = result[name]
                    output.write(f"{name}={json.dumps(value, separators=(',', ':')) if not isinstance(value, str) else value}\n")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result))


if __name__ == "__main__":
    main()
