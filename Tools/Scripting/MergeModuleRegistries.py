"""Merge build-time generated registries without runtime assembly scanning."""
import json
from pathlib import Path
import sys

manifest_path, registry_path, source_identity, umbrella_identity = sys.argv[1:]
manifest = json.loads(Path(manifest_path).read_text(encoding="utf-8-sig"))
directory = Path(registry_path)
types, ids, factories = [], set(), []
for index, module in enumerate(manifest["modules"]):
    source = directory / str(index)
    metadata = json.loads((source / "GameScripts.metadata.json").read_text(encoding="utf-8-sig"))
    if metadata["identity"] != module["sha256"] or metadata["gameplaySourceIdentity"] != module["identity"]:
        raise ValueError(f"Module registration identity mismatch: {module['id']}")
    for entry in metadata["types"]:
        if entry["id"] in ids:
            raise ValueError(f"Duplicate script type ID across modules: {entry['id']}")
        ids.add(entry["id"])
        entry["moduleId"] = module["id"]
        types.append(entry)
    (directory / f"Module{index}.Registry.g.cs").write_bytes((source / "GameScripts.Registry.g.cs").read_bytes())
    factories.append(f".. Module{index}Registry.Create()")
merged = json.dumps({"version": 1, "identity": umbrella_identity, "gameplaySourceIdentity": source_identity, "types": types,
    "modules": [{key: module[key] for key in ("id", "assembly", "identity", "sha256", "category", "references")} for module in manifest["modules"]]}, separators=(",", ":"))
(directory / "GameScripts.metadata.json").write_text(merged, encoding="utf-8")
(directory / "GameScripts.Registry.g.cs").write_text("// Generated. Do not edit.\nusing HEngine;\nnamespace HEngine.Generated;\npublic static class GameScriptRegistry {\npublic const string Metadata = " + json.dumps(merged) + ";\npublic static ScriptRegistration[] Create() => [" + ",".join(factories) + "];\n}\n", encoding="utf-8")
