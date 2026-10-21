"""Bake an imported HEngine animation onto a different skeleton's bind pose.

The mapping is data, not a naming convention. Source node names may include an
FBX namespace; an unqualified mapping name is accepted only when unambiguous.
The output is an ordinary .hanim and needs no runtime retargeting cost.
"""

import argparse
import bisect
import json
import math
from pathlib import Path


IDENTITY = (1.0, 0.0, 0.0, 0.0)


def normalized(q):
    length = math.sqrt(sum(v * v for v in q))
    if not math.isfinite(length) or length < 1e-8:
        raise ValueError("Invalid or zero length rotation")
    return tuple(v / length for v in q)


def mul(a, b):
    w, x, y, z = a
    p, u, v, t = b
    return (w*p-x*u-y*v-z*t, w*u+x*p+y*t-z*v,
            w*v-x*t+y*p+z*u, w*t+x*v-y*u+z*p)


def inverse(q):
    w, x, y, z = normalized(q)
    return (w, -x, -y, -z)


def rotate(q, v):
    p = mul(mul(q, (0.0, *v)), inverse(q))
    return p[1:]


def rotation_between(start, end):
    def unit(vector):
        length = math.sqrt(sum(v*v for v in vector))
        if length < 1e-8 or not math.isfinite(length):
            raise ValueError("Pose alignment needs nonzero bone segments")
        return tuple(v/length for v in vector)

    a, b = unit(start), unit(end)
    dot = max(-1.0, min(1.0, sum(x*y for x, y in zip(a, b))))
    if dot > 1 - 1e-7:
        return IDENTITY
    cross = (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
    if dot < -1 + 1e-7:
        axis = unit((0, -a[2], a[1]) if abs(a[0]) < 0.9 else (-a[2], 0, a[0]))
        return (0.0, *axis)
    return normalized((1+dot, *cross))


def slerp(a, b, fraction):
    a, b = normalized(a), normalized(b)
    dot = sum(x*y for x, y in zip(a, b))
    if dot < 0:
        b, dot = tuple(-v for v in b), -dot
    if dot > 0.9995:
        return normalized(tuple(x + fraction*(y-x) for x, y in zip(a, b)))
    angle = math.acos(max(-1.0, min(1.0, dot)))
    scale = math.sin(angle)
    return normalized(tuple((math.sin((1-fraction)*angle)*x +
                             math.sin(fraction*angle)*y) / scale for x, y in zip(a, b)))


def matrix_rotation(m):
    if len(m) != 16 or not all(math.isfinite(v) for v in m):
        raise ValueError("Source bind transform must be a finite 4x4 matrix")
    # Column-major matrix; strip positive scale before converting to a quaternion.
    columns = [tuple(m[c*4+r] for r in range(3)) for c in range(3)]
    lengths = [math.sqrt(sum(v*v for v in col)) for col in columns]
    if min(lengths) < 1e-8:
        raise ValueError("Source bind transform has a singular axis")
    c = [tuple(v/lengths[i] for v in col) for i, col in enumerate(columns)]
    determinant = (c[0][0]*(c[1][1]*c[2][2]-c[1][2]*c[2][1]) -
                   c[1][0]*(c[0][1]*c[2][2]-c[0][2]*c[2][1]) +
                   c[2][0]*(c[0][1]*c[1][2]-c[0][2]*c[1][1]))
    if determinant < 0.999 or any(abs(sum(a*b for a, b in zip(c[i], c[j]))) > 1e-3
                                for i, j in ((0, 1), (0, 2), (1, 2))):
        raise ValueError("Source bind transform has reflection or shear")
    r = [[c[col][row] for col in range(3)] for row in range(3)]
    trace = r[0][0] + r[1][1] + r[2][2]
    if trace > 0:
        s = math.sqrt(trace + 1) * 2
        q = ((s/4), (r[2][1]-r[1][2])/s,
             (r[0][2]-r[2][0])/s, (r[1][0]-r[0][1])/s)
    else:
        i = max(range(3), key=lambda n: r[n][n])
        j, k = (i+1)%3, (i+2)%3
        s = math.sqrt(max(0.0, 1+r[i][i]-r[j][j]-r[k][k])) * 2
        if s < 1e-8:
            raise ValueError("Cannot decode source bind rotation")
        v = [0.0]*4
        v[i+1] = s/4
        v[0] = (r[k][j]-r[j][k])/s
        v[j+1] = (r[j][i]+r[i][j])/s
        v[k+1] = (r[k][i]+r[i][k])/s
        q = tuple(v)
    return normalized(q)


def pose_matrix(bone):
    w, x, y, z = normalized(bone["localBindR"])
    sx, sy, sz = bone["localBindS"]
    tx, ty, tz = bone["localBindT"]
    return [(1-2*(y*y+z*z))*sx, 2*(x*y+w*z)*sx, 2*(x*z-w*y)*sx, 0,
            2*(x*y-w*z)*sy, (1-2*(x*x+z*z))*sy, 2*(y*z+w*x)*sy, 0,
            2*(x*z+w*y)*sz, 2*(y*z-w*x)*sz, (1-2*(x*x+y*y))*sz, 0,
            tx, ty, tz, 1]


def source_nodes(root):
    nodes = {}
    order = []

    def visit(node, parent):
        name = node["name"]
        if not name or name in nodes:
            raise ValueError("Source hierarchy has empty or duplicate node names")
        transform = node["localTransform"]
        nodes[name] = {"parent": parent, "bind": matrix_rotation(transform),
                       "translation": tuple(transform[12:15]),
                       "scale": tuple(math.sqrt(sum(transform[c*4+r]**2 for r in range(3)))
                                      for c in range(3))}
        order.append(name)
        for child in node["children"]:
            visit(child, name)

    visit(root, None)
    for name in order:
        node = nodes[name]
        parent_node = nodes[node["parent"]] if node["parent"] else None
        parent_rotation = parent_node["worldBind"] if parent_node else IDENTITY
        parent_translation = parent_node["worldTranslation"] if parent_node else (0, 0, 0)
        parent_scale = parent_node["worldScale"] if parent_node else (1, 1, 1)
        node["worldBind"] = normalized(mul(parent_rotation, node["bind"]))
        offset = rotate(parent_rotation, tuple(a*b for a, b in zip(parent_scale, node["translation"])))
        node["worldTranslation"] = tuple(a+b for a, b in zip(parent_translation, offset))
        node["worldScale"] = tuple(a*b for a, b in zip(parent_scale, node["scale"]))
    return nodes, order


def target_nodes(skeleton):
    bones = skeleton["bones"]
    by_index = {bone["index"]: bone for bone in bones}
    if len(by_index) != len(bones) or len({b["name"] for b in bones}) != len(bones):
        raise ValueError("Target skeleton has duplicate bone indices or names")
    roots = [b for b in bones if b["parentIndex"] == -1]
    if len(roots) != 1:
        raise ValueError("Target skeleton must have one root bone")
    children = {b["index"]: [] for b in bones}
    for bone in bones:
        if bone["parentIndex"] >= 0:
            if bone["parentIndex"] not in by_index:
                raise ValueError("Target skeleton has an unknown parent index")
            children[bone["parentIndex"]].append(bone)
    order = []
    world_bind = {}

    def visit(bone, parent_world):
        name = bone["name"]
        world_bind[name] = normalized(mul(parent_world, bone["localBindR"]))
        order.append(bone)
        return {"name": name, "localTransform": pose_matrix(bone),
                "children": [visit(child, world_bind[name]) for child in children[bone["index"]]]}

    hierarchy = visit(roots[0], IDENTITY)
    if len(order) != len(bones):
        raise ValueError("Target skeleton contains a cycle or disconnected bone")
    return hierarchy, order, world_bind, by_index


def resolve_mapping(mapping, nodes, targets):
    result = {}
    for requested, target in mapping.items():
        matches = ([requested] if requested in nodes else
                   [name for name in nodes if name.rsplit(":", 1)[-1] == requested])
        if len(matches) != 1:
            raise ValueError(f"Source bone {requested!r} is missing or ambiguous: {matches}")
        if target not in targets or target in result:
            raise ValueError(f"Target bone {target!r} is missing or mapped twice")
        result[target] = matches[0]
    if not result:
        raise ValueError("Bone map is empty")
    return result


def sample(keys, time, fields, fallback, interpolate):
    if not keys:
        return fallback
    stamps = [key["timeStamp"] for key in keys]
    if stamps != sorted(stamps) or not all(math.isfinite(t) for t in stamps):
        raise ValueError("Animation keys must have finite increasing timestamps")
    index = bisect.bisect_right(stamps, time)
    if index == 0 or len(keys) == 1:
        return tuple(keys[0][f] for f in fields)
    if index == len(keys):
        return tuple(keys[-1][f] for f in fields)
    before, after = keys[index-1], keys[index]
    a, b = (tuple(key[f] for f in fields) for key in (before, after))
    duration = after["timeStamp"] - before["timeStamp"]
    return interpolate(a, b, (time-before["timeStamp"]) / duration if duration > 0 else 1)


def retarget(source, skeleton, config):
    source_by_name, source_order = source_nodes(source["rootNode"])
    hierarchy, target_order, _, by_index = target_nodes(skeleton)
    target_by_name = {bone["name"]: bone for bone in target_order}
    mapping = resolve_mapping(config["boneMap"], source_by_name, target_by_name)
    rotation_mode = config.get("rotationMode", "bindDelta")
    if rotation_mode not in ("bindDelta", "absolute"):
        raise ValueError("rotationMode must be bindDelta or absolute")
    pose_alignment = config.get("poseAlignment", {})
    for name, child in pose_alignment.items():
        if name not in mapping or child not in mapping or target_by_name[child]["parentIndex"] != target_by_name[name]["index"]:
            raise ValueError("Pose alignment requires mapped direct parent-child bones")
    target_reference = {}
    for bone in target_order:
        name = bone["name"]
        parent_bone = by_index.get(bone["parentIndex"])
        parent_name = parent_bone["name"] if parent_bone else None
        parent_rotation = target_reference[parent_name] if parent_name else IDENTITY
        reference_rotation = normalized(mul(parent_rotation, bone["localBindR"]))
        if name in pose_alignment:
            child_name = pose_alignment[name]
            target_direction = rotate(reference_rotation, target_by_name[child_name]["localBindT"])
            source_name, source_child = mapping[name], mapping[child_name]
            source_direction = tuple(a-b for a, b in zip(
                source_by_name[source_child]["worldTranslation"],
                source_by_name[source_name]["worldTranslation"]))
            reference_rotation = normalized(mul(rotation_between(target_direction, source_direction), reference_rotation))
        target_reference[name] = reference_rotation
    hip = config.get("hipTarget")
    if hip is not None and hip not in mapping:
        raise ValueError("hipTarget must be a mapped bone")
    motion = config.get("hipMotion", "none")
    if motion not in ("none", "vertical", "full"):
        raise ValueError("hipMotion must be none, vertical, or full")
    up_axis = config.get("upAxis", 1)
    if up_axis not in (0, 1, 2):
        raise ValueError("upAxis must be 0, 1, or 2")
    translation_scale = float(config.get("translationScale", 1))
    if not math.isfinite(translation_scale) or translation_scale <= 0:
        raise ValueError("translationScale must be positive and finite")
    matrices = skeleton["inverseBindMatrices"]
    if len(matrices) != len(skeleton["bones"]):
        raise ValueError("Target skeleton inverse bind palette is incomplete")
    output = {"version": 1, "name": source.get("name", "Retargeted"),
              "meshResPath": config.get("meshResPath", ""),
              "targetSkeletonPath": config["targetSkeletonPath"],
              "rootNode": hierarchy,
              "boneInfos": [{"name": bone["name"], "id": bone["index"],
                             "offsetMatrix": matrices[i]}
                            for i, bone in enumerate(skeleton["bones"])],
              "clips": []}
    for clip in source["clips"]:
        channels = {c["name"]: c for c in clip["channels"]}
        if len(channels) != len(clip["channels"]):
            raise ValueError("Source clip has duplicate channels")
        if any(name not in source_by_name for name in channels):
            raise ValueError("Source clip has a channel outside its hierarchy")
        times = {0.0}
        for channel in channels.values():
            for key_type in ("rotationKeys", "positionKeys"):
                times.update(key["timeStamp"] for key in channel[key_type])
        times = sorted(t for t in times if 0 <= t <= clip["duration"])
        if not times or not all(math.isfinite(t) for t in times):
            raise ValueError("Source clip has invalid keyframe times")
        result_channels = {target: {"name": target, "positionKeys": [],
                                    "rotationKeys": [], "scaleKeys": []}
                           for target in mapping}
        for time in times:
            source_world = {}
            source_position = {}
            source_scale = {}
            for name in source_order:
                node = source_by_name[name]
                channel = channels.get(name, {})
                local = sample(channel.get("rotationKeys", []), time,
                               ("qw", "qx", "qy", "qz"), node["bind"], slerp)
                local_translation = sample(channel.get("positionKeys", []), time,
                                           ("px", "py", "pz"), node["translation"],
                                           lambda a, b, f: tuple(x+(y-x)*f for x, y in zip(a, b)))
                local_scale = sample(channel.get("scaleKeys", []), time,
                                     ("sx", "sy", "sz"), node["scale"],
                                     lambda a, b, f: tuple(x+(y-x)*f for x, y in zip(a, b)))
                parent_name = node["parent"]
                parent = source_world[parent_name] if parent_name else IDENTITY
                parent_position = source_position[parent_name] if parent_name else (0, 0, 0)
                parent_scale = source_scale[parent_name] if parent_name else (1, 1, 1)
                offset = rotate(parent, tuple(a*b for a, b in zip(parent_scale, local_translation)))
                source_position[name] = tuple(a+b for a, b in zip(parent_position, offset))
                source_scale[name] = tuple(a*b for a, b in zip(parent_scale, local_scale))
                source_world[name] = normalized(mul(parent, local))
            target_world = {}
            for bone in target_order:
                name = bone["name"]
                parent_bone = by_index.get(bone["parentIndex"])
                parent = target_world[parent_bone["name"]] if parent_bone else IDENTITY
                if name in mapping:
                    source_name = mapping[name]
                    if rotation_mode == "absolute":
                        desired = source_world[source_name]
                    else:
                        delta = mul(source_world[source_name], inverse(source_by_name[source_name]["worldBind"]))
                        desired = normalized(mul(delta, target_reference[name]))
                    local = normalized(mul(inverse(parent), desired))
                    previous = result_channels[name]["rotationKeys"]
                    if previous and sum(local[i]*previous[-1][field]
                                        for i, field in enumerate(("qw", "qx", "qy", "qz"))) < 0:
                        local = tuple(-v for v in local)
                    result_channels[name]["rotationKeys"].append(
                        dict(zip(("qw", "qx", "qy", "qz"), local), timeStamp=time))
                else:
                    local = normalized(bone["localBindR"])
                target_world[name] = normalized(mul(parent, local))
            if hip and motion != "none":
                source_name = mapping[hip]
                source_node = source_by_name[source_name]
                world_delta = [source_position[source_name][i]-source_node["worldTranslation"][i]
                               for i in range(3)]
                if motion == "vertical":
                    world_delta = [v if i == up_axis else 0.0 for i, v in enumerate(world_delta)]
                target_parent = by_index.get(target_by_name[hip]["parentIndex"])
                parent_world = target_world[target_parent["name"]] if target_parent else IDENTITY
                local_delta = rotate(inverse(parent_world), world_delta)
                position = [target_by_name[hip]["localBindT"][i] + local_delta[i]*translation_scale
                            for i in range(3)]
                result_channels[hip]["positionKeys"].append(
                    dict(zip(("px", "py", "pz"), position), timeStamp=time))
        output["clips"].append({"name": clip["name"], "duration": clip["duration"],
                                "ticksPerSecond": clip["ticksPerSecond"],
                                "channels": list(result_channels.values()),
                                "events": clip.get("events", [])})
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True, help="Imported .hanim or directory of clips")
    parser.add_argument("--skeleton", type=Path, required=True, help="Target .hskel")
    parser.add_argument("--config", type=Path, required=True, help="JSON bone map and project asset paths")
    parser.add_argument("--output", type=Path, required=True, help="Retargeted .hanim or output directory")
    args = parser.parse_args()
    skeleton = json.loads(args.skeleton.read_text(encoding="utf-8"))
    config = json.loads(args.config.read_text(encoding="utf-8"))
    sources = sorted(args.source.glob("*.hanim")) if args.source.is_dir() else [args.source]
    if not sources:
        raise ValueError("Source directory has no .hanim clips")
    if args.source.is_dir() and args.output.suffix:
        raise ValueError("Directory input requires an output directory")
    outputs = []
    for path in sources:
        source = json.loads(path.read_text(encoding="utf-8"))
        result = retarget(source, skeleton, config)
        destination = args.output / path.name if args.source.is_dir() else args.output
        outputs.append((destination, result))
    # Validate the whole pack before writing any outputs.
    for destination, result in outputs:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n",
                               encoding="utf-8")
    print(f"Retargeted {sum(len(result['clips']) for _, result in outputs)} clip(s) to {args.output}")


if __name__ == "__main__":
    main()
