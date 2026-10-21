"""Read-only UE actor visibility probe for glTF/lightmap coverage comparisons."""

import json
import os
from pathlib import Path

import unreal


level = os.environ["HE_UE_LEVEL"]
output = Path(os.environ["HE_UE_VISIBILITY_REPORT"]).resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise RuntimeError("Visibility report must be outside UE Content")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)
names = set(filter(None, os.environ.get("HE_UE_ACTOR_NAMES", "").split(",")))
rows = []
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    if names and actor.get_name() not in names:
        continue
    if not actor.get_components_by_class(unreal.StaticMeshComponent):
        continue
    row = {"name": actor.get_name(), "class": actor.get_class().get_name(),
           "visibilityMethods": {}, "properties": {}, "components": []}
    for method_name in ("is_hidden_ed", "is_hidden", "is_editor_only"):
        method = getattr(actor, method_name, None)
        if method:
            try:
                row["visibilityMethods"][method_name] = bool(method())
            except Exception as error:
                row["visibilityMethods"][method_name] = {"error": str(error)[:200]}
    for property_name in ("hidden", "hidden_in_game", "is_editor_only_actor",
                          "is_temporarily_hidden_in_editor", "is_hidden_ed_layer"):
        try:
            row["properties"][property_name] = str(actor.get_editor_property(property_name))
        except Exception as error:
            row["properties"][property_name] = {"error": str(error)[:160]}
    for component in actor.get_components_by_class(unreal.StaticMeshComponent):
        row["components"].append({"name": component.get_name(),
                                  "visible": str(component.get_editor_property("visible")),
                                  "hiddenInGame": str(component.get_editor_property("hidden_in_game"))})
    rows.append(row)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(rows, indent=2) + "\n", encoding="utf-8")
print("HE_UE_ACTOR_VISIBILITY " + str(output) + " " + str(len(rows)))
