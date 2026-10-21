"""Attach stable IDs and UE texture parameters to a static UE glTF export.

Usage: python Tools/UE/PrepareLevelForHE.py <export.gltf> <materials.json>
<output.gltf> [texture-folder] --profile <level-profile.json>
[--texture-manifest <texture-export.json>]. This edits only
a derived file beside the project-owned export.
The glTF exporter omits material textures in commandlet mode; this script
uses inspected MaterialInstance parameters and separately exported PNGs.
Set profile identityKey to a stable UE asset path for exports that may move
between directories. Profiles without it keep their legacy path-based IDs.
The texture manifest resolves same-named assets from different UE packages.
"""

import argparse
import copy
import json
import math
import struct
import uuid
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("materials", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("texture_folder", nargs="?", default="Textures")
parser.add_argument("--profile", required=True, type=Path,
                    help="Project-owned collision and material rendering choices")
parser.add_argument("--texture-manifest", type=Path,
                    help="ExportLevelTextures.py result; resolves duplicate UE texture basenames")
args = parser.parse_args()
source_path, manifest_path, output_path = args.source, args.materials, args.output
texture_folder = args.texture_folder
profile = json.loads(args.profile.read_text(encoding="utf-8"))
if profile.get("schema") != 1:
    raise ValueError("UE level profile schema must be 1")
identity_key = profile.get("identityKey")
if identity_key is not None and (not isinstance(identity_key, str) or
                                 not identity_key or identity_key != identity_key.strip() or
                                 len(identity_key) > 512 or
                                 any(ord(character) < 32 for character in identity_key)):
    raise ValueError("identityKey must be a stable, nonempty single-line string")


def profile_names(key):
    names = profile.get(key, [])
    if not isinstance(names, list) or any(not isinstance(name, str) or not name
                                          for name in names) or len(set(names)) != len(names):
        raise ValueError("Invalid UE level profile list: " + key)
    return tuple(names)


collision_prefixes = profile_names("collisionMeshPrefixes")
double_sided_materials = set(profile_names("doubleSidedMaterials"))
masked_materials = set(profile_names("maskedMaterials"))
alpha_cutoff = profile.get("alphaCutoff", 0.5)
alpha_cutoff_overrides = profile.get("alphaCutoffOverrides", {})
emissive_factor = profile.get("emissiveFactor", [1.0, 1.0, 1.0])
light_overrides = profile.get("lightOverrides", {})
if not isinstance(alpha_cutoff, (float, int)) or not math.isfinite(alpha_cutoff) or not 0 <= alpha_cutoff <= 1:
    raise ValueError("Invalid UE level profile alphaCutoff")
if not isinstance(alpha_cutoff_overrides, dict) or any(
        not isinstance(name, str) or name not in masked_materials or
        not isinstance(value, (float, int)) or not math.isfinite(value) or
        not 0 <= value <= 1 for name, value in alpha_cutoff_overrides.items()):
    raise ValueError("Invalid UE level profile alphaCutoffOverrides")
if not isinstance(emissive_factor, list) or len(emissive_factor) != 3 or any(
        not isinstance(value, (float, int)) or not math.isfinite(value) or value < 0
        for value in emissive_factor):
    raise ValueError("Invalid UE level profile emissiveFactor")
if not isinstance(light_overrides, dict) or any(
        not isinstance(name, str) or not name or not isinstance(value, dict) or
        set(value) != {"color", "intensity"} or
        not isinstance(value["color"], list) or len(value["color"]) != 3 or
        any(not isinstance(channel, (int, float)) or not math.isfinite(channel) or
            not 0 <= channel <= 1 for channel in value["color"]) or
        not isinstance(value["intensity"], (int, float)) or
        not math.isfinite(value["intensity"]) or value["intensity"] <= 0
        for name, value in light_overrides.items()):
    raise ValueError("Invalid UE level profile lightOverrides")
source = json.loads(source_path.read_text(encoding="utf-8"))
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
document = copy.deepcopy(source)
namespace = uuid.uuid5(uuid.NAMESPACE_URL, "hengine:ue-level:" + (
    identity_key if identity_key is not None else str(source_path.resolve()).lower()))
binary = bytearray((source_path.parent / source["buffers"][0]["uri"]).read_bytes())


def accessor_bytes(index):
    accessor = document["accessors"][index]
    view = document["bufferViews"][accessor["bufferView"]]
    components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[accessor["type"]]
    width = {5121: 1, 5123: 2, 5125: 4, 5126: 4}[accessor["componentType"]] * components
    stride = view.get("byteStride", width)
    start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    return b"".join(binary[start + row * stride:start + row * stride + width]
                    for row in range(accessor["count"]))


def append_accessor(payload, template, count):
    while len(binary) % 4:
        binary.append(0)
    offset = len(binary)
    binary.extend(payload)
    view_index = len(document["bufferViews"])
    document["bufferViews"].append({"buffer": 0, "byteOffset": offset, "byteLength": len(payload)})
    accessor = {"bufferView": view_index, "componentType": template["componentType"],
                "count": count, "type": template["type"]}
    if template.get("normalized"):
        accessor["normalized"] = True
    index = len(document["accessors"])
    document["accessors"].append(accessor)
    return index


def merge_primitives(group):
    first = group[0]
    keys = set(first["attributes"])
    if any(set(primitive["attributes"]) != keys for primitive in group):
        raise ValueError("Cannot merge UE sections with different vertex attributes")
    counts = [document["accessors"][p["attributes"]["POSITION"]]["count"] for p in group]
    merged = {"attributes": {}, "material": first["material"]}
    for key in first["attributes"]:
        accessors = [document["accessors"][p["attributes"][key]] for p in group]
        if any((a["componentType"], a["type"]) !=
               (accessors[0]["componentType"], accessors[0]["type"]) for a in accessors):
            raise ValueError("Cannot merge UE sections with different attribute types")
        payload = b"".join(accessor_bytes(p["attributes"][key]) for p in group)
        merged["attributes"][key] = append_accessor(payload, accessors[0], sum(counts))
    indices = []
    vertex_offset = 0
    for primitive, count in zip(group, counts):
        if "indices" not in primitive:
            indices.extend(range(vertex_offset, vertex_offset + count))
        else:
            index_accessor = document["accessors"][primitive["indices"]]
            fmt = {5121: "B", 5123: "H", 5125: "I"}[index_accessor["componentType"]]
            values = struct.iter_unpack("<" + fmt, accessor_bytes(primitive["indices"]))
            indices.extend(vertex_offset + value[0] for value in values)
        vertex_offset += count
    merged["indices"] = append_accessor(
        struct.pack("<" + "I" * len(indices), *indices),
        {"componentType": 5125, "type": "SCALAR"}, len(indices))
    return merged


def identity(kind, name):
    return {"he_schema": 1, "he_id": str(uuid.uuid5(namespace, kind + ":" + name))}


def resolved_parameters(row):
    parent = row.get("parent")
    params = resolved_parameters(parent) if isinstance(parent, dict) else {}
    params.update(row.get("textures", {}))
    params.update(row.get("effectiveTextures", {}))
    return params


def resolved_vectors(row):
    parent = row.get("parent")
    params = resolved_vectors(parent) if isinstance(parent, dict) else {}
    params.update(row.get("vectors", {}))
    return params


by_name = {}
for material_path, row in manifest.items():
    short_name = material_path.rsplit("/", 1)[-1].split(".", 1)[0]
    by_name.setdefault(short_name, []).append((material_path, row))
material_source_paths = profile.get("materialSourcePaths", {})
if not isinstance(material_source_paths, dict) or any(
        not isinstance(name, str) or not isinstance(path, str) or path not in manifest
        for name, path in material_source_paths.items()):
    raise ValueError("materialSourcePaths must map glTF material names to manifest paths")
material_names = [material.get("name", str(index))
                  for index, material in enumerate(document["materials"])]
if len(set(material_names)) != len(material_names):
    raise ValueError("glTF material names must be unique for stable HE resource IDs")

exported_textures = None
if args.texture_manifest is not None:
    rows = json.loads(args.texture_manifest.read_text(encoding="utf-8"))
    if not isinstance(rows, list):
        raise ValueError("UE texture export manifest must be a list")
    exported_textures = {}
    output_owners = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("asset"), str) or not row["asset"]:
            raise ValueError("Invalid UE texture export manifest asset")
        if row.get("ok") is not True or not isinstance(row.get("bytes"), int) or row["bytes"] <= 0:
            raise ValueError("UE texture export did not complete: " + row["asset"])
        recorded = row.get("relativeOutput", row.get("output"))
        if not isinstance(recorded, str) or not recorded:
            raise ValueError("UE texture export has no output: " + row["asset"])
        filename = recorded.replace("\\", "/").rsplit("/", 1)[-1]
        if not filename.lower().endswith(".png") or filename in (".png", "..png"):
            raise ValueError("Invalid UE texture export filename: " + recorded)
        package = row["asset"].split(".", 1)[0]
        if package in exported_textures and exported_textures[package] != filename:
            raise ValueError("UE texture package has conflicting exports: " + package)
        previous = output_owners.get(filename.casefold())
        if previous is not None and previous != package:
            raise ValueError("UE texture exports share a filename: " + filename)
        output_owners[filename.casefold()] = package
        exported_textures[package] = filename
images = []
textures = []
texture_index = {}
legacy_output_owners = {}


def texture(path):
    if not path:
        return None
    package = path.split(".", 1)[0]
    if exported_textures is not None:
        if package not in exported_textures:
            raise ValueError("UE texture missing from export manifest: " + package)
        filename = exported_textures[package]
    else:
        filename = package.rsplit("/", 1)[-1] + ".png"
        previous = legacy_output_owners.get(filename.casefold())
        if previous is not None and previous != package:
            raise ValueError("UE texture basenames collide; pass --texture-manifest: " + filename)
        legacy_output_owners[filename.casefold()] = package
    name = filename[:-4]
    relative = texture_folder + "/" + filename
    if not (output_path.parent / relative).is_file():
        hint = " (pass --texture-manifest for renamed UE exports)" if exported_textures is None else ""
        raise FileNotFoundError(str(output_path.parent / relative) + hint)
    if relative not in texture_index:
        texture_index[relative] = len(textures)
        images.append({"uri": relative, "name": name})
        textures.append({"source": len(images) - 1})
    return {"index": texture_index[relative]}


for index, material in enumerate(document["materials"]):
    name = material.get("name", str(index))
    material["extras"] = identity("material", name)
    material.pop("extensions", None)
    selected_path = material_source_paths.get(name)
    if selected_path is not None:
        source_material = manifest[selected_path]
    else:
        candidates = by_name.get(name, [])
        if len(candidates) > 1:
            raise ValueError("UE material name is ambiguous; set materialSourcePaths: " + name)
        source_material = candidates[0][1] if candidates else {}
    parameters = resolved_parameters(source_material)
    vectors = resolved_vectors(source_material)
    color = texture(parameters.get("BaseColor") or parameters.get("Diffuse")
                    or parameters.get("1 Diffuse"))
    normal = texture(parameters.get("Normal") or parameters.get("1 Normal"))
    orm = texture(parameters.get("ORM"))
    emissive_path = parameters.get("EmissiveMask", "")
    emissive = texture(emissive_path) if not emissive_path.split(".", 1)[0].endswith("/T_LinearBlack") else None
    pbr = material.setdefault("pbrMetallicRoughness", {})
    diffuse_tint = next((value for key, value in vectors.items()
                         if key.lower() in ("diffuse color", "1 diffuse color")), None)
    if diffuse_tint is not None:
        if len(diffuse_tint) < 3 or not all(math.isfinite(value) and value >= 0
                                           for value in diffuse_tint[:3]):
            raise ValueError("Invalid UE diffuse tint: " + name)
        original = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
        pbr["baseColorFactor"] = [original[index] * diffuse_tint[index]
                                  for index in range(3)] + [original[3]]
    if color:
        pbr["baseColorTexture"] = color
    if orm:
        pbr["metallicRoughnessTexture"] = orm
        material["occlusionTexture"] = orm
    else:
        pbr["metallicFactor"] = 0.0
        pbr["roughnessFactor"] = 0.8
    if normal:
        material["normalTexture"] = normal
    if emissive:
        material["emissiveTexture"] = emissive
        material["emissiveFactor"] = emissive_factor
    if name in double_sided_materials:
        material["doubleSided"] = True
    if name in masked_materials:
        material["alphaMode"] = "MASK"
        material["alphaCutoff"] = alpha_cutoff_overrides.get(
            name, material.get("alphaCutoff", alpha_cutoff))

omitted_primitives = []
for index, mesh in enumerate(document["meshes"]):
    mesh["extras"] = identity("mesh", mesh.get("name", str(index)))
    valid = []
    for primitive_index, primitive in enumerate(mesh["primitives"]):
        counts = {document["accessors"][accessor]["count"]
                  for accessor in primitive["attributes"].values()}
        if len(counts) == 1 and 0 not in counts:
            # UE foliage exports a third UV channel for an engine-specific
            # billboard path; HE materials below use UV0, and the scene
            # importer intentionally supports at most UV1.
            primitive["attributes"].pop("TEXCOORD_2", None)
            valid.append(primitive)
        else:
            omitted_primitives.append([mesh["name"], primitive_index, sorted(counts)])
    if not valid:
        raise ValueError("No valid primitives in " + mesh["name"])
    by_material = {}
    for primitive in valid:
        by_material.setdefault(primitive.get("material"), []).append(primitive)
    mesh["primitives"] = [merge_primitives(group) if len(group) > 1 else group[0]
                          for group in by_material.values()]

node_names = [node.get("name", str(index)) for index, node in enumerate(document["nodes"])]
parents = {child: parent_index for parent_index, parent in enumerate(document["nodes"])
           for child in parent.get("children", [])}


def mirrored(index):
    result = False
    while True:
        scale = document["nodes"][index].get("scale", [1, 1, 1])
        result ^= scale[0] * scale[1] * scale[2] < 0
        if index not in parents:
            return result
        index = parents[index]


if len(set(node_names)) != len(node_names):
    # Unreal helper nodes may repeat names; only actor/mesh nodes need stable
    # names across re-exports. Keep deterministic indexed identities for helpers.
    from collections import Counter
    repeated = {name for name, count in Counter(node_names).items() if count > 1}
else:
    repeated = set()
for index, node in enumerate(document["nodes"]):
    name = node_names[index]
    node["extras"] = identity("node", name if name not in repeated else f"{name}:{index}")
    if "mesh" in node:
        mesh_name = document["meshes"][node["mesh"]].get("name", "")
        # The same static triangle collision serves the character and the
        # spring-arm sphere cast. UE facade/statue geometry must participate:
        # floor-only collision lets the camera pass straight through it.
        if mesh_name.startswith(collision_prefixes) and not mirrored(index):
            node["extras"]["he_collision"] = "mesh"
    node_extensions = node.get("extensions", {})
    if "KHR_lights_punctual" in node_extensions:
        node["extensions"] = {"KHR_lights_punctual": node_extensions["KHR_lights_punctual"]}
    else:
        node.pop("extensions", None)
    if "mesh" in node and document["meshes"][node["mesh"]].get("name") == "SM_SkySphere":
        node.pop("mesh")

document["scenes"][document.get("scene", 0)]["extras"] = identity("scene", "world")
document["extensions"] = {"KHR_lights_punctual":
                          copy.deepcopy(source["extensions"]["KHR_lights_punctual"])}
light_names = [light.get("name") for light in document["extensions"]["KHR_lights_punctual"]["lights"]]
for name, override in light_overrides.items():
    if light_names.count(name) != 1:
        raise ValueError("lightOverrides name must match exactly one glTF light: " + name)
    light = document["extensions"]["KHR_lights_punctual"]["lights"][light_names.index(name)]
    light["color"] = override["color"]
    light["intensity"] = override["intensity"]
document["extensionsUsed"] = ["KHR_lights_punctual"]
document["images"] = images
document["textures"] = textures
binary_name = output_path.with_suffix(".bin").name
document["buffers"][0]["uri"] = binary_name
document["buffers"][0]["byteLength"] = len(binary)
output_path.with_suffix(".bin").write_bytes(binary)
output_path.write_text(json.dumps(document, ensure_ascii=False, separators=(",", ":")) + "\n", encoding="utf-8")
print(json.dumps({"output": str(output_path), "nodes": len(document["nodes"]),
                  "meshes": len(document["meshes"]), "materials": len(document["materials"]),
                  "textures": len(textures), "omittedInvalidUEPrimitives": omitted_primitives},
                 ensure_ascii=False))
