"""Inspect UE Niagara assets and their material instance parameters read-only.

Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>. Set
HE_UE_ASSET_PATHS to a JSON array of /Game asset paths and
HE_UE_INSPECTION_OUTPUT to an output JSON path. No UE asset is saved.
"""

import json
import os
from pathlib import Path

import unreal


paths = json.loads(os.environ["HE_UE_ASSET_PATHS"])
if not isinstance(paths, list) or not paths or any(
    not isinstance(path, str) or not path.startswith("/Game/") for path in paths
):
    raise ValueError("HE_UE_ASSET_PATHS must contain /Game asset paths")
output = Path(os.environ["HE_UE_INSPECTION_OUTPUT"]).resolve()
registry = unreal.AssetRegistryHelpers.get_asset_registry()
options = unreal.AssetRegistryDependencyOptions(
    include_hard_package_references=True,
    include_soft_package_references=True,
    include_searchable_names=False,
    include_soft_management_references=False,
    include_hard_management_references=False,
)


def named_parameters(asset, property_name, value):
    return [
        {"name": str(item.parameter_info.name), "value": value(item.parameter_value)}
        for item in asset.get_editor_property(property_name)
    ]


report = {}
for path in paths:
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if asset is None:
        raise RuntimeError("Missing UE asset: " + path)
    record = {
        "class": asset.get_class().get_name(),
        "path": asset.get_path_name(),
        "dependencies": sorted(str(dependency) for dependency in registry.get_dependencies(path, options)),
    }
    if isinstance(asset, unreal.MaterialInstanceConstant):
        parent = asset.get_editor_property("parent")
        record["parent"] = parent.get_path_name() if parent else None
        record["scalarParameters"] = named_parameters(
            asset, "scalar_parameter_values", float)
        record["vectorParameters"] = named_parameters(
            asset, "vector_parameter_values",
            lambda color: [color.r, color.g, color.b, color.a])
        record["textureParameters"] = named_parameters(
            asset, "texture_parameter_values",
            lambda texture: texture.get_path_name() if texture else None)
    report[path] = record

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("HE_UE_NIAGARA_INSPECTION " + str(output))
