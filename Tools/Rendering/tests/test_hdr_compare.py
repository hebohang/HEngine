import array
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path


TOOL_DIR = Path(__file__).resolve().parents[1]
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import hdr_compare

try:
    from PIL import Image as PillowImage

    HAS_PILLOW = True
except ImportError:
    PillowImage = None
    HAS_PILLOW = False


FIXTURE_PATH = Path(__file__).resolve().parent / "fixtures" / "synthetic_cases.json"


def rgb_image(width, height, values):
    return hdr_compare.FloatImage(width, height, 3, array.array("f", values))


def uniform_image(value):
    return rgb_image(1, 1, [value, value, value])


class HdrCompareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with FIXTURE_PATH.open("r", encoding="utf-8") as stream:
            cls.fixtures = json.load(stream)

    def assertClose(self, actual, expected, tolerance=1.0e-6):
        self.assertIsNotNone(actual)
        self.assertLessEqual(abs(actual - expected), tolerance)

    def test_pfm_round_trip_preserves_top_down_rows(self):
        source = rgb_image(
            2,
            2,
            [
                1.0, 2.0, 3.0,
                4.0, 5.0, 6.0,
                7.0, 8.0, 9.0,
                10.0, 11.0, 12.0,
            ],
        )
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "roundtrip.pfm"
            hdr_compare.write_pfm(path, source)
            loaded = hdr_compare.read_pfm(path)
        self.assertEqual((loaded.width, loaded.height, loaded.channels), (2, 2, 3))
        self.assertEqual(list(loaded.values), list(source.values))

    def test_big_endian_scaled_pfm_bytes_are_decoded_independently(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "big_endian.pfm"
            path.write_bytes(
                b"PF\n1 2\n2.0\n"
                + struct.pack(">6f", 3.0, 4.0, 5.0, 0.5, 1.0, 1.5)
            )
            loaded = hdr_compare.read_pfm(path)
        self.assertEqual(list(loaded.values), [1.0, 2.0, 3.0, 6.0, 8.0, 10.0])

    def test_float_image_normalizes_double_array_to_float32(self):
        source = hdr_compare.FloatImage(1, 1, 3, array.array("d", [1.0, 2.0, 3.0]))
        self.assertEqual(source.values.typecode, "f")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "float32.pfm"
            hdr_compare.write_pfm(path, source)
            loaded = hdr_compare.read_pfm(path)
        self.assertEqual(list(loaded.values), [1.0, 2.0, 3.0])

    def test_raw_float_sidecar_and_bottom_up_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            raw_path = directory / "capture.raw"
            sidecar_path = directory / "capture.raw.json"
            bottom_row = [7.0, 8.0, 9.0]
            top_row = [1.0, 2.0, 3.0]
            raw_path.write_bytes(struct.pack("<6f", *(bottom_row + top_row)))
            sidecar_path.write_text(
                json.dumps(
                    {
                        "width": 1,
                        "height": 2,
                        "channels": 3,
                        "dtype": "float32",
                        "endianness": "little",
                        "layout": "HWC",
                        "row_order": "bottom-up",
                        "channel_order": "RGB",
                        "color_space": "scene-linear",
                        "color_primaries": "Rec.709/sRGB",
                    }
                ),
                encoding="utf-8",
            )
            loaded = hdr_compare.load_float_image(raw_path)
        self.assertEqual(list(loaded.values), top_row + bottom_row)

    def test_big_endian_half_bgra_raw_bytes_are_reordered(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            raw_path = directory / "capture.f16"
            sidecar_path = directory / "capture.f16.json"
            raw_path.write_bytes(struct.pack(">4e", 3.0, 2.0, 1.0, 4.0))
            sidecar_path.write_text(
                json.dumps(
                    {
                        "width": 1,
                        "height": 1,
                        "channels": 4,
                        "dtype": "float16",
                        "endianness": "big",
                        "layout": "HWC",
                        "row_order": "top-down",
                        "channel_order": "BGRA",
                        "color_space": "scene-linear",
                        "color_primaries": "Rec.709/sRGB",
                    }
                ),
                encoding="utf-8",
            )
            loaded = hdr_compare.load_float_image(raw_path)
        self.assertEqual(list(loaded.values), [1.0, 2.0, 3.0, 4.0])

    def test_non_object_sidecar_reports_comparison_error(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            raw_path = directory / "invalid.raw"
            raw_path.write_bytes(b"")
            (directory / "invalid.raw.json").write_text("[]", encoding="utf-8")
            with self.assertRaises(hdr_compare.ComparisonError):
                hdr_compare.load_float_image(raw_path)

    def test_identical_metrics_are_zero(self):
        image = rgb_image(2, 1, [0.0, 0.0, 0.0, 1.0, 2.0, 4.0])
        metrics = hdr_compare.compute_image_metrics(image, image, bytearray([1, 1]))
        self.assertEqual(metrics["nrmse"], 0.0)
        self.assertEqual(metrics["relative_l1"], 0.0)
        self.assertEqual(metrics["luminance"]["relative_l1"], 0.0)
        self.assertEqual(metrics["chromaticity"]["rmse"], 0.0)

    def test_black_transition_is_not_misreported_as_chromaticity(self):
        reference = rgb_image(1, 1, [0.0, 0.0, 0.0])
        candidate = rgb_image(1, 1, [1.0, 1.0, 1.0])
        metrics = hdr_compare.compute_image_metrics(reference, candidate, bytearray([1]))
        self.assertIsNone(metrics["chromaticity"]["rmse"])
        self.assertEqual(metrics["chromaticity"]["valid_pixel_count"], 0)
        self.assertEqual(metrics["chromaticity"]["black_transition_pixel_count"], 1)

    def test_fixture_double_radiance_has_unit_normalized_error(self):
        fixture = self.fixtures["double_radiance"]
        reference = rgb_image(fixture["width"], fixture["height"], fixture["reference"])
        candidate = rgb_image(fixture["width"], fixture["height"], fixture["candidate"])
        metrics = hdr_compare.compute_image_metrics(reference, candidate, bytearray([1, 1]))
        self.assertClose(metrics["nrmse"], fixture["expected_nrmse"])
        self.assertClose(metrics["relative_l1"], fixture["expected_relative_l1"])
        self.assertClose(metrics["luminance"]["nrmse"], 1.0)
        self.assertClose(metrics["chromaticity"]["rmse"], 0.0)

    def test_edge_dilation_and_named_scope_masking(self):
        edge = bytearray([1, 0, 0, 0, 0, 0, 0, 0, 0])
        radius_one = hdr_compare.dilate_mask(edge, 3, 3, 1)
        self.assertEqual(sum(radius_one), 4)
        interior = hdr_compare.mask_without(hdr_compare.full_mask(9), radius_one)
        self.assertEqual(sum(interior), 5)
        roi = bytearray([0, 0, 0, 0, 1, 1, 0, 0, 0])
        self.assertEqual(sum(hdr_compare.mask_and(roi, interior)), 1)

    @unittest.skipUnless(HAS_PILLOW, "Pillow is optional")
    def test_rgb_mask_uses_max_channel_like_float_masks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rgb_mask.png"
            image = PillowImage.new("RGB", (4, 1))
            image.putdata([(255, 0, 0), (0, 255, 0), (0, 0, 255), (0, 0, 0)])
            image.save(str(path))
            mask = hdr_compare.load_mask(path, 4, 1, 0.5)
        self.assertEqual(list(mask), [1, 1, 1, 0])

    @unittest.skipUnless(HAS_PILLOW, "Pillow is optional")
    def test_16_bit_mask_is_not_normalized_as_8_bit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "mask16.png"
            image = PillowImage.new("I", (2, 1))
            image.putdata([1000, 65535])
            image.save(str(path))
            mask = hdr_compare.load_mask(path, 2, 1, 0.5)
        self.assertEqual(list(mask), [0, 1])

    def test_bleeding_and_occluded_leak_metrics(self):
        reference = rgb_image(1, 1, [1.0, 1.0, 1.0])
        candidate = rgb_image(1, 1, [2.0, 1.0, 1.0])
        mask = bytearray([1])
        bleeding = hdr_compare.compute_bleeding_roi(reference, candidate, mask, "red")
        self.assertGreater(bleeding["channel_to_luminance_delta"], 0.0)
        leak = hdr_compare.compute_occluded_leak(reference, candidate, mask)
        expected_excess = 0.2126
        self.assertClose(leak["positive_excess_mean_luminance"], expected_excess)
        self.assertClose(leak["leak_ratio_to_reference"], expected_excess)

        equal_mean_reference = rgb_image(2, 1, [0.0, 0.0, 0.0, 2.0, 2.0, 2.0])
        equal_mean_candidate = rgb_image(2, 1, [1.0, 1.0, 1.0, 1.0, 1.0, 1.0])
        noise_only = hdr_compare.compute_occluded_leak(
            equal_mean_reference,
            equal_mean_candidate,
            bytearray([1, 1]),
        )
        self.assertClose(noise_only["leak_ratio_to_reference"], 0.0)
        self.assertGreater(
            noise_only["positive_pixel_excess_ratio_to_reference"],
            0.0,
        )

    def test_temporal_stddev_and_disocclusion_recovery(self):
        temporal = hdr_compare.TemporalAccumulator(1, 1, bytearray([1]))
        temporal.add(uniform_image(1.0))
        temporal.add(uniform_image(3.0))
        temporal_result = temporal.finalize()
        self.assertTrue(temporal_result["available"])
        self.assertClose(temporal_result["mean_pixel_stddev"], 1.0)
        self.assertClose(temporal_result["normalized_temporal_stddev"], 0.5)

        residual = hdr_compare.TemporalAccumulator(1, 1, bytearray([1]))
        residual.add(uniform_image(1.0), uniform_image(1.0))
        residual.add(uniform_image(3.0), uniform_image(3.0))
        residual_result = residual.finalize()
        self.assertEqual(residual_result["mode"], "reference_residual")
        self.assertClose(residual_result["normalized_temporal_stddev"], 0.0)

        fixture = self.fixtures["disocclusion"]
        disocclusion = hdr_compare.DisocclusionAccumulator(
            1,
            1,
            bytearray([1]),
            0,
            fixture["threshold"],
            fixture["consecutive_frames"],
        )
        reference = uniform_image(fixture["reference"])
        for value in fixture["frames"]:
            disocclusion.add(uniform_image(value), reference)
        result = disocclusion.finalize()
        self.assertTrue(result["recovered"])
        self.assertEqual(result["recovery_frame"], fixture["expected_recovery_frame"])
        self.assertEqual(result["confirmation_frame"], 3)
        self.assertEqual(result["confirmation_frames_after_start"], 3)

    def test_temporal_glob_uses_natural_frame_order(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            for name in ("frame_10.pfm", "frame_2.pfm", "frame_1.pfm"):
                (directory / name).write_bytes(b"")
            paths = hdr_compare._collect_sequence_paths([], str(directory / "frame_*.pfm"))
        self.assertEqual(
            [Path(path).name for path in paths],
            ["frame_1.pfm", "frame_2.pfm", "frame_10.pfm"],
        )

    def test_temporal_glob_typo_is_not_silently_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(hdr_compare.ComparisonError):
                hdr_compare._collect_sequence_paths([], str(Path(directory) / "missing_*.pfm"))

    def test_hdr_flip_is_explicitly_unavailable_or_adapter_supplied(self):
        unavailable = hdr_compare.load_hdr_flip_adapter(None)
        self.assertFalse(unavailable["available"])
        self.assertIn("no substitute", unavailable["reason"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "flip.json"
            path.write_text(
                json.dumps(
                    {
                        "schema": "hdr-flip-adapter.v1",
                        "mean": 0.02,
                        "p95": 0.08,
                        "tool": "approved-hdr-flip-runner",
                        "version": "1.0",
                        "reference_sha256": "a" * 64,
                        "candidate_sha256": "b" * 64,
                        "reference_decoded_rgb_sha256": "c" * 64,
                        "candidate_decoded_rgb_sha256": "d" * 64,
                        "width": 16,
                        "height": 8,
                        "color_space": "scene-linear-rgb",
                        "color_primaries": "Rec.709/sRGB",
                        "scope": "global",
                        "parameters": {
                            "pixels_per_degree": 67.0,
                            "tone_mapper": "ACES",
                            "start_exposure": -12.0,
                            "stop_exposure": 1.0,
                            "exposure_count": 14,
                            "exposure_policy": "automatic",
                            "p95_method": "nearest-rank-unweighted-pixels"
                        },
                    }
                ),
                encoding="utf-8",
            )
            supplied = hdr_compare.load_hdr_flip_adapter(
                path,
                {
                    "reference_sha256": "a" * 64,
                    "candidate_sha256": "b" * 64,
                    "reference_decoded_rgb_sha256": "c" * 64,
                    "candidate_decoded_rgb_sha256": "d" * 64,
                    "width": 16,
                    "height": 8,
                },
            )
            policy_path = Path(directory) / "policy.json"
            policy_path.write_text(
                json.dumps(
                    {
                        "schema": "hengine.hdr-flip-policy.v1",
                        "tool": "approved-hdr-flip-runner",
                        "version": "1.0",
                        "pixels_per_degree": 67.0,
                        "tone_mapper": "ACES",
                        "exposure_policy": "automatic",
                        "p95_method": "nearest-rank-unweighted-pixels",
                    }
                ),
                encoding="utf-8",
            )
            gated = hdr_compare.load_hdr_flip_adapter(
                path,
                {
                    "reference_sha256": "a" * 64,
                    "candidate_sha256": "b" * 64,
                    "reference_decoded_rgb_sha256": "c" * 64,
                    "candidate_decoded_rgb_sha256": "d" * 64,
                    "width": 16,
                    "height": 8,
                },
                policy_path,
            )
        self.assertTrue(supplied["available"])
        self.assertEqual(supplied["mean"], 0.02)
        self.assertEqual(supplied["p95"], 0.08)
        self.assertFalse(supplied["gate_eligible"])
        self.assertTrue(gated["gate_eligible"])
        self.assertEqual(gated["gated_mean"], 0.02)

    def test_unrecovered_disocclusion_and_unapproved_flip_cannot_pass_gates(self):
        args = hdr_compare.build_argument_parser().parse_args(
            [
                "--reference",
                "reference.pfm",
                "--candidate",
                "candidate.pfm",
                "--output-dir",
                "result",
                "--max-disocclusion-recovery-frames",
                "8",
                "--disocclusion-roi",
                "moved_surface",
                "--max-hdr-flip-mean",
                "0.03",
            ]
        )
        report = {
            "metrics": {
                "disocclusion": {"available": True, "frames_after_start": None},
                "hdr_flip": {
                    "available": True,
                    "mean": 0.01,
                    "gated_mean": None,
                    "gate_eligible": False,
                },
            }
        }
        thresholds = hdr_compare.evaluate_thresholds(args, report)
        self.assertEqual(thresholds["overall"], "FAIL")
        self.assertTrue(
            all(check["status"] == "UNAVAILABLE" for check in thresholds["checks"])
        )

    def test_disocclusion_gate_requires_explicit_roi(self):
        args = hdr_compare.build_argument_parser().parse_args(
            [
                "--reference",
                "reference.pfm",
                "--candidate",
                "candidate.pfm",
                "--output-dir",
                "result",
                "--max-disocclusion-recovery-frames",
                "8",
            ]
        )
        with self.assertRaises(hdr_compare.ComparisonError):
            hdr_compare.validate_arguments(args)

    def test_cli_writes_report_float_heatmap_and_threshold_result(self):
        reference = rgb_image(2, 1, [1.0, 1.0, 1.0, 2.0, 2.0, 2.0])
        candidate = rgb_image(2, 1, [1.1, 1.1, 1.1, 2.0, 2.0, 2.0])
        mask = hdr_compare.FloatImage(2, 1, 1, array.array("f", [1.0, 0.0]))
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            reference_path = directory / "reference.pfm"
            candidate_path = directory / "candidate.pfm"
            mask_path = directory / "occluded.pfm"
            output_path = directory / "result"
            hdr_compare.write_pfm(reference_path, reference)
            hdr_compare.write_pfm(candidate_path, candidate)
            hdr_compare.write_pfm(mask_path, mask)
            exit_code = hdr_compare.main(
                [
                    "--reference",
                    str(reference_path),
                    "--candidate",
                    str(candidate_path),
                    "--output-dir",
                    str(output_path),
                    "--roi-mask",
                    "occluded={0}".format(mask_path),
                    "--occluded-roi",
                    "occluded",
                    "--max-nrmse",
                    "0.1",
                    "--no-heatmap-png",
                ]
            )
            self.assertEqual(exit_code, 0)
            report_path = output_path / "hdr_comparison.json"
            self.assertTrue(report_path.is_file())
            self.assertTrue((output_path / "luminance_absolute_error.pfm").is_file())
            report = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(report["schema"], hdr_compare.SCHEMA)
        self.assertEqual(report["thresholds"]["overall"], "PASS")
        self.assertIn(
            "leak_ratio_to_reference", report["metrics"]["occluded_leak"]["interior"]
        )
        self.assertFalse(report["metrics"]["hdr_flip"]["available"])


if __name__ == "__main__":
    unittest.main()
