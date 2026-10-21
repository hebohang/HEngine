"""CPU-only contract tests; no native importer or renderer is launched."""
import copy
import base64
from pathlib import Path
import tempfile
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import uuid

import content_sync as sync
import baked_model_adapter as adapter
from baked_model_acceptance import stretch_clip, move_mouth_bind, run as run_acceptance


def identified(**values):
    return dict(values, extras=dict(he_schema=1, he_id=str(uuid.uuid4())))


def document():
    return dict(scene=0, scenes=[identified(nodes=[0])], nodes=[identified(name="Model", mesh=0, skin=0), identified(name="Joint")],
        skins=[identified(joints=[1])], meshes=[identified(primitives=[dict(material=0)])], materials=[identified(name="Mat")],
        animations=[identified(name="Walk"), identified(name="Idle")])


def native_outputs(project, staging, doc):
    prefix = staging.relative_to(project).as_posix()
    sync.atomic(staging / "model.bin", b"geometry")
    sync.atomic(staging / "mat.hmat", sync.encode({}))
    bones = [dict(name=doc["nodes"][index]["name"], index=i, localBindT=doc["nodes"][index].get("translation", [0, 0, 0]))
        for i, index in enumerate(doc["skins"][0]["joints"])]
    sync.atomic(staging / "model.hskel", sync.encode(dict(bones=bones)))
    sync.atomic(staging / "model.hmesh", sync.encode(dict(skeletonPath=prefix + "/model.hskel",
        bufferVec=[dict(uri=(prefix + "/model.bin").replace("/", "\\"))], primitives=[dict(materialResPath=prefix + "/mat.hmat")], weights=[])))
    for clip in doc["animations"]:
        duration = clip.get("duration", 1)
        if clip.get("samplers"):
            accessor = doc["accessors"][clip["samplers"][0]["input"]]
            view = doc["bufferViews"][accessor["bufferView"]]
            payload = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
            start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            duration = max(struct.unpack_from("<f", payload, start + i * view.get("byteStride", 4))[0] for i in range(accessor["count"]))
        sync.atomic(staging / (clip["name"] + ".hanim"), sync.encode(dict(name=clip["name"], meshResPath=prefix + "/model.hmesh",
            targetSkeletonPath=prefix + "/model.hskel", boneInfos=bones, clips=[dict(name=clip["name"], duration=duration)])))


class BakedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.project = Path(self.temp.name)
        sync.atomic(self.project / "Project.heproject", b"{}")
        self.source = self.project / "source.gltf"
        self.doc = document()
        sync.atomic(self.source, sync.encode(self.doc))
        self.importer = self.project / "importer.exe"
        sync.atomic(self.importer, b"mock identity")
        self.args = SimpleNamespace(action="sync", project=str(self.project), source=str(self.source), importer=str(self.importer),
            initialize_ids=False, mode="baked-model", default_clip_id=self.doc["animations"][0]["extras"]["he_id"], timeout=5, verify="none", fault="")
        self.calls = []

    def tearDown(self):
        self.temp.cleanup()

    def process(self, command, log, timeout):
        self.calls.append(command)
        if command[-1] == "--validate-scene":
            return 0
        native_outputs(self.project, Path(command[3]), sync.read_json(command[2]))
        return 0

    def execute(self):
        report = self.project / "report"
        report.mkdir(exist_ok=True)
        with patch.object(sync, "run_process", self.process):
            return sync.synchronize(self.args, report)

    def record(self):
        return self.project / ".he-content" / self.doc["scenes"][0]["extras"]["he_id"] / "state.json"

    def test_native_dependency_dag_and_noop(self):
        result = self.execute()
        state = sync.read_json(self.record())
        before = self.record().read_bytes()
        self.assertEqual(len(state["skins"]), 1)
        self.assertEqual(len(state["clips"]), 2)
        for path in state["artifacts"]:
            if path.endswith((".hmesh", ".hanim")):
                self.assertFalse(any(".baked-staging" in value for value in sync.strings(sync.read_json(self.project / path))))
        self.assertFalse(self.execute()["changed"])
        self.assertEqual(before, self.record().read_bytes())
        self.assertNotIn("--scene", self.calls[0])
        self.assertFalse(list((self.project / "Assets/Imported/.baked-staging").iterdir()))

    def test_animation_update_preserves_engine_fields(self):
        result = self.execute()
        scene = self.project / result["scene"]
        current = sync.read_json(scene)
        key = next(iter(current))
        current[key]["PythonScriptComponent"] = dict(Path="Scripts/actor.py")
        current[key]["AnimatorComponent"]["playSpeed"] = 0.65
        current[key]["MeshComponent"]["MaterialOverrides"] = [current[key]["MeshComponent"]["Path"]]
        old_path = current[key]["AnimatorComponent"]["animResPath"]
        sync.atomic(scene, sync.encode(current))
        self.doc["animations"][0]["duration"] = 2
        sync.atomic(self.source, sync.encode(self.doc))
        self.execute()
        actual = sync.read_json(scene)[key]
        self.assertNotEqual(old_path, actual["AnimatorComponent"]["animResPath"])
        self.assertEqual(actual["PythonScriptComponent"], current[key]["PythonScriptComponent"])
        self.assertEqual(actual["MeshComponent"], current[key]["MeshComponent"])
        self.assertEqual(actual["AnimatorComponent"]["playSpeed"], 0.65)

    def test_default_deletion_fails_without_publication(self):
        result = self.execute()
        scene = self.project / result["scene"]
        before = (scene.read_bytes(), self.record().read_bytes())
        self.doc["animations"].pop(0)
        sync.atomic(self.source, sync.encode(self.doc))
        with self.assertRaisesRegex(sync.Failure, "default clip ID"):
            self.execute()
        self.assertEqual(before, (scene.read_bytes(), self.record().read_bytes()))

    def test_unused_delete_and_external_reference_rejection(self):
        self.execute()
        state = sync.read_json(self.record())
        deleted = self.doc["animations"].pop()
        path = state["clips"][deleted["extras"]["he_id"]]["path"]
        external = self.project / "Assets/external.hanimctrl"
        sync.atomic(external, sync.encode(dict(nodes=[dict(clipPath=path)])))
        sync.atomic(self.source, sync.encode(self.doc))
        before = self.record().read_bytes()
        with self.assertRaisesRegex(sync.Failure, "referenced by"):
            self.execute()
        self.assertEqual(before, self.record().read_bytes())
        external.unlink()
        self.execute()
        self.assertEqual(len(sync.read_json(self.record())["clips"]), 1)
        self.assertTrue((self.project / path).is_file())

    def test_fault_recovery(self):
        result = self.execute()
        scene = self.project / result["scene"]
        for fault in ("before-scene", "after-scene"):
            old_scene = scene.read_bytes()
            self.doc["animations"][0]["duration"] = 3 if fault == "before-scene" else 4
            sync.atomic(self.source, sync.encode(self.doc))
            self.args.fault = fault
            with self.assertRaises(sync.Failure):
                self.execute()
            if fault == "before-scene":
                self.assertEqual(scene.read_bytes(), old_scene)
            self.args.fault = ""
            self.execute()
            self.assertFalse((self.record().parent / "pending.json").exists())
            self.assertEqual(sync.file_hash(scene), sync.read_json(self.record())["sceneSha256"])

    def test_real_prefab_scene_controller_references_block_clip_deletion(self):
        self.execute()
        state = sync.read_json(self.record())
        removed = self.doc["animations"].pop()
        clip_path = state["clips"][removed["extras"]["he_id"]]["path"]
        component = dict(AnimatorComponent=dict(animResPath=clip_path.replace("/", "\\")))
        cases = [("legacy.hprefab", component),
            ("nested.hprefab", dict(schemaVersion=2, root="root", entities={"root": {}}, nested=[dict(id="child", path="Assets/base.hprefab", overrides={"visual": component})])),
            ("direct.hprefab", dict(schemaVersion=2, root="root", entities={"root": component}, nested=[])),
            ("external.he", {"uuid": component}),
            ("external.hanimctrl", dict(nodes=[dict(clipPath=clip_path)]))]
        sync.atomic(self.source, sync.encode(self.doc))
        before = self.record().read_bytes()
        for name, value in cases:
            with self.subTest(format=name):
                path = self.project / "Assets" / name
                sync.atomic(path, sync.encode(value))
                with self.assertRaisesRegex(sync.Failure, "referenced by"):
                    self.execute()
                self.assertEqual(before, self.record().read_bytes())
                path.unlink()
        # Labels, arbitrary script strings and old refresh baselines are not
        # active resource bindings and cannot veto an unused clip deletion.
        noise = dict(TagComponent=dict(Tag=clip_path), PythonScriptComponent=dict(Comment=clip_path),
            PrefabInstanceComponent=dict(BaselineJson=sync.encode(component).decode()))
        sync.atomic(self.project / "Assets/noise.he", sync.encode({"uuid": noise}))
        sync.atomic(self.project / "Assets/noise.hprefab", sync.encode(dict(schemaVersion=2, root="root", entities={"root": noise}, nested=[])))
        self.execute()
        self.assertNotIn(removed["extras"]["he_id"], sync.read_json(self.record())["clips"])

    def test_controller_skeleton_reference_is_typed(self):
        self.assertEqual(set(adapter.authored_resource_references(dict(targetSkeletonPath="Assets/rig.hskel", name="Assets/not-a-reference.hskel"), ".hanimctrl")), {"Assets/rig.hskel"})

    def test_native_failure_does_not_publish(self):
        self.execute()
        before = self.record().read_bytes()
        self.process = lambda *args: 1
        with self.assertRaisesRegex(sync.Failure, "Native conversion failed"):
            self.execute()
        self.assertEqual(before, self.record().read_bytes())

    def test_real_fixture_timestamp_copy_on_write(self):
        source = Path(__file__).resolve().parents[2] / "Project/SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
        doc = sync.read_json(source)
        for item in doc["animations"]:
            item["extras"] = dict(he_id=item["name"])
        changed = stretch_clip(doc, "Walk", 1.5)
        for before, after in zip(doc["animations"], changed["animations"]):
            if before["name"] != "Walk":
                self.assertEqual(before, after)
        self.assertEqual(doc["skins"], changed["skins"])
        self.assertGreater(changed["buffers"][0]["byteLength"], doc["buffers"][0]["byteLength"])

    def test_acceptance_driver_all_stages_with_simulated_native_output(self):
        relative = "SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
        upstream = Path(__file__).resolve().parents[2] / "Project" / relative
        sync.atomic(self.project / relative, upstream.read_bytes())
        def process(command, log, timeout):
            if command[-1] == "--validate-scene":
                return 0
            doc = sync.read_json(command[2])
            if doc.get("buffers", [{}])[0].get("uri", "").endswith(",AA=="):
                return 1
            native_outputs(self.project, Path(command[3]), doc)
            return 0
        with patch.object(sync, "run_process", process):
            report = run_acceptance(SimpleNamespace(project=str(self.project), importer=str(self.importer), importer_sha256=sync.file_hash(self.importer), report=str(self.project / "acceptance"), timeout=5))
        self.assertEqual(report["status"], "PASS")
        self.assertEqual(len(report["checks"]), 13)
        self.assertTrue(report["originalUnchanged"])
        self.assertEqual(report["runtimePlayback"], "NOT_VERIFIED")
        self.assertTrue(report["importerIdentityUnchanged"])
        self.assertEqual(report["contentSyncSha256"], sync.file_hash(sync.__file__))

    def test_bind_edit_preserves_bind_pose_equation_and_other_channels(self):
        source = Path(__file__).resolve().parents[2] / "Project/SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
        before = sync.read_json(source)
        after = move_mouth_bind(before)
        index = next(i for i, node in enumerate(before["nodes"]) if node["name"] == "Mouth")
        joint = before["skins"][0]["joints"].index(index)
        def values(doc, accessor_index, width):
            accessor = doc["accessors"][accessor_index]
            view = doc["bufferViews"][accessor["bufferView"]]
            payload = base64.b64decode(doc["buffers"][0]["uri"].split(",", 1)[1])
            offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            return [struct.unpack_from("<" + "f" * width, payload, offset + i * view.get("byteStride", width * 4)) for i in range(accessor["count"])]
        old_matrices = values(before, before["skins"][0]["inverseBindMatrices"], 16)
        new_matrices = values(after, after["skins"][0]["inverseBindMatrices"], 16)
        for i in range(len(old_matrices)):
            if i != joint:
                self.assertEqual(old_matrices[i], new_matrices[i])
        # Parent cancels: L_old B_old == L_new B_new at the rest pose.
        for column in range(4):
            for row in range(3):
                old = old_matrices[joint][column * 4 + row] + before["nodes"][index]["translation"][row] * old_matrices[joint][column * 4 + 3]
                new = new_matrices[joint][column * 4 + row] + after["nodes"][index]["translation"][row] * new_matrices[joint][column * 4 + 3]
                self.assertAlmostEqual(old, new, places=6)
        self.assertEqual(before["meshes"], after["meshes"])
        for old_animation, new_animation in zip(before["animations"], after["animations"]):
            for old_channel, new_channel in zip(old_animation["channels"], new_animation["channels"]):
                old_sampler = old_animation["samplers"][old_channel["sampler"]]
                new_sampler = new_animation["samplers"][new_channel["sampler"]]
                if old_channel["target"] == dict(node=index, path="translation"):
                    self.assertEqual(old_sampler["input"], new_sampler["input"])
                    for old, new in zip(values(before, old_sampler["output"], 3), values(after, new_sampler["output"], 3)):
                        self.assertAlmostEqual(new[0] - old[0], 0.05, places=6)
                        self.assertEqual(old[1:], new[1:])
                else:
                    self.assertEqual(old_sampler, new_sampler)

    def test_acceptance_requires_pin_and_new_report(self):
        folder = self.project / "existing-report"
        folder.mkdir()
        marker = folder / "evidence.txt"
        marker.write_text("preserve")
        args = SimpleNamespace(project=str(self.project), importer=str(self.importer), importer_sha256="0" * 64, report=str(folder), timeout=5)
        with self.assertRaisesRegex(ValueError, "identity"):
            run_acceptance(args)
        args.importer_sha256 = sync.file_hash(self.importer)
        with self.assertRaises(FileExistsError):
            run_acceptance(args)
        self.assertEqual(marker.read_text(), "preserve")

    def test_acceptance_records_importer_change_as_failure(self):
        relative = "SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
        sync.atomic(self.project / relative, (Path(__file__).resolve().parents[2] / "Project" / relative).read_bytes())
        folder = self.project / "changed-importer-report"
        args = SimpleNamespace(project=str(self.project), importer=str(self.importer), importer_sha256=sync.file_hash(self.importer), report=str(folder), timeout=5)
        def changed_process(command, log, timeout):
            if command[-1] != "--validate-scene":
                native_outputs(self.project, Path(command[3]), sync.read_json(command[2]))
                self.importer.write_bytes(b"different executable")
            return 0
        with patch.object(sync, "run_process", changed_process):
            with self.assertRaisesRegex(AssertionError, "Importer changed"):
                run_acceptance(args)
        report = sync.read_json(folder / "report.json")
        self.assertEqual(report["status"], "FAIL")
        self.assertFalse(report["importerIdentityUnchanged"])


if __name__ == "__main__":
    unittest.main()
