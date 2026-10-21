"""Headless contract for the UE texture exporter without a running editor."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("ExportLevelTextures.py")


class ExportLevelTexturesTests(unittest.TestCase):
    def test_same_basename_from_two_packages_remains_distinct_on_reexport(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            references = ["/Game/Forest/T_Leaf.T_Leaf", "/Game/Garden/T_Leaf.T_Leaf"]
            materials = {"/Game/Forest/M.M": {"textures": {"BaseColor": references[0]}},
                         "/Game/Garden/M.M": {"textures": {"BaseColor": references[1]}}}
            manifest_input = root / "materials.json"
            manifest_input.write_text(json.dumps(materials), encoding="utf-8")
            (root / "unreal.py").write_text(
                "from pathlib import Path\n"
                "class Texture: pass\n"
                "class EditorAssetLibrary:\n"
                "    @staticmethod\n"
                "    def load_asset(path): return Texture()\n"
                "class AssetExportTask:\n"
                "    def __init__(self): self.errors = []\n"
                "class TextureExporterPNG: pass\n"
                "class Exporter:\n"
                "    @staticmethod\n"
                "    def run_asset_export_task(task):\n"
                "        Path(task.filename).write_bytes(b'PNG fixture')\n"
                "        return True\n", encoding="utf-8")
            environment = dict(os.environ, HE_UE_MATERIAL_INPUT=str(manifest_input),
                               HE_UE_TEXTURE_OUTPUT_DIR=str(root / "Textures"),
                               PYTHONPATH=str(root))
            command = [sys.executable, str(SCRIPT)]
            first = subprocess.run(command, env=environment, capture_output=True,
                                   text=True, timeout=30)
            self.assertEqual(first.returncode, 0, first.stderr)
            exported = json.loads((root / "texture-export.json").read_text(encoding="utf-8"))
            self.assertEqual({row["asset"] for row in exported}, set(references))
            self.assertEqual(len({Path(row["output"]).name.casefold() for row in exported}), 2)
            self.assertTrue(all(row["ok"] and row["bytes"] > 0 and
                                row["relativeOutput"].startswith("Textures/")
                                for row in exported))
            second = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, timeout=30)
            self.assertEqual(second.returncode, 0, second.stderr)
            repeated = json.loads((root / "texture-export.json").read_text(encoding="utf-8"))
            self.assertEqual([row["output"] for row in exported],
                             [row["output"] for row in repeated])
            self.assertTrue(all(row["reused"] for row in repeated))


if __name__ == "__main__":
    unittest.main()
