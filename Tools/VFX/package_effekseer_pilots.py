#!/usr/bin/env python3
"""Build the two reviewed Effekseer CC0 pilot packages reproducibly."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import zipfile


FIXED_ZIP_TIME = (2026, 8, 13, 0, 0, 0)


PILOTS = (
    {
        "name": "BossSpawnWarning",
        "asset_id": "effekseer01-official",
        "source_root": Path(
            "Project/SourceAssets/EffekseerInbox/effekseer01-official/source/Effekseer01"
        ),
        "files": (
            ("Simple_Ring_Shape1.1806.efkefc", "BossSpawnWarning.efkefc"),
            ("Texture/Ring01.png", "Texture/Ring01.png"),
        ),
        "output": Path("Project/Assets/Effects/Survivor/Pilot/BossSpawnWarning.efkpkg"),
    },
    {
        "name": "LevelUpCelebration",
        "asset_id": "hato01",
        "source_root": Path(
            "Project/SourceAssets/EffekseerInbox/hato01/source/"
            "HATO-Springhascome/celebration"
        ),
        "files": (
            ("iwai-ume.1806.efkefc", "LevelUpCelebration.efkefc"),
            ("fbx/iwai.efkmodel", "fbx/iwai.efkmodel"),
            ("texture/hana-anime.png", "texture/hana-anime.png"),
            ("texture/particle2.png", "texture/particle2.png"),
        ),
        "output": Path("Project/Assets/Effects/Survivor/Pilot/LevelUpCelebration.efkpkg"),
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
            raise FileNotFoundError("Required reviewed source is missing: {}".format(source))
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
        raise RuntimeError("Refusing to overwrite stale temporary package: {}".format(temporary))

    try:
        with zipfile.ZipFile(
            str(temporary), "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for source, archive_relative in sorted(inputs, key=lambda item: item[1].lower()):
                info = zipfile.ZipInfo(archive_relative, FIXED_ZIP_TIME)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                archive.writestr(info, source.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
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
    parser.add_argument("--dry-run", action="store_true", help="Validate and print without writing")
    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    reports = [build_package(repo_root, spec, args.dry_run) for spec in PILOTS]
    print(json.dumps(reports, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
