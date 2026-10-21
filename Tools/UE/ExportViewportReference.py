"""Capture the actual UE editor viewport for HE/UE visual comparison.

Set HE_UE_CAMERA_CONFIG to the same JSON used by ExportCameraReference.py and
run UnrealEditor-Cmd.exe with -ExecutePythonScript (full editor mode). The
camera change and screenshot task are transient; no UE asset is saved.
Verify the output PNG exists after the editor process exits because Unreal's
high-resolution screenshot task completes asynchronously.
"""

import json
import os
from pathlib import Path

import unreal


config_path = os.environ.get("HE_UE_CAMERA_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_CAMERA_CONFIG to an absolute JSON path")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
level = config["level"]
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load " + level)
location = config["locationCm"]
if len(location) != 3:
    raise ValueError("locationCm must contain UE X/Y/Z")
resolution = int(config.get("resolution", 512))
if resolution < 32 or resolution > 2048:
    raise ValueError("resolution must be 32..2048")
fov = float(config.get("horizontalFovDegrees", 90))
if not 1.0 < fov < 179.0:
    raise ValueError("horizontalFovDegrees must be in (1, 179)")
name = config["name"]
if not name or not all(character.isalnum() or character in "_-" for character in name):
    raise ValueError("name must contain only letters, digits, underscore or dash")
output_directory = Path(config["outputDirectory"])
if not output_directory.is_absolute():
    raise ValueError("outputDirectory must be absolute")
output_directory = output_directory.resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output_directory == content or content in output_directory.parents:
    raise ValueError("Do not write reference images into UE Content")
output_directory.mkdir(parents=True, exist_ok=True)
output = output_directory / (name + "-viewport.png")
hide_mesh_prefixes = config.get("hideMeshPrefixes", [])
if not isinstance(hide_mesh_prefixes, list) or any(
        not isinstance(prefix, str) or not prefix.startswith("/Game/")
        for prefix in hide_mesh_prefixes):
    raise ValueError("hideMeshPrefixes must list UE /Game/ mesh path prefixes")
hide_actor_names = config.get("hideActorNames", [])
if not isinstance(hide_actor_names, list) or any(
        not isinstance(actor_name, str) or not actor_name for actor_name in hide_actor_names) or len(hide_actor_names) != len(set(hide_actor_names)):
    raise ValueError("hideActorNames must list distinct nonempty actor names")

camera_location = unreal.Vector(*location)
camera_rotation = unreal.Rotator(pitch=float(config.get("pitchDegrees", 0)),
                               yaw=float(config.get("yawDegrees", 0)), roll=0)
unreal.EditorLevelLibrary.set_level_viewport_camera_info(camera_location, camera_rotation)
camera = unreal.EditorLevelLibrary.spawn_actor_from_class(
    unreal.CameraActor, camera_location, camera_rotation, transient=True)
if camera is None:
    raise RuntimeError("Could not create transient camera")
camera_component = camera.get_editor_property("camera_component")
camera_component.set_editor_property("field_of_view", fov)
camera_component.set_editor_property("aspect_ratio", 1.0)
camera_component.set_editor_property("constrain_aspect_ratio", True)
hidden_mesh_actors = 0
hidden_actor_names = []
if hide_mesh_prefixes or hide_actor_names:
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if not isinstance(actor, unreal.StaticMeshActor):
            continue
        actor_name = actor.get_name()
        by_name = actor_name in hide_actor_names
        for mesh_component in actor.get_components_by_class(unreal.StaticMeshComponent):
            mesh = mesh_component.get_editor_property("static_mesh")
            by_mesh = mesh and any(mesh.get_path_name().startswith(prefix)
                                   for prefix in hide_mesh_prefixes)
            if by_name or by_mesh:
                # This runs in a disposable editor process. Keep visibility off
                # until the asynchronous screenshot has actually completed.
                mesh_component.set_visibility(False)
                hidden_mesh_actors += 1
                if by_name:
                    hidden_actor_names.append(actor_name)
    if hide_mesh_prefixes and not hidden_mesh_actors:
        raise RuntimeError("No static mesh actors match hideMeshPrefixes")
    if set(hidden_actor_names) != set(hide_actor_names):
        raise RuntimeError("Missing static mesh actors: " +
                           ", ".join(sorted(set(hide_actor_names) - set(hidden_actor_names))))
task = unreal.AutomationLibrary.take_high_res_screenshot(
    resolution, resolution, str(output), camera=camera,
    delay=0.1, force_game_view=True)
if not task.is_valid_task():
    raise RuntimeError("UE did not schedule the viewport screenshot")
report = {"level": level, "locationCm": location,
          "pitchDegrees": config.get("pitchDegrees", 0),
          "yawDegrees": config.get("yawDegrees", 0),
          "horizontalFovDegrees": fov, "resolution": resolution,
          "output": str(output),
          "taskDoneAtReturn": task.is_task_done(),
          "hiddenMeshActors": hidden_mesh_actors,
          "hiddenActorNames": sorted(set(hidden_actor_names))}
(output_directory / (name + "-viewport-task.json")).write_text(
    json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("HE_UE_VIEWPORT_TASK " + json.dumps(report))
