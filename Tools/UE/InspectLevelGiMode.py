"""Read-only probe for the UE level's active static/dynamic GI configuration."""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_GI_MODE_REPORT"]).resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise RuntimeError("Report must be outside UE Content")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)

rows = []
world = unreal.EditorLevelLibrary.get_editor_world()
settings = world.get_world_settings()
for name in ("force_no_precomputed_lighting", "precompute_visibility"):
    try:
        rows.append({"object": "WorldSettings", "property": name,
                     "value": str(settings.get_editor_property(name))})
    except Exception as error:
        rows.append({"object": "WorldSettings", "property": name,
                     "error": str(error)[:160]})
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    if not isinstance(actor, unreal.PostProcessVolume):
        continue
    pp = actor.get_editor_property("settings")
    for name in ("dynamic_global_illumination_method",
                 "reflection_method", "indirect_lighting_intensity",
                 "override_dynamic_global_illumination_method",
                 "override_reflection_method"):
        try:
            rows.append({"object": actor.get_name(), "property": name,
                         "value": str(pp.get_editor_property(name))})
        except Exception as error:
            rows.append({"object": actor.get_name(), "property": name,
                         "error": str(error)[:160]})
    for name in ("unbound", "blend_weight", "enabled", "priority"):
        try:
            rows.append({"object": actor.get_name(), "property": name,
                         "value": str(actor.get_editor_property(name))})
        except Exception as error:
            rows.append({"object": actor.get_name(), "property": name,
                         "error": str(error)[:160]})
for variable in ("r.DynamicGlobalIlluminationMethod", "r.ReflectionMethod",
                 "r.AllowStaticLighting"):
    try:
        rows.append({"object": "Console", "property": variable,
                     "value": unreal.SystemLibrary.get_console_variable_int_value(variable)})
    except Exception as error:
        rows.append({"object": "Console", "property": variable,
                     "error": str(error)[:160]})

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
print("HE_UE_GI_MODE " + str(output) + " " + str(len(rows)))
