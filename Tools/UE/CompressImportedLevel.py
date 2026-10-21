"""Create mipmapped BC7 texture/material copies for an imported HE level.

Usage: python Tools/UE/CompressImportedLevel.py <project> <imported-scene>
<material-output-dir> <texconv.exe> [--texture-policy <json>]. The optional
policy maps project-relative source texture paths to preserveAlphaCoverage.
Immutable content-sync resources and UE
source files are never modified. The mapping manifest can be applied by a
project scene composer and verified on rerun.
"""

import argparse
import hashlib
import json
import math
import struct
import subprocess
from pathlib import Path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("project", type=Path)
parser.add_argument("scene_path", type=Path)
parser.add_argument("output_dir", type=Path)
parser.add_argument("texconv", type=Path)
parser.add_argument("--texture-policy", type=Path)
args = parser.parse_args()
project, scene_path, output_dir, texconv = (
    args.project, args.scene_path, args.output_dir, args.texconv)
policy = {}
if args.texture_policy is not None:
    policy_document = json.loads(args.texture_policy.read_text(encoding="utf-8"))
    if policy_document.get("schema") != 1 or not isinstance(policy_document.get("textures"), dict):
        raise ValueError("texture policy must have schema 1 and a textures object")
    for path, settings in policy_document["textures"].items():
        if not isinstance(path, str) or not path.startswith("Assets/") or ".." in Path(path).parts:
            raise ValueError("texture policy path must be project-relative: " + str(path))
        if not isinstance(settings, dict) or type(settings.get("preserveAlphaCoverage")) is not bool:
            raise ValueError("texture policy requires preserveAlphaCoverage boolean: " + path)
        policy[path] = settings["preserveAlphaCoverage"]
scene = json.loads(scene_path.read_text(encoding="utf-8"))
output_dir.mkdir(parents=True, exist_ok=True)
texture_dir = output_dir / "Compressed"
texture_dir.mkdir(exist_ok=True)
manifest_path = output_dir / "compressed-manifest.json"
previous = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file() else {}
previous_by_output = {row["output"]: row for row in previous.get("textures", [])}


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_dds(path, srgb):
    with path.open("rb") as source:
        header = source.read(148)
    if len(header) != 148 or header[:4] != b"DDS " or header[84:88] != b"DX10":
        raise ValueError("Invalid DDS header: " + str(path))
    height, width = struct.unpack_from("<II", header, 12)
    mips = struct.unpack_from("<I", header, 28)[0]
    fmt = struct.unpack_from("<I", header, 128)[0]
    expected = 148
    w, h = width, height
    for _ in range(mips):
        expected += ((w + 3) // 4) * ((h + 3) // 4) * 16
        w, h = max(1, w // 2), max(1, h // 2)
    if not width or not height or mips != 1 + int(math.log2(max(width, height))) \
            or path.stat().st_size != expected or fmt != (99 if srgb else 98):
        raise ValueError("Invalid BC7 format or mip payload: " + str(path))
    return {"width": width, "height": height, "mips": mips, "bytes": expected}


paths = sorted({reference for entity in scene.values()
                for reference in entity.get("MeshComponent", {}).get("MaterialOverrides", [])})
materials = {}
jobs = {}
used_policy = set()
for reference in paths:
    source_path = project / reference
    material = json.loads(source_path.read_text(encoding="utf-8"))
    destination = output_dir / (source_path.stem + ".hmat")
    materials[reference] = (destination, material)
    for field, binding in material.get("Textures", {}).items():
        texture_reference = binding.get("Path", "")
        if not texture_reference.startswith("Assets/"):
            continue
        srgb = binding.get("ColorSpace") == "SRGB"
        cutoff = material.get("Parameters", {}).get("g_AlphaCutoff", {}).get("Value", 0.5) \
            if field == "g_AlbedoMap" and material.get("BlendMode") == "Mask" else None
        preserve_coverage = policy.get(texture_reference, True) if cutoff is not None else True
        if texture_reference in policy and cutoff is not None:
            used_policy.add(texture_reference)
        jobs.setdefault((texture_reference, srgb, cutoff, preserve_coverage), []).append((reference, field))
if used_policy != set(policy):
    raise ValueError("texture policy references unused texture(s): " + str(sorted(set(policy) - used_policy)))

records = []
for index, ((reference, srgb, cutoff, preserve_coverage), users) in enumerate(sorted(jobs.items()), 1):
    source_path = project / reference
    source_hash = digest(source_path)
    suffix = "_" + hashlib.sha256((reference + str(srgb) + str(cutoff)).encode()).hexdigest()[:10]
    if cutoff is not None and not preserve_coverage:
        suffix += "_sourcealpha"
    destination = texture_dir / (source_path.stem + suffix + ".dds")
    relative = destination.relative_to(project).as_posix()
    old = previous_by_output.get(relative)
    reusable = bool(old and old.get("sourceSha256") == source_hash and destination.is_file()
                    and old.get("outputSha256") == digest(destination))
    if not reusable:
        command = [str(texconv), "-nologo", "-y", "-singleproc", "-f",
                   "BC7_UNORM_SRGB" if srgb else "BC7_UNORM", "-m", "0",
                   "-dx10", "-nowic", "-sepalpha", "-if", "BOX", "-sx", suffix,
                   "-o", str(texture_dir), "-gpu", "0"]
        command += ["-srgb"] if srgb else ["--ignore-srgb"]
        if cutoff is not None and preserve_coverage:
            command += ["--keep-coverage", str(cutoff)]
        result = subprocess.run(command + [str(source_path)], capture_output=True,
                                text=True, timeout=600)
        if result.returncode or not destination.is_file():
            raise RuntimeError("texconv failed for " + str(source_path) + ": " + result.stdout[-1000:])
    info = verify_dds(destination, srgb)
    for material_reference, field in users:
        material = materials[material_reference][1]
        material["Textures"][field]["Path"] = relative
        exposed = material.get("Properties", {}).get("ExposedProperties", {})
        if field in exposed:
            exposed[field] = relative
    records.append({"source": reference, "sourceSha256": source_hash,
                    "output": relative, "outputSha256": digest(destination),
                    "srgb": srgb, "cutoff": cutoff,
                    "preserveAlphaCoverage": preserve_coverage if cutoff is not None else None,
                    **info})
    print(f"{index}/{len(jobs)} {destination.name}", flush=True)

mapping = {}
for reference, (destination, material) in materials.items():
    destination.write_text(json.dumps(material, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    mapping[reference] = destination.relative_to(project).as_posix()
manifest = {"schema": 1, "sourceScene": scene_path.relative_to(project).as_posix(),
            "sourceSceneSha256": digest(scene_path), "materials": mapping, "textures": records}
manifest_path.write_text(
    json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps({"materials": len(mapping), "textures": len(records),
                  "bytes": sum(row["bytes"] for row in records)}, ensure_ascii=False))
