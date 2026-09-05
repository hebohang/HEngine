#!/usr/bin/env python3
"""Verify HAssetImporter preserves KHR_materials_emissive_strength.

This test intentionally exercises the compiled importer rather than duplicating
Assimp's glTF property mapping in Python. It creates one uniquely named output
directory inside Project/Assets (required by HAssetImporter), validates the
generated light material, and removes only that directory in a finally block.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[3]
PROJECT_ROOT = REPOSITORY_ROOT / "Project"
PROJECT_ASSETS_ROOT = PROJECT_ROOT / "Assets"
SOURCE_GLTF = PROJECT_ROOT / "SourceAssets" / "CornellBox" / "Generated" / "CornellBox.gltf"


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--importer",
        type=Path,
        required=True,
        help="Absolute path to the freshly built HAssetImporter executable.",
    )
    return parser.parse_args()


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as source:
        value = json.load(source)
    if not isinstance(value, dict):
        raise ValueError(f"expected a JSON object: {path}")
    return value


def _find_light_material(directory: Path) -> tuple[Path, dict[str, Any]]:
    candidates: list[tuple[Path, dict[str, Any]]] = []
    for path in sorted(directory.glob("*.hmat")):
        document = _load_json(path)
        properties = document.get("Properties", {})
        if path.stem.casefold() == "light" or (
            isinstance(properties, dict)
            and str(properties.get("name", "")).casefold() == "light"
        ):
            candidates.append((path, document))
    if len(candidates) != 1:
        raise ValueError(f"expected one imported light material, found {len(candidates)}")
    return candidates[0]


def _emissive_value(material: dict[str, Any]) -> list[float]:
    parameters = material.get("Parameters")
    if isinstance(parameters, dict):
        emissive = parameters.get("g_EmissiveFactor")
        if isinstance(emissive, dict) and isinstance(emissive.get("Value"), list):
            return [float(value) for value in emissive["Value"][:3]]
    properties = material.get("Properties")
    if isinstance(properties, dict) and isinstance(properties.get("emissiveFactor"), list):
        return [float(value) for value in properties["emissiveFactor"][:3]]
    raise ValueError("imported light material has no emissive factor")


def _import_once(importer: Path, output_directory: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(importer),
            str(PROJECT_ROOT.resolve()),
            str(SOURCE_GLTF.resolve()),
            str(output_directory),
        ],
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
        check=False,
    )


def _file_hashes(directory: Path) -> dict[str, str]:
    return {
        path.name: hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(directory.iterdir())
        if path.is_file()
    }


def main() -> int:
    arguments = _parse_arguments()
    importer = arguments.importer.resolve()
    if not importer.is_file():
        raise ValueError(f"HAssetImporter not found: {importer}")
    if not SOURCE_GLTF.is_file():
        raise ValueError(f"generated Cornell glTF not found: {SOURCE_GLTF}")

    source_document = _load_json(SOURCE_GLTF)
    light = next(material for material in source_document["materials"] if material["name"] == "light")
    strength = float(light["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"])
    expected = [float(value) * strength for value in light["emissiveFactor"]]
    if max(expected) <= 1.0:
        raise ValueError("fixture is not HDR and cannot detect a dropped emissive strength")

    PROJECT_ASSETS_ROOT.mkdir(parents=True, exist_ok=True)
    temporary_directory = Path(
        tempfile.mkdtemp(prefix=".cornell-assimp-validation-", dir=PROJECT_ASSETS_ROOT)
    ).resolve()
    assets_root = PROJECT_ASSETS_ROOT.resolve()
    try:
        temporary_directory.relative_to(assets_root)
        if not temporary_directory.name.startswith(".cornell-assimp-validation-"):
            raise ValueError("refusing to use an unexpected validation directory")
        first_result = _import_once(importer, temporary_directory)
        if first_result.returncode != 0:
            output = (first_result.stdout + "\n" + first_result.stderr)[-4000:]
            raise ValueError(f"HAssetImporter exited with {first_result.returncode}:\n{output}")
        first_hashes = _file_hashes(temporary_directory)
        for generated_path in temporary_directory.iterdir():
            if not generated_path.is_file():
                raise ValueError(
                    f"refusing to clean unexpected imported directory entry: {generated_path}"
                )
            generated_path.unlink()
        second_result = _import_once(importer, temporary_directory)
        if second_result.returncode != 0:
            output = (second_result.stdout + "\n" + second_result.stderr)[-4000:]
            raise ValueError(f"second HAssetImporter run exited with {second_result.returncode}:\n{output}")
        second_hashes = _file_hashes(temporary_directory)
        if first_hashes != second_hashes:
            raise ValueError(
                "two clean Cornell imports were not byte deterministic: "
                f"first={first_hashes}, second={second_hashes}"
            )
        material_path, material = _find_light_material(temporary_directory)
        actual = _emissive_value(material)
        if len(actual) != 3 or not all(math.isfinite(value) for value in actual):
            raise ValueError(f"invalid imported emissive value: {actual}")
        if any(abs(left - right) > 2.0e-4 for left, right in zip(actual, expected)):
            raise ValueError(f"emissive strength was not preserved: expected {expected}, found {actual}")
        print(
            "Assimp emissive-strength import PASS: "
            f"{material_path.name} -> [{actual[0]:.6g}, {actual[1]:.6g}, {actual[2]:.6g}]; "
            f"deterministicFiles={len(first_hashes)}"
        )
        return 0
    finally:
        # The resolved path and unique prefix were checked before any recursive
        # removal. This never targets the project Assets root or user content.
        if temporary_directory.parent == assets_root and temporary_directory.name.startswith(
            ".cornell-assimp-validation-"
        ):
            shutil.rmtree(temporary_directory, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as exception:
        print(f"Assimp emissive-strength validation failed: {exception}", file=sys.stderr)
        raise SystemExit(1)
