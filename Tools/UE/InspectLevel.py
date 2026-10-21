"""Read an Unreal level into a portable actor/asset inventory without saving it.

Set HE_UE_LEVEL and HE_UE_INSPECTION_OUTPUT, then run through UnrealEditor-Cmd
with -run=pythonscript -script=<this file>. The output stays outside UE Content.
"""

import collections
import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_INSPECTION_OUTPUT"])
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load UE level: " + level)

rows = []
classes = collections.Counter()
meshes = collections.Counter()
materials = collections.Counter()
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    class_name = actor.get_class().get_name()
    classes[class_name] += 1
    location = actor.get_actor_location()
    rotation = actor.get_actor_rotation()
    row = {
        "label": actor.get_actor_label(),
        "name": actor.get_name(),
        "class": class_name,
        "location": [location.x, location.y, location.z],
        "rotation": [rotation.roll, rotation.pitch, rotation.yaw],
        "components": [],
    }
    for component in actor.get_components_by_class(unreal.StaticMeshComponent):
        asset = component.get_editor_property("static_mesh")
        asset_path = asset.get_path_name() if asset else ""
        if asset_path:
            meshes[asset_path] += 1
        component_row = {
            "class": component.get_class().get_name(),
            "mesh": asset_path,
            "materials": [],
        }
        for index in range(component.get_num_materials()):
            material = component.get_material(index)
            path = material.get_path_name() if material else ""
            component_row["materials"].append(path)
            if path:
                materials[path] += 1
        if isinstance(component, unreal.InstancedStaticMeshComponent):
            component_row["instanceCount"] = component.get_instance_count()
        row["components"].append(component_row)
    rows.append(row)

document = {
    "level": level,
    "summary": {
        "actors": len(rows),
        "classes": dict(classes.most_common()),
        "uniqueStaticMeshes": len(meshes),
        "uniqueMaterials": len(materials),
        "topMeshes": meshes.most_common(30),
        "topMaterials": materials.most_common(30),
    },
    "actors": rows,
}
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print("HE_UE_LEVEL_INSPECTION " + json.dumps(document["summary"], ensure_ascii=False))
