"""Export a UE world to project-owned glTF without modifying source assets.

Environment: HE_UE_LEVEL, HE_UE_GLTF_OUTPUT. Run with UnrealEditor-Cmd
-run=pythonscript -script=<this file>. Errors and warnings are written beside
the glTF for repeatable inspection.
"""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_GLTF_OUTPUT"])
output.parent.mkdir(parents=True, exist_ok=True)
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load UE level: " + level)
world = unreal.EditorAssetLibrary.load_asset(level)
if not isinstance(world, unreal.World):
    raise RuntimeError("Could not load UE World asset: " + level)

options = unreal.GLTFExportOptions()
options.export_uniform_scale = 0.01
options.export_lights = True
options.export_cameras = True
options.adjust_normalmaps = True
options.bake_material_inputs = unreal.GLTFMaterialBakeMode.USE_MESH_DATA
options.default_material_bake_size = unreal.GLTFMaterialBakeSize(1024, 1024)
messages = unreal.GLTFExporter.export_to_gltf(world, str(output), options, set())
report = {
    "level": level,
    "output": str(output),
    "exists": output.is_file(),
    "bytes": output.stat().st_size if output.is_file() else 0,
    "warnings": list(messages.warnings) if messages else [],
    "errors": list(messages.errors) if messages else ["No export result"],
}
output.with_suffix(".export.json").write_text(
    json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
print("HE_UE_GLTF_EXPORT " + json.dumps({
    "exists": report["exists"], "bytes": report["bytes"],
    "warningCount": len(report["warnings"]), "errorCount": len(report["errors"]),
}, ensure_ascii=False))
if not report["exists"] or report["errors"]:
    raise RuntimeError("UE glTF export failed; inspect " + str(output.with_suffix(".export.json")))
