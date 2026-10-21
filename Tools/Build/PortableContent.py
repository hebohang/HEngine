"""Project-neutral asset and attribution copying for portable game exports."""

import hashlib
import json
import shutil
from pathlib import Path


ASSET_EXTENSIONS = frozenset({
    ".hmesh", ".hmat", ".bin", ".png", ".jpg", ".jpeg", ".hanim",
    ".hanimctrl", ".hskel", ".he", ".hprefab", ".wav", ".ogg",
    ".mp3", ".hui", ".rml", ".rcss", ".ttf", ".otf",
})
JSON_ASSET_EXTENSIONS = frozenset({
    ".hmesh", ".hmat", ".hanim", ".hanimctrl", ".hskel", ".he",
    ".hprefab", ".hui",
})


def _inside(root, relative):
    root = Path(root).resolve()
    path = (root / relative).resolve()
    if Path(relative).is_absolute() or not path.is_relative_to(root):
        raise ValueError(f"Unsafe project path: {relative}")
    return path


def _copy_with_digest(source, target, payload=None):
    if payload is None:
        payload = source.read_bytes()
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(payload)
    return {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}


def export_assets(project, output, content, roots, extensions=ASSET_EXTENSIONS):
    """Copy reachable Assets files and return content-manifest asset entries."""
    project, output = Path(project).resolve(), Path(output).resolve()
    queued, copied = set(roots), {}

    def collect(value):
        if isinstance(value, str):
            name = value.replace("\\", "/")
            if name.startswith("Assets/") and Path(name).suffix.lower() in extensions:
                queued.add(name)
        elif isinstance(value, dict):
            for child in value.values():
                collect(child)
        elif isinstance(value, (list, tuple)):
            for child in value:
                collect(child)

    collect(content)
    while queued:
        name = min(queued)
        queued.remove(name)
        if name in copied:
            continue
        source = _inside(project, name)
        if not source.is_file():
            raise FileNotFoundError(source)
        payload = source.read_bytes()
        if source.suffix.lower() in JSON_ASSET_EXTENSIONS:
            collect(json.loads(payload))
        target = _inside(output, name)
        copied[name] = _copy_with_digest(source, target, payload)
    return copied


def copy_project_files(project, output, relative_dir):
    source_dir = _inside(project, relative_dir)
    if source_dir.is_dir():
        for source in source_dir.iterdir():
            if source.is_file():
                shutil.copy2(source, _inside(output, source.name))


def copy_licenses(project, output, package_config, copied):
    project, output = Path(project).resolve(), Path(output).resolve()
    for entry in package_config.get("Licenses", []):
        source = _inside(project, entry["Source"])
        target = _inside(output / "Licenses", entry["Output"])
        relative = target.relative_to(output).as_posix()
        copied[relative] = _copy_with_digest(source, target)
