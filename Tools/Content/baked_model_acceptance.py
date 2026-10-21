"""Actual CPU/native skeletal reimport evidence; never starts a renderer.

Run with a built HAssetImporter. A fresh task-owned Cactus source copy is used.
Scene edits below author the engine-owned script/overrides BEFORE reimport;
only content_sync publishes the changed native mesh/skeleton/animation refs.
Runtime playback, external controllers and prefab refresh remain NOT_VERIFIED.
"""
import argparse
import base64
import copy
import json
from pathlib import Path
import struct
from types import SimpleNamespace
import uuid

import content_sync as sync


def stretch_clip(document, clip_id, factor):
    """Copy-on-write FLOAT timestamp accessors in our self-contained fixture."""
    result = copy.deepcopy(document)
    clip = next(a for a in result["animations"] if a["extras"]["he_id"] == clip_id)
    if len(result["buffers"]) != 1 or not result["buffers"][0]["uri"].startswith("data:"):
        raise ValueError("Fixture requires one embedded buffer")
    header, encoded = result["buffers"][0]["uri"].split(",", 1)
    payload = bytearray(base64.b64decode(encoded, validate=True))
    remap = {}
    for sampler in clip["samplers"]:
        index = sampler["input"]
        if index not in remap:
            accessor = result["accessors"][index]
            if accessor["componentType"] != 5126 or accessor["type"] != "SCALAR" or "sparse" in accessor:
                raise ValueError("Unexpected fixture timestamp accessor")
            view = result["bufferViews"][accessor["bufferView"]]
            start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
            stride = view.get("byteStride", 4)
            values = [struct.unpack_from("<f", payload, start + i * stride)[0] * factor for i in range(accessor["count"])]
            payload.extend(b"\0" * (-len(payload) % 4))
            new_view = dict(buffer=0, byteOffset=len(payload), byteLength=4 * len(values))
            payload.extend(struct.pack("<" + "f" * len(values), *values))
            result["bufferViews"].append(new_view)
            result["accessors"].append(dict(bufferView=len(result["bufferViews"]) - 1, componentType=5126,
                count=len(values), type="SCALAR", min=[min(values)], max=[max(values)]))
            remap[index] = len(result["accessors"]) - 1
        sampler["input"] = remap[index]
    result["buffers"][0].update(byteLength=len(payload), uri=header + "," + base64.b64encode(payload).decode("ascii"))
    return result


def move_mouth_bind(document, delta=(0.05, 0.0, 0.0)):
    """Author a consistent leaf-joint bind edit in the known Cactus fixture.

    Mouth has local matrix L=T(t). Changing L to T(t+d) requires
    inverseBind' = inverse(L') L inverseBind = T(-d) inverseBind.
    Thus parentWorld L' inverseBind' equals parentWorld L inverseBind.
    Absolute translation animation keys also move by d; times, rotations,
    scales, vertex positions and weights remain intact. Standard library only.
    """
    result = copy.deepcopy(document)
    matches = [i for i, node in enumerate(result["nodes"]) if node.get("name") == "Mouth"]
    if len(matches) != 1 or len(result["skins"]) != 1:
        raise ValueError("Expected the unique Cactus Mouth joint")
    index = matches[0]
    node = result["nodes"][index]
    if node.get("children") or any(key in node for key in ("matrix", "rotation", "scale")):
        raise ValueError("Mouth fixture must be a translation-only leaf")
    skin = result["skins"][0]
    joint_index = skin["joints"].index(index)
    header, encoded = result["buffers"][0]["uri"].split(",", 1)
    if len(result["buffers"]) != 1 or not header.startswith("data:"):
        raise ValueError("Expected a self-contained fixture")
    payload = bytearray(base64.b64decode(encoded, validate=True))
    def read(index, kind, width):
        accessor = result["accessors"][index]
        view = result["bufferViews"][accessor["bufferView"]]
        if accessor["componentType"] != 5126 or accessor["type"] != kind or "sparse" in accessor or view.get("buffer", 0) != 0:
            raise ValueError("Unexpected fixture accessor")
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        return [list(struct.unpack_from("<" + "f" * width, payload, start + i * view.get("byteStride", width * 4))) for i in range(accessor["count"])]
    def append(values, kind, width):
        payload.extend(b"\0" * (-len(payload) % 4))
        result["bufferViews"].append(dict(buffer=0, byteOffset=len(payload), byteLength=len(values) * width * 4))
        for row in values:
            payload.extend(struct.pack("<" + "f" * width, *row))
        result["accessors"].append(dict(bufferView=len(result["bufferViews"]) - 1, componentType=5126, count=len(values), type=kind))
        return len(result["accessors"]) - 1
    matrices = read(skin["inverseBindMatrices"], "MAT4", 16)
    matrix = matrices[joint_index]
    # glTF stores column-major matrices. Left multiplication by T(-d).
    for column in range(4):
        for row in range(3):
            matrix[column * 4 + row] -= delta[row] * matrix[column * 4 + 3]
    skin["inverseBindMatrices"] = append(matrices, "MAT4", 16)
    node["translation"] = [value + shift for value, shift in zip(node.get("translation", [0, 0, 0]), delta)]
    changed_channels = 0
    for animation in result["animations"]:
        for channel in animation["channels"]:
            if channel["target"] != dict(node=index, path="translation"):
                continue
            sampler = copy.deepcopy(animation["samplers"][channel["sampler"]])
            if sampler.get("interpolation", "LINEAR") not in ("LINEAR", "STEP"):
                raise ValueError("This fixture edit does not rewrite cubic spline tangents")
            values = read(sampler["output"], "VEC3", 3)
            sampler["output"] = append([[v + d for v, d in zip(row, delta)] for row in values], "VEC3", 3)
            animation["samplers"].append(sampler)
            channel["sampler"] = len(animation["samplers"]) - 1
            changed_channels += 1
    if changed_channels != len(result["animations"]):
        raise ValueError("Each fixture animation must provide exactly one Mouth translation channel")
    result["buffers"][0].update(byteLength=len(payload), uri=header + "," + base64.b64encode(payload).decode("ascii"))
    return result


def run(args):
    project = Path(args.project).resolve()
    importer = Path(args.importer).resolve()
    expected_importer = args.importer_sha256.lower()
    if len(expected_importer) != 64 or any(c not in "0123456789abcdef" for c in expected_importer):
        raise ValueError("--importer-sha256 must be a SHA-256 hex digest")
    if sync.file_hash(importer) != expected_importer:
        raise ValueError("Importer SHA-256 does not match the requested executable identity")
    report_dir = Path(args.report).resolve()
    report_dir.mkdir(parents=True, exist_ok=False)
    original = project / "SourceAssets/Quaternius/CuteAnimatedMonsters/Derived/Cactus/Cactus.gltf"
    original_hash = sync.file_hash(original)
    run_id = uuid.uuid4().hex
    root = project / "SourceAssets/VFXAuthoring/SkeletalReimport" / run_id
    root.mkdir(parents=True)
    source = root / "Cactus.gltf"
    sync.atomic(source, original.read_bytes())
    # The copy receives independent IDs even if a future upstream file has IDs.
    doc, _ = sync.gltf_document(source)
    for kind in ("scenes", "nodes", "meshes", "materials", "skins", "animations"):
        for item in doc.get(kind, []):
            item.setdefault("extras", {}).pop("he_id", None)
    sync.atomic(source, sync.encode(doc))
    sync.initialize_gltf(source)
    baseline, _ = sync.gltf_document(source)
    clip_id = next(a["extras"]["he_id"] for a in baseline["animations"] if a["name"] == "Walk")
    document_id = baseline["scenes"][baseline.get("scene", 0)]["extras"]["he_id"]
    state_path = project / ".he-content" / document_id / "state.json"
    scene = project / "Assets/Imported" / document_id / "Scene.he"
    common = dict(action="sync", project=str(project), source=str(source), importer=str(Path(args.importer).resolve()),
        blender="", mode="baked-model", default_clip_id=clip_id, initialize_ids=False, verify="none", timeout=args.timeout, fault="")
    checks = []
    def step(name, expected=None, fault=""):
        if sync.file_hash(importer) != expected_importer:
            raise AssertionError("Importer changed before stage " + name)
        folder = report_dir / name
        folder.mkdir()
        options = SimpleNamespace(**{**common, "fault": fault})
        try:
            result = sync.synchronize(options, folder)
            if expected:
                raise AssertionError("Expected failure " + expected)
        except sync.Failure as error:
            if error.diagnostic["code"] != expected:
                raise
            result = dict(ok=False, status=expected, diagnostic=error.diagnostic)
        sync.atomic(folder / "result.json", sync.encode(result))
        if sync.file_hash(importer) != expected_importer:
            raise AssertionError("Importer changed during stage " + name)
        entry = dict(stage=name, status="PASS", observed=result.get("status"))
        if result.get("ok"):
            scene_copy, state_copy = folder / "published-scene.json", folder / "published-state.json"
            # Each stage directory is newly created; these snapshots are the
            # exact committed bytes, before a later stage can replace them.
            scene_copy.write_bytes(scene.read_bytes())
            state_copy.write_bytes(state_path.read_bytes())
            entry["snapshot"] = dict(scene=str(scene_copy), sceneSha256=sync.file_hash(scene_copy),
                registry=str(state_copy), registrySha256=sync.file_hash(state_copy))
        checks.append(entry)
        return result
    def snapshot():
        return dict(scene=sync.file_hash(scene), state=sync.file_hash(state_path))
    report = dict(status="FAIL", evidenceType="native-import", importer=str(Path(args.importer).resolve()),
        importerSha256=expected_importer, original=str(original), originalSha256=original_hash,
        source=str(source), defaultClipId=clip_id, checks=checks,
        driverSha256=sync.file_hash(__file__), adapterSha256=sync.file_hash(Path(__file__).with_name("baked_model_adapter.py")),
        contentSyncSha256=sync.file_hash(Path(__file__).with_name("content_sync.py")),
        runtimePlayback="NOT_VERIFIED", visualStatus="NOT_REVIEWED", prefabRefresh="NOT_VERIFIED", skeletonBindEdit="NOT_VERIFIED",
        externalControllerMigration="NOT_SUPPORTED", scene=str(scene))
    try:
        step("01-initial")
        before = snapshot()
        old_record = sync.read_json(state_path)
        resources = {name: ((project / name).stat().st_mtime_ns, sync.file_hash(project / name)) for name in old_record["artifacts"]}
        step("02-noop")
        assert before == snapshot(), "No-op changed scene or registry"
        assert resources == {name: ((project / name).stat().st_mtime_ns, sync.file_hash(project / name)) for name in resources}, "No-op rewrote resources"
        current = sync.read_json(scene)
        entity_id = next(iter(current))
        entity = current[entity_id]
        mesh = sync.read_json(project / entity["MeshComponent"]["Path"])
        material = mesh["primitives"][0]["materialResPath"]
        entity["PythonScriptComponent"] = {"Path": "Scripts/VFXCharacterAcceptance.py"}
        entity["AnimatorComponent"]["playSpeed"] = 0.65
        entity["AnimatorComponent"]["bLoop"] = False
        entity["MeshComponent"]["MaterialOverrides"] = [material]
        sync.atomic(scene, sync.encode(current))
        authored = copy.deepcopy(entity)
        old_clip = sync.read_json(project / old_record["clips"][clip_id]["path"])
        sync.atomic(source, sync.encode(stretch_clip(baseline, clip_id, 1.5)))
        step("03-source-animation-change")
        updated = sync.read_json(state_path)
        actual = sync.read_json(scene)[entity_id]
        assert updated["identities"] == old_record["identities"]
        assert updated["skins"] == old_record["skins"], "Time-only edit changed skeleton"
        assert actual["MeshComponent"] == authored["MeshComponent"], "Time-only edit changed mesh or override"
        assert actual["PythonScriptComponent"] == authored["PythonScriptComponent"]
        assert actual["AnimatorComponent"]["playSpeed"] == 0.65 and actual["AnimatorComponent"]["bLoop"] is False
        assert actual["AnimatorComponent"]["animResPath"] == updated["clips"][clip_id]["path"] != old_record["clips"][clip_id]["path"]
        new_clip = sync.read_json(project / updated["clips"][clip_id]["path"])
        assert new_clip["boneInfos"] == old_clip["boneInfos"]
        old_duration = old_clip["clips"][0]["duration"]
        new_duration = new_clip["clips"][0]["duration"]
        assert abs(new_duration / old_duration - 1.5) < 1e-5, "Native clip duration did not follow source timestamps"
        report["duration"] = dict(before=old_duration, after=new_duration)
        good_source = source.read_bytes()
        before = snapshot()
        deleted = sync.read_json(source)
        deleted["animations"] = [a for a in deleted["animations"] if a["extras"]["he_id"] != clip_id]
        sync.atomic(source, sync.encode(deleted))
        step("04-delete-default-rejected", "default_clip_missing")
        assert snapshot() == before
        sync.atomic(source, good_source)
        broken = sync.read_json(source)
        broken["buffers"][0]["uri"] = "data:application/octet-stream;base64,AA=="
        sync.atomic(source, sync.encode(broken))
        step("05-native-failure-retains-old", "conversion_failed")
        assert snapshot() == before
        sync.atomic(source, good_source)
        unused = next(key for key in updated["clips"] if key != clip_id)
        blocker = scene.parent / "AcceptanceExternalController.hanimctrl"
        sync.atomic(blocker, sync.encode(dict(version=1, name="DeletionReferenceProbe", targetSkeletonPath=next(iter(updated["skins"].values()))["path"],
            nodes=[dict(id=0, type="State", name="Probe", clipPath=updated["clips"][unused]["path"])], parameters=[], transitions=[], entryNodeId=0)))
        deleted = sync.read_json(source)
        deleted["animations"] = [a for a in deleted["animations"] if a["extras"]["he_id"] != unused]
        sync.atomic(source, sync.encode(deleted))
        try:
            step("05a-delete-referenced-rejected", "delete_resource_referenced")
            assert snapshot() == before
        finally:
            blocker.unlink(missing_ok=True)
        step("05b-delete-unused")
        assert unused not in sync.read_json(state_path)["clips"]
        assert (project / updated["clips"][unused]["path"]).is_file(), "Old immutable resource was removed"
        sync.atomic(source, good_source)
        step("05c-restore-source-clip")
        before = snapshot()
        sync.atomic(source, sync.encode(stretch_clip(baseline, clip_id, 1.75)))
        step("06-before-publication", "injected_failure", "before-scene")
        assert snapshot() == before
        step("07-recover-before")
        sync.atomic(source, sync.encode(stretch_clip(baseline, clip_id, 2.0)))
        step("08-after-publication", "injected_failure", "after-scene")
        step("09-recover-after")
        assert not (state_path.parent / "pending.json").exists()
        pre_bind_record = sync.read_json(state_path)
        pre_bind_entity = sync.read_json(scene)[entity_id]
        pre_bind_clip = sync.read_json(project / pre_bind_record["clips"][clip_id]["path"])
        pre_bind_mesh = sync.read_json(project / pre_bind_entity["MeshComponent"]["Path"])
        sync.atomic(source, sync.encode(move_mouth_bind(sync.read_json(source))))
        step("10-consistent-mouth-bind-edit")
        bind_record = sync.read_json(state_path)
        bind_entity = sync.read_json(scene)[entity_id]
        assert bind_record["identities"] == pre_bind_record["identities"]
        assert set(bind_record["skins"]) == set(pre_bind_record["skins"])
        skin_id = next(iter(bind_record["skins"]))
        old_skin, new_skin = pre_bind_record["skins"][skin_id], bind_record["skins"][skin_id]
        assert old_skin["joints"] == new_skin["joints"] and old_skin["path"] != new_skin["path"]
        old_bones = {bone["name"]: bone for bone in sync.read_json(project / old_skin["path"])["bones"]}
        new_bones = {bone["name"]: bone for bone in sync.read_json(project / new_skin["path"])["bones"]}
        assert set(old_bones) == set(new_bones)
        assert abs(new_bones["Mouth"]["localBindT"][0] - old_bones["Mouth"]["localBindT"][0] - 0.05) < 1e-5
        for name in old_bones:
            if name != "Mouth":
                assert old_bones[name] == new_bones[name], "Unrelated native rest bone changed: " + name
        bind_mesh = sync.read_json(project / bind_entity["MeshComponent"]["Path"])
        assert bind_entity["MeshComponent"]["Path"] != pre_bind_entity["MeshComponent"]["Path"]
        assert bind_mesh["skeletonPath"] == new_skin["path"]
        assert bind_mesh["weights"] == pre_bind_mesh["weights"]
        for key, value in bind_record["clips"].items():
            animation = sync.read_json(project / value["path"])
            assert animation["meshResPath"] == bind_entity["MeshComponent"]["Path"]
            assert animation["targetSkeletonPath"] == new_skin["path"]
            assert value["path"] != pre_bind_record["clips"][key]["path"]
        bind_clip = sync.read_json(project / bind_record["clips"][clip_id]["path"])
        assert bind_entity["AnimatorComponent"]["animResPath"] == bind_record["clips"][clip_id]["path"]
        assert bind_clip["clips"][0]["duration"] == pre_bind_clip["clips"][0]["duration"]
        assert bind_clip["boneInfos"] != pre_bind_clip["boneInfos"], "Bind offsets did not change"
        assert bind_entity["PythonScriptComponent"] == authored["PythonScriptComponent"]
        assert bind_entity["MeshComponent"]["MaterialOverrides"] == authored["MeshComponent"]["MaterialOverrides"]
        assert bind_entity["AnimatorComponent"]["playSpeed"] == 0.65 and bind_entity["AnimatorComponent"]["bLoop"] is False
        report["skeletonBindEdit"] = dict(status="PASS", joint="Mouth", localDelta=[0.05, 0, 0],
            inverseBindRule="B_new = T(-delta) B_old", translationKeys="same delta; times/rotation/scale unchanged",
            skinId=skin_id, before=old_skin["path"], after=new_skin["path"], runtimeDeformation="NOT_VERIFIED")
        assert sync.file_hash(original) == original_hash
        report.update(status="PASS", finalStateSha256=sync.file_hash(state_path), finalSceneSha256=sync.file_hash(scene),
            sourceSha256=sync.file_hash(source), record=str(state_path))
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        report["originalUnchanged"] = sync.file_hash(original) == original_hash
        report["finalImporterSha256"] = sync.file_hash(importer) if importer.is_file() else None
        report["importerIdentityUnchanged"] = report["finalImporterSha256"] == expected_importer
        if not report["importerIdentityUnchanged"]:
            report.update(status="FAIL", error="Importer identity changed during acceptance")
        sync.atomic(report_dir / "report.json", sync.encode(report))
    if report["status"] != "PASS":
        raise AssertionError(report.get("error", "Acceptance failed"))
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", default="Project")
    parser.add_argument("--importer", required=True)
    parser.add_argument("--importer-sha256", required=True)
    parser.add_argument("--report", required=True)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    import importlib.util
    repository = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location("baked_storage", repository / "Tools/Agent/storage.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    folder = Path(args.report).resolve()
    if folder.exists():
        raise FileExistsError("Report directory must be new: " + str(folder))
    with module.Storage(repository).workflow_scope(str(Path(args.project).resolve()), "skeletal-reimport", [folder]) as scope:
        try:
            report = run(args)
            scope["status"] = "success"
        except Exception:
            scope["status"] = "failed"
            raise
    print(json.dumps({key: report[key] for key in ("status", "scene", "source", "runtimePlayback")}))


if __name__ == "__main__":
    main()
