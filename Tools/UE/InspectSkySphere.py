"""Inspect a UE BP Sky Sphere's runtime material colors without saving assets.

Set HE_UE_LEVEL and HE_UE_SKY_OUTPUT, then run via UnrealEditor-Cmd with
-run=pythonscript. The output path must be outside UE Content.
"""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)

fields = (
    "zenith_color", "horizon_color", "cloud_color", "overall_color",
    "sun_color", "sun_brightness", "cloud_speed", "cloud_opacity",
    "stars_brightness", "colors_determined_by_sun_position",
    "directional_light_actor", "directional_light_actor_name",
)
rows = []
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    if "Sky_Sphere" not in actor.get_class().get_name():
        continue
    row = {"name": actor.get_name(), "class": actor.get_class().get_name(),
           "properties": {}, "components": []}
    for field in fields:
        try:
            row["properties"][field] = str(actor.get_editor_property(field))
        except Exception:
            pass
    for component in actor.get_components_by_class(unreal.ActorComponent):
        item = {"name": component.get_name(),
                "class": component.get_class().get_name(), "materials": []}
        if hasattr(component, "get_num_materials"):
            for index in range(component.get_num_materials()):
                material = component.get_material(index)
                if material is None:
                    continue
                entry = {"index": index, "name": material.get_path_name(),
                         "class": material.get_class().get_name(),
                         "vectors": {}, "scalars": {}}
                for parameter in ("Horizon color", "Zenith color", "Cloud color",
                                  "Overall Color", "Sun color"):
                    try:
                        entry["vectors"][parameter] = str(
                            material.get_vector_parameter_value(parameter))
                    except Exception:
                        pass
                for parameter in ("Sun brightness", "Cloud opacity", "Stars brightness"):
                    try:
                        entry["scalars"][parameter] = float(
                            material.get_scalar_parameter_value(parameter))
                    except Exception:
                        pass
                item["materials"].append(entry)
        if item["materials"]:
            row["components"].append(item)
    rows.append(row)

if not rows:
    raise RuntimeError("No BP Sky Sphere actor found in " + level)
output = Path(os.environ["HE_UE_SKY_OUTPUT"])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({"level": level, "skyActors": rows}, indent=2) + "\n",
                  encoding="utf-8")
print("HE_UE_SKY " + str(output) + " actors=" + str(len(rows)))
