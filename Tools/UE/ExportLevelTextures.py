"""Export textures referenced by an inspected UE level into a separate project.

Environment: HE_UE_MATERIAL_INPUT and HE_UE_TEXTURE_OUTPUT_DIR. The source
project is read-only and the manifest records each result for retry/reimport.
"""

import json
import os
import sys
from pathlib import Path

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
from TextureExportNames import export_filenames


materials = json.loads(Path(os.environ["HE_UE_MATERIAL_INPUT"]).read_text(encoding="utf-8"))
root = Path(os.environ["HE_UE_TEXTURE_OUTPUT_DIR"])
root.mkdir(parents=True, exist_ok=True)
references = set()
used_parameters = {"BaseColor", "Diffuse", "1 Diffuse", "Normal", "1 Normal",
                   "ORM", "EmissiveMask"}


def collect(row):
    for collection in (row.get("textures", {}), row.get("effectiveTextures", {})):
        references.update(value for key, value in collection.items()
                          if key in used_parameters and value and
                          not value.split(".", 1)[0].endswith("/T_LinearBlack"))
    if isinstance(row.get("parent"), dict):
        collect(row["parent"])


for material in materials.values():
    collect(material)
filenames = export_filenames(references)
results = []
for reference in sorted(references):
    package = reference.split(".", 1)[0]
    output = root / filenames[reference]
    row = {"asset": reference, "output": str(output),
           "relativeOutput": output.relative_to(root.parent).as_posix()}
    try:
        if output.is_file() and output.stat().st_size > 0:
            row["ok"] = True
            row["reused"] = True
        else:
            asset = unreal.EditorAssetLibrary.load_asset(package)
            if not isinstance(asset, unreal.Texture):
                raise RuntimeError("UE texture unavailable: " + package)
            task = unreal.AssetExportTask()
            task.object = asset
            task.filename = str(output)
            task.exporter = unreal.TextureExporterPNG()
            task.automated = True
            task.prompt = False
            task.replace_identical = True
            row["ok"] = bool(unreal.Exporter.run_asset_export_task(task))
            row["errors"] = list(task.errors)
        row["bytes"] = output.stat().st_size if output.is_file() else 0
    except Exception as exc:
        row["ok"] = False
        row["error"] = repr(exc)
    results.append(row)
manifest = root.parent / "texture-export.json"
manifest.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("HE_UE_TEXTURE_EXPORT " + json.dumps({
    "count": len(results), "ok": sum(bool(row.get("ok") and row["bytes"]) for row in results),
    "manifest": str(manifest),
}, ensure_ascii=False))
if any(not row.get("ok") or not row.get("bytes") for row in results):
    raise RuntimeError("Some textures failed to export; inspect " + str(manifest))
