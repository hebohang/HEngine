"""Source identity/no-op checks; requires no engine or GPU."""
import tempfile
from pathlib import Path
import unittest

from generate_sampling_fixture import generate


class SamplingFixtureTests(unittest.TestCase):
    def test_rebuild_is_identical_and_preserves_mtime(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generate(root)
            before = {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in root.iterdir()}
            generate(root)
            self.assertEqual(before, {p.name: (p.read_bytes(), p.stat().st_mtime_ns) for p in root.iterdir()})

    def test_phase_changes_keep_stable_entity_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            baseline = generate(directory)
            ids = [n["extras"]["he_id"] for n in baseline["nodes"]]
            for phase in ["material", "geometry", "rename"]:
                changed = generate(directory, phase)
                self.assertEqual(ids, [n["extras"]["he_id"] for n in changed["nodes"]])
            self.assertEqual(len(generate(directory, "add")["nodes"]), len(ids) + 1)
            self.assertEqual(len(generate(directory, "delete")["nodes"]), len(ids) - 1)

    def test_sampler_and_attribute_matrix(self):
        with tempfile.TemporaryDirectory() as directory:
            data = generate(directory, "add")
            self.assertEqual({s["minFilter"] for s in data["samplers"]}, {9728, 9729, 9984, 9985, 9986, 9987})
            for mesh in data["meshes"]:
                self.assertIn("TEXCOORD_1", mesh["primitives"][0]["attributes"])
                self.assertIn("COLOR_0", mesh["primitives"][0]["attributes"])
            for material in data["materials"]:
                index = material["pbrMetallicRoughness"]["baseColorTexture"]["index"]
                self.assertEqual(data["textures"][index]["source"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
