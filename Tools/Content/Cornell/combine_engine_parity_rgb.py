#!/usr/bin/env python3
"""Combine three achromatic PBRT renders into strict HEngine linear-RGB truth.

PBRT v4 performs spectral transport even when a scene uses ``rgb`` parameters.
The Cornell generator therefore emits independent achromatic R, G, and B
scenes. This tool decodes their PFM files to top-left row order, validates the
camera orientation against generated Cornell masks, takes R from the R render,
G from the G render, and B from the B render, and writes deterministic float32
PFM plus a SHA-256 provenance sidecar.

This module intentionally remains compatible with Python 3.7.
"""

from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple


SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[3]
DEFAULT_MANIFEST_PATH = SCRIPT_PATH.with_name("CornellSceneManifest.v1.json")
DEFAULT_GENERATED_DIRECTORY = (
    REPOSITORY_ROOT / "Project" / "SourceAssets" / "CornellBox" / "Generated"
)


class CombinationError(ValueError):
    """Raised when a parity input or contract is invalid."""


class FloatImage(object):
    def __init__(self, width: int, height: int, values: Sequence[float]) -> None:
        self.width = int(width)
        self.height = int(height)
        self.channels = 3
        self.values = array.array("f", values)
        expected = self.width * self.height * self.channels
        if self.width <= 0 or self.height <= 0 or len(self.values) != expected:
            raise CombinationError("float image dimensions do not match its RGB payload")


def _sha256_bytes(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def _sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while True:
            block = source.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def _json_bytes(value: Any) -> bytes:
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def _portable_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return resolved.name


def _read_pfm_header_line(source: Any, label: str) -> bytes:
    while True:
        line = source.readline()
        if not line:
            raise CombinationError("{} PFM header is truncated".format(label))
        stripped = line.strip()
        if stripped and not stripped.startswith(b"#"):
            return stripped


def read_pfm(path: Path) -> FloatImage:
    """Read an RGB PFM into top-left row order and native float32 values."""

    path = path.resolve()
    if not path.is_file():
        raise CombinationError("PFM input is missing: {}".format(path))
    with path.open("rb") as source:
        magic = _read_pfm_header_line(source, str(path))
        if magic != b"PF":
            raise CombinationError("{} must be an RGB PF PFM".format(path))
        dimensions = _read_pfm_header_line(source, str(path)).split()
        if len(dimensions) != 2:
            raise CombinationError("{} PFM dimensions are invalid".format(path))
        try:
            width = int(dimensions[0])
            height = int(dimensions[1])
        except ValueError as exception:
            raise CombinationError("{} PFM dimensions are not integers".format(path)) from exception
        if width <= 0 or height <= 0:
            raise CombinationError("{} PFM dimensions must be positive".format(path))
        scale_tokens = _read_pfm_header_line(source, str(path)).split()
        if len(scale_tokens) != 1:
            raise CombinationError("{} PFM scale is invalid".format(path))
        try:
            scale = float(scale_tokens[0])
        except ValueError as exception:
            raise CombinationError("{} PFM scale is not numeric".format(path)) from exception
        if not math.isfinite(scale) or scale == 0.0:
            raise CombinationError("{} PFM scale must be finite and non-zero".format(path))
        payload = source.read()

    value_count = width * height * 3
    if len(payload) != value_count * 4:
        raise CombinationError(
            "{} PFM payload size is {}, expected {}".format(
                path, len(payload), value_count * 4
            )
        )
    values = array.array("f")
    values.frombytes(payload)
    file_byte_order = "little" if scale < 0.0 else "big"
    if file_byte_order != sys.byteorder:
        values.byteswap()
    scale_magnitude = abs(scale)
    if scale_magnitude != 1.0:
        for index in range(len(values)):
            values[index] = values[index] * scale_magnitude
    if any(not math.isfinite(value) for value in values):
        raise CombinationError("{} PFM contains a non-finite value".format(path))

    row_values = width * 3
    top_down = array.array("f")
    for top_y in range(height):
        disk_y = height - 1 - top_y
        start = disk_y * row_values
        top_down.extend(values[start : start + row_values])
    return FloatImage(width, height, top_down)


def pfm_bytes(image: FloatImage) -> bytes:
    """Encode top-left RGB data as deterministic little-endian bottom-up PFM."""

    if any(not math.isfinite(value) for value in image.values):
        raise CombinationError("cannot write a PFM containing non-finite values")
    row_values = image.width * 3
    bottom_up = array.array("f")
    for disk_y in range(image.height):
        top_y = image.height - 1 - disk_y
        start = top_y * row_values
        bottom_up.extend(image.values[start : start + row_values])
    if sys.byteorder != "little":
        bottom_up.byteswap()
    header = "PF\n{} {}\n-1.0\n".format(image.width, image.height).encode("ascii")
    return header + bottom_up.tobytes()


def _decoded_float_sha256(image: FloatImage) -> str:
    values = array.array("f", image.values)
    if sys.byteorder != "little":
        values.byteswap()
    return _sha256_bytes(values.tobytes())


def read_binary_pgm_mask(path: Path, width: int, height: int) -> bytearray:
    data = path.resolve().read_bytes()
    tokens: List[bytes] = []
    index = 0
    while len(tokens) < 4:
        while index < len(data) and data[index] in b" \t\r\n":
            index += 1
        if index >= len(data):
            raise CombinationError("PGM header is truncated: {}".format(path))
        if data[index] == ord("#"):
            while index < len(data) and data[index] not in b"\r\n":
                index += 1
            continue
        start = index
        while index < len(data) and data[index] not in b" \t\r\n#":
            index += 1
        tokens.append(data[start:index])
    if tokens[0] != b"P5":
        raise CombinationError("mask must be binary P5 PGM: {}".format(path))
    try:
        mask_width = int(tokens[1])
        mask_height = int(tokens[2])
        maximum = int(tokens[3])
    except ValueError as exception:
        raise CombinationError("mask PGM header is invalid: {}".format(path)) from exception
    if (mask_width, mask_height) != (width, height) or maximum != 255:
        raise CombinationError("mask extent/max value does not match parity inputs: {}".format(path))
    if index >= len(data) or data[index] not in b" \t\r\n":
        raise CombinationError("mask PGM has no payload separator: {}".format(path))
    if data[index] == ord("\r") and index + 1 < len(data) and data[index + 1] == ord("\n"):
        index += 2
    else:
        index += 1
    payload = bytearray(data[index:])
    if len(payload) != width * height or any(value not in (0, 255) for value in payload):
        raise CombinationError("mask PGM payload is not a binary image: {}".format(path))
    if 255 not in payload:
        raise CombinationError("mask PGM contains no selected pixels: {}".format(path))
    return payload


def _masked_mean(image: FloatImage, channel: int, mask: Sequence[int]) -> float:
    total = 0.0
    count = 0
    for pixel, selected in enumerate(mask):
        if selected:
            total += float(image.values[pixel * 3 + channel])
            count += 1
    if count == 0:
        raise CombinationError("orientation mask contains no selected pixels")
    return total / count


def _positive_ratio(numerator: float, denominator: float) -> float:
    if numerator <= 0.0 or not math.isfinite(numerator):
        return 0.0
    if denominator <= 1.0e-20:
        return float("inf")
    return numerator / denominator


def _load_contract(manifest_path: Path) -> Tuple[Dict[str, Any], str]:
    manifest_bytes = manifest_path.resolve().read_bytes()
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exception:
        raise CombinationError("Cornell manifest is not valid UTF-8 JSON") from exception
    try:
        contract = manifest["engineParity"]["offlineRgbTruth"]
        channels = contract["channelScenes"]
        if list(channels) != ["R", "G", "B"]:
            raise CombinationError("manifest channel order must be R/G/B")
        if [int(channels[name]["sourceChannelIndex"]) for name in ("R", "G", "B")] != [0, 1, 2]:
            raise CombinationError("manifest channel mapping must be R=0, G=1, B=2")
    except (KeyError, TypeError, ValueError) as exception:
        if isinstance(exception, CombinationError):
            raise
        raise CombinationError("Cornell strict RGB truth contract is invalid") from exception
    return manifest, _sha256_bytes(manifest_bytes)


def combine_channel_pfms(
    red_path: Path,
    green_path: Path,
    blue_path: Path,
    output_path: Path,
    manifest_path: Path = DEFAULT_MANIFEST_PATH,
    generated_directory: Path = DEFAULT_GENERATED_DIRECTORY,
    provenance_path: Optional[Path] = None,
    replace_existing: bool = False,
) -> Dict[str, Any]:
    input_paths = {"R": red_path.resolve(), "G": green_path.resolve(), "B": blue_path.resolve()}
    if len(set(input_paths.values())) != 3:
        raise CombinationError("R/G/B PFM inputs must be three distinct files")
    output_path = output_path.resolve()
    if output_path.suffix.lower() != ".pfm":
        raise CombinationError("combined output must use the .pfm extension")
    if provenance_path is None:
        provenance_path = Path(str(output_path) + ".json")
    provenance_path = provenance_path.resolve()
    if output_path in set(input_paths.values()) or provenance_path in set(input_paths.values()):
        raise CombinationError("combined outputs must not overwrite an input")
    if output_path == provenance_path:
        raise CombinationError("PFM and provenance paths must differ")
    if not replace_existing and (output_path.exists() or provenance_path.exists()):
        raise CombinationError("combined output already exists; pass --replace to overwrite")

    manifest, manifest_hash = _load_contract(manifest_path)
    contract = manifest["engineParity"]["offlineRgbTruth"]
    orientation_contract = contract["orientationValidation"]
    scene_paths = {
        channel_name: generated_directory
        / str(contract["channelScenes"][channel_name]["sceneFile"])
        for channel_name in ("R", "G", "B")
    }
    for channel_name, scene_path in scene_paths.items():
        if not scene_path.is_file():
            raise CombinationError(
                "canonical {} channel PBRT scene is missing: {}".format(
                    channel_name, scene_path
                )
            )
    images = {name: read_pfm(path) for name, path in input_paths.items()}
    red_image = images["R"]
    for name, image in images.items():
        if (image.width, image.height) != (red_image.width, red_image.height):
            raise CombinationError("{} PFM dimensions do not match R".format(name))

    mask_paths = {
        "red": generated_directory / "CornellBox.material-red.pgm",
        "green": generated_directory / "CornellBox.material-green.pgm",
        "light": generated_directory / "CornellBox.material-light.pgm",
        "nonEmissive": generated_directory / "CornellBox.material-non_emissive.pgm",
    }
    masks = {
        name: read_binary_pgm_mask(path, red_image.width, red_image.height)
        for name, path in mask_paths.items()
    }
    red_wall_ratio = _positive_ratio(
        _masked_mean(images["R"], 0, masks["red"]),
        _masked_mean(images["R"], 0, masks["green"]),
    )
    green_wall_ratio = _positive_ratio(
        _masked_mean(images["G"], 1, masks["green"]),
        _masked_mean(images["G"], 1, masks["red"]),
    )
    minimum_red_ratio = float(
        orientation_contract["redRenderRedWallToGreenWallMinimum"]
    )
    minimum_green_ratio = float(
        orientation_contract["greenRenderGreenWallToRedWallMinimum"]
    )
    if red_wall_ratio < minimum_red_ratio:
        raise CombinationError(
            "R render failed horizontal orientation/channel validation: red/green wall ratio {:.6g} < {:.6g}".format(
                red_wall_ratio, minimum_red_ratio
            )
        )
    if green_wall_ratio < minimum_green_ratio:
        raise CombinationError(
            "G render failed horizontal orientation/channel validation: green/red wall ratio {:.6g} < {:.6g}".format(
                green_wall_ratio, minimum_green_ratio
            )
        )
    minimum_light_ratio = float(orientation_contract["lightToNonEmissiveMinimum"])
    light_ratios: Dict[str, float] = {}
    for channel_index, channel_name in enumerate(("R", "G", "B")):
        light_ratio = _positive_ratio(
            _masked_mean(images[channel_name], channel_index, masks["light"]),
            _masked_mean(images[channel_name], channel_index, masks["nonEmissive"]),
        )
        light_ratios[channel_name] = light_ratio
        if light_ratio < minimum_light_ratio:
            raise CombinationError(
                "{} render failed vertical/light orientation validation: light/non-emissive ratio {:.6g} < {:.6g}".format(
                    channel_name, light_ratio, minimum_light_ratio
                )
            )

    combined_values = array.array("f")
    for pixel in range(red_image.width * red_image.height):
        combined_values.extend(
            (
                images["R"].values[pixel * 3 + 0],
                images["G"].values[pixel * 3 + 1],
                images["B"].values[pixel * 3 + 2],
            )
        )
    combined = FloatImage(red_image.width, red_image.height, combined_values)
    output_bytes = pfm_bytes(combined)

    provenance: Dict[str, Any] = {
        "schema": "hengine.cornell-engine-parity-rgb-combination.v1",
        "algorithm": contract["combineRule"],
        "colorSpace": manifest["engineParity"]["rgbColorSpace"],
        "dimensions": {"width": combined.width, "height": combined.height},
        "generatorVersion": manifest["generatorVersion"],
        "manifest": {
            "path": _portable_path(manifest_path),
            "sha256": manifest_hash,
        },
        "tool": {
            "path": _portable_path(SCRIPT_PATH),
            "sha256": _sha256_path(SCRIPT_PATH),
            "pythonMinimum": "3.7",
        },
        "inputs": {
            channel_name: {
                "path": _portable_path(input_paths[channel_name]),
                "sha256": _sha256_path(input_paths[channel_name]),
                "decodedFloat32Sha256": _decoded_float_sha256(images[channel_name]),
                "selectedChannelIndex": channel_index,
            }
            for channel_index, channel_name in enumerate(("R", "G", "B"))
        },
        "scenes": {
            channel_name: {
                "path": _portable_path(scene_paths[channel_name]),
                "sha256": _sha256_path(scene_paths[channel_name]),
                "expectedOutputFile": contract["channelScenes"][channel_name][
                    "outputFile"
                ],
            }
            for channel_name in ("R", "G", "B")
        },
        "masks": {
            name: {
                "path": _portable_path(path),
                "sha256": _sha256_path(path),
                "selectedPixelCount": sum(1 for value in masks[name] if value),
            }
            for name, path in mask_paths.items()
        },
        "orientationValidation": {
            "decodedRowOrder": "top-left",
            "pbrtScreenWindow": manifest["camera"]["pbrtScreenWindow"],
            "redRenderRedWallToGreenWallRatio": red_wall_ratio,
            "greenRenderGreenWallToRedWallRatio": green_wall_ratio,
            "lightToNonEmissiveRatio": light_ratios,
            "status": "PASS",
        },
        "output": {
            "path": _portable_path(output_path),
            "sha256": _sha256_bytes(output_bytes),
            "decodedFloat32Sha256": _decoded_float_sha256(combined),
            "encoding": "RGB float32 PFM, little-endian, bottom-up on disk",
        },
    }
    provenance_bytes = _json_bytes(provenance)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    provenance_path.parent.mkdir(parents=True, exist_ok=True)
    output_temporary = output_path.with_name(output_path.name + ".tmp")
    provenance_temporary = provenance_path.with_name(provenance_path.name + ".tmp")
    try:
        output_temporary.write_bytes(output_bytes)
        provenance_temporary.write_bytes(provenance_bytes)
        output_temporary.replace(output_path)
        provenance_temporary.replace(provenance_path)
    finally:
        for temporary in (output_temporary, provenance_temporary):
            if temporary.exists():
                temporary.unlink()
    return provenance


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--r", required=True, type=Path, help="PFM from the R achromatic PBRT scene")
    parser.add_argument("--g", required=True, type=Path, help="PFM from the G achromatic PBRT scene")
    parser.add_argument("--b", required=True, type=Path, help="PFM from the B achromatic PBRT scene")
    parser.add_argument("--output", required=True, type=Path, help="Combined strict linear-RGB PFM")
    parser.add_argument("--provenance", type=Path, help="Optional provenance JSON path; defaults to OUTPUT.json")
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST_PATH)
    parser.add_argument("--generated-dir", type=Path, default=DEFAULT_GENERATED_DIRECTORY)
    parser.add_argument("--replace", action="store_true", help="Replace existing output/provenance files")
    return parser.parse_args()


def main() -> int:
    arguments = _parse_arguments()
    provenance = combine_channel_pfms(
        arguments.r,
        arguments.g,
        arguments.b,
        arguments.output,
        arguments.manifest,
        arguments.generated_dir,
        arguments.provenance,
        arguments.replace,
    )
    print(
        "Combined strict Cornell EngineParityRGB: {}x{}, output SHA-256 {}, orientation {}".format(
            provenance["dimensions"]["width"],
            provenance["dimensions"]["height"],
            provenance["output"]["sha256"],
            provenance["orientationValidation"]["status"],
        )
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CombinationError, OSError, KeyError, TypeError, ValueError) as exception:
        print("Strict Cornell EngineParityRGB combination failed: {}".format(exception), file=sys.stderr)
        raise SystemExit(1)
