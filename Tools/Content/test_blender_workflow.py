"""Real Blender export/identity regression. Creates and cleans only its own document."""
import argparse
import json
from pathlib import Path
import shutil
import tempfile
import unittest

import content_sync as sync
from test_native_import import PROJECT, IMPORTER


class BlenderWorkflowTests(unittest.TestCase):
    def test_authoring_identity_and_evaluated_sharing(self):
        blender = sync.find_blender("")
        tools = Path(__file__).parent.resolve()
        with tempfile.TemporaryDirectory(prefix="blender-test-", dir=str(PROJECT / ".he-content")) as directory:
            directory = Path(directory)
            source = directory / "source.blend"
            assets = task = None
            def run(arguments):
                self.assertEqual(sync.run_process([blender, "--background", "--factory-startup", "--disable-autoexec"] + arguments,
                                                 directory / "blender.log", 120), 0)
            run(["--python-exit-code", "7", "--python", tools / "make_acceptance_scene.py", "--", source])
            args = argparse.Namespace(project=str(PROJECT), source=str(source), importer=str(IMPORTER),
                action="sync", blender=blender, initialize_ids=True, verify="none", timeout=120, fault="")
            counter = 0
            def synchronize():
                nonlocal counter
                report = directory / ("report-" + str(counter)); counter += 1; report.mkdir()
                return sync.synchronize(args, report)
            try:
                first = synchronize()
                assets = (PROJECT / first["scene"]).parent
                task = PROJECT / ".he-content" / assets.name
                record = sync.read_json(task / "state.json")
                original = next((key, node) for key, node in record["sourceObjects"].items() if node["name"] == "Shared Crate 0")
                snapshot = {p: (sync.file_hash(p), p.stat().st_mtime_ns) for p in assets.rglob("*") if p.is_file()}
                snapshot[task / "state.json"] = (sync.file_hash(task / "state.json"), (task / "state.json").stat().st_mtime_ns)
                self.assertFalse(synchronize()["changed"])
                self.assertEqual(snapshot, {p: (sync.file_hash(p), p.stat().st_mtime_ns) for p in snapshot})
                request = directory / "batch.json"
                request.write_bytes(sync.encode(dict(source=str(source), result=str(directory / "batch-result.json"), operations=[
                    dict(op="rename", id=original[0], value="Renamed original"),
                    dict(op="translate", id=original[0], value=[1, 0, .5]),
                    dict(op="parent", id=original[0], parentId=None),
                    dict(op="duplicate", id=original[0], newName="Copied shared instance") ])))
                run([source, "--python-exit-code", "7", "--python", tools / "blender_batch.py", "--", request])
                result = synchronize()
                updated = sync.read_json(task / "state.json")
                self.assertEqual(result["newResources"], 0)
                self.assertEqual(updated["identities"][original[0]], record["identities"][original[0]])
                self.assertEqual(updated["sourceObjects"][original[0]]["name"], "Renamed original")
                copied = next(node for node in updated["sourceObjects"].values() if node["name"] == "Copied shared instance")
                self.assertEqual(copied["meshId"], original[1]["meshId"])
                self.assertEqual(copied["meshPath"], original[1]["meshPath"])
                self.assertNotEqual(copied["uuid"], original[1]["uuid"])
                self.assertFalse(synchronize()["changed"])
            finally:
                for path, root in [(assets, PROJECT / "Assets/Imported"), (task, PROJECT / ".he-content")]:
                    if path and path.exists() and path.parent == root and sync.uuid.UUID(path.name):
                        shutil.rmtree(str(path))


if __name__ == "__main__":
    unittest.main(verbosity=2)
