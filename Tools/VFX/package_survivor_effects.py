#!/usr/bin/env python3
"""Build the reviewed multi-resource Survivor Effekseer packages reproducibly."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


FIXED_ZIP_TIME = (2026, 8, 23, 0, 0, 0)


PACKAGES = (
    {
        "name": "FireballToon",
        "asset_id": "pierre02",
        "source_root": Path(
            "Project/SourceAssets/EffekseerInbox/pierre02/source/Pierre02"
        ),
        "files": (
            ("FireBall.1806.efkefc", "FireballToon.efkefc"),
            ("Texture/ExperimentalNoise.png", "Texture/ExperimentalNoise.png"),
            ("Texture/Fire_Blur.png", "Texture/Fire_Blur.png"),
            ("Texture/Fire_Single.png", "Texture/Fire_Single.png"),
            ("Texture/Particle_Soft.png", "Texture/Particle_Soft.png"),
            ("Texture/Smoke.png", "Texture/Smoke.png"),
            ("Texture/Splash.png", "Texture/Splash.png"),
            ("Texture/Star.png", "Texture/Star.png"),
        ),
        "output": Path("Project/Assets/Effects/Survivor/FireballToon.efkpkg"),
    },
    {
        "name": "AuraButterfly",
        "asset_id": "hato01",
        "source_root": Path(
            "Project/SourceAssets/EffekseerInbox/hato01/source/"
            "HATO-Springhascome/Butterfliesflutter"
        ),
        "files": (
            ("aoi-chou.1806.efkefc", "AuraButterfly.efkefc"),
            ("fbx/hane2.efkmodel", "fbx/hane2.efkmodel"),
            ("texture/aoi-hane.png", "texture/aoi-hane.png"),
            ("texture/particle2.png", "texture/particle2.png"),
        ),
        "output": Path("Project/Assets/Effects/Survivor/AuraButterfly.efkpkg"),
    },
    {
        "name": "PetalBloom",
        "asset_id": "hato01",
        "source_root": Path(
            "Project/SourceAssets/EffekseerInbox/hato01/source/"
            "HATO-Springhascome/Flowersbloom"
        ),
        "files": (
            ("sakura1.1806.efkefc", "PetalBloom.efkefc"),
            ("fbx/14.efkmodel", "fbx/14.efkmodel"),
            ("texture/particle2.png", "texture/particle2.png"),
            ("texture/sakura-1.png", "texture/sakura-1.png"),
        ),
        "output": Path("Project/Assets/Effects/Survivor/PetalBloom.efkpkg"),
    },
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_package(repo_root: Path, spec: dict, dry_run: bool) -> dict:
    source_root = repo_root / spec["source_root"]
    output = repo_root / spec["output"]
    inputs = []
    for source_relative, archive_relative in spec["files"]:
        source = source_root / source_relative
        if not source.is_file():
            raise FileNotFoundError(f"Required reviewed source is missing: {source}")
        inputs.append((source, archive_relative.replace("\\", "/")))

    report = {
        "asset_id": spec["asset_id"],
        "name": spec["name"],
        "output": str(spec["output"]).replace("\\", "/"),
        "inputs": [
            {
                "path": str(source.relative_to(repo_root)).replace("\\", "/"),
                "archive_path": archive_relative,
                "size": source.stat().st_size,
                "sha256": sha256_file(source),
            }
            for source, archive_relative in inputs
        ],
    }
    if dry_run:
        return report

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    if temporary.exists():
        raise RuntimeError(f"Refusing to overwrite stale temporary package: {temporary}")

    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for source, archive_relative in sorted(
                inputs, key=lambda item: item[1].lower()
            ):
                info = zipfile.ZipInfo(archive_relative, FIXED_ZIP_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, source.read_bytes())
        temporary.replace(output)
    finally:
        if temporary.exists():
            temporary.unlink()

    report["package_size"] = output.stat().st_size
    report["package_sha256"] = sha256_file(output)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="HEngineDev repository root",
    )
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    reports = [build_package(repo_root, spec, args.dry_run) for spec in PACKAGES]
    print(json.dumps(reports, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
