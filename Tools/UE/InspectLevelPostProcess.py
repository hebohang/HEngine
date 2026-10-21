"""Read post-process volumes from a UE level without saving its assets.

Set HE_UE_LEVEL to the level asset path and HE_UE_POSTPROCESS_OUTPUT to a
project-owned JSON path. Run through UnrealEditor-Cmd's pythonscript command.
Only enabled overrides are candidates for an HE scene translation; the saved
values of disabled fields are included to make that distinction inspectable.
"""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_POSTPROCESS_OUTPUT"])
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load UE level: " + level)

fields = (
    "bloom_intensity", "bloom_threshold", "bloom_method",
    "auto_exposure_method", "auto_exposure_bias", "auto_exposure_min_brightness",
    "auto_exposure_max_brightness", "auto_exposure_speed_up", "auto_exposure_speed_down",
    "color_saturation", "color_contrast", "color_gamma", "color_gain", "color_offset",
    "color_saturation_shadows", "color_saturation_midtones", "color_saturation_highlights",
    "film_slope", "film_toe", "film_shoulder", "film_black_clip", "film_white_clip",
    "scene_color_tint", "white_temp", "white_tint", "vignette_intensity",
    "ambient_occlusion_intensity", "ambient_occlusion_radius",
    "motion_blur_amount", "depth_of_field_fstop",
)


def read_property(owner, name):
    try:
        value = owner.get_editor_property(name)
    except Exception as error:
        return {"available": False, "error": str(error)[:180]}
    if isinstance(value, (bool, int, float, str)):
        return {"available": True, "value": value}
    if hasattr(value, "get_path_name"):
        return {"available": True, "value": value.get_path_name()}
    return {"available": True, "value": str(value)[:500]}


rows = []
for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if not isinstance(actor, unreal.PostProcessVolume):
        continue
    settings = actor.get_editor_property("settings")
    values = {}
    for field in fields:
        value = read_property(settings, field)
        override = read_property(settings, "override_" + field)
        values[field] = {"override": override, **value}
    blendables = []
    weighted = read_property(settings, "weighted_blendables")
    if weighted["available"]:
        try:
            for entry in settings.get_editor_property("weighted_blendables").get_editor_property("array"):
                asset = entry.get_editor_property("object")
                blendables.append({"asset": asset.get_path_name() if asset else None,
                                   "weight": entry.get_editor_property("weight")})
        except Exception as error:
            blendables.append({"inspectionError": str(error)[:180]})
    rows.append({
        "label": actor.get_actor_label(),
        "path": actor.get_path_name(),
        "enabled": read_property(actor, "enabled"),
        "unbound": read_property(actor, "unbound"),
        "blendWeight": read_property(actor, "blend_weight"),
        "priority": read_property(actor, "priority"),
        "fields": values,
        "blendables": blendables,
    })
rows.sort(key=lambda row: row["path"])
report = {"level": level, "volumes": rows}
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print("HE_UE_POSTPROCESS " + json.dumps({"level": level, "volumes": len(rows),
                                          "output": str(output)}, ensure_ascii=False))
