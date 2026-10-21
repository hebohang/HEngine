"""Pack HELightmapBridge BGRA8 mip chains as linear BC7 DDS assets.

Only derived assets are written. UE source uassets and exported raw mips remain
untouched. The output manifest maps UE texture paths to project-relative DDSs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path


def dds_bgra8(texture: dict) -> bytes:
    mips = texture["rawMips"]
    width, height = texture["sourceWidth"], texture["sourceHeight"]
    if texture["basis"] != 0 or texture["sourceFormat"] != 2 or not mips:
        raise ValueError("Expected an HQ TSF_BGRA8 lightmap with exported mips")
    payloads = []
    for index, mip in enumerate(mips):
        expected_width, expected_height = max(1, width >> index), max(1, height >> index)
        if (mip["mip"], mip["width"], mip["height"]) != (
                index, expected_width, expected_height):
            raise ValueError("Missing or mismatched UE source mip")
        data = Path(mip["file"]).read_bytes()
        if len(data) != expected_width * expected_height * 4:
            raise ValueError("Invalid BGRA8 mip length")
        payloads.append(data)
    flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x20000
    caps = 0x1000 | 0x8 | 0x400000
    header = struct.pack("<7I", 124, flags, height, width, width * 4, 0, len(mips))
    header += bytes(11 * 4)
    header += struct.pack("<8I", 32, 0x41, 0, 32,
                          0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    header += struct.pack("<5I", caps, 0, 0, 0, 0)
    if len(header) != 124:
        raise AssertionError("DDS header must be 124 bytes")
    return b"DDS " + header + b"".join(payloads)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", type=Path)
    parser.add_argument("project", type=Path)
    parser.add_argument("output_dir", type=Path, help="Project-relative DDS directory")
    parser.add_argument("texconv", type=Path)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    project = args.project.resolve()
    destination = (project / args.output_dir).resolve()
    if destination == project or project not in destination.parents:
        raise ValueError("DDS output must stay inside the HE project")
    if not args.texconv.is_file():
        raise FileNotFoundError(args.texconv)
    report = json.loads(args.report.read_text(encoding="utf-8"))
    textures = {item["path"]: item for binding in report["bindings"]
                for item in binding["textures"] if item["basis"] == 0}
    if not textures:
        raise ValueError("No HQ textures in UE bridge report")
    destination.mkdir(parents=True, exist_ok=True)
    packed = {}
    selected = sorted(textures.items())[:args.limit or None]
    for ue_path, texture in selected:
        stem = Path(texture["rawMips"][0]["file"]).stem
        dds_path = destination / (stem + ".dds")
        with tempfile.TemporaryDirectory(prefix="he-lightmap-dds-", dir=project / "Saved") as temp:
            source = Path(temp) / (stem + ".dds")
            source.write_bytes(dds_bgra8(texture))
            result = subprocess.run(
                [str(args.texconv), "-nologo", "-y", "-singleproc", "-f", "BC7_UNORM",
                 "-m", "0", "-dx10", "-sepalpha", "-o", str(destination), str(source)],
                capture_output=True, text=True, check=False)
            if result.returncode or not dds_path.is_file():
                raise RuntimeError("texconv failed for " + ue_path + ": " + result.stdout[-1200:] + result.stderr[-1200:])
        data = dds_path.read_bytes()
        if (data[:4] != b"DDS " or len(data) < 148 or
                struct.unpack_from("<I", data, 28)[0] != len(texture["rawMips"]) or
                struct.unpack_from("<I", data, 128)[0] != 98):
            raise ValueError("BC7 DDS missing its original UE mip count: " + str(dds_path))
        packed[ue_path] = {
            "assetPath": dds_path.relative_to(project).as_posix(),
            "width": texture["sourceWidth"], "height": texture["sourceHeight"],
            "mips": len(texture["rawMips"]), "sha256": hashlib.sha256(data).hexdigest(),
        }
        if len(packed) % 20 == 0:
            print(json.dumps({"packed": len(packed), "total": len(selected)}), flush=True)
    manifest = destination / "HQLightmapTextures.json"
    manifest.write_text(json.dumps({"schema": "he.ue.lightmap-textures.v1",
                                    "textures": packed}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"packed": len(packed), "manifest": str(manifest)}))


if __name__ == "__main__":
    main()
