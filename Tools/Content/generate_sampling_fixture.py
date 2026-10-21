"""Deterministic, dependency-free glTF fixture for material sampling acceptance."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import uuid
import zlib


def identity(label):
    return {"he_schema": 1, "he_id": str(uuid.uuid5(uuid.NAMESPACE_URL, "he-sampling/" + label))}


def write_changed(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def encode(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")


def png(width, height, pixels):
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data) & 0xffffffff)
    scanlines = b"".join(b"\0" + bytes(pixels[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b"")


def generate(output, phase="initial"):
    output = Path(output)
    pixels = []
    for y in range(8):
        for x in range(8):
            pixels.extend((255 if x % 2 else 32, y * 32, 255 if (x + y) % 2 else 0, 255 if x < 4 else 80))
    write_changed(output / "grid.png", png(8, 8, pixels))
    write_changed(output / "normal.png", png(2, 2, [180,128,235,255] * 4))
    specs = [
        ("uv0", 0, 10497, 10497, 9987, 9729),
        ("uv1", 1, 10497, 10497, 9987, 9729),
        ("vertex_color", 0, 10497, 10497, 9987, 9729),
        ("transform", 1, 10497, 10497, 9987, 9729),
        ("nearest_repeat", 0, 10497, 10497, 9728, 9728),
        ("linear_clamp", 0, 33071, 33071, 9729, 9729),
        ("mirror_u_clamp_v", 0, 33648, 33071, 9984, 9728),
        ("clamp_u_mirror_v", 1, 33071, 33648, 9985, 9729),
        ("point_trilinear", 0, 10497, 33648, 9986, 9728),
        ("linear_trilinear", 1, 33648, 10497, 9987, 9729),
        ("alpha_mask", 1, 33648, 33071, 9728, 9728),
        ("normal_uv1", 1, 10497, 10497, 9987, 9729),
    ]
    if phase == "add":
        specs.append(("added", 1, 33071, 33071, 9728, 9728))
    if phase == "delete":
        specs = specs[:-1]
    document = dict(asset={"version": "2.0", "generator": "HEngine sampling fixture/1"},
        extensionsUsed=["KHR_texture_transform"], scene=0,
        scenes=[dict(nodes=[], extras=identity("scene"))], nodes=[], meshes=[], materials=[],
        images=[{"uri": "grid.png"}, {"uri": "normal.png"}], textures=[], samplers=[], accessors=[], bufferViews=[], buffers=[])
    binary = bytearray()

    def accessor(values, components, kind="FLOAT"):
        while len(binary) % 4:
            binary.append(0)
        start = len(binary)
        flat = [number for vertex in values for number in vertex]
        binary.extend(struct.pack("<" + ("f" if kind == "FLOAT" else "H") * len(flat), *flat))
        view = len(document["bufferViews"])
        document["bufferViews"].append(dict(buffer=0, byteOffset=start, byteLength=len(binary) - start))
        result = dict(bufferView=view, componentType=5126 if kind == "FLOAT" else 5123,
            type="SCALAR" if components == 1 else "VEC" + str(components), count=len(values))
        if components == 3:
            result["min"] = [min(v[n] for v in values) for n in range(3)]
            result["max"] = [max(v[n] for v in values) for n in range(3)]
        document["accessors"].append(result)
        return len(document["accessors"]) - 1

    for index, (label, uv, wrap_s, wrap_t, minimum, maximum) in enumerate(specs):
        document["samplers"].append(dict(wrapS=wrap_s, wrapT=wrap_t, minFilter=minimum, magFilter=maximum))
        document["textures"].append(dict(source=0, sampler=index))
        texture_index = len(document["textures"]) - 1
        tex = dict(index=texture_index, texCoord=uv)
        if label in {"transform", "normal_uv1"}:
            tex["extensions"] = {"KHR_texture_transform": dict(offset=[-.2, .15], scale=[1.4, .65], rotation=.6, texCoord=1)}
        factor = [.5, 1, .4, 1] if phase == "material" else [1, 1, 1, 1]
        material = dict(name=label, extras=identity("material/" + label), doubleSided=True,
            pbrMetallicRoughness=dict(baseColorTexture=tex, baseColorFactor=factor, metallicFactor=.1, roughnessFactor=.7))
        if label == "alpha_mask":
            material.update(alphaMode="MASK", alphaCutoff=.5)
        if label == "normal_uv1":
            document["textures"].append(dict(source=1, sampler=index))
            material["normalTexture"] = dict(index=len(document["textures"]) - 1, texCoord=1, scale=.4)
            material["occlusionTexture"] = dict(index=texture_index, texCoord=1, strength=.25)
        document["materials"].append(material)
        width = 1.4 if phase == "geometry" else 1.0
        position = accessor([[0,0,0], [width,0,0], [width,1,0], [0,1,0]], 3)
        normal = accessor([[0,0,1]] * 4, 3)
        tangent = accessor([[1,0,0,1]] * 4, 4)
        uv0 = accessor([[-.25,-.25], [1.25,-.25], [1.25,1.25], [-.25,1.25]], 2)
        uv1 = accessor([[0,0], [.45,0], [.45,.75], [0,.75]], 2)
        colors = [[1,.3,.2,1], [.2,1,.3,1], [.3,.2,1,.5], [1,1,.2,1]] if label == "vertex_color" else [[1,1,1,1]] * 4
        color = accessor(colors, 4)
        indices = accessor([[0],[1],[2],[0],[2],[3]], 1, "USHORT")
        document["meshes"].append(dict(name=label, extras=identity("mesh/" + label), primitives=[dict(
            attributes=dict(POSITION=position, NORMAL=normal, TANGENT=tangent, TEXCOORD_0=uv0, TEXCOORD_1=uv1, COLOR_0=color), indices=indices, material=index)]))
        document["nodes"].append(dict(name=label + ("_renamed" if phase == "rename" else ""), mesh=index,
            translation=[(index % 4) * 1.4, (index // 4) * 1.4, 0], extras=identity("node/" + label)))
        document["scenes"][0]["nodes"].append(index)
    document["buffers"] = [dict(byteLength=len(binary), uri="sampling.bin")]
    write_changed(output / "sampling.bin", binary)
    write_changed(output / "sampling.gltf", encode(document))
    manifest = dict(schema=1, phase=phase, units="metres", camera=dict(position=[2.5,1.9,8], target=[2.5,1.9,0]),
        features=[item[0] for item in specs], sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest()
            for name in ["sampling.gltf", "sampling.bin", "grid.png", "normal.png"]})
    write_changed(output / "manifest.json", encode(manifest))
    return document


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--phase", choices=["initial", "material", "geometry", "rename", "add", "delete"], default="initial")
    args = parser.parse_args()
    generate(args.output, args.phase)
    print(json.dumps({"ok": True, "source": str(args.output / "sampling.gltf"), "phase": args.phase}))
