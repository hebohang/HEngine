"""Shared project scene contract and explicit portable-backend capability checks."""
import json
from pathlib import Path

# Implemented by Platform/Web/SceneRenderer, not a project-defined claim of support.
RENDER_COMPONENTS = {"TagComponent", "TransformComponent", "MeshComponent", "EditorEntityStateComponent", "SceneFolderComponent"}


def read(project, name):
    project = Path(project).resolve()
    path = (project / name).resolve()
    if not path.is_relative_to(project) or Path(name).is_absolute():
        raise ValueError(f"Project path escapes root: {name}")
    return json.loads(path.read_text(encoding="utf-8"))


def validate(project):
    project = Path(project).resolve()
    contract = read(project, "Config/SharedContent.json")
    if contract.get("schema") != 1:
        raise ValueError("Unsupported SharedContent schema")
    descriptor = read(project, "Project.heproject")
    game_config = descriptor["GameConfig"]
    scene_path = read(project, game_config)["Game"]["DefaultScene"]
    if not scene_path.startswith("Assets/") or not scene_path.endswith(".he"):
        raise ValueError("Shared startup scene must be an Assets-relative .he file")
    # A selected export preset cannot quietly ship a different scene from Editor.
    for profile in read(project, "Config/BuildProfiles.json")["profiles"]:
        selected = profile.get("gameConfig", game_config)
        if profile["platform"] == "Android":
            selected = read(project, profile.get("androidConfig", "Config/Android.json"))["gameConfig"]
        selected_scene = read(project, selected)["Game"]["DefaultScene"]
        if selected_scene != scene_path:
            if profile["platform"] == "Windows" and selected_scene in contract.get("nativeSceneVariants", []):
                read(project, selected_scene)  # Explicit native learning variants must exist.
                continue
            raise ValueError(f"{profile['id']}: startup scene differs from Project.heproject ({scene_path})")
    scene = read(project, scene_path)
    fallbacks = contract.get("portableComponentFallbacks", {})
    if any(not isinstance(reason, str) or not reason.strip() for reason in fallbacks.values()):
        raise ValueError("Every portable component fallback needs an explicit reason")
    adapters = set(contract.get("portableScriptAdapters", []))
    used_fallbacks = set()
    for entity_id, entity in scene.items():
        if not isinstance(entity, dict):
            raise ValueError(f"{scene_path}: invalid entity {entity_id}")
        for component, data in entity.items():
            if component in RENDER_COMPONENTS:
                continue
            if component == "ScriptComponent":
                identity = f"{data.get('Backend', '')}:{data.get('Type', '')}"
                if identity in adapters:
                    continue
                raise ValueError(f"{scene_path}: {entity_id}: script {identity} has no registered portable adapter")
            if component in fallbacks:
                used_fallbacks.add(component)
                continue
            raise ValueError(f"{scene_path}: {entity_id}: {component} is not implemented by Web/WeChat; add backend support or explicitly declare a project fallback")
        visited = {entity_id}
        parent = entity.get("TransformComponent", {}).get("ParentUuid", "")
        while parent:
            if parent not in scene or parent in visited:
                raise ValueError(f"{scene_path}: {entity_id}: missing transform parent or hierarchy cycle")
            visited.add(parent)
            parent = scene[parent].get("TransformComponent", {}).get("ParentUuid", "")
    return {"scenePath": scene_path, "gameConfig": game_config,
            "portableFallbacks": {name: fallbacks[name] for name in sorted(used_fallbacks)}}


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("project", type=Path)
    args = parser.parse_args()
    result = validate(args.project)
    print(f"Shared content validation PASS: {result['scenePath']}")
    for name, reason in result["portableFallbacks"].items():
        print(f"Explicit portable fallback: {name}: {reason}")
