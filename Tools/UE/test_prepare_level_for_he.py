"""Regression checks for authored UE mask coverage in derived glTF exports."""

import json
import struct
import subprocess
import sys
import tempfile
import unittest
import uuid
from pathlib import Path

from TextureExportNames import export_filenames


SCRIPT = Path(__file__).with_name("PrepareLevelForHE.py")


class PrepareLevelMaskTests(unittest.TestCase):
    def test_duplicate_texture_basenames_require_export_manifest(self):
        references = ["/Game/Forest/T_Leaf.T_Leaf", "/Game/Garden/T_Leaf.T_Leaf"]
        names = export_filenames(references)
        self.assertEqual(len({name.casefold() for name in names.values()}), 2)
        self.assertEqual(names, export_filenames(reversed(references)))
        self.assertEqual(export_filenames(["/Game/Only/T_Bark.T_Bark"]),
                         {"/Game/Only/T_Bark.T_Bark": "T_Bark.png"})
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "scene.gltf"
            document = {
                "asset": {"version": "2.0"},
                "buffers": [{"uri": "scene.bin", "byteLength": 36}],
                "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
                "accessors": [{"bufferView": 0, "componentType": 5126,
                               "count": 3, "type": "VEC3"}],
                "materials": [{"name": "Forest"}, {"name": "Garden"}],
                "meshes": [{"name": "Tree", "primitives": [
                    {"attributes": {"POSITION": 0}, "material": 0}]}],
                "nodes": [{"name": "TreeActor", "mesh": 0}],
                "scenes": [{"nodes": [0]}], "scene": 0,
                "extensions": {"KHR_lights_punctual": {"lights": []}},
            }
            source_bytes = (json.dumps(document) + "\n").encode("utf-8")
            source.write_bytes(source_bytes)
            (root / "scene.bin").write_bytes(struct.pack("<9f", *([0.0] * 9)))
            material_manifest = {
                "/Game/Forest/Forest.Forest": {"textures": {"BaseColor": references[0]}},
                "/Game/Garden/Garden.Garden": {"textures": {"BaseColor": references[1]}},
            }
            (root / "materials.json").write_text(json.dumps(material_manifest), encoding="utf-8")
            (root / "profile.json").write_text('{"schema":1}', encoding="utf-8")
            texture_dir = root / "Textures"
            texture_dir.mkdir()
            (texture_dir / "T_Leaf.png").write_bytes(b"legacy collision")
            rows = []
            for reference in references:
                filename = names[reference]
                (texture_dir / filename).write_bytes(b"PNG fixture")
                rows.append({"asset": reference, "output": str(texture_dir / filename),
                             "relativeOutput": "Textures/" + filename,
                             "ok": True, "bytes": 11})
            (root / "texture-export.json").write_text(json.dumps(rows), encoding="utf-8")
            command = [sys.executable, str(SCRIPT), str(source), str(root / "materials.json"),
                       str(root / "derived.gltf"), "--profile", str(root / "profile.json")]
            missing_manifest = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(missing_manifest.returncode, 0)
            self.assertIn("texture basenames collide", missing_manifest.stderr)
            result = subprocess.run(command + ["--texture-manifest", str(root / "texture-export.json")],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            derived = json.loads((root / "derived.gltf").read_text(encoding="utf-8"))
            self.assertEqual({image["uri"] for image in derived["images"]},
                             {"Textures/" + name for name in names.values()})
            self.assertEqual(source.read_bytes(), source_bytes)

    def test_ambiguous_material_name_fails_without_explicit_path(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            document = {
                "asset": {"version": "2.0"},
                "buffers": [{"uri": "scene.bin", "byteLength": 36}],
                "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
                "accessors": [{"bufferView": 0, "componentType": 5126,
                               "count": 3, "type": "VEC3"}],
                "materials": [{"name": "Same"}],
                "meshes": [{"name": "Mesh", "primitives": [
                    {"attributes": {"POSITION": 0}, "material": 0}]}],
                "nodes": [{"name": "Actor", "mesh": 0}],
                "scenes": [{"nodes": [0]}], "scene": 0,
                "extensions": {"KHR_lights_punctual": {"lights": []}},
            }
            (root / "scene.gltf").write_text(json.dumps(document), encoding="utf-8")
            (root / "scene.bin").write_bytes(struct.pack("<9f", *([0.0] * 9)))
            material_paths = ["/Game/A/Same.Same", "/Game/B/Same.Same"]
            (root / "materials.json").write_text(
                json.dumps({path: {} for path in material_paths}), encoding="utf-8")
            profile = root / "profile.json"
            profile.write_text('{"schema":1}', encoding="utf-8")
            command = [sys.executable, str(SCRIPT), str(root / "scene.gltf"),
                       str(root / "materials.json"), str(root / "derived.gltf"),
                       "--profile", str(profile)]
            ambiguous = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(ambiguous.returncode, 0)
            self.assertIn("material name is ambiguous", ambiguous.stderr)
            profile.write_text(json.dumps({"schema": 1,
                "materialSourcePaths": {"Same": material_paths[0]}}), encoding="utf-8")
            resolved = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(resolved.returncode, 0, resolved.stderr)

    def test_identity_key_survives_export_move_without_changing_legacy_ids(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            document = {
                "asset": {"version": "2.0"},
                "buffers": [{"uri": "scene.bin", "byteLength": 36}],
                "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
                "accessors": [{"bufferView": 0, "componentType": 5126,
                               "count": 3, "type": "VEC3"}],
                "materials": [{"name": "Leaf"}],
                "meshes": [{"name": "Tree", "primitives": [
                    {"attributes": {"POSITION": 0}, "material": 0}]}],
                "nodes": [{"name": "TreeActor", "mesh": 0}],
                "scenes": [{"nodes": [0]}],
                "scene": 0,
                "extensions": {"KHR_lights_punctual": {"lights": []}},
            }

            def prepare(directory, identity_key):
                directory.mkdir()
                source = directory / "scene.gltf"
                source_bytes = (json.dumps(document) + "\n").encode("utf-8")
                source.write_bytes(source_bytes)
                (directory / "scene.bin").write_bytes(struct.pack("<9f", *([0.0] * 9)))
                manifest = directory / "materials.json"
                manifest.write_text("{}", encoding="utf-8")
                profile = directory / "profile.json"
                choices = {"schema": 1}
                if identity_key is not None:
                    choices["identityKey"] = identity_key
                profile.write_text(json.dumps(choices), encoding="utf-8")
                output = directory / "derived.gltf"
                result = subprocess.run(
                    [sys.executable, str(SCRIPT), str(source), str(manifest),
                     str(output), "--profile", str(profile)],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(source.read_bytes(), source_bytes)
                derived = json.loads(output.read_text(encoding="utf-8"))
                return source, [derived[key][0]["extras"]["he_id"]
                                for key in ("materials", "meshes", "nodes", "scenes")]

            key = "/Game/SICKA_DYNASTY/Maps/MAP_Demo"
            _, first_ids = prepare(root / "first", key)
            _, moved_ids = prepare(root / "moved", key)
            _, other_ids = prepare(root / "other", key + "_Other")
            legacy_source, legacy_ids = prepare(root / "legacy", None)
            self.assertEqual(first_ids, moved_ids)
            self.assertTrue(all(left != right for left, right in zip(first_ids, other_ids)))
            self.assertEqual(legacy_ids[2], str(uuid.uuid5(
                uuid.uuid5(uuid.NAMESPACE_URL,
                           "hengine:ue-level:" + str(legacy_source.resolve()).lower()),
                "node:TreeActor")))

    def test_preserves_authored_cutoff_and_uses_profile_only_as_fallback(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "scene.gltf"
            output = root / "derived.gltf"
            materials = root / "materials.json"
            profile = root / "profile.json"
            (root / "scene.bin").write_bytes(struct.pack("<9f", *([0.0] * 9)))
            document = {
                "asset": {"version": "2.0"},
                "buffers": [{"uri": "scene.bin", "byteLength": 36}],
                "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}],
                "accessors": [{"bufferView": 0, "componentType": 5126,
                               "count": 3, "type": "VEC3"}],
                "materials": [
                    {"name": "Leaf", "alphaMode": "MASK", "alphaCutoff": 0.3333},
                    {"name": "Roof"},
                    {"name": "Override", "alphaMode": "MASK", "alphaCutoff": 0.25},
                ],
                "meshes": [{"name": "Triangle", "primitives": [
                    {"attributes": {"POSITION": 0}, "material": 0}]}],
                "nodes": [{"name": "TriangleNode", "mesh": 0}],
                "scenes": [{"nodes": [0]}],
                "scene": 0,
                "extensions": {"KHR_lights_punctual": {"lights": []}},
            }
            source_bytes = (json.dumps(document) + "\n").encode("utf-8")
            source.write_bytes(source_bytes)
            materials.write_text("{}", encoding="utf-8")
            profile.write_text(json.dumps({
                "schema": 1,
                "maskedMaterials": ["Leaf", "Roof", "Override"],
                "alphaCutoff": 0.5,
                "alphaCutoffOverrides": {"Override": 0.4},
            }), encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(SCRIPT), str(source), str(materials),
                 str(output), "--profile", str(profile)],
                capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            derived = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual([material["alphaCutoff"] for material in derived["materials"]],
                             [0.3333, 0.5, 0.4])
            self.assertEqual(source.read_bytes(), source_bytes)


if __name__ == "__main__":
    unittest.main()
