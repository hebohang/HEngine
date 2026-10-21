"""Export saved UE level light facts without changing the level or assets.

Set HE_UE_LIGHT_CONFIG to a JSON file with level, output, and optional
actorLabels. Run with UnrealEditor-Cmd -run=pythonscript -script=<this file>.
The output must stay outside the UE Content directory.
"""

import json
import os
from pathlib import Path

import unreal


config_path = os.environ.get("HE_UE_LIGHT_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_LIGHT_CONFIG to an absolute JSON path")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
level = config["level"]
output = Path(config["output"])
labels = config.get("actorLabels", [])
if not isinstance(level, str) or not level.startswith("/Game/"):
    raise ValueError("level must be a /Game/ asset path")
if not output.is_absolute():
    raise ValueError("output must be an absolute path")
output = output.resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise ValueError("Do not write into UE Content")
if not isinstance(labels, list) or any(not isinstance(v, str) or not v for v in labels) or len(set(labels)) != len(labels):
    raise ValueError("actorLabels must list distinct nonempty labels")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)


def property_value(component, name):
    try:
        value = component.get_editor_property(name)
    except Exception:
        return None
    if isinstance(value, (int, float, bool)):
        return value
    if hasattr(value, "name"):
        return str(value.name)
    return str(value)


rows = []
for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    label = actor.get_actor_label()
    if labels and label not in labels:
        continue
    components = list(actor.get_components_by_class(unreal.DirectionalLightComponent) or [])
    components += list(actor.get_components_by_class(unreal.PointLightComponent) or [])
    components += list(actor.get_components_by_class(unreal.SpotLightComponent) or [])
    seen = set()
    for component in components:
        if component.get_path_name() in seen:
            continue
        seen.add(component.get_path_name())
        color = component.get_editor_property("light_color")
        location = actor.get_actor_location()
        row = {"actorLabel": label, "componentClass": component.get_class().get_name(),
               "locationCm": [location.x, location.y, location.z],
               "intensity": property_value(component, "intensity"),
               "intensityUnits": property_value(component, "intensity_units"),
               "lightColorRgb8": [color.r, color.g, color.b],
               "useTemperature": property_value(component, "use_temperature"),
               "temperatureKelvin": property_value(component, "temperature"),
               "attenuationRadiusCm": property_value(component, "attenuation_radius"),
               "innerConeDegrees": property_value(component, "inner_cone_angle"),
               "outerConeDegrees": property_value(component, "outer_cone_angle"),
               "castShadows": property_value(component, "cast_shadows"),
               "visible": property_value(component, "visible")}
        rows.append(row)
if labels and (len(rows) != len(labels) or {row["actorLabel"] for row in rows} != set(labels)):
    raise RuntimeError("Each actorLabels entry must match exactly one light component")
rows.sort(key=lambda row: row["actorLabel"])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({"schema": 1, "level": level, "lights": rows}, indent=2) + "\n", encoding="utf-8")
print("HE_UE_LIGHT_FACTS " + str(output) + " " + str(len(rows)))
