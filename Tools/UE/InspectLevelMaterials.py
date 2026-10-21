"""Inventory UE material instance parameter chains used by a level.

Set HE_UE_INSPECTION_INPUT and HE_UE_MATERIAL_OUTPUT. This only reads UE
assets; the resulting JSON belongs outside the UE project.
"""

import json
import os
from pathlib import Path

import unreal


source = json.loads(Path(os.environ["HE_UE_INSPECTION_INPUT"]).read_text(encoding="utf-8"))
output = Path(os.environ["HE_UE_MATERIAL_OUTPUT"])


def parameter_name(item):
    return str(item.get_editor_property("parameter_info").get_editor_property("name"))


def inspect(material):
    row = {"asset": material.get_path_name(), "class": material.get_class().get_name()}
    if isinstance(material, unreal.MaterialInstance):
        row["textures"] = {
            parameter_name(item): item.get_editor_property("parameter_value").get_path_name()
            if item.get_editor_property("parameter_value") else ""
            for item in material.get_editor_property("texture_parameter_values")
        }
        row["effectiveTextures"] = {}
        for name in unreal.MaterialEditingLibrary.get_texture_parameter_names(material):
            value = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(
                material, name)
            if value:
                row["effectiveTextures"][str(name)] = value.get_path_name()
        row["scalars"] = {
            parameter_name(item): item.get_editor_property("parameter_value")
            for item in material.get_editor_property("scalar_parameter_values")
        }
        row["vectors"] = {
            parameter_name(item): [getattr(item.get_editor_property("parameter_value"), c)
                                   for c in ("r", "g", "b", "a")]
            for item in material.get_editor_property("vector_parameter_values")
        }
        parent = material.get_editor_property("parent")
        row["parent"] = inspect(parent) if parent else None
    return row


names = set()
for actor in source["actors"]:
    for component in actor["components"]:
        names.update(filter(None, component["materials"]))
materials = {}
for path in sorted(names):
    asset = unreal.EditorAssetLibrary.load_asset(path.split(".", 1)[0])
    materials[path] = inspect(asset) if asset else {"asset": path, "error": "unavailable"}
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(materials, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("HE_UE_MATERIAL_INVENTORY " + json.dumps({
    "materials": len(materials),
    "unavailable": [path for path, row in materials.items() if "error" in row],
}, ensure_ascii=False))
