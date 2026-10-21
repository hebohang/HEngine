"""Publish the existing sampling fixture and capture it with an owned, pinned Game.

Requires prebuilt binaries. Command/import checks and unreviewed GPU images are
reported separately; this is not a pixel oracle or a performance benchmark.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import shutil
import sys
import time
import uuid

import content_sync
from generate_sampling_fixture import generate, encode

ROOT = Path(__file__).resolve().parents[2]
PROJECT = ROOT / "Project"
sys.path.insert(0, str(ROOT / "Tools/VFX"))
from preview import Session, Storage, digest, write_json


class CleanupFailure(RuntimeError):
    """A stop failure leaves ownership uncertain and must protect evidence."""


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def source_for_run(directory, phase, namespace):
    document = generate(directory, phase)

    def rekey(value):
        if isinstance(value, dict):
            extras = value.get("extras", {})
            if "he_id" in extras:
                extras["he_id"] = str(uuid.uuid5(namespace, extras["he_id"]))
            for child in value.values():
                rekey(child)
        elif isinstance(value, list):
            for child in value:
                rekey(child)

    rekey(document)
    content_sync.atomic(directory / "sampling.gltf", encode(document))
    manifest = read(directory / "manifest.json")
    manifest["sha256"]["sampling.gltf"] = digest(directory / "sampling.gltf")
    content_sync.atomic(directory / "manifest.json", encode(manifest))
    return document


def inspect_published(state, authored):
    materials, meshes = {}, []
    for name, expected in state["artifacts"].items():
        path = PROJECT / name
        require(path.is_file() and digest(path) == expected, "Published artifact hash mismatch: " + name)
        if path.suffix == ".hmat":
            materials[name] = read(path)
        elif path.suffix == ".hmesh":
            meshes.append(read(path))
    require(materials and meshes, "No published materials/meshes")
    current = []
    for source_material in authored["materials"]:
        identity = source_material["extras"]["he_id"]
        published = state["materials"].get(identity)
        require(published in materials, "Authored material missing from publication: " + identity)
        material = materials[published]
        current.append(material)
        actual = material["Parameters"]["g_BaseColorFactor"]["Value"]
        expected = source_material["pbrMetallicRoughness"]["baseColorFactor"]
        require(len(actual) == 4 and all(abs(a-b) < 1e-5 for a, b in zip(actual, expected)),
                "Published authored base color differs: " + identity)
    # The artifact set also contains mesh defaults and old materials retained
    # by explicit instance overrides. Their hashes remain checked above.
    sampling = [m["Textures"]["g_AlbedoMap"]["Sampling"] for m in current]
    require({s["minFilter"] for s in sampling} == {9728, 9729, 9984, 9985, 9986, 9987}, "Missing minFilter modes")
    require(any(s["texCoord"] == 1 for s in sampling), "UV1 selector lost")
    require(any(abs(s["rotation"] - .6) < 1e-5 for s in sampling), "Texture transform lost")
    for mesh in meshes:
        require(bool(mesh["primitives"]), "Empty published mesh")
        for primitive in mesh["primitives"]:
            require({"TEXCOORD_1", "COLOR_0"}.issubset(primitive["attributes"]), "Mesh UV1/color lost")
    return dict(materials=len(materials), authoredMaterials=len(current), meshes=len(meshes), artifacts=len(state["artifacts"]))


def capture(scene, directory, args, namespace):
    directory.mkdir(parents=True)
    document = copy.deepcopy(read(scene))
    for entity in document.values():
        entity.pop("PythonScriptComponent", None)  # Preserve override on imported scene; never execute it here.
    baseline = read(PROJECT / "Assets/Scenes/VFXAuthoringPreview.he")
    for identity, entity in baseline.items():
        if not any(component in entity for component in ("CameraComponent", "DirectionalLightComponent", "EnvironmentLightComponent")):
            continue
        entity = copy.deepcopy(entity)
        if "CameraComponent" in entity:
            entity["TransformComponent"]["Translation"] = [2.5, 1.9, 8.0]
            entity["TransformComponent"]["Rotation"] = [0.0, 0.0, 0.0]
        if "DirectionalLightComponent" in entity:
            entity["DirectionalLightComponent"]["Color"] = [1.0, 1.0, 1.0]
            # Light direction is transformed +Y. Tilt it toward the XY panels;
            # zero rotation is grazing and cannot test their normal maps.
            entity["DirectionalLightComponent"]["Intensity"] = 4.0
            entity["TransformComponent"]["Rotation"] = [-45.0, 0.0, 0.0]
        document[str(uuid.uuid5(namespace, "capture/" + identity))] = entity
    scratch = PROJECT / "Assets/Effects/Authoring/SamplingAcceptance" / str(namespace)
    scratch.mkdir(parents=True, exist_ok=True)
    capture_scene = scratch / "Capture.he"
    content_sync.atomic(capture_scene, encode(document))
    shutil.copyfile(capture_scene, directory / "Capture.he")
    session = Session(directory, args.backend, args.existing_build_sha256,
                      scene=capture_scene.relative_to(PROJECT).as_posix())
    result = dict(scenePath=str(capture_scene), sceneSha256=digest(capture_scene),
                  visualStatus="NOT_REVIEWED", commandStatus="RUNNING")
    try:
        session.start()
        result["session"] = session.call("session.status")
        ticket = session.call("runtime.step", dict(frames=60))
        deadline = time.monotonic() + 120
        while True:
            state = session.call("session.status")["world"]["runtimeControl"]
            if state["completedSteps"] >= ticket["targetCompletedSteps"] and not state["pendingSteps"] and not state["stepInProgress"]:
                break
            if time.monotonic() >= deadline:
                raise TimeoutError("Sampling capture step ticket timed out")
            time.sleep(.1)
        receipt = session.call("capture.frame", dict(filename="sampling.png"))
        shutil.copyfile(receipt["path"], directory / "sampling.png")
        require((directory / "sampling.png").stat().st_size > 0, "Empty screenshot")
        result.update(commandStatus="PASS", capture=receipt, imageSha256=digest(directory / "sampling.png"))
    finally:
        try:
            session.stop()
        except Exception as error:
            result["cleanupError"] = str(error)[:1200]
            result["sceneRetainedForRecovery"] = True
            raise CleanupFailure("Owned Session stop failed: " + str(error)[:1200]) from error
        else:
            # Only confirmed stop permits removal. Failed stop preserves the
            # live scene as recovery material and aborts subsequent imports.
            capture_scene.unlink(missing_ok=True)
            result["sceneRetainedForRecovery"] = False
        finally:
            write_json(directory / "capture.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--importer", type=Path, required=True)
    parser.add_argument("--importer-sha256", required=True)
    parser.add_argument("--existing-build-sha256", required=True)
    parser.add_argument("--backend", choices=["DX12", "Vulkan"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.importer = args.importer.resolve()
    binary = ROOT / "out/agent/game-relwithdebinfo/bin/StarbloomSurvivor.exe"
    require(digest(args.importer) == args.importer_sha256.lower(), "Importer pin mismatch")
    require(digest(binary) == args.existing_build_sha256.lower(), "Game pin mismatch")
    args.existing_build_sha256 = args.existing_build_sha256.lower()
    output = args.output.resolve()
    allowed = (ROOT / "out/agent/rendering-evidence/vfx-authoring").resolve()
    require(allowed in output.parents, "Output must be a new child of " + str(allowed))
    output.mkdir(parents=True, exist_ok=False)
    namespace = uuid.uuid4()
    source = output / "source"
    report = dict(schemaVersion=1, status="RUNNING", backend=args.backend, phases=[],
                  importerSha256=digest(args.importer), executableSha256=digest(binary),
                  driverSha256=digest(__file__), runNamespace=str(namespace),
                  visualStatus="NOT_REVIEWED", performanceStatus="NOT_BENCHMARKED",
                  evidenceType="runtime-manual", sourceIdentityScope="Pinned existing binaries; not a build of current source",
                  limitations=["No quantitative rendered pixel oracle", "Raster captures only; no RT/depth/shadow image acceptance",
                               "Capture-only camera/light composition; not source camera import",
                               "Realtime GI retains constant material approximation"])
    with Storage(ROOT).workflow_scope(str(PROJECT), "sampling-acceptance", [output]) as run:
        try:
            previous = None
            owner = None
            override = None
            for phase in ("initial", "noop", "material", "geometry", "rename", "add", "delete"):
                require(digest(args.importer) == report["importerSha256"], "Importer changed during run")
                phase_output = output / phase
                phase_output.mkdir()
                if phase != "noop":
                    authored = source_for_run(source, phase, namespace)
                for name in ("sampling.gltf", "sampling.bin", "grid.png", "normal.png", "manifest.json"):
                    shutil.copyfile(source / name, phase_output / name)
                arguments = argparse.Namespace(action="sync", source=source / "sampling.gltf", project=PROJECT,
                    importer=args.importer, blender=None, initialize_ids=False, verify="none", timeout=120, fault=None)
                summary = content_sync.synchronize(arguments, phase_output)
                write_json(phase_output / "sync.json", summary)
                scene = PROJECT / summary["scene"]
                document_id = authored["scenes"][0]["extras"]["he_id"]
                state = read(PROJECT / ".he-content" / document_id / "state.json")
                document = read(scene)
                checks = inspect_published(state, authored)
                require(summary["metrics"]["entities"] == len(authored["nodes"]), "Published entity count mismatch")
                if phase == "noop":
                    require(summary["status"] == "unchanged" and not summary["changed"] and summary["newResources"] == 0,
                            "No-op unexpectedly changed published content")
                    require((digest(scene), scene.stat().st_mtime_ns) == previous, "No-op touched scene")
                if owner is not None:
                    require(document[owner]["PythonScriptComponent"] == override["PythonScriptComponent"], "Script override lost")
                    require(document[owner]["MeshComponent"]["MaterialOverrides"] == override["MaterialOverrides"], "Material override lost")
                    checks["overridesPreserved"] = True
                if phase == "material":
                    require(state["artifacts"] != previous_artifacts, "Material edit did not change artifacts")
                if phase == "geometry":
                    require({p for p in state["artifacts"] if p.endswith(".hmesh")} != previous_meshes,
                            "Geometry edit did not change meshes")
                if phase == "rename":
                    require(all(e["TagComponent"]["Tag"].endswith("_renamed") for e in document.values()), "Rename did not publish")
                shutil.copyfile(scene, phase_output / "Imported.he")
                shutil.copyfile(PROJECT / ".he-content" / document_id / "state.json", phase_output / "state.json")
                entry = dict(phase=phase, checks=checks, summary=summary)
                report["phases"].append(entry)
                if phase in ("initial", "material", "geometry"):
                    entry["capture"] = capture(scene, phase_output / "gpu", args, namespace)
                if phase == "noop":
                    owner = next(identity for identity, entity in document.items() if entity["TagComponent"]["Tag"] == "uv0")
                    material = next(path for path in state["artifacts"] if path.endswith(".hmat"))
                    override = dict(PythonScriptComponent=dict(Path="Scripts/VFXCharacterAcceptance.py"), MaterialOverrides=[material])
                    document[owner]["PythonScriptComponent"] = override["PythonScriptComponent"]
                    document[owner]["MeshComponent"]["MaterialOverrides"] = override["MaterialOverrides"]
                    content_sync.atomic(scene, encode(document))
                previous = (digest(scene), scene.stat().st_mtime_ns)
                previous_artifacts = state["artifacts"]
                previous_meshes = {p for p in state["artifacts"] if p.endswith(".hmesh")}
                write_json(output / "report.json", report)
            rejected = output / "rejected-uv3"
            rejected.mkdir()
            before = (digest(scene), digest(PROJECT / ".he-content" / document_id / "state.json"))
            authored["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 3
            content_sync.atomic(source / "sampling.gltf", encode(authored))
            try:
                content_sync.synchronize(arguments, rejected)
            except content_sync.Failure as error:
                require(error.diagnostic["code"] == "conversion_failed", "UV3 failed for an unrelated reason")
                require("texture_sampling" in json.dumps(read(rejected / "conversion.json"))
                        or "texture_sampling" in (rejected / "convert.log").read_text(encoding="utf-8", errors="replace"),
                        "UV3 rejection lacks a texture-coordinate diagnostic")
            else:
                raise AssertionError("Unsupported UV3 was accepted")
            require(before == (digest(scene), digest(PROJECT / ".he-content" / document_id / "state.json")),
                    "Rejected import changed published scene/state")
            report["rejectedUv3PreservedPublication"] = True
            source_for_run(source, "delete", namespace)
            require(digest(binary) == report["executableSha256"] and digest(args.importer) == report["importerSha256"], "Binaries changed")
            report["status"] = "PASS"
            run["status"] = "success"
        except Exception as error:
            report.update(status="FAIL", error=str(error)[:2400])
            run["status"] = "unknown" if isinstance(error, CleanupFailure) else "failed"
            raise
        finally:
            write_json(output / "report.json", report)
    print(json.dumps(dict(status=report["status"], visualStatus=report["visualStatus"], report=str(output / "report.json"))))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(json.dumps(dict(status="FAIL", error=str(error)[:2400])), file=sys.stderr)
        sys.exit(1)
