"""Portable HEngine offline lighting entry point (Python 3.10+, stdlib only).

All output is staged in the project; an interrupted bake cannot replace a scene.
The Cycles adapter writes a manifest and an independently reviewable baked scene.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def asset(root, relative):
    if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
        raise ValueError("Expected a project-relative asset path")
    result = (root / relative).resolve()
    if not result.is_relative_to(root):
        raise ValueError("Asset escapes project: " + relative)
    return result


def read(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def verify(root, manifest, config_path=None):
    data = read(manifest)
    if data.get("schema") != "hengine.lighting-bake" or data.get("version") != 1:
        raise ValueError("Unsupported bake manifest")
    changed = []
    if config_path is not None and not any(asset(root, relative) == config_path for relative in data["inputs"]):
        raise ValueError("Manifest belongs to a different bake configuration")
    for group in ("inputs", "outputs"):
        for relative, expected in data[group].items():
            path = asset(root, relative)
            if not path.is_file() or digest(path) != expected:
                changed.append(group + ": " + relative)
    for name, expected in data["tools"].items():
        if digest(Path(__file__).parent / name) != expected:
            changed.append("tools: " + name)
    engine_root=Path(__file__).resolve().parents[2]
    for relative, expected in data.get("engineInputs",{}).items():
        path=asset(engine_root,"Engine/Content/"+relative)
        if asset(root, relative).is_file():
            changed.append("project now overrides engine asset: " + relative)
        if not path.is_file() or digest(path)!=expected:
            changed.append("engineInputs: " + relative)
    if changed:
        raise ValueError("Stale or damaged bake:\n" + "\n".join(changed[:30]))
    return data


def publish(stage, output):
    """Keep the last generation recoverable if the directory swap fails."""
    previous = None
    if output.exists():
        previous = output.with_name(output.name + ".previous-" + uuid.uuid4().hex[:8])
        output.rename(previous)
    try:
        stage.rename(output)
    except OSError:
        if previous is not None and not output.exists():
            previous.rename(output)
        raise


def main():
    if sys.version_info < (3, 10):
        raise RuntimeError("Offline lighting requires Python 3.10+; set HE_LIGHTING_PYTHON")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["bake", "check"])
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--config", default="Config/LightingBake.json")
    parser.add_argument("--manifest", default="")
    parser.add_argument("--blender", default="")
    parser.add_argument("--force", action="store_true", help="Recompute even when dependencies match")
    parser.add_argument("--timeout", type=int, default=7200)
    args = parser.parse_args()
    root = args.project.resolve(strict=True)
    if not (root / "Project.heproject").is_file():
        raise ValueError("Not an HEngine project")
    config_path = asset(root, args.config)
    config = read(config_path)
    output = asset(root, config["output"])
    generated = (root / "Assets" / "Generated").resolve()
    if output == generated or not output.is_relative_to(generated):
        raise ValueError("Bake output must be inside Assets/Generated")
    if config_path.is_relative_to(output) or asset(root, config["scene"]).is_relative_to(output):
        raise ValueError("Bake output cannot contain its authoring scene or configuration")
    manifest = asset(root, args.manifest) if args.manifest else output / "manifest.json"
    if args.action == "check":
        verify(root, manifest, config_path)
        print("PASS: bake inputs, outputs and tool versions match")
        return
    if manifest.is_file() and not args.force:
        try:
            verify(root,manifest,config_path)
        except (ValueError,OSError):
            pass
        else:
            print("PASS: cached bake is current; no solver work required")
            return
    if not args.blender:
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "Content"))
        from content_sync import find_blender
        args.blender = str(find_blender(""))
    output.parent.mkdir(parents=True, exist_ok=True)
    lock = output.parent / (output.name + ".bake.lock")
    with open(lock, "x", encoding="utf-8") as stream:
        stream.write(str(os.getpid()))
    stage = output.parent / (output.name + ".staging-" + uuid.uuid4().hex[:12])
    log = stage / "bake.log"
    try:
        stage.mkdir()
        request = stage / "request.json"
        tool_hashes={name:digest(Path(__file__).with_name(name)) for name in
                     ("bake.py", "cycles_bake.py", "lighting_math.py")}
        request.write_text(json.dumps({"project": str(root), "config": args.config,
            "stage": str(stage), "output": config["output"], "tools": tool_hashes}), encoding="utf-8")
        command = [args.blender, "--background", "--factory-startup", "--disable-autoexec",
            "--python-exit-code", "7", "--python", str(Path(__file__).with_name("cycles_bake.py")),
            "--", str(request)]
        with open(log, "w", encoding="utf-8") as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=args.timeout, check=False)
        if result.returncode:
            raise RuntimeError(f"Cycles failed ({result.returncode}); log: {log}")
        data = read(stage / "manifest.json")
        for name, expected in tool_hashes.items():
            if digest(Path(__file__).with_name(name)) != expected or data["tools"][name] != expected:
                raise RuntimeError("Bake tool changed during work: " + name)
        # Refuse publication if authoring files changed while Cycles was running.
        for relative, expected in data["inputs"].items():
            if digest(asset(root, relative)) != expected:
                raise RuntimeError("Input changed during bake: " + relative)
        for relative, expected in data.get("engineInputs",{}).items():
            if asset(root,relative).is_file():
                raise RuntimeError("Project asset override appeared during bake: " + relative)
            if digest(asset(Path(__file__).resolve().parents[2],"Engine/Content/"+relative))!=expected:
                raise RuntimeError("Engine asset changed during bake: " + relative)
        for relative, expected in data["outputs"].items():
            published=asset(root, relative)
            if not published.is_relative_to(output):
                raise RuntimeError("Output escaped bake directory")
            staged=stage/published.relative_to(output)
            if not staged.is_file() or digest(staged) != expected:
                raise RuntimeError("Incomplete bake output: " + relative)
        publish(stage, output)
        verify(root, output / "manifest.json", config_path)
        print("PASS: published", output / "manifest.json")
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("FAIL:", error, file=sys.stderr)
        sys.exit(1)
