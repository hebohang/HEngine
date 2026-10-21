"""Black-box engine importer tests (not third-party cgltf tests)."""
import argparse
import base64
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid

from content_sync import encode, read_json

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "Project"
IMPORTER = Path(os.environ.get("HE_ASSET_IMPORTER", str(
    ROOT / "out/agent/editor-relwithdebinfo/bin/Programs/HAssetImporter/HAssetImporter.exe")))


def metadata(label):
    return dict(he_schema=1, he_id=str(uuid.uuid5(uuid.NAMESPACE_URL, "he-native-test/" + label)))


def fixture():
    binary = b"".join(struct.pack("<6f", *p, 0, 0, 1) for p in [(0, 0, 0), (1, 0, 0), (0, 1, 0)])
    binary += struct.pack("<3H", 0, 1, 2) + b"\0\0"
    binary += bytes([0, 0, 255, 0, 0, 255])
    document = dict(asset={"version": "2.0"}, scene=0,
        scenes=[dict(nodes=[0], extras=metadata("scene"))],
        nodes=[dict(name="Triangle", mesh=0, extras=metadata("node"))],
        meshes=[dict(name="Geometry", extras=metadata("mesh"), primitives=[dict(attributes={"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 3}, indices=2, material=0)])],
        materials=[dict(extras=metadata("material"), pbrMetallicRoughness={"metallicFactor": 0.0})],
        buffers=[dict(byteLength=len(binary), uri="data:application/octet-stream;base64," + base64.b64encode(binary).decode())],
        bufferViews=[dict(buffer=0, byteOffset=0, byteLength=72, byteStride=24), dict(buffer=0, byteOffset=72, byteLength=6), dict(buffer=0, byteOffset=80, byteLength=6)],
        accessors=[dict(bufferView=0, componentType=5126, type="VEC3", count=3, min=[0, 0, 0], max=[1, 1, 0]),
                   dict(bufferView=0, byteOffset=12, componentType=5126, type="VEC3", count=3),
                   dict(bufferView=1, componentType=5123, type="SCALAR", count=3),
                   dict(bufferView=2, componentType=5121, normalized=True, type="VEC2", count=3)])
    return document, binary


class NativeImporterTests(unittest.TestCase):
    def setUp(self):
        (PROJECT / ".he-content").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="native-test-", dir=str(PROJECT / ".he-content"))
        self.directory = Path(self.temp.name)
        self.staging = self.directory / "stage"
        self.staging.mkdir()
        self.document, self.binary = fixture()

    def tearDown(self):
        self.temp.cleanup()

    def convert(self, glb=False):
        source = self.directory / ("source.glb" if glb else "source.gltf")
        if glb:
            self.document["buffers"][0].pop("uri", None)
            text = encode(self.document)
            text += b" " * (-len(text) % 4)
            binary = self.binary + b"\0" * (-len(self.binary) % 4)
            source.write_bytes(struct.pack("<III", 0x46546C67, 2, 28+len(text)+len(binary)) + struct.pack("<II", len(text), 0x4E4F534A) + text + struct.pack("<II", len(binary), 0x004E4942) + binary)
        else:
            source.write_bytes(encode(self.document))
        result = subprocess.run([str(IMPORTER), str(PROJECT), str(source), str(self.staging), "--scene"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        report = read_json(self.staging / "conversion.json")
        self.assertEqual(result.returncode == 0, report["ok"])
        return report

    def values(self, attribute):
        mesh = read_json(self.staging / "mesh-0.hmesh")
        accessor = mesh["accessorVec"][mesh["primitives"][0]["attributes"][attribute]]
        view = mesh["bufferViewVec"][accessor["bufferViewIndex"]]
        data = (self.staging / "mesh-0.bin").read_bytes()
        return struct.unpack_from("<" + "f" * (view["byteLength"] // 4), data, view["byteOffset"])

    def test_stride_normalized_and_indices(self):
        self.assertTrue(self.convert()["ok"])
        self.assertEqual(self.values("POSITION"), (0,0,0, 1,0,0, 0,1,0))
        self.assertEqual(self.values("TEXCOORD_0"), (0,0, 1,0, 0,1))

    def test_glb_embedded(self):
        self.assertTrue(self.convert(glb=True)["ok"])
        self.assertEqual(self.values("NORMAL"), (0,0,1)*3)

    def test_external_buffer(self):
        (self.directory / "buffer.bin").write_bytes(self.binary)
        self.document["buffers"][0]["uri"] = "buffer.bin"
        report = self.convert()
        self.assertTrue(report["ok"])
        self.assertEqual(len(report["dependencies"]), 1)

    def test_sparse_accessor(self):
        binary = self.binary + b"\0" * (-len(self.binary) % 4)
        index_offset = len(binary)
        binary += bytes([1, 0, 0, 0])
        value_offset = len(binary)
        binary += struct.pack("<3f", 2, 0, 0)
        self.document["buffers"][0] = dict(byteLength=len(binary), uri="data:application/octet-stream;base64,"+base64.b64encode(binary).decode())
        self.document["bufferViews"].extend([dict(buffer=0, byteOffset=index_offset, byteLength=1), dict(buffer=0, byteOffset=value_offset, byteLength=12)])
        self.document["accessors"][0]["sparse"] = dict(count=1, indices=dict(bufferView=3, componentType=5121), values=dict(bufferView=4))
        self.assertTrue(self.convert()["ok"])
        self.assertEqual(self.values("POSITION")[3], 2)

    def test_invalid_index_fails(self):
        binary = bytearray(self.binary)
        struct.pack_into("<H", binary, 72, 55)
        self.document["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(binary).decode()
        self.assertFalse(self.convert()["ok"])

    def test_missing_dependency_fails(self):
        self.document["buffers"][0]["uri"] = "missing.bin"
        report = self.convert()
        self.assertEqual(report["diagnostics"][0]["code"], "dependency_missing")

    def test_sparse_triangle_indices(self):
        binary = self.binary + b"\0" * (-len(self.binary) % 4)
        offset = len(binary)
        binary += bytes([1, 2, 0, 0]) + struct.pack("<2H", 2, 1)
        self.document["buffers"][0] = dict(byteLength=len(binary), uri="data:application/octet-stream;base64,"+base64.b64encode(binary).decode())
        self.document["bufferViews"].extend([dict(buffer=0, byteOffset=offset, byteLength=2), dict(buffer=0, byteOffset=offset+4, byteLength=4)])
        self.document["accessors"][2]["sparse"] = dict(count=2, indices=dict(bufferView=3, componentType=5121), values=dict(bufferView=4))
        report = self.convert()
        self.assertTrue(report["ok"], report["diagnostics"])
        mesh = read_json(self.staging / "mesh-0.hmesh")
        accessor = mesh["accessorVec"][mesh["primitives"][0]["indices"]]
        view = mesh["bufferViewVec"][accessor["bufferViewIndex"]]
        self.assertEqual(struct.unpack_from("<3I", (self.staging / "mesh-0.bin").read_bytes(), view["byteOffset"]), (0,2,1))

    def test_path_escape_fails(self):
        self.document["buffers"][0]["uri"] = "../escape.bin"
        self.assertEqual(self.convert()["diagnostics"][0]["code"], "dependency_escape")

    def test_required_extension_has_no_fallback(self):
        self.document["extensionsUsed"] = ["KHR_draco_mesh_compression"]
        self.document["extensionsRequired"] = ["KHR_draco_mesh_compression"]
        self.assertEqual(self.convert()["diagnostics"][0]["code"], "extension_unsupported")

    def test_missing_identity_fails(self):
        self.document["nodes"][0].pop("extras")
        report = self.convert()
        self.assertFalse(report["ok"])
        self.assertEqual(report["diagnostics"][0]["name"], "Triangle")

    def test_duplicate_identity_fails(self):
        self.document["nodes"].append(copy.deepcopy(self.document["nodes"][0]))
        self.document["scenes"][0]["nodes"].append(1)
        self.assertEqual(self.convert()["diagnostics"][0]["code"], "identity_duplicate")

    def test_mirrored_trs(self):
        self.document["nodes"][0]["scale"] = [-2, 1, 0.5]
        self.assertTrue(self.convert()["ok"])

    def test_shear_rejected(self):
        self.document["nodes"][0]["matrix"] = [1,0,0,0, .5,1,0,0, 0,0,1,0, 0,0,0,1]
        self.assertEqual(self.convert()["diagnostics"][0]["code"], "transform_shear")

    def test_distinct_material_slots(self):
        self.document["materials"].append(dict(extras=metadata("second-material")))
        primitive = copy.deepcopy(self.document["meshes"][0]["primitives"][0])
        primitive["material"] = 1
        self.document["meshes"][0]["primitives"].append(primitive)
        report = self.convert()
        self.assertTrue(report["ok"])
        mesh = read_json(self.staging / "mesh-0.hmesh")
        self.assertNotEqual(mesh["primitives"][0]["materialResPath"], mesh["primitives"][1]["materialResPath"])
        self.assertEqual(len(next(iter(report["nodes"].values()))["slots"]), 2)

    def test_static_collision_roundtrip(self):
        self.document["nodes"][0]["extras"]["he_collision"] = "mesh"
        report = self.convert()
        self.assertTrue(report["ok"], report["diagnostics"])
        self.assertTrue((self.staging / "mesh-0.hcollision").exists())

    def test_orthographic_camera_preserves_clip_planes(self):
        self.document["cameras"] = [dict(type="orthographic", orthographic=dict(xmag=4, ymag=2, znear=.1, zfar=50))]
        self.document["nodes"][0]["camera"] = 0
        self.document["nodes"][0]["extras"]["he_primary"] = True
        report = self.convert()
        self.assertTrue(report["ok"], report["diagnostics"])
        camera = next(iter(report["nodes"].values()))["entity"]["CameraComponent"]["Camera"]
        self.assertAlmostEqual(camera["mOrthographicNear"], .1)
        self.assertEqual(camera["mOrthographicFar"], 50)
        self.assertEqual(camera["mAspectRatio"], 2)

    def test_directional_light_preserves_gltf_minus_z_axis(self):
        self.document["extensionsUsed"] = ["KHR_lights_punctual"]
        self.document["extensions"] = {"KHR_lights_punctual": {"lights": [
            {"type": "directional", "color": [1, 0.8, 0.6], "intensity": 4}
        ]}}
        self.document["nodes"][0]["extensions"] = {"KHR_lights_punctual": {"light": 0}}
        report = self.convert()
        self.assertTrue(report["ok"], report["diagnostics"])
        light = next(iter(report["nodes"].values()))["entity"]["DirectionalLightComponent"]
        self.assertEqual(light["LocalDirection"], [0, 0, -1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
