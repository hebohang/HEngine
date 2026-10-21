"""Keep the explicit Assimp baked-model path and native mesh preflight covered."""
from pathlib import Path
import tempfile
import unittest

import content_sync as sync
from test_native_import import PROJECT, IMPORTER


class LegacyImportTests(unittest.TestCase):
    def test_existing_obj_and_gltf_baked_models(self):
        for relative in ["Assets/Models/Box.obj", "SourceAssets/CornellBox/Generated/CornellBox.gltf"]:
            with self.subTest(source=relative), tempfile.TemporaryDirectory(prefix="legacy-smoke-", dir=str(PROJECT / "Assets/Imported")) as root:
                root = Path(root)
                self.assertEqual(sync.run_process([IMPORTER, PROJECT, PROJECT / relative, root], root / "import.log", 60), 0)
                meshes = list(root.glob("*.hmesh"))
                self.assertTrue(meshes)
                scene = sync.read_json(PROJECT / "Assets/CornellBox/CornellBox.he")
                for entity in scene.values():
                    if "MeshComponent" in entity:
                        mesh = entity["MeshComponent"]
                        mesh["Path"] = mesh["meshResPath"] = meshes[0].relative_to(PROJECT).as_posix()
                        mesh["MaterialOverrides"] = []
                candidate = root / "candidate.he"
                candidate.write_bytes(sync.encode(scene))
                self.assertEqual(sync.run_process([IMPORTER, PROJECT, candidate, root, "--validate-scene"], root / "preflight.log", 60), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
