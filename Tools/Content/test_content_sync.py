import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import os
import sys
from types import SimpleNamespace

spec = importlib.util.spec_from_file_location("content_sync", Path(__file__).with_name("content_sync.py"))
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


def entity():
    return {"TagComponent": {"Tag": "Source"},
            "TransformComponent": {"Translation": [0, 0, 0], "Rotation": [0, 0, 0], "Scale": [1, 1, 1], "ParentUuid": ""},
            "MeshComponent": {"Path": "Assets/a.hmesh", "meshResPath": "Assets/a.hmesh", "MaterialOverrides": ["Assets/red.hmat", "Assets/blue.hmat"]}}


class ContentTests(unittest.TestCase):
    def test_missing_importer_fails_before_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            project = root / "project"
            project.mkdir()
            (project / "Project.heproject").write_text("{}", encoding="utf-8")
            source = root / "scene.gltf"
            source.write_text("{}", encoding="utf-8")
            args = SimpleNamespace(project=str(project), source=str(source),
                                   importer=str(root / "missing-importer"), mode="scene")
            with self.assertRaises(sync.Failure) as raised:
                sync.synchronize(args, root / "report")
            self.assertEqual(raised.exception.diagnostic["code"], "importer_argument")
            self.assertFalse((project / ".he-content" / "staging").exists())

    def setUp(self):
        self.old = {"object": entity()}
        self.keys = {"object": ["mesh/red", "mesh/blue"]}

    def merge(self, current, incoming, slots=None):
        return sync.merge_scene(self.old, current, incoming, self.keys, slots or self.keys)

    def test_noop_preserves_play_components(self):
        current = copy.deepcopy(self.old)
        current["object"]["PythonScriptComponent"] = {"Path": "Scripts/Controller.py"}
        merged, changes = self.merge(current, self.old)
        self.assertEqual(merged, current)
        self.assertEqual(changes, dict(added=0, modified=0, deleted=0))

    def test_transform_only_retains_geometry(self):
        incoming = copy.deepcopy(self.old)
        incoming["object"]["TransformComponent"]["Translation"] = [1, 2, 3]
        merged, changes = self.merge(self.old, incoming)
        self.assertEqual(changes["modified"], 1)
        self.assertEqual(merged["object"]["MeshComponent"], self.old["object"]["MeshComponent"])

    def test_spot_light_parameters_follow_reimport(self):
        old = copy.deepcopy(self.old)
        old["object"]["SpotLightComponent"] = {
            "Intensity": 1, "IntensityUnits": 1, "LightColor": [1, 0.5, 0.2],
            "Radius": 8, "InnerConeAngle": 0, "OuterConeAngle": 30,
        }
        incoming = copy.deepcopy(old)
        incoming["object"]["SpotLightComponent"]["OuterConeAngle"] = 35
        merged, changes = sync.merge_scene(old, old, incoming, self.keys, self.keys)
        self.assertEqual(changes["modified"], 1)
        self.assertEqual(merged["object"]["SpotLightComponent"]["OuterConeAngle"], 35)

    def test_owned_field_conflict_even_source_unchanged(self):
        current = copy.deepcopy(self.old)
        current["object"]["TagComponent"]["Tag"] = "Engine rename"
        with self.assertRaises(sync.Failure) as error:
            self.merge(current, self.old)
        self.assertEqual(error.exception.diagnostic["property"], "TagComponent.Tag")

    def test_slot_reorder_preserves_override_identity(self):
        current, incoming = copy.deepcopy(self.old), copy.deepcopy(self.old)
        current["object"]["MeshComponent"]["MaterialOverrides"][0] = "Assets/game.hmat"
        incoming["object"]["MeshComponent"]["MaterialOverrides"].reverse()
        merged, _ = self.merge(current, incoming, {"object": ["mesh/blue", "mesh/red"]})
        self.assertEqual(merged["object"]["MeshComponent"]["MaterialOverrides"], ["Assets/blue.hmat", "Assets/game.hmat"])

    def test_replaced_material_rejects_old_override(self):
        current = copy.deepcopy(self.old)
        current["object"]["MeshComponent"]["MaterialOverrides"][0] = "Assets/game.hmat"
        with self.assertRaises(sync.Failure):
            self.merge(current, self.old, {"object": ["mesh/new", "mesh/blue"]})

    def test_deletion_with_gameplay_is_rejected(self):
        current = copy.deepcopy(self.old)
        current["object"]["Rigidbody3DComponent"] = {"mass": 12}
        with self.assertRaises(sync.Failure):
            self.merge(current, {})

    def test_deletion_with_user_reference_is_rejected(self):
        current = copy.deepcopy(self.old)
        current["user"] = {"link": "object"}
        with self.assertRaises(sync.Failure):
            self.merge(current, {})

    def test_plain_source_deletion(self):
        merged, changes = self.merge(self.old, {})
        self.assertEqual(merged, {})
        self.assertEqual(changes["deleted"], 1)

    def test_concurrent_component_addition_rejected(self):
        current, incoming = copy.deepcopy(self.old), copy.deepcopy(self.old)
        current["object"]["CameraComponent"] = {"Primary": False}
        incoming["object"]["CameraComponent"] = {"Primary": True}
        with self.assertRaises(sync.Failure):
            self.merge(current, incoming)

    def test_atomic_noop_keeps_mtime(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "scene.he"
            self.assertTrue(sync.atomic(path, b"scene"))
            before = path.stat().st_mtime_ns
            self.assertFalse(sync.atomic(path, b"scene"))
            self.assertEqual(path.stat().st_mtime_ns, before)

    def test_recover_after_scene_before_record(self):
        with tempfile.TemporaryDirectory() as root:
            task = Path(root)
            scene = task / "scene.he"
            sync.atomic(scene, b"after")
            sync.atomic(task / "pending.json", sync.encode(dict(before=sync.digest(b"before"), after=sync.digest(b"after"), record={"baseline": "new"})))
            sync.recover(task, scene)
            self.assertEqual(sync.read_json(task / "state.json"), {"baseline": "new"})
            self.assertFalse((task / "pending.json").exists())

    def test_recover_before_scene_retains_old_record(self):
        with tempfile.TemporaryDirectory() as root:
            task = Path(root)
            scene = task / "scene.he"
            sync.atomic(scene, b"before")
            sync.atomic(task / "state.json", b'{"old":true}')
            sync.atomic(task / "pending.json", sync.encode(dict(before=sync.digest(b"before"), after=sync.digest(b"after"), record={"new": True})))
            sync.recover(task, scene)
            self.assertEqual(sync.read_json(task / "state.json"), {"old": True})

    def test_recovery_does_not_overwrite_external_scene(self):
        with tempfile.TemporaryDirectory() as root:
            task = Path(root)
            scene = task / "scene.he"
            sync.atomic(scene, b"external")
            sync.atomic(task / "pending.json", sync.encode(dict(before="old", after="new", record={})))
            with self.assertRaises(sync.Failure):
                sync.recover(task, scene)
            self.assertEqual(scene.read_bytes(), b"external")
            self.assertTrue((task / "pending.json").exists())

    def test_content_graph_deduplicates_geometry(self):
        with tempfile.TemporaryDirectory() as root:
            staging = Path(root)
            (staging / "a.bin").write_bytes(b"same geometry")
            (staging / "b.bin").write_bytes(b"same geometry")
            for name in ["a", "b"]:
                (staging / (name + ".hmesh")).write_text(json.dumps({"buffer": "__content__/" + name + ".bin"}))
            mapping, resources = sync.prepare_resources(staging, "Assets/Imported/test")
            self.assertEqual(mapping["__content__/a.hmesh"], mapping["__content__/b.hmesh"])
            self.assertEqual(len(resources), 2)

    def test_path_jail(self):
        with tempfile.TemporaryDirectory() as root:
            for escape in ["../user.he", "../../other"]:
                with self.assertRaises(sync.Failure):
                    sync.confined(root, escape)

    @unittest.skipUnless(os.name == "nt", "Windows owned-process tree contract")
    def test_timeout_reaps_child_process(self):
        import ctypes
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            pid_file = root / "child.pid"
            script = root / "spawn.py"
            script.write_text("import subprocess,sys,pathlib\np=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])\npathlib.Path(sys.argv[1]).write_text(str(p.pid))\np.wait()\n")
            with self.assertRaises(sync.Failure) as failure:
                sync.run_process([sys.executable, script, pid_file], root / "timeout.log", 1)
            self.assertEqual(failure.exception.exit_code, 5)
            process_id = int(pid_file.read_text())
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.restype = ctypes.c_void_p
            kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            handle = kernel.OpenProcess(0x100000, False, process_id)
            if handle:
                try:
                    self.assertEqual(kernel.WaitForSingleObject(handle, 2000), 0)
                finally:
                    kernel.CloseHandle(handle)


if __name__ == "__main__":
    unittest.main(verbosity=2)
