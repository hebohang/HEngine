"""Bind the existing split YBot walk clip through the engine's controller schema.

No source mesh, clip, skeleton, generated serializer, or generic loader is edited.
The numerical audit mirrors Animation/Bone/Animator TRS and root-motion semantics;
it is an offline input contract, not evidence of a rendered runtime animation.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys

import numpy as np


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def save(path, data):
    text = json.dumps(data, indent=2, ensure_ascii=False, allow_nan=False) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def matrix(values):
    assert len(values) == 16
    result = np.array(values, dtype=np.float64).reshape(4, 4, order="F")
    assert np.isfinite(result).all()
    return result


def interpolate(keys, fields, time, quaternion=False):
    times = np.array([key["timeStamp"] for key in keys])
    assert len(keys) > 0 and np.isfinite(times).all()
    assert len(keys) == 1 or np.all(np.diff(times) > 0)
    values = np.array([[key[field] for field in fields] for key in keys])
    assert np.isfinite(values).all()
    if len(keys) == 1:
        result = values[0]
    else:
        index = min(int(np.searchsorted(times, time, side="right")) - 1, len(keys) - 2)
        assert index >= 0 and time <= times[-1]
        factor = (time - times[index]) / (times[index + 1] - times[index])
        a, b = values[index:index + 2]
        if quaternion:
            a, b = a / np.linalg.norm(a), b / np.linalg.norm(b)
            dot = np.dot(a, b)
            if dot < 0:
                b, dot = -b, -dot
            if dot < 0.999999:
                angle = np.arccos(np.clip(dot, -1, 1))
                result = (np.sin((1 - factor) * angle) * a + np.sin(factor * angle) * b) / np.sin(angle)
            else:
                result = a * (1 - factor) + b * factor
        else:
            result = a * (1 - factor) + b * factor
    return result / np.linalg.norm(result) if quaternion else result


def channel_matrix(channel, time):
    p = interpolate(channel["positionKeys"], ("px", "py", "pz"), time)
    w, x, y, z = interpolate(channel["rotationKeys"], ("qw", "qx", "qy", "qz"), time, True)
    s = interpolate(channel["scaleKeys"], ("sx", "sy", "sz"), time)
    r = np.array([[1 - 2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
                  [2*(x*y+z*w), 1 - 2*(x*x+z*z), 2*(y*z-x*w)],
                  [2*(x*z-y*w), 2*(y*z+x*w), 1 - 2*(x*x+y*y)]])
    result = np.eye(4)
    result[:3, :3] = r @ np.diag(s)
    result[:3, 3] = p
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path("Examples/RenderingTests"))
    args = parser.parse_args()
    project = args.project.resolve()
    base = project / "Assets/Models/YBot"
    output = project / "Assets/RenderingBenchmarks/Validation"
    sources = [base / name for name in ("Y Bot.hmesh", "Y Bot.bin", "Y Bot_Skeleton.hskel", "StandardWalk.hanim", "YBot_Locomotion.hanimctrl")]
    original_hashes = {str(path.relative_to(project)).replace("\\", "/"): sha(path) for path in sources}
    mesh, skeleton, animation, controller = [load(base / name) for name in ("Y Bot.hmesh", "Y Bot_Skeleton.hskel", "StandardWalk.hanim", "YBot_Locomotion.hanimctrl")]
    assert mesh["skeletonPath"] == controller["targetSkeletonPath"]
    assert not animation["boneInfos"], "This fixture intentionally exercises controller skeleton binding for a split clip."
    assert animation["targetSkeletonPath"] in ("", "None", controller["targetSkeletonPath"])
    bones = skeleton["bones"]
    assert len(bones) == len(skeleton["inverseBindMatrices"])
    assert sorted(bone["index"] for bone in bones) == list(range(len(bones)))
    bone_map = {bone["name"]: (bone["index"], matrix(offset)) for bone, offset in zip(bones, skeleton["inverseBindMatrices"])}
    assert len(bone_map) == len(bones)
    clip = animation["clips"][0]
    channels = {channel["name"]: channel for channel in clip["channels"]}
    assert len(channels) == len(clip["channels"])
    nodes, animated_ancestors = {}, {}

    def audit_hierarchy(node, ancestors):
        name = node["name"]
        assert name not in nodes
        matrix(node["localTransform"])
        nodes[name] = node
        current = ancestors + ([name] if name in channels else [])
        if name in bone_map:
            animated_ancestors[name] = current
        for child in node["children"]:
            audit_hierarchy(child, current)

    audit_hierarchy(animation["rootNode"], [])
    assert set(bone_map).issubset(nodes) and set(channels).issubset(nodes)
    assert all(animated_ancestors.values())

    def palette(frame):
        time = (frame / 60.0 * clip["ticksPerSecond"]) % clip["duration"]
        result = np.tile(np.eye(4), (len(bones), 1, 1))

        def visit(node, parent):
            name = node["name"]
            local = matrix(node["localTransform"])
            if name in channels:
                local = channel_matrix(channels[name], time)
                if "Hips_$AssimpFbx$_Translation" in name:
                    bind = matrix(node["localTransform"])
                    local[0, 3], local[2, 3] = bind[0, 3], bind[2, 3]
            global_matrix = parent @ local
            if name in bone_map:
                index, offset = bone_map[name]
                result[index] = global_matrix @ offset
            for child in node["children"]:
                visit(child, global_matrix)

        visit(animation["rootNode"], np.eye(4))
        assert np.isfinite(result).all()
        return result

    palettes = {frame: palette(frame) for frame in (0, 20, 40)}
    buffers = [(project / buffer["uri"].replace("\\", "/")).read_bytes() for buffer in mesh["bufferVec"]]

    def accessor(index):
        acc = mesh["accessorVec"][index]
        view = mesh["bufferViewVec"][acc["bufferViewIndex"]]
        dtype = np.dtype({5126: "<f4", 5125: "<u4"}[acc["componentType"]])
        components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[acc["type"]]
        stride = view["byteStride"] or components * dtype.itemsize
        offset = view["byteOffset"] + acc["byteOffset"]
        required = acc["byteOffset"] + max(0, acc["count"] - 1) * stride + components * dtype.itemsize
        assert required <= view["byteLength"]
        return np.ndarray((acc["count"], components), dtype, buffers[view["bufferIndex"]], offset,
                          strides=(stride, dtype.itemsize)).astype(np.float64)

    primitive_results, influenced_bones = [], set()
    for primitive_index, primitive in enumerate(mesh["primitives"]):
        attributes = primitive["attributes"]
        pos, joints, weights = [accessor(attributes[key]) for key in ("POSITION", "JOINTS_0", "WEIGHTS_0")]
        assert len(pos) == len(joints) == len(weights)
        assert np.isfinite(pos).all() and np.isfinite(joints).all() and np.isfinite(weights).all()
        assert np.all(weights >= 0) and np.all(weights <= 1)
        assert np.all(joints == np.rint(joints))
        live = weights > 0
        assert np.all(joints[live] >= 0) and np.all(joints[live] < len(bones))
        # Unused influence slots can carry -1; they contribute exactly zero.
        safe_joints = np.where(live, joints, 0).astype(np.int64)
        sum_error = np.abs(weights.sum(axis=1) - 1)
        assert np.max(sum_error) < 1e-5
        indices = accessor(primitive["indices"]).reshape(-1)
        assert np.all(indices >= 0) and np.all(indices < len(pos))
        influenced_bones.update(int(value) for value in joints[live])
        homogeneous = np.column_stack((pos, np.ones(len(pos))))
        skinned = {}
        for frame, pose in palettes.items():
            transforms = np.sum(pose[safe_joints] * weights[:, :, None, None], axis=1)
            skinned[frame] = np.einsum("nij,nj->ni", transforms, homogeneous)[:, :3]
        displacement = np.linalg.norm(skinned[40] - skinned[20], axis=1) * 0.01
        assert np.mean(displacement) > 0.01 and np.max(displacement) > 0.1
        primitive_results.append({"primitive": primitive_index, "vertices": len(pos), "triangles": len(indices) // 3,
                                  "maxWeightSumError": float(np.max(sum_error)),
                                  "movedVertexCountAbove1mm": int(np.count_nonzero(displacement > 0.001)),
                                  "worldDisplacementFrame20To40Meters": {"mean": float(np.mean(displacement)), "p95": float(np.percentile(displacement, 95)), "max": float(np.max(displacement))},
                                  "frame20ObjectBounds": [np.min(skinned[20], axis=0).tolist(), np.max(skinned[20], axis=0).tolist()],
                                  "frame40ObjectBounds": [np.min(skinned[40], axis=0).tolist(), np.max(skinned[40], axis=0).tolist()]})

    # Reuse the existing canonical resource values, retaining the FBX pivot tree.
    validation_controller = copy.deepcopy(controller)
    validation_controller["name"] = "YBotWalkValidation"
    validation_controller["nodes"] = [node for node in validation_controller["nodes"] if node["id"] in (1, 3)]
    validation_controller["parameters"] = []
    entry_transition = next(copy.deepcopy(item) for item in controller["transitions"] if item["fromNodeId"] == controller["entryNodeId"])
    entry_transition["toNodeId"] = 3
    validation_controller["transitions"] = [entry_transition]
    controller_path = output / "YBotWalkValidation.hanimctrl"
    save(controller_path, validation_controller)
    scene_path = output / "SurfaceCoverage.he"
    scene = load(scene_path)
    original_scene = copy.deepcopy(scene)
    targets = [node for node in scene.values() if node.get("MeshComponent", {}).get("Path") == "Assets/Models/YBot/Y Bot.hmesh"]
    assert len(targets) == 1
    node = targets[0]
    node["AnimatorComponent"]["controllerPath"] = str(controller_path.relative_to(project)).replace("\\", "/")
    node["AnimatorComponent"]["animResPath"] = "None"
    assert node["AnimatorComponent"]["bPlay"] and node["AnimatorComponent"]["bLoop"]
    assert not node["AnimatorComponent"]["bEnableRootMotion"] and node["AnimatorComponent"]["bLockRootMotionXZ"]
    assert node["AnimatorComponent"]["playSpeed"] == 1 and node["TransformComponent"]["Scale"] == [0.01] * 3
    node["TagComponent"]["Tag"] = "YBot StandardWalk bound to YBot skeleton"
    for entity_id, entity in scene.items():
        unchanged = copy.deepcopy(entity)
        if entity is node:
            for key in ("controllerPath", "animResPath"):
                unchanged["AnimatorComponent"][key] = original_scene[entity_id]["AnimatorComponent"][key]
            unchanged["TagComponent"]["Tag"] = original_scene[entity_id]["TagComponent"]["Tag"]
        assert unchanged == original_scene[entity_id]
    save(scene_path, scene)
    bone_changes = [{"name": bone["name"], "id": bone["index"],
                     "maxAbsPaletteElementDelta20To40": float(np.max(np.abs(palettes[40][bone["index"]] - palettes[20][bone["index"]]))),
                     "animatedAncestors": animated_ancestors[bone["name"]]} for bone in bones]
    assert sum(item["maxAbsPaletteElementDelta20To40"] > 1e-4 for item in bone_changes) > 40
    assert original_hashes == {str(path.relative_to(project)).replace("\\", "/"): sha(path) for path in sources}
    report = {"schema": "hengine.ybot-validation-input.v1", "evidenceType": "unit-test", "status": "PASS",
              "commandArgv": [sys.executable] + sys.argv,
              "runtimeStatus": "NOT_RUN_BY_ASSET_TOOL", "method": "Existing generated AnimControllerRes schema binds immutable split AnimRes to MeshRes SkeletonRes; numerical input contract mirrors Animation/Bone/Animator and 4-weight skinning.",
              "provenance": "Existing repository YBot/Mixamo-named assets; original download, license and author records are absent from this checkout. No new external asset download or redistribution grant is asserted.",
              "sourceSha256": original_hashes, "controllerSha256": sha(controller_path), "sceneSha256": sha(scene_path),
              "fixedStepSeconds": 1 / 60, "sampleFrames": [0, 20, 40], "sampleAnimationTicks": [0, 10, 20],
              "clipDurationTicks": clip["duration"], "ticksPerSecond": clip["ticksPerSecond"],
              "boneCount": len(bones), "clipChannelCount": len(channels), "hierarchyNodeCount": len(nodes),
              "weightedBoneIds": sorted(influenced_bones), "directChannelsAreNotRequired": "FBX pivot ancestors animate deform bones; full hierarchy is preserved.",
              "worldScale": 0.01, "rootMotionEnabled": False, "rootMotionLockXZ": True,
              "primitives": primitive_results, "boneChanges": bone_changes}
    report_path = output / "ybot_animation_validation.json"
    save(report_path, report)
    print(json.dumps({"status": "PASS", "controller": str(controller_path), "report": str(report_path),
                      "bones": len(bones), "primitives": primitive_results}, allow_nan=False))


if __name__ == "__main__":
    main()
