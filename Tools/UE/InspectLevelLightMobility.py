"""Read-only UE level light mobility probe; writes only outside UE Content."""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_LIGHT_MOBILITY_REPORT"]).resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise RuntimeError("Report must be outside UE Content")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)

rows = []
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    for component in actor.get_components_by_class(unreal.LightComponentBase):
        mobility = component.get_editor_property("mobility")
        rows.append({"actor": actor.get_name(),
                     "label": actor.get_actor_label(),
                     "class": actor.get_class().get_name(),
                     "component": component.get_name(),
                     "mobility": str(mobility)})

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
print("HE_UE_LIGHT_MOBILITY " + str(output) + " " + str(len(rows)))
