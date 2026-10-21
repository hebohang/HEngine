"""Read saved UE DecalActor placement and material paths into a JSON sidecar.

Set HE_UE_DECAL_CONFIG to a JSON file with level, output, and optional
materialPathPrefixes. Run via UnrealEditor-Cmd -run=pythonscript. No UE asset
or level is written.
"""

import json
import os
from pathlib import Path

import unreal


config_path = os.environ.get("HE_UE_DECAL_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_DECAL_CONFIG to an absolute JSON path")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
level = config["level"]
output = Path(config["output"])
prefixes = config.get("materialPathPrefixes", [])
if not isinstance(level, str) or not level.startswith("/Game/"):
    raise ValueError("level must be a /Game/ asset path")
if not output.is_absolute():
    raise ValueError("output must be absolute")
output = output.resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise ValueError("Do not write into UE Content")
if not isinstance(prefixes, list) or any(not isinstance(v, str) or not v.startswith("/Game/") for v in prefixes):
    raise ValueError("materialPathPrefixes must list /Game/ paths")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)


def xyz(vector):
    return [vector.x, vector.y, vector.z]


rows = []
for actor in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors():
    if not isinstance(actor, unreal.DecalActor):
        continue
    components = actor.get_components_by_class(unreal.DecalComponent) or []
    if len(components) != 1:
        raise RuntimeError("Expected exactly one decal component: " + actor.get_actor_label())
    decal = components[0]
    material = decal.get_editor_property("decal_material")
    material_path = material.get_path_name() if material else None
    if not material_path or (prefixes and not any(material_path.startswith(v) for v in prefixes)):
        continue
    location = actor.get_actor_location()
    rotation = actor.get_actor_rotation()
    right = unreal.MathLibrary.get_right_vector(rotation)
    up = unreal.MathLibrary.get_up_vector(rotation)
    forward = unreal.MathLibrary.get_forward_vector(rotation)
    size = decal.get_editor_property("decal_size")
    rows.append({"actorLabel": actor.get_actor_label(), "actorName": actor.get_name(),
                 "locationCm": xyz(location),
                 "rotationDegrees": [rotation.pitch, rotation.yaw, rotation.roll],
                 "right": xyz(right), "up": xyz(up), "forward": xyz(forward),
                 "decalSizeCm": xyz(size), "material": material_path,
                 "sortOrder": decal.get_editor_property("sort_order"),
                 "visible": decal.get_editor_property("visible")})
rows.sort(key=lambda row: row["actorName"])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({"schema": 1, "level": level, "decals": rows}, indent=2) + "\n", encoding="utf-8")
print("HE_UE_DECALS " + str(output) + " " + str(len(rows)))
