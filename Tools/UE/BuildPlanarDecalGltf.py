"""Bake downward UE decal placements to cullable planar glTF mesh groups.

This is for static, nearly horizontal receivers such as ground leaf decals.
Projection onto arbitrary walls and uneven geometry requires a true decal pass.
Original UE assets and the placement JSON remain untouched.
"""

import argparse
import json
import math
import os
import struct
import uuid
from collections import defaultdict
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("facts", type=Path)
parser.add_argument("base_color", type=Path)
parser.add_argument("output", type=Path)
parser.add_argument("--normal-texture", type=Path,
                    help="Optional tangent-space UE decal normal texture")
parser.add_argument("--height-offset-cm", type=float, default=-4.0)
parser.add_argument("--cell-size-cm", type=float, default=1000.0)
parser.add_argument("--receiver-gltf", type=Path,
                    help="Use static receiver triangles to place each decal above the actual ground")
parser.add_argument("--surface-offset-cm", type=float, default=1.0)
parser.add_argument("--surface-report", type=Path,
                    help="Write sampled receiver heights and seam fallback details")
parser.add_argument("--subdivisions", type=int, default=1,
                    help="Subdivide each placement for ground conformance (1..4)")
parser.add_argument("--conform-vertices", action="store_true",
                    help="Sample each grid vertex against the receiver mesh")
args = parser.parse_args()
if not math.isfinite(args.height_offset_cm) or not -20 <= args.height_offset_cm <= 20:
    raise ValueError("height offset must be within +/-20 cm")
if not math.isfinite(args.cell_size_cm) or not 100 <= args.cell_size_cm <= 10000:
    raise ValueError("cell size must be 100..10000 cm")
if not math.isfinite(args.surface_offset_cm) or not 0 <= args.surface_offset_cm <= 10:
    raise ValueError("surface offset must be 0..10 cm")
if not 1 <= args.subdivisions <= 4:
    raise ValueError("subdivisions must be 1..4")
if args.conform_vertices and not args.receiver_gltf:
    raise ValueError("vertex conformance requires --receiver-gltf")
facts = json.loads(args.facts.read_text(encoding="utf-8"))
if facts.get("schema") != 1 or not isinstance(facts.get("decals"), list):
    raise ValueError("Invalid UE decal facts")
if not args.base_color.is_file():
    raise FileNotFoundError(args.base_color)
if args.normal_texture and not args.normal_texture.is_file():
    raise FileNotFoundError(args.normal_texture)
if args.receiver_gltf:
    from GltfGroundSampler import GltfGroundSampler
    sampler = GltfGroundSampler(args.receiver_gltf)
else:
    sampler = None


def he_vector(ue):
    return (ue[0] / 100.0, ue[2] / 100.0, ue[1] / 100.0)


def receiver_height(x, z, near_y, search_m=0.75):
    surface = sampler.highest_surface(x, z, near_y, search_m)
    if surface is not None:
        return surface, False
    # Tile seams can leave a point just outside both meshes. Prefer the median
    # of small neighboring probes over silently placing a decal in mid-air.
    nearby = [sampler.highest_surface(x + dx, z + dz, near_y, search_m)
              for dx, dz in ((.1, 0), (-.1, 0), (0, .1), (0, -.1),
                             (.2, 0), (-.2, 0), (0, .2), (0, -.2))]
    nearby = sorted(value for value in nearby if value is not None)
    return (nearby[len(nearby) // 2], True) if nearby else (None, False)


groups = defaultdict(list)
surface_heights = {}
surface_report = []
for decal in facts["decals"]:
    forward = decal["forward"]
    if len(forward) != 3 or forward[2] > -0.8:
        raise ValueError("Only ground-facing planar decals are supported: " + decal["actorName"])
    size = decal["decalSizeCm"]
    if len(size) != 3 or any(not math.isfinite(v) or v <= 0 for v in size):
        raise ValueError("Invalid decal size: " + decal["actorName"])
    x, y, z = decal["locationCm"]
    if sampler:
        surface, seam_fallback = receiver_height(x / 100.0, y / 100.0, z / 100.0)
        if surface is None:
            raise ValueError("No ground receiver near decal: " + decal["actorName"])
        surface_heights[decal["actorName"]] = surface + args.surface_offset_cm / 100.0
        surface_report.append({"actorName": decal["actorName"],
                               "actorHeightMeters": z / 100.0,
                               "receiverHeightMeters": surface,
                               "seamFallback": seam_fallback})
    cell = (math.floor(x / args.cell_size_cm), math.floor(y / args.cell_size_cm),
            math.floor(z / 100.0))
    groups[cell].append(decal)
if not groups:
    raise ValueError("No supported decal placements")

binary = bytearray()
views = []
accessors = []


def accessor(values, fmt, component_type, kind, components, target):
    while len(binary) % 4:
        binary.append(0)
    start = len(binary)
    for value in values:
        binary.extend(struct.pack("<" + fmt * components, *value))
    view = len(views)
    views.append({"buffer": 0, "byteOffset": start, "byteLength": len(binary) - start,
                  "target": target})
    index = len(accessors)
    row = {"bufferView": view, "componentType": component_type, "count": len(values),
           "type": kind}
    if kind == "VEC3" and values:
        row["min"] = [min(value[i] for value in values) for i in range(3)]
        row["max"] = [max(value[i] for value in values) for i in range(3)]
    accessors.append(row)
    return index


namespace = uuid.uuid5(uuid.NAMESPACE_URL, "hengine:ue-planar-decals:" + facts["level"])


def identity(kind, name):
    return {"he_schema": 1, "he_id": str(uuid.uuid5(namespace, kind + ":" + name))}


meshes = []
nodes = []
vertex_sample_misses = 0
skipped_triangles = 0
for cell, decals in sorted(groups.items()):
    origin = (cell[0] * args.cell_size_cm / 100.0, 0.0,
              cell[1] * args.cell_size_cm / 100.0)
    positions, normals, tangents, texcoords, indices = [], [], [], [], []
    for decal in sorted(decals, key=lambda value: value["actorName"]):
        center = he_vector(decal["locationCm"])
        center_y = surface_heights.get(decal["actorName"],
                                       center[1] + args.height_offset_cm / 100.0)
        center = (center[0], center_y, center[2])
        right = he_vector(decal["right"])
        up = he_vector(decal["up"])
        # A direction conversion does not retain the UE cm-to-m scale.
        right = tuple(value * 100.0 for value in right)
        up = tuple(value * 100.0 for value in up)
        normal = tuple(-value * 100.0 for value in he_vector(decal["forward"]))
        half_width = decal["decalSizeCm"][1] / 200.0
        half_height = decal["decalSizeCm"][2] / 200.0
        first = len(positions)
        divisions = args.subdivisions
        valid_grid = []
        for row in range(divisions + 1):
            sy = -1 + 2 * row / divisions
            for column in range(divisions + 1):
                sx = -1 + 2 * column / divisions
                world = tuple(center[i] + sx * half_width * right[i] +
                              sy * half_height * up[i] for i in range(3))
                if args.conform_vertices:
                    height, _ = receiver_height(world[0], world[2],
                                                decal["locationCm"][2] / 100.0,
                                                search_m=1.0)
                    if height is None:
                        vertex_sample_misses += 1
                    else:
                        world = (world[0], height + args.surface_offset_cm / 100.0,
                                 world[2])
                    valid_grid.append(height is not None)
                positions.append(tuple(world[i] - origin[i] for i in range(3)))
                normals.append(normal)
                tangents.append((*right, -1.0))
                texcoords.append(((sx + 1) / 2, (1 - sy) / 2))
        for row in range(divisions):
            for column in range(divisions):
                lower = first + row * (divisions + 1) + column
                upper = lower + divisions + 1
                for triangle in ((lower, lower + 1, upper + 1),
                                 (lower, upper + 1, upper)):
                    if args.conform_vertices and not all(valid_grid[index - first]
                                                         for index in triangle):
                        skipped_triangles += 1
                    else:
                        indices.extend(triangle)
    if len(positions) > 65535:
        raise ValueError("Decal cell needs more than 65535 vertices")
    name = "PlanarDecals_" + "_".join(str(part) for part in cell)
    primitive = {"attributes": {
        "POSITION": accessor(positions, "f", 5126, "VEC3", 3, 34962),
        "NORMAL": accessor(normals, "f", 5126, "VEC3", 3, 34962),
        "TANGENT": accessor(tangents, "f", 5126, "VEC4", 4, 34962),
        "TEXCOORD_0": accessor(texcoords, "f", 5126, "VEC2", 2, 34962)},
        "indices": accessor([(value,) for value in indices], "H", 5123, "SCALAR", 1, 34963),
        "material": 0}
    mesh_index = len(meshes)
    meshes.append({"name": name, "primitives": [primitive], "extras": identity("mesh", name)})
    nodes.append({"name": name, "mesh": mesh_index, "translation": origin,
                  "extras": identity("node", name)})

image_uri = Path(os.path.relpath(args.base_color.resolve(), args.output.parent.resolve())).as_posix()
images = [{"uri": image_uri}]
textures = [{"source": 0}]
material = {"name": "PlanarDecal", "doubleSided": True, "alphaMode": "BLEND",
            "extras": identity("material", "PlanarDecal"),
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                     "metallicFactor": 0.0, "roughnessFactor": 0.8}}
if args.normal_texture:
    images.append({"uri": Path(os.path.relpath(args.normal_texture.resolve(),
                                               args.output.parent.resolve())).as_posix()})
    textures.append({"source": 1})
    material["normalTexture"] = {"index": 1, "scale": 1.0}
document = {"asset": {"version": "2.0", "generator": "HEngine BuildPlanarDecalGltf"},
            "buffers": [{"uri": args.output.with_suffix(".bin").name, "byteLength": len(binary)}],
            "bufferViews": views, "accessors": accessors,
            "images": images, "textures": textures, "materials": [material],
            "meshes": meshes, "nodes": nodes,
            "scenes": [{"nodes": list(range(len(nodes))), "extras": identity("scene", "world")}],
            "scene": 0}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.with_suffix(".bin").write_bytes(binary)
args.output.write_text(json.dumps(document, separators=(",", ":")) + "\n", encoding="utf-8")
if args.surface_report:
    args.surface_report.parent.mkdir(parents=True, exist_ok=True)
    args.surface_report.write_text(json.dumps({"schema": 1, "decals": surface_report}, indent=2)
                                   + "\n", encoding="utf-8")
print(json.dumps({"output": str(args.output), "decals": len(facts["decals"]),
                  "cells": len(groups),
                  "vertices": sum(len(group) * (args.subdivisions + 1)**2
                                  for group in groups.values()),
                  "groundSampled": len(surface_heights),
                  "seamFallbacks": sum(row["seamFallback"] for row in surface_report),
                  "vertexSampleMisses": vertex_sample_misses,
                  "skippedTriangles": skipped_triangles,
                  "normalTexture": bool(args.normal_texture)}))
