#!/usr/bin/env python3
"""Headless contract tests for the deterministic Cornell benchmark generator."""

from __future__ import annotations

import json
import struct
import sys
import unittest
from pathlib import Path


THIS_DIRECTORY = Path(__file__).resolve().parent
if str(THIS_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(THIS_DIRECTORY))

import generate_cornell_scene as cornell  # noqa: E402


class CornellSceneGeneratorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest = json.loads(cornell.DEFAULT_MANIFEST_PATH.read_text(encoding="utf-8"))
        cls.artifacts, cls.summary = cornell.generate_artifacts(cornell.DEFAULT_MANIFEST_PATH)
        cls.engine_rgb = cornell.derive_engine_rgb(cls.manifest)

    def test_official_geometry_camera_and_light_contract(self) -> None:
        self.assertEqual(self.summary["triangleCount"], 38)
        self.assertEqual(self.summary["vertexCount"], 114)
        self.assertEqual(self.summary["boundsM"]["min"], [0.0, 0.0, 0.0])
        self.assertEqual(self.summary["boundsM"]["max"], [0.556, 0.5488, 0.5592])
        self.assertAlmostEqual(self.summary["lightAreaM2"], 0.01365, places=12)
        self.assertGreater(self.summary["lightDistributionWeight"], 0.0)
        self.assertAlmostEqual(self.summary["verticalFovDegrees"], 39.30764811610661, places=11)
        self.assertEqual(self.manifest["camera"]["positionMm"], [278.0, 273.0, -800.0])
        self.assertEqual(self.manifest["camera"]["direction"], [0.0, 0.0, 1.0])
        self.assertEqual(self.manifest["camera"]["focalLengthM"], 0.035)
        self.assertEqual(self.manifest["camera"]["filmWidthM"], 0.025)
        self.assertEqual(self.manifest["camera"]["filmHeightM"], 0.025)

    def test_rgb_conversion_is_locked_without_per_channel_tuning(self) -> None:
        expected = {
            "white": [0.737940752, 0.736140874, 0.737375354],
            "green": [0.0522972322, 0.396334563, 0.0864580004],
            "red": [0.52352872, 0.050016831, 0.0503598825],
            "light": [1.0, 0.528913854, 0.133958759],
        }
        self.assertFalse(self.manifest["engineParity"]["perChannelTuningAllowed"])
        for name, values in expected.items():
            self.assertEqual(len(self.summary["engineLinearRgb"][name]), 3)
            for actual, reference in zip(self.summary["engineLinearRgb"][name], values):
                self.assertAlmostEqual(actual, reference, places=9)

    def test_gltf_is_identity_baked_hard_split_and_hdr_emissive(self) -> None:
        gltf = json.loads(self.artifacts[cornell.GLTF_FILENAME].decode("utf-8"))
        self.assertEqual(len(gltf["nodes"]), 1)
        self.assertEqual(gltf["nodes"][0]["matrix"], cornell.IDENTITY_MATRIX)
        self.assertTrue(gltf["meshes"][0]["extras"]["bakedWorldSpace"])
        self.assertEqual(gltf["meshes"][0]["extras"]["triangleCount"], 38)
        self.assertEqual(gltf["meshes"][0]["extras"]["vertexCount"], 114)
        self.assertEqual(gltf["extensionsUsed"], ["KHR_materials_emissive_strength"])
        light = gltf["materials"][3]
        self.assertEqual(light["name"], "light")
        self.assertEqual(
            light["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"],
            18.4,
        )
        self.assertGreater(max(light["extras"]["emissiveRadiance"]), 1.0)
        for primitive in gltf["meshes"][0]["primitives"]:
            self.assertTrue(primitive["extras"]["flatNormals"])
            self.assertTrue(primitive["extras"]["hardSplitVertices"])

    def test_pbrt_measured_proxy_and_strict_channels_are_distinct_and_pre_tonemap(self) -> None:
        measured = self.artifacts[cornell.MEASURED_PBRT_FILENAME].decode("utf-8")
        proxy = self.artifacts[cornell.PARITY_PROXY_PBRT_FILENAME].decode("utf-8")
        self.assertIn('"spectrum reflectance"', measured)
        self.assertIn('AreaLightSource "diffuse" "spectrum L"', measured)
        self.assertIn("400 0 500 8 600 15.6 700 18.4", measured)
        self.assertIn('"rgb reflectance"', proxy)
        self.assertIn('AreaLightSource "diffuse" "rgb L" [18.4 ', proxy)
        self.assertIn("RGB-authored spectral proxy only", proxy)
        self.assertIn("Do not use this scene as strict channel-wise HEngine RGB truth", proxy)

        strict_scenes = []
        for channel_index, channel_name in enumerate(("R", "G", "B")):
            filename = cornell.PARITY_CHANNEL_PBRT_FILENAMES[channel_name]
            scene = self.artifacts[filename].decode("utf-8")
            strict_scenes.append(scene)
            self.assertIn(f"Strict HEngine linear-RGB parity source for channel {channel_name}", scene)
            for material_name in ("white", "green", "red"):
                scalar = self.engine_rgb[material_name][channel_index]
                achromatic = " ".join([cornell._format_number(scalar)] * 3)
                self.assertIn(f'"rgb reflectance" [{achromatic}]', scene)
            light_scalar = (
                self.engine_rgb["light"][channel_index]
                * float(self.manifest["engineParity"]["emissiveStrength"])
            )
            achromatic_light = " ".join([cornell._format_number(light_scalar)] * 3)
            self.assertIn(f'AreaLightSource "diffuse" "rgb L" [{achromatic_light}]', scene)
            self.assertIn('Integrator "path" "integer maxdepth" [9]', scene)
            self.assertIn('"float screenwindow" [1 -1 -1 1]', scene)

        all_pbrt = measured + proxy + "".join(strict_scenes)
        self.assertNotIn("ToneMap", all_pbrt)
        self.assertNotIn("firefly", all_pbrt.lower())
        self.assertIn('"bool savefp16" [false]', measured)
        self.assertIn('"float iso" [100]', measured)
        self.assertIn('Option "seed" 1', measured)
        self.assertIn('Integrator "path" "integer maxdepth" [9]', measured)
        self.assertIn('"float screenwindow" [1 -1 -1 1]', measured)
        self.assertIn('"float screenwindow" [1 -1 -1 1]', proxy)

    def test_hengine_scene_uses_identity_mesh_and_official_camera(self) -> None:
        scene = json.loads(
            self.artifacts[cornell.HENGINE_SCENE_FILENAME].decode("utf-8")
        )
        mesh = next(value for value in scene.values() if "MeshComponent" in value)
        camera = next(value for value in scene.values() if "CameraComponent" in value)
        self.assertEqual(
            mesh["MeshComponent"]["Path"],
            "Assets/CornellBox/CornellBox.hmesh",
        )
        self.assertEqual(
            mesh["TransformComponent"],
            {
                "Rotation": [0.0, 0.0, 0.0],
                "Scale": [1.0, 1.0, 1.0],
                "Translation": [0.0, 0.0, 0.0],
            },
        )
        self.assertEqual(camera["TransformComponent"]["Translation"], [0.278, 0.273, -0.8])
        self.assertAlmostEqual(
            camera["CameraComponent"]["Camera"]["mPerspectiveFOV"],
            0.686047881,
            places=9,
        )
        self.assertTrue(camera["CameraComponent"]["FixedAspectRatio"])

    def test_masks_have_stable_ids_edges_and_nonempty_rois(self) -> None:
        def payload(filename: str) -> bytes:
            document = self.artifacts[filename]
            header_end = 0
            newline_count = 0
            for index, byte in enumerate(document):
                if byte == ord("\n"):
                    newline_count += 1
                    if newline_count == 4:
                        header_end = index + 1
                        break
            self.assertGreater(header_end, 0)
            return document[header_end:]

        material_ids = payload(cornell.MATERIAL_MASK_FILENAME)
        edges = payload(cornell.EDGE_MASK_FILENAME)
        roi_ids = payload(cornell.ROI_MASK_FILENAME)
        expected_pixels = 512 * 512
        self.assertEqual(len(material_ids), expected_pixels)
        self.assertEqual(len(edges), expected_pixels)
        self.assertEqual(len(roi_ids), expected_pixels)
        self.assertEqual(set(material_ids), {0, 1, 2, 3, 4})
        self.assertEqual(set(edges), {0, 255})
        self.assertLess(edges.count(255), int(expected_pixels * 0.15))
        self.assertEqual(set(roi_ids), {0, 1, 2, 3, 4})
        metadata = json.loads(self.artifacts[cornell.ROI_METADATA_FILENAME].decode("utf-8"))
        self.assertEqual([label["id"] for label in metadata["labels"]], [1, 2, 3, 4])
        self.assertTrue(all(label["pixelCount"] >= 64 for label in metadata["labels"]))
        self.assertEqual(metadata["labels"][2]["lightAreaVisibility"], "fullyOccluded")
        self.assertEqual(metadata["lightAreaOcclusion"]["blockerSurfaceNames"], ["short_block", "tall_block"])
        for label in metadata["labels"]:
            binary = payload(label["binaryMask"])
            self.assertEqual(set(binary), {0, 255})
            self.assertGreater(binary.count(255), 0)
        for label in metadata["materialIdLabels"][1:]:
            binary = payload(label["binaryMask"])
            self.assertEqual(set(binary), {0, 255})

    def test_short_block_disocclusion_roi_is_exact_and_versioned(self) -> None:
        document = self.artifacts[cornell.DISOCCLUSION_MASK_FILENAME]
        header_end = 0
        newline_count = 0
        for index, byte in enumerate(document):
            if byte == ord("\n"):
                newline_count += 1
                if newline_count == 4:
                    header_end = index + 1
                    break
        self.assertGreater(header_end, 0)
        mask = document[header_end:]
        self.assertEqual(len(mask), 512 * 512)
        self.assertEqual(set(mask), {0, 255})
        self.assertEqual(mask.count(255), 3492)

        metadata = json.loads(
            self.artifacts[cornell.DISOCCLUSION_METADATA_FILENAME].decode("utf-8")
        )
        self.assertEqual(self.manifest["generatorVersion"], "1.2.0")
        self.assertEqual(metadata["movingSurfaceName"], "short_block")
        self.assertEqual(metadata["translationM"], [-0.05, 0.0, 0.0])
        self.assertEqual(metadata["sourceBoundsM"]["min"], [0.082, 0.0, 0.065])
        self.assertEqual(metadata["sourceBoundsM"]["max"], [0.29, 0.165, 0.272])
        self.assertEqual(metadata["movedBoundsM"]["min"], [0.032, 0.0, 0.065])
        self.assertEqual(metadata["movedBoundsM"]["max"], [0.24, 0.165, 0.272])
        self.assertAlmostEqual(metadata["greenWallClearanceM"], 0.032, places=12)
        self.assertAlmostEqual(metadata["tallBlockSeparationM"], 0.025, places=12)
        self.assertEqual(metadata["rawPixelCount"], 5899)
        self.assertEqual(
            metadata["rawPixelBoundsTopLeftInclusiveExclusive"],
            [247, 328, 380, 482],
        )
        self.assertEqual(metadata["pixelCount"], 3492)
        self.assertEqual(
            metadata["pixelBoundsTopLeftInclusiveExclusive"],
            [250, 331, 296, 470],
        )
        self.assertEqual(
            metadata["revealedSurfacePixelCounts"],
            {"floor": 1536, "back_wall": 900, "tall_block": 1056},
        )
        self.assertEqual(
            metadata["recoveryContract"],
            {
                "consecutiveFrames": 3,
                "maxRecoveryFrames": 8,
                "postMoveCaptureFrames": 16,
                "referenceMaxBounces": 8,
                "referenceSamples": 512,
                "relativeL1Threshold": 0.05,
            },
        )

    def test_checked_in_outputs_are_byte_identical_and_hash_complete(self) -> None:
        checked_in_directory = cornell.DEFAULT_OUTPUT_DIRECTORY
        self.assertTrue(checked_in_directory.is_dir())
        for filename, expected in self.artifacts.items():
            self.assertEqual((checked_in_directory / filename).read_bytes(), expected, filename)
        hash_document = json.loads(self.artifacts[cornell.OUTPUT_HASH_FILENAME].decode("utf-8"))
        self.assertEqual(len(hash_document["generatorSha256"]), 64)
        self.assertEqual(set(hash_document["files"]), set(self.artifacts) - {cornell.OUTPUT_HASH_FILENAME})
        for filename, expected_hash in hash_document["files"].items():
            self.assertEqual(cornell._sha256(self.artifacts[filename]), expected_hash)

    def test_binary_layout_is_little_endian_float32_and_uint16(self) -> None:
        gltf = json.loads(self.artifacts[cornell.GLTF_FILENAME].decode("utf-8"))
        binary = self.artifacts[cornell.BUFFER_FILENAME]
        first_primitive = gltf["meshes"][0]["primitives"][0]
        positions = cornell._decode_accessor(
            gltf,
            binary,
            first_primitive["attributes"]["POSITION"],
        )
        indices = cornell._decode_accessor(gltf, binary, first_primitive["indices"])
        self.assertEqual(indices, list(range(len(indices))))
        self.assertEqual(len(positions) % 3, 0)
        first_view = gltf["bufferViews"][gltf["accessors"][first_primitive["attributes"]["POSITION"]]["bufferView"]]
        first_value = struct.unpack_from("<f", binary, first_view["byteOffset"])[0]
        self.assertAlmostEqual(first_value, 0.5528, places=6)

    def test_assimp_source_reads_and_applies_emissive_strength(self) -> None:
        importer_path = (
            cornell.REPOSITORY_ROOT
            / "Engine"
            / "Source"
            / "Plugins"
            / "Importers"
            / "MeshImporter"
            / "AssimpImporter.cpp"
        )
        source = importer_path.read_text(encoding="utf-8")
        self.assertIn("AI_MATKEY_EMISSIVE_INTENSITY, emissiveStrength", source)
        self.assertIn("std::isfinite(emissiveStrength)", source)
        self.assertGreaterEqual(source.count("* emissiveStrength"), 3)


if __name__ == "__main__":
    unittest.main()
