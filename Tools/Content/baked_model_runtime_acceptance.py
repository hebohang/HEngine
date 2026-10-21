"""Compare verified initial/final native snapshots in isolated Game display scenes.

No build, importer or source modification. GPU execution is explicit via this
driver. Visual approval and prefab propagation are separate from command PASS.
"""
import argparse
import copy
import json
import math
import re
from pathlib import Path
import shutil
import sys
import time
import uuid

import content_sync as sync

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "Project"


def checked(path, expected):
    path = Path(path).resolve()
    if sync.file_hash(path).lower() != expected.lower():
        raise ValueError("Evidence identity mismatch: " + str(path))
    return sync.read_json(path)


def validate_inputs(report_path, report_hash, project=PROJECT):
    report_path = Path(report_path).resolve()
    report = checked(report_path, report_hash)
    if report.get("status") != "PASS" or report.get("originalUnchanged") is not True or report.get("importerIdentityUnchanged") is not True:
        raise ValueError("A successful pinned native report is required")
    if not isinstance(report.get("skeletonBindEdit"), dict) or report["skeletonBindEdit"].get("status") != "PASS":
        raise ValueError("Native bind edit was not verified")
    checked(report["scene"], report["finalSceneSha256"])
    checked(report["record"], report["finalStateSha256"])
    if sync.file_hash(report["source"]) != report["sourceSha256"]:
        raise ValueError("Final source changed after native acceptance")
    cases = {}
    for label, stage in (("initial", "01-initial"), ("final", "10-consistent-mouth-bind-edit")):
        entries = [item for item in report["checks"] if item["stage"] == stage and item["status"] == "PASS"]
        if len(entries) != 1 or "snapshot" not in entries[0]:
            raise ValueError("Exact committed snapshot missing for " + stage + "; run fresh native acceptance")
        snapshot = entries[0]["snapshot"]
        for key in ("scene", "registry"):
            if not Path(snapshot[key]).resolve().is_relative_to(report_path.parent):
                raise ValueError("Snapshot is outside its native evidence directory")
        scene = checked(snapshot["scene"], snapshot["sceneSha256"])
        registry = checked(snapshot["registry"], snapshot["registrySha256"])
        if snapshot["sceneSha256"] != registry["sceneSha256"] or registry["defaultClipId"] != report["defaultClipId"]:
            raise ValueError("Snapshot scene/registry/default clip identity mismatch")
        for path, expected in registry["artifacts"].items():
            if sync.file_hash(sync.confined(project, path)) != expected:
                raise ValueError("Native resource was changed: " + path)
        clip_path = registry["clips"][report["defaultClipId"]]["path"]
        clip = sync.read_json(sync.confined(project, clip_path))
        if len(scene) != 1 or len(clip["clips"]) != 1 or "Walk" not in clip["clips"][0]["name"]:
            raise ValueError("Expected the single imported model/Walk fixture")
        entity = next(iter(scene.values()))
        if entity["AnimatorComponent"]["animResPath"] != clip_path or clip["meshResPath"] != entity["MeshComponent"]["Path"]:
            raise ValueError("Snapshot mesh/animation references differ")
        mesh = sync.read_json(sync.confined(project, clip["meshResPath"]))
        if clip["targetSkeletonPath"] != mesh["skeletonPath"] or clip["targetSkeletonPath"] not in {item["path"] for item in registry["skins"].values()}:
            raise ValueError("Snapshot skeleton dependency mismatch")
        cases[label] = dict(scene=scene, registry=registry, snapshot=snapshot, clipPath=clip_path,
            durationSeconds=clip["clips"][0]["duration"] / clip["clips"][0]["ticksPerSecond"])
        if not math.isfinite(cases[label]["durationSeconds"]) or cases[label]["durationSeconds"] <= 0:
            raise ValueError("Invalid native clip duration")
        if label == "final" and (snapshot["sceneSha256"] != report["finalSceneSha256"] or snapshot["registrySha256"] != report["finalStateSha256"]):
            raise ValueError("Final snapshot is not the native report's final published state")
    if cases["initial"]["registry"]["identities"] != cases["final"]["registry"]["identities"]:
        raise ValueError("Imported model identity changed")
    return report, cases


def differences(before, after, path=""):
    if isinstance(before, dict) and isinstance(after, dict):
        result = []
        for key in sorted(set(before) | set(after)):
            child = path + "/" + key
            if key not in before or key not in after:
                result.append(dict(path=child, before=before.get(key), after=after.get(key)))
            else:
                result.extend(differences(before[key], after[key], child))
        return result
    return [] if before == after else [dict(path=path, before=before, after=after)]


def prepare_case(case, label, project=PROJECT, scale=0.01):
    if not math.isfinite(scale) or scale <= 0:
        raise ValueError("Display scale must be positive and finite")
    token = uuid.uuid4().hex + "-" + label
    folder = project / "Assets/Imported/BakedRuntime" / token
    folder.mkdir(parents=True, exist_ok=False)
    scene = copy.deepcopy(case["scene"])
    entity_id = next(iter(scene))
    entity = scene[entity_id]
    entity["TagComponent"]["Tag"] = token
    entity["TransformComponent"]["Scale"] = [scale] * 3
    entity["TransformComponent"]["Rotation"] = [0.0, 3.141592653589793, 0.0]
    entity["PythonScriptComponent"] = dict(Path="Scripts/BakedModelRuntimeProbe.py")
    # Display-test controls, explicitly recorded; original native scene untouched.
    entity["AnimatorComponent"].update(controllerPath="None", playSpeed=1.0, bLoop=False, bPlay=True)
    identity = dict(ParentUuid="", Translation=[0.0, 0.0, 0.0], Rotation=[0.0, 0.0, 0.0], Scale=[1.0, 1.0, 1.0])
    camera_transform = {**identity, "Translation": [0.0, 3.0, -8.0], "Rotation": [2.98, 0.0, 3.141592653589793]}
    scene[str(uuid.uuid4())] = dict(TagComponent=dict(Tag="Baked display camera"), TransformComponent=camera_transform,
        CameraComponent=dict(Camera=dict(mAspectRatio=1280/720, mProjectionType=0, mPerspectiveFOV=1.047198,
            mPerspectiveNear=0.1, mPerspectiveFar=400.0, mOrthographicSize=10.0, mOrthographicNear=-1.0, mOrthographicFar=1.0),
            Primary=True, FixedAspectRatio=False, bOverrideExposure=True,
            ExposureEV100=0.3219280948873623, ExposureCompensation=0.0))
    scene[str(uuid.uuid4())] = dict(TagComponent=dict(Tag="Baked display light"),
        TransformComponent={**identity, "Rotation": [1.919862177, -0.436332313, 0.0]},
        DirectionalLightComponent=dict(Intensity=4.0, Color=[1.0, 1.0, 1.0], bCastShadows=True))
    # Directional light rays follow local +Y; illuminate the camera-facing -Z side.
    scene[str(uuid.uuid4())] = dict(TagComponent=dict(Tag="Baked display fill"),
        TransformComponent={**identity, "Rotation": [1.745329252, 0.610865238, 0.0]},
        DirectionalLightComponent=dict(Intensity=1.0, Color=[1.0, 1.0, 1.0], bCastShadows=False))
    request = dict(nonce=token, clipPath=case["clipPath"], durationSeconds=case["durationSeconds"], entityId=entity_id)
    sync.atomic(folder / "request.json", sync.encode(request))
    sync.atomic(folder / "Scene.he", sync.encode(scene))
    return dict(scene=(folder / "Scene.he").relative_to(project).as_posix(), entityId=entity_id, nonce=token,
        sceneSha256=sync.file_hash(folder / "Scene.he"), requestSha256=sync.file_hash(folder / "request.json"),
        differences=differences(case["scene"], scene), resources=case["registry"]["artifacts"],
        snapshot=case["snapshot"], clipPath=case["clipPath"], meshPath=entity["MeshComponent"]["Path"])


def validate_observation(value, entity_id):
    if value.get("available") is not True or value.get("objectsTruncated"):
        raise ValueError("Rendered animation observation unavailable/truncated")
    # RenderWorld entityId is an internal ECS integer, not the persisted UUID.
    # The harness has exactly one mesh; world.get independently checks its UUID.
    objects = value.get("objects", [])
    if value.get("skinnedObjectCount") != 1 or len(objects) != 1:
        raise ValueError("Expected exactly one rendered skinned object for the verified single-model harness")
    item = objects[0]
    if item.get("currentPaletteCount", 0) <= 0 or not item.get("hashCoversFullPalettes") or item.get("nonFiniteComparisons") != 0:
        raise ValueError("Invalid or incomplete bone palette")
    weights = item["weights"]
    if not weights.get("complete") or weights.get("inspectedVertices", 0) <= 0 or any(weights.get(key) != 0 for key in ("invalidWeights", "positiveInfluencesOutsidePalette", "verticesWithoutValidWeight")):
        raise ValueError("Invalid/incomplete rendered skin weights")
    return item


def execute_case(harness, folder, backend, pin, timeout):
    sys.path.insert(0, str(ROOT / "Tools/VFX"))
    from preview import Session
    from character_acceptance import owned_run_directory
    session = Session(folder, backend, pin, harness["scene"])
    result = dict(status="FAIL", harness=harness, checkpoints=[], visualStatus="NOT_REVIEWED")
    try:
        session.start()
        directory = owned_run_directory(session, pin)
        session.call("runtime.pause")
        deadline = time.monotonic() + timeout
        sequence = 0
        while sequence < 3:
            ticket = session.call("runtime.step", dict(frames=4))
            while True:
                control = session.call("session.status")["world"]["runtimeControl"]
                if control["completedSteps"] >= ticket["targetCompletedSteps"] and not control["pendingSteps"] and not control["stepInProgress"]:
                    break
                if time.monotonic() > deadline:
                    raise TimeoutError("Runtime step did not settle")
                time.sleep(0.1)
            receipt = directory / "Saved/Reports/BakedModelRuntimeProbe.json"
            if receipt.is_file():
                if receipt.stat().st_size > 32768:
                    raise ValueError("Oversized runtime receipt")
                value = sync.read_json(receipt)
                if value.get("nonce") != harness["nonce"] or value.get("status") == "FAIL":
                    raise ValueError("Runtime probe failed or identity differs: " + str(value.get("error", "nonce")))
                if value.get("sequence") == sequence + 1:
                    if value["entityId"] != harness["entityId"] or value["clipPath"] != harness["clipPath"]:
                        raise ValueError("Runtime receipt imported identity mismatch")
                    observation = session.call("render.animation.status")
                    world_entity = session.call("world.get", dict(uuid=harness["entityId"], components=["MeshComponent", "AnimatorComponent"]))
                    if world_entity.get("found") is not True or world_entity.get("uuid") != harness["entityId"]:
                        raise ValueError("Imported UUID missing from the active runtime scene")
                    components = world_entity["components"]
                    if components["AnimatorComponent"]["animResPath"] != harness["clipPath"]:
                        raise ValueError("Actual UUID animation component differs from the verified snapshot")
                    if components["MeshComponent"]["Path"] != harness["meshPath"]:
                        raise ValueError("Actual UUID mesh component differs from the verified snapshot")
                    item = validate_observation(observation, harness["entityId"])
                    capture = session.call("capture.frame", dict(filename="baked-pose-" + str(sequence) + ".png"))
                    destination = folder / ("pose-" + str(sequence) + ".png")
                    shutil.copyfile(capture["path"], destination)
                    result["checkpoints"].append(dict(receipt=value, observation=observation, palette=item, worldEntity=world_entity,
                        objectAssociation="Exactly one skinned RenderWorld object and one mesh in the verified harness; render entityId is not the persisted UUID.",
                        capture=dict(receipt=capture, path=str(destination), sha256=sync.file_hash(destination)),
                        timing="Settled runtime-step barrier; script and capture frame numbers are recorded independently."))
                    sequence += 1
                    sync.atomic(directory / "Saved/Reports/BakedModelRuntimeProbeAck.json", sync.encode(dict(nonce=harness["nonce"], sequence=sequence)))
            if time.monotonic() > deadline:
                raise TimeoutError("Missing bounded runtime probe checkpoints")
        if len({item["palette"]["currentPaletteHash"] for item in result["checkpoints"]}) < 2:
            raise ValueError("Actual rendered Walk palette did not change")
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = str(error)
    finally:
        try:
            session.stop()
        except Exception as error:
            result.update(status="FAIL", cleanupError=str(error))
        native_error = re.compile(r"VUID-|Vulkan validation: Validation Error:|NVRHI validation error:|DX12 Debug Layer:|VK_ERROR_DEVICE_LOST|DXGI_ERROR_DEVICE_(REMOVED|HUNG|RESET)|NVRHI Vulkan: Device Removed|fatal error|Unhandled exception", re.I)
        errors, error_count = [], 0
        for name in ("game.stdout.log", "game.stderr.log"):
            path = folder / name
            if path.is_file():
                with path.open(encoding="utf-8", errors="replace") as stream:
                    for line in stream:
                        if native_error.search(line):
                            error_count += 1
                            if len(errors) < 50:
                                errors.append(line[:1200].rstrip())
        result["nativeErrors"] = dict(count=error_count, firstErrors=errors)
        if error_count:
            result.update(status="FAIL", error="Native validation/device error in retained streams")
        sync.atomic(folder / "report.json", sync.encode(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-report", required=True)
    parser.add_argument("--native-report-sha256", required=True)
    parser.add_argument("--backend", choices=["DX12", "Vulkan"], required=True)
    parser.add_argument("--game-sha256", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--model-scale", type=float, default=0.01)
    args = parser.parse_args()
    if not re.fullmatch("[0-9a-fA-F]{64}", args.game_sha256):
        raise ValueError("Explicit Game SHA-256 required")
    native, cases = validate_inputs(args.native_report, args.native_report_sha256)
    output = Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(ROOT / "Tools/VFX"))
    from preview import Storage
    with Storage(ROOT).workflow_scope(str(PROJECT), "skeletal-runtime", [output]) as scope:
        report = dict(status="FAIL", evidenceType="runtime-manual", backend=args.backend, gameSha256=args.game_sha256,
            nativeReport=str(Path(args.native_report).resolve()), nativeReportSha256=args.native_report_sha256,
            driverSha256=sync.file_hash(__file__), probeSha256=sync.file_hash(PROJECT / "Scripts/BakedModelRuntimeProbe.py"),
            visualStatus="NOT_REVIEWED", prefabPropagation="NOT_VERIFIED", scope="Independent display copies of exact native snapshots; not Prefab integration", cases={})
        try:
            for label, case in cases.items():
                folder = output / label
                folder.mkdir()
                harness = prepare_case(case, label, scale=args.model_scale)
                report["cases"][label] = execute_case(harness, folder, args.backend, args.game_sha256, args.timeout)
                if report["cases"][label]["status"] != "PASS":
                    raise ValueError(label + " runtime failed")
            # All original scene/registry/resource identities must still match.
            validate_inputs(args.native_report, args.native_report_sha256)
            report["status"] = "PASS"
            report["comparison"] = [dict(progress=a["receipt"]["progress"], initialPalette=a["palette"]["currentPaletteHash"],
                finalPalette=b["palette"]["currentPaletteHash"], equalPalette=a["palette"]["currentPaletteHash"] == b["palette"]["currentPaletteHash"])
                for a, b in zip(report["cases"]["initial"]["checkpoints"], report["cases"]["final"]["checkpoints"])]
        except Exception as error:
            report["error"] = str(error)
        finally:
            try:
                validate_inputs(args.native_report, args.native_report_sha256)
                report["nativeEvidenceUnchanged"] = True
                if sync.file_hash(PROJECT / "Scripts/BakedModelRuntimeProbe.py") != report["probeSha256"]:
                    raise ValueError("Runtime probe script changed during acceptance")
                for case in report["cases"].values():
                    harness = case["harness"]
                    if sync.file_hash(PROJECT / harness["scene"]) != harness["sceneSha256"]:
                        raise ValueError("Display scene changed during runtime acceptance")
                    if sync.file_hash((PROJECT / harness["scene"]).with_name("request.json")) != harness["requestSha256"]:
                        raise ValueError("Display probe request changed during acceptance")
            except Exception as error:
                report.update(status="FAIL", finalIdentityError=str(error))
            scope["status"] = "success" if report["status"] == "PASS" else "failed"
            sync.atomic(output / "report.json", sync.encode(report))
    print(json.dumps(dict(status=report["status"], report=str(output / "report.json"), visualStatus="NOT_REVIEWED")))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
