"""Real importer regression: named shape deltas, stable paths, material edits, failed source rollback.
Run with Python 3.10+: --importer <HAssetImporter.exe> --project <root> --output <new project asset folder>.
Outputs are retained for runtime/visual inspection; never removes an existing folder.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def write_fixture(path, amount):
    data = bytearray()
    views, accessors = [], []
    def stream(values, fmt, kind, count, component=5126):
        while len(data) % 4: data.append(0)
        offset = len(data)
        data.extend(struct.pack("<" + fmt * len(values), *values))
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(data) - offset})
        accessors.append({"bufferView": len(views)-1, "componentType": component, "type": kind, "count": count})
        return len(accessors)-1
    positions = stream([-0.5,0,0, 0.5,0,0, 0,1,0], "f", "VEC3", 3)
    normals = stream([0,0,1]*3, "f", "VEC3", 3)
    indices = stream([0,1,2], "H", "SCALAR", 3, 5123)
    target = stream([0,0,0, 0,0,0, 0,amount,0], "f", "VEC3", 3)
    document = {"asset":{"version":"2.0"}, "scene":0, "scenes":[{"nodes":[0]}],
        "nodes":[{"mesh":0,"translation":[2,0,0]}], "meshes":[{"name":"MorphFixture", "weights":[0],
        "extras":{"targetNames":["JawOpen"]}, "primitives":[{"attributes":{"POSITION":positions,"NORMAL":normals},
        "indices":indices,"material":0,"targets":[{"POSITION":target}]}]}],
        "materials":[{"name":"Fixture","pbrMetallicRoughness":{"baseColorFactor":[0.6,0.2,0.1,1]}}],
        "buffers":[{"byteLength":len(data)}], "bufferViews":views,"accessors":accessors}
    encoded = json.dumps(document).encode()
    encoded += b" " * (-len(encoded)%4)
    data += b"\0" * (-len(data)%4)
    path.write_bytes(struct.pack("<III",0x46546c67,2,28+len(encoded)+len(data))+
        struct.pack("<II",len(encoded),0x4e4f534a)+encoded+struct.pack("<II",len(data),0x004e4942)+data)


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--importer",type=Path,required=True)
    parser.add_argument("--project",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args = parser.parse_args()
    project, output = args.project.resolve(), args.output.resolve()
    assert output.is_relative_to(project), "Fixture output must be inside selected project"
    assert not output.exists(), "Choose a new output directory to preserve existing work"
    output.mkdir(parents=True)
    source = output / "Fixture.glb"
    def run(label, expected=0):
        result = subprocess.run([str(args.importer.resolve()),str(project),str(source),str(output)],capture_output=True)
        (output / (label+".log")).write_bytes(result.stdout+result.stderr)
        assert (result.returncode == 0) == (expected == 0), (label,result.returncode)
    write_fixture(source,0.2)
    run("first")
    mesh_path = output / "Fixture.hmesh"
    mesh = json.loads(mesh_path.read_text(encoding="utf-8"))
    assert mesh["primitives"][0]["targetNames"] == ["JawOpen"]
    position = mesh["accessorVec"][mesh["primitives"][0]["attributes"]["POSITION"]]
    view = mesh["bufferViewVec"][position["bufferViewIndex"]]
    binary = (project / mesh["bufferVec"][view["bufferIndex"]]["uri"]).read_bytes()
    x = struct.unpack_from("<f",binary,view["byteOffset"]+position["byteOffset"])[0]
    assert abs(x-1.5)<1e-5, "Static morph import lost node transform"
    initial_names = sorted(path.name for path in output.glob("*.hmesh"))
    material = next(output.glob("*.hmat"))
    content = json.loads(material.read_text(encoding="utf-8"))
    content["roughness"] = 0.271
    material.write_text(json.dumps(content,indent=2),encoding="utf-8")
    material_hash = digest(material)
    buffer_path = project / mesh["bufferVec"][0]["uri"]
    before = digest(buffer_path)
    write_fixture(source,0.4)
    run("second")
    assert sorted(path.name for path in output.glob("*.hmesh")) == initial_names
    assert digest(buffer_path) != before, "Reimport did not update vertex/morph data"
    assert digest(material) == material_hash, "Reimport overwrote artist material edits"
    assert ".he-reimport-" not in mesh_path.read_text(encoding="utf-8")
    stable = {str(path):digest(path) for path in [mesh_path,buffer_path,material]}
    source.write_bytes(b"invalid glb")
    run("invalid",expected=1)
    assert all(digest(Path(path)) == value for path,value in stable.items()), "Failed import changed live assets"
    write_fixture(source,0.4)
    report = {"status":"PASS","checks":["named morph import","stable paths","updated deltas","material preservation","invalid source preserves live assets"],"files":stable}
    (output / "result.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(json.dumps(report))

if __name__ == "__main__": main()
