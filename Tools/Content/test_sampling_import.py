"""Current HAssetImporter + existing content merge contract, without a GPU."""
import argparse
import copy
import json
from pathlib import Path
import subprocess
import tempfile

import content_sync
from generate_sampling_fixture import generate, write_changed, encode


def verify(importer, project, report_path):
    checks = []
    with tempfile.TemporaryDirectory(prefix="he-sampling-", dir=project / ".he-content") as directory:
        root = Path(directory)

        def convert(phase):
            source = root / "source"
            generate(source, phase)
            stage = root / phase
            stage.mkdir(exist_ok=True)
            result = subprocess.run([str(importer), str(project), str(source / "sampling.gltf"), str(stage), "--scene"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            (stage / "import.log").write_bytes(result.stdout)
            output = json.loads((stage / "conversion.json").read_text(encoding="utf-8"))
            if result.returncode or not output["ok"]:
                raise AssertionError(output.get("diagnostics", []))
            return output, stage

        original, stage = convert("initial")
        materials = [json.loads(p.read_text(encoding="utf-8")) for p in stage.glob("material-*.hmat")]
        sampling = [m["Textures"]["g_AlbedoMap"]["Sampling"] for m in materials]
        assert {s["minFilter"] for s in sampling} == {9728,9729,9984,9985,9986,9987}
        assert any(s["texCoord"] == 1 for s in sampling)
        assert any(abs(s["rotation"] - .6) < 1e-5 for s in sampling)
        for path in stage.glob("mesh-*.hmesh"):
            mesh = json.loads(path.read_text(encoding="utf-8"))
            for primitive in mesh["primitives"]:
                assert {"TEXCOORD_1", "COLOR_0"}.issubset(primitive["attributes"])
        checks.append("native UV1/color/transform/all minFilter values serialized")

        previous = {key: value["entity"] for key, value in original["nodes"].items()}
        slots = {key: value["slots"] for key, value in original["nodes"].items()}
        current = copy.deepcopy(previous)
        owner = next(key for key, value in original["nodes"].items() if value["name"] == "uv0")
        current[owner]["PythonScriptComponent"] = {"Path": "Scripts/SamplingOwner.py"}
        current[owner]["MeshComponent"]["MaterialOverrides"] = ["Assets/Materials/EngineOverride.hmat"]
        for phase in ["initial", "material", "geometry", "rename", "add", "delete"]:
            output, unused = convert(phase)
            incoming = {key: value["entity"] for key, value in output["nodes"].items()}
            incoming_slots = {key: value["slots"] for key, value in output["nodes"].items()}
            merged, delta = content_sync.merge_scene(previous, current, incoming, slots, incoming_slots)
            assert merged[owner]["PythonScriptComponent"] == current[owner]["PythonScriptComponent"]
            assert merged[owner]["MeshComponent"]["MaterialOverrides"] == current[owner]["MeshComponent"]["MaterialOverrides"]
            checks.append("merge " + phase + " preserves script and instance material override")

        # A rejected source cannot mutate the last accepted stage or engine scene.
        before = {p.name: p.read_bytes() for p in stage.iterdir() if p.is_file()}
        bad = generate(root / "source")
        bad["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["texCoord"] = 3
        write_changed(root / "source/sampling.gltf", encode(bad))
        rejected = root / "rejected"
        result = subprocess.run([str(importer), str(project), str(root / "source/sampling.gltf"), str(rejected), "--scene"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        assert result.returncode != 0
        assert before == {p.name: p.read_bytes() for p in stage.iterdir() if p.is_file()}
        checks.append("unsupported UV fails without changing accepted resources")
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps({"ok": True, "checks": checks, "importer": str(importer)}, indent=2), encoding="utf-8")
    print(json.dumps({"ok": True, "checks": len(checks), "report": str(report_path)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--importer", required=True, type=Path)
    parser.add_argument("--project", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    (args.project / ".he-content").mkdir(parents=True, exist_ok=True)
    verify(args.importer.resolve(), args.project.resolve(), args.report.resolve())
