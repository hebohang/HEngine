"""Small geometry contract for offline glTF ground height sampling."""

import json
import struct
import tempfile
import unittest
from pathlib import Path

from GltfGroundSampler import GltfGroundSampler


class GltfGroundSamplerTests(unittest.TestCase):
    def test_world_transforms_and_highest_overlapping_receiver(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            vertices = ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1))
            binary = b"".join(struct.pack("<fff", *vertex) for vertex in vertices)
            binary += struct.pack("<HHHHHH", 0, 1, 2, 0, 2, 3)
            (root / "ground.bin").write_bytes(binary)
            gltf = {
                "asset": {"version": "2.0"},
                "buffers": [{"uri": "ground.bin", "byteLength": len(binary)}],
                "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48},
                                {"buffer": 0, "byteOffset": 48, "byteLength": 12}],
                "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4,
                               "type": "VEC3"},
                              {"bufferView": 1, "componentType": 5123, "count": 6,
                               "type": "SCALAR"}],
                "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
                "nodes": [{"translation": [0, 0.2, 0], "mesh": 0},
                          {"translation": [0, 0.4, 0], "mesh": 0},
                          {"translation": [2, 0.2, 0], "mesh": 0}],
                "scenes": [{"nodes": [0, 1, 2]}], "scene": 0,
            }
            source = root / "ground.gltf"
            source.write_text(json.dumps(gltf), encoding="utf-8")
            sampler = GltfGroundSampler(source)
            self.assertAlmostEqual(sampler.highest_surface(0.5, 0.5, 0.3), 0.4)
            self.assertAlmostEqual(sampler.highest_surface(2.5, 0.5, 0.3), 0.2)
            self.assertIsNone(sampler.highest_surface(1.5, 0.5, 0.3))


if __name__ == "__main__":
    unittest.main()
