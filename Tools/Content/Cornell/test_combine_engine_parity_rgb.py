#!/usr/bin/env python3
"""Tests for strict three-render Cornell EngineParityRGB PFM combination."""

from __future__ import annotations

import array
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Sequence


THIS_DIRECTORY = Path(__file__).resolve().parent
if str(THIS_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(THIS_DIRECTORY))

import combine_engine_parity_rgb as combine  # noqa: E402


WIDTH = 6
HEIGHT = 4


def _pixel_index(x: int, y: int) -> int:
    return y * WIDTH + x


def _make_mask(selected: Sequence[int]) -> bytearray:
    result = bytearray(WIDTH * HEIGHT)
    for pixel in selected:
        result[pixel] = 255
    return result


def _write_pgm(path: Path, mask: Sequence[int]) -> None:
    path.write_bytes(
        "P5\n# strict parity test mask\n{} {}\n255\n".format(WIDTH, HEIGHT).encode("ascii")
        + bytes(mask)
    )


def _constant_image(value: float) -> combine.FloatImage:
    return combine.FloatImage(WIDTH, HEIGHT, [value] * (WIDTH * HEIGHT * 3))


def _set_pixel(image: combine.FloatImage, x: int, y: int, value: float) -> None:
    base = _pixel_index(x, y) * 3
    image.values[base : base + 3] = array.array("f", [value, value, value])


def _write_pfm(path: Path, image: combine.FloatImage) -> None:
    path.write_bytes(combine.pfm_bytes(image))


class StrictEngineParityRgbCombinationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.generated = self.root / "Generated"
        self.generated.mkdir()

        self.red_pixels = [_pixel_index(0, y) for y in range(HEIGHT)]
        self.green_pixels = [_pixel_index(WIDTH - 1, y) for y in range(HEIGHT)]
        self.light_pixels = [_pixel_index(x, 0) for x in range(2, 4)]
        self.non_emissive_pixels = [
            pixel for pixel in range(WIDTH * HEIGHT) if pixel not in self.light_pixels
        ]
        masks = {
            "CornellBox.material-red.pgm": _make_mask(self.red_pixels),
            "CornellBox.material-green.pgm": _make_mask(self.green_pixels),
            "CornellBox.material-light.pgm": _make_mask(self.light_pixels),
            "CornellBox.material-non_emissive.pgm": _make_mask(self.non_emissive_pixels),
        }
        for filename, mask in masks.items():
            _write_pgm(self.generated / filename, mask)
        contract = json.loads(
            combine.DEFAULT_MANIFEST_PATH.read_text(encoding="utf-8")
        )["engineParity"]["offlineRgbTruth"]
        for channel_name in ("R", "G", "B"):
            scene_name = contract["channelScenes"][channel_name]["sceneFile"]
            (self.generated / scene_name).write_text(
                "# deterministic synthetic {} scene\n".format(channel_name),
                encoding="utf-8",
            )

        self.images: Dict[str, combine.FloatImage] = {
            "R": _constant_image(0.2),
            "G": _constant_image(0.2),
            "B": _constant_image(0.3),
        }
        for y in range(HEIGHT):
            _set_pixel(self.images["R"], 0, y, 2.0)
            _set_pixel(self.images["G"], WIDTH - 1, y, 2.0)
        for x in range(2, 4):
            _set_pixel(self.images["R"], x, 0, 10.0)
            _set_pixel(self.images["G"], x, 0, 8.0)
            _set_pixel(self.images["B"], x, 0, 6.0)

        self.inputs: Dict[str, Path] = {}
        for channel_name in ("R", "G", "B"):
            path = self.root / (channel_name + ".pfm")
            _write_pfm(path, self.images[channel_name])
            self.inputs[channel_name] = path

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def _combine(self, suffix: str = "") -> Dict[str, object]:
        return combine.combine_channel_pfms(
            self.inputs["R"],
            self.inputs["G"],
            self.inputs["B"],
            self.root / ("combined" + suffix + ".pfm"),
            combine.DEFAULT_MANIFEST_PATH,
            self.generated,
        )

    def test_combines_selected_channels_and_writes_hash_provenance(self) -> None:
        provenance = self._combine()
        output = combine.read_pfm(self.root / "combined.pfm")
        for pixel in range(WIDTH * HEIGHT):
            self.assertAlmostEqual(output.values[pixel * 3], self.images["R"].values[pixel * 3])
            self.assertAlmostEqual(output.values[pixel * 3 + 1], self.images["G"].values[pixel * 3 + 1])
            self.assertAlmostEqual(output.values[pixel * 3 + 2], self.images["B"].values[pixel * 3 + 2])
        sidecar = json.loads((self.root / "combined.pfm.json").read_text(encoding="utf-8"))
        self.assertEqual(sidecar, provenance)
        self.assertEqual(sidecar["orientationValidation"]["status"], "PASS")
        self.assertEqual(sidecar["output"]["sha256"], combine._sha256_path(self.root / "combined.pfm"))
        self.assertEqual([sidecar["inputs"][name]["selectedChannelIndex"] for name in ("R", "G", "B")], [0, 1, 2])

    def test_pfm_round_trip_preserves_top_left_orientation(self) -> None:
        image = combine.FloatImage(2, 2, [
            1.0, 2.0, 3.0,
            4.0, 5.0, 6.0,
            7.0, 8.0, 9.0,
            10.0, 11.0, 12.0,
        ])
        path = self.root / "orientation.pfm"
        _write_pfm(path, image)
        decoded = combine.read_pfm(path)
        self.assertEqual(list(decoded.values), list(image.values))
        payload = path.read_bytes().split(b"\n", 3)[3]
        first_disk_pixel = struct.unpack_from("<3f", payload, 0)
        self.assertEqual(first_disk_pixel, (7.0, 8.0, 9.0))

    def test_rejects_dimension_mismatch(self) -> None:
        wrong = combine.FloatImage(WIDTH - 1, HEIGHT, [0.25] * ((WIDTH - 1) * HEIGHT * 3))
        _write_pfm(self.inputs["B"], wrong)
        with self.assertRaisesRegex(combine.CombinationError, "dimensions do not match"):
            self._combine()

    def test_rejects_non_finite_input(self) -> None:
        values = array.array("f", [0.25] * (WIDTH * HEIGHT * 3))
        values[5] = float("nan")
        if sys.byteorder != "little":
            values.byteswap()
        self.inputs["B"].write_bytes(
            "PF\n{} {}\n-1.0\n".format(WIDTH, HEIGHT).encode("ascii")
            + values.tobytes()
        )
        with self.assertRaisesRegex(combine.CombinationError, "non-finite"):
            self._combine()

    def test_rejects_horizontal_camera_or_channel_swap(self) -> None:
        flipped = combine.FloatImage(WIDTH, HEIGHT, self.images["R"].values)
        for y in range(HEIGHT):
            for x in range(WIDTH // 2):
                left = (y * WIDTH + x) * 3
                right = (y * WIDTH + (WIDTH - 1 - x)) * 3
                left_value = array.array("f", flipped.values[left : left + 3])
                flipped.values[left : left + 3] = flipped.values[right : right + 3]
                flipped.values[right : right + 3] = left_value
        _write_pfm(self.inputs["R"], flipped)
        with self.assertRaisesRegex(combine.CombinationError, "horizontal orientation"):
            self._combine()

    def test_rejects_vertical_camera_or_row_order_error(self) -> None:
        flipped = combine.FloatImage(WIDTH, HEIGHT, self.images["B"].values)
        row_values = WIDTH * 3
        for y in range(HEIGHT // 2):
            opposite = HEIGHT - 1 - y
            first = y * row_values
            second = opposite * row_values
            first_row = array.array("f", flipped.values[first : first + row_values])
            flipped.values[first : first + row_values] = flipped.values[second : second + row_values]
            flipped.values[second : second + row_values] = first_row
        _write_pfm(self.inputs["B"], flipped)
        with self.assertRaisesRegex(combine.CombinationError, "vertical/light orientation"):
            self._combine()

    def test_output_and_provenance_are_deterministic(self) -> None:
        first = self._combine("-first")
        first_pfm = (self.root / "combined-first.pfm").read_bytes()
        first_json = (self.root / "combined-first.pfm.json").read_bytes()
        second = self._combine("-second")
        second_pfm = (self.root / "combined-second.pfm").read_bytes()
        second_json = (self.root / "combined-second.pfm.json").read_bytes()
        self.assertEqual(first_pfm, second_pfm)
        self.assertEqual(first["output"]["sha256"], second["output"]["sha256"])
        # Output filenames are provenance, so normalize only those two fields
        # before asserting byte-stable metadata content.
        first_document = json.loads(first_json.decode("utf-8"))
        second_document = json.loads(second_json.decode("utf-8"))
        first_document["output"]["path"] = "combined.pfm"
        second_document["output"]["path"] = "combined.pfm"
        self.assertEqual(first_document, second_document)


if __name__ == "__main__":
    unittest.main()
