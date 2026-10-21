"""End-to-end native publication contracts; uses real HAssetImporter preflight."""
import argparse
import base64
import copy
import json
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import uuid

import content_sync as sync
from test_native_import import fixture, PROJECT, IMPORTER


class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="workflow-test-", dir=str(PROJECT / ".he-content"))
        self.directory = Path(self.temp.name)
        self.source = self.directory / "source.gltf"
        self.document, _ = fixture()
        self.document_id = str(uuid.uuid4())
        self.document["scenes"][0]["extras"]["he_id"] = self.document_id
        self.root = PROJECT / "Assets/Imported" / self.document_id
        self.task = PROJECT / ".he-content" / self.document_id
        self.write()
        self.args = argparse.Namespace(project=str(PROJECT), source=str(self.source), importer=str(IMPORTER),
            action="sync", blender="", initialize_ids=False, verify="none", timeout=30, fault="")

    def tearDown(self):
        for path, root in [(self.root, PROJECT / "Assets/Imported"), (self.task, PROJECT / ".he-content")]:
            if path.exists() and path.parent == root and path.name == self.document_id:
                shutil.rmtree(str(path))
        self.temp.cleanup()

    def write(self):
        sync.atomic(self.source, sync.encode(self.document))

    def run_sync(self):
        return sync.synchronize(self.args, self.directory)

    def snapshot(self):
        return {str(p.relative_to(self.root)): (sync.file_hash(p), p.stat().st_mtime_ns)
                for p in self.root.rglob("*") if p.is_file()}

    def scene(self):
        return sync.read_json(self.root / "Scene.he")

    def test_first_import_and_noop_no_mtime_change(self):
        first = self.run_sync()
        before = self.snapshot()
        state_time = (self.task / "state.json").stat().st_mtime_ns
        second = self.run_sync()
        self.assertTrue(first["changed"])
        self.assertFalse(second["changed"])
        self.assertEqual(first["importVersion"], second["importVersion"])
        self.assertEqual(before, self.snapshot())
        self.assertEqual(state_time, (self.task / "state.json").stat().st_mtime_ns)

    def test_transform_rename_and_instance_preserve_ids_and_mesh(self):
        self.run_sync()
        first = self.scene()
        original_id = next(iter(first))
        mesh = first[original_id]["MeshComponent"]["Path"]
        self.document["nodes"][0].update(name="Renamed", translation=[1, 2, 3])
        self.write()
        result = self.run_sync()
        self.assertEqual(result["newResources"], 0)
        self.assertEqual(next(iter(self.scene())), original_id)
        duplicate = copy.deepcopy(self.document["nodes"][0])
        duplicate["extras"]["he_id"] = str(uuid.uuid4())
        duplicate["name"] = "Shared instance"
        self.document["nodes"].append(duplicate)
        self.document["scenes"][0]["nodes"].append(1)
        self.write()
        result = self.run_sync()
        self.assertEqual(result["newResources"], 0)
        self.assertEqual({e["MeshComponent"]["Path"] for e in self.scene().values()}, {mesh})

    def test_material_edit_does_not_rebuild_mesh(self):
        self.run_sync()
        before = {k:v for k,v in self.snapshot().items() if k.endswith((".bin", ".hmesh"))}
        self.document["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = [.2, .3, .4, 1]
        self.write()
        self.assertEqual(self.run_sync()["newResources"], 1)
        after = {k:v for k,v in self.snapshot().items() if k.endswith((".bin", ".hmesh"))}
        self.assertEqual(before, after)

    def test_geometry_edit_only_rebuilds_mesh_and_buffer(self):
        self.run_sync()
        before = {k:v for k,v in self.snapshot().items() if k.endswith(".hmat")}
        old_identity = next(iter(self.scene()))
        binary = bytearray(base64.b64decode(self.document["buffers"][0]["uri"].split(",", 1)[1]))
        struct.pack_into("<f", binary, 24, 2.0)
        self.document["accessors"][0]["max"] = [2,1,0]
        self.document["buffers"][0]["uri"] = "data:application/octet-stream;base64,"+base64.b64encode(binary).decode()
        self.write()
        self.assertEqual(self.run_sync()["newResources"], 2)
        self.assertEqual(next(iter(self.scene())), old_identity)
        self.assertEqual(before, {k:v for k,v in self.snapshot().items() if k.endswith(".hmat")})

    def test_same_name_recreated_object_has_new_identity(self):
        self.run_sync()
        old_identity = next(iter(self.scene()))
        node = self.document["nodes"][0]
        self.document["nodes"] = []
        self.document["scenes"][0]["nodes"] = []
        self.write()
        self.assertEqual(self.run_sync()["changes"]["deleted"], 1)
        node["extras"]["he_id"] = str(uuid.uuid4())
        self.document["nodes"] = [node]
        self.document["scenes"][0]["nodes"] = [0]
        self.write()
        self.assertEqual(self.run_sync()["newResources"], 0)
        self.assertNotEqual(next(iter(self.scene())), old_identity)

    def test_gameplay_component_survives_reimport(self):
        self.run_sync()
        current = self.scene()
        object_id = next(iter(current))
        current[object_id]["Rigidbody3DComponent"] = {"mass": 7.5}
        sync.atomic(self.root / "Scene.he", sync.encode(current))
        self.document["nodes"][0]["translation"] = [1, 0, 0]
        self.write()
        self.run_sync()
        self.assertEqual(self.scene()[object_id]["Rigidbody3DComponent"]["mass"], 7.5)

    def test_owned_conflict_retains_scene_and_resources(self):
        self.run_sync()
        current = self.scene()
        current[next(iter(current))]["TransformComponent"]["Translation"] = [99,0,0]
        sync.atomic(self.root / "Scene.he", sync.encode(current))
        before = self.snapshot()
        with self.assertRaises(sync.Failure) as error:
            self.run_sync()
        self.assertEqual(error.exception.diagnostic["code"], "owned_field_conflict")
        self.assertEqual(before, self.snapshot())

    def test_plan_does_not_publish(self):
        self.run_sync()
        before = self.snapshot()
        self.document["nodes"][0]["name"] = "Planned rename"
        self.write()
        self.args.action = "plan"
        self.assertTrue(self.run_sync()["changed"])
        self.assertEqual(before, self.snapshot())

    def test_fault_before_scene_preserves_old_then_recovers(self):
        self.run_sync()
        old = (self.root / "Scene.he").read_bytes()
        self.document["nodes"][0]["name"] = "New version"
        self.write()
        self.args.fault = "before-scene"
        with self.assertRaises(sync.Failure):
            self.run_sync()
        self.assertEqual((self.root / "Scene.he").read_bytes(), old)
        self.args.fault = ""
        self.run_sync()
        self.assertFalse((self.task / "pending.json").exists())
        self.assertNotEqual((self.root / "Scene.he").read_bytes(), old)

    def test_fault_after_scene_recovers_record_without_rewrite(self):
        self.run_sync()
        self.document["nodes"][0]["name"] = "Committed new version"
        self.write()
        self.args.fault = "after-scene"
        with self.assertRaises(sync.Failure):
            self.run_sync()
        snapshot = self.snapshot()
        self.args.fault = ""
        result = self.run_sync()
        self.assertFalse(result["changed"])
        self.assertEqual(snapshot, self.snapshot())
        self.assertFalse((self.task / "pending.json").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
