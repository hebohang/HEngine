"""Export selected UE StaticMesh assets as FBX into an external HE project.

Set HE_UE_MESH_EXPORT_CONFIG to an absolute JSON configuration path. Source
assets are loaded read-only; this script does not save packages or levels.
"""

import json
import os
from pathlib import Path

import unreal


config_path = os.environ.get("HE_UE_MESH_EXPORT_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_MESH_EXPORT_CONFIG")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
output = Path(config["outputDirectory"]).resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if not output.is_absolute() or output == content or content in output.parents:
    raise ValueError("Output must be outside UE Content")
mesh_paths = config["meshes"]
if not isinstance(mesh_paths, list) or not mesh_paths or len(set(mesh_paths)) != len(mesh_paths):
    raise ValueError("meshes must be a unique nonempty list")
output.mkdir(parents=True, exist_ok=True)
results = []
for asset_path in mesh_paths:
    if not isinstance(asset_path, str) or not asset_path.startswith("/Game/"):
        raise ValueError("Invalid StaticMesh path: " + repr(asset_path))
    asset = unreal.EditorAssetLibrary.load_asset(asset_path)
    if not isinstance(asset, unreal.StaticMesh):
        raise TypeError("Expected StaticMesh: " + asset_path)
    target = output / (asset.get_name() + ".fbx")
    task = unreal.AssetExportTask()
    task.object = asset
    task.filename = str(target)
    task.automated = True
    task.prompt = False
    task.replace_identical = True
    success = bool(unreal.Exporter.run_asset_export_task(task))
    row = {"asset": asset_path, "output": str(target), "ok": success,
           "bytes": target.stat().st_size if target.is_file() else 0,
           "errors": list(task.errors)}
    results.append(row)
report = output / "export-report.json"
report.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
if any(not row["ok"] or not row["bytes"] for row in results):
    raise RuntimeError("UE StaticMesh export failed; inspect " + str(report))
print("HE_UE_STATIC_MESH_EXPORT " + str(report))
