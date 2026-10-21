"""Capture an LDR UE reference image from a fixed camera for HE visual QA.

Set HE_UE_CAMERA_CONFIG to a JSON file outside UE Content. Run with the full
editor -ExecutePythonScript mode so level primitives are registered. No UE
level, render target or source asset is saved.
"""

import json
import os
import time
from pathlib import Path

import unreal


config_path = os.environ.get("HE_UE_CAMERA_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_CAMERA_CONFIG to an absolute JSON path")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
level = config["level"]
loaded = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level)
if not loaded:
    raise RuntimeError("Could not load " + level)
world = unreal.EditorAssetLibrary.load_asset(level)
if world is None:
    raise RuntimeError("Could not resolve " + level)

location = config["locationCm"]
if len(location) != 3:
    raise ValueError("locationCm must contain UE X/Y/Z")
resolution = int(config.get("resolution", 512))
if resolution < 32 or resolution > 2048:
    raise ValueError("resolution must be 32..2048")
fov = float(config.get("horizontalFovDegrees", 90))
if not 1.0 < fov < 179.0:
    raise ValueError("horizontalFovDegrees must be in (1, 179)")
capture_source_name = config.get("captureSource", "SCS_FINAL_COLOR_LDR")
if capture_source_name not in ("SCS_FINAL_COLOR_LDR", "SCS_BASE_COLOR",
                               "SCS_SCENE_DEPTH"):
    raise ValueError("Unsupported captureSource")
capture_source = getattr(unreal.SceneCaptureSource, capture_source_name)
warmup_captures = int(config.get("warmupCaptures", 0))
if not 0 <= warmup_captures <= 32:
    raise ValueError("warmupCaptures must be 0..32")
warmup_delay = float(config.get("warmupDelaySeconds", 0.1))
if not 0.0 <= warmup_delay <= 1.0:
    raise ValueError("warmupDelaySeconds must be 0..1")
output = Path(config["outputDirectory"])
if not output.is_absolute():
    raise ValueError("outputDirectory must be absolute")
output = output.resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise ValueError("Do not write reference images into UE Content")
output.mkdir(parents=True, exist_ok=True)
name = config["name"]
if not name or not all(character.isalnum() or character in "_-" for character in name):
    raise ValueError("name must contain only letters, digits, underscore or dash")
if "forceMeshLodPrefixes" in config and "forceMeshLod0Prefixes" in config:
    raise ValueError("Use one forced mesh LOD prefix field")
force_lod_prefixes = config.get("forceMeshLodPrefixes",
                                config.get("forceMeshLod0Prefixes", []))
if not isinstance(force_lod_prefixes, list) or any(
        not isinstance(prefix, str) or not prefix.startswith("/Game/")
        for prefix in force_lod_prefixes):
    raise ValueError("forceMeshLodPrefixes must list UE /Game/ mesh path prefixes")
forced_lod_index = config.get("forceMeshLodIndex", 0)
if not isinstance(forced_lod_index, int) or not 0 <= forced_lod_index <= 7:
    raise ValueError("forceMeshLodIndex must be 0..7")
if forced_lod_index and not force_lod_prefixes:
    raise ValueError("forceMeshLodIndex requires forceMeshLodPrefixes")
hide_mesh_prefixes = config.get("hideMeshPrefixes", [])
if not isinstance(hide_mesh_prefixes, list) or any(
        not isinstance(prefix, str) or not prefix.startswith("/Game/")
        for prefix in hide_mesh_prefixes):
    raise ValueError("hideMeshPrefixes must list UE /Game/ mesh path prefixes")
hide_niagara_prefixes = config.get("hideNiagaraAssetPrefixes", [])
if not isinstance(hide_niagara_prefixes, list) or any(
        not isinstance(prefix, str) or not prefix.startswith("/Game/")
        for prefix in hide_niagara_prefixes):
    raise ValueError("hideNiagaraAssetPrefixes must list UE /Game/ Niagara asset path prefixes")
hide_actor_labels = config.get("hideActorLabels", [])
if not isinstance(hide_actor_labels, list) or any(
        not isinstance(label, str) or not label for label in hide_actor_labels) or len(set(hide_actor_labels)) != len(hide_actor_labels):
    raise ValueError("hideActorLabels must list distinct nonempty actor labels")

target = unreal.RenderingLibrary.create_render_target2d(
    world, resolution, resolution,
    unreal.TextureRenderTargetFormat.RTF_RGBA16F
    if capture_source_name == "SCS_SCENE_DEPTH"
    else unreal.TextureRenderTargetFormat.RTF_RGBA8)
actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
    unreal.SceneCapture2D, unreal.Vector(*location),
    unreal.Rotator(pitch=float(config.get("pitchDegrees", 0)),
                   yaw=float(config.get("yawDegrees", 0)), roll=0), transient=True)
if actor is None:
    raise RuntimeError("Could not spawn SceneCapture2D")
forced_lod_components = []
hidden_actor_count = 0
hidden_niagara_count = 0
hidden_label_count = 0
hidden_niagara_components = []
try:
    components = actor.get_components_by_class(unreal.SceneCaptureComponent2D)
    if len(components) != 1:
        raise RuntimeError("Expected one capture component")
    component = components[0]
    if force_lod_prefixes or hide_mesh_prefixes or hide_niagara_prefixes or hide_actor_labels:
        for candidate in unreal.EditorLevelLibrary.get_all_level_actors():
            if candidate.get_actor_label() in hide_actor_labels:
                component.hide_actor_components(candidate)
                hidden_label_count += 1
                continue
            if hide_niagara_prefixes and isinstance(candidate, unreal.NiagaraActor):
                niagara_components = candidate.get_components_by_class(unreal.NiagaraComponent)
                if any(niagara_component.get_editor_property("asset") and
                       any(niagara_component.get_editor_property("asset").get_path_name().startswith(prefix)
                           for prefix in hide_niagara_prefixes)
                       for niagara_component in niagara_components):
                    component.hide_actor_components(candidate)
                    for niagara_component in niagara_components:
                        previous_visibility = niagara_component.get_editor_property("visible")
                        hidden_niagara_components.append((niagara_component, previous_visibility))
                        niagara_component.set_visibility(False)
                    hidden_niagara_count += 1
                continue
            if not isinstance(candidate, unreal.StaticMeshActor):
                continue
            mesh_components = candidate.get_components_by_class(unreal.StaticMeshComponent)
            if len(mesh_components) != 1:
                continue
            mesh_component = mesh_components[0]
            mesh = mesh_component.get_editor_property("static_mesh")
            if not mesh:
                continue
            mesh_path = mesh.get_path_name()
            if any(mesh_path.startswith(prefix) for prefix in force_lod_prefixes):
                if forced_lod_index >= mesh.get_num_lods():
                    raise ValueError("Requested LOD {} exceeds {} on {}".format(
                        forced_lod_index, mesh.get_num_lods(), mesh_path))
                previous_lod = mesh_component.get_editor_property("forced_lod_model")
                forced_lod_components.append((mesh_component, previous_lod))
                # UE uses 0 for automatic selection and 1-based forced LODs.
                mesh_component.set_editor_property("forced_lod_model", forced_lod_index + 1)
            if any(mesh_path.startswith(prefix) for prefix in hide_mesh_prefixes):
                component.hide_actor_components(candidate)
                hidden_actor_count += 1
        if force_lod_prefixes and not forced_lod_components:
            raise RuntimeError("No static mesh actors match forceMeshLodPrefixes")
        if hide_mesh_prefixes and not hidden_actor_count:
            raise RuntimeError("No static mesh actors match hideMeshPrefixes")
        if hide_niagara_prefixes and not hidden_niagara_count:
            raise RuntimeError("No Niagara actors match hideNiagaraAssetPrefixes")
        if hide_actor_labels and hidden_label_count != len(hide_actor_labels):
            raise RuntimeError("Could not uniquely hide every hideActorLabels entry")
    component.set_editor_property("fov_angle", fov)
    component.set_editor_property("capture_source", capture_source)
    component.set_editor_property("capture_every_frame", False)
    component.set_editor_property("capture_on_movement", False)
    component.set_editor_property("always_persist_rendering_state", True)
    component.set_editor_property("texture_target", target)
    for _ in range(warmup_captures):
        component.capture_scene()
        time.sleep(warmup_delay)
    component.capture_scene()
    unreal.RenderingLibrary.export_render_target(world, target, str(output), name)
finally:
    for niagara_component, previous_visibility in hidden_niagara_components:
        niagara_component.set_visibility(previous_visibility)
    for mesh_component, previous_lod in forced_lod_components:
        mesh_component.set_editor_property("forced_lod_model", previous_lod)
    actor.destroy_actor()
files = [{"file": path.name, "bytes": path.stat().st_size}
         for path in (output / name, output / (name + ".png"), output / (name + ".exr"))
         if path.is_file()]
if not files:
    raise RuntimeError("UE did not export camera reference")
report = {"level": level, "name": name, "locationCm": location,
          "pitchDegrees": config.get("pitchDegrees", 0),
          "yawDegrees": config.get("yawDegrees", 0),
          "horizontalFovDegrees": fov, "resolution": resolution,
          "captureSource": capture_source_name,
          "warmupCaptures": warmup_captures, "warmupDelaySeconds": warmup_delay,
          "forcedLod0Actors": len(forced_lod_components) if forced_lod_index == 0 else 0,
          "forcedLodActors": len(forced_lod_components),
          "forcedLodIndex": forced_lod_index if forced_lod_components else None,
          "hiddenActors": hidden_actor_count,
          "hiddenNiagaraActors": hidden_niagara_count,
          "hiddenActorLabels": hidden_label_count,
          "files": files}
(output / (name + "-report.json")).write_text(
    json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("HE_UE_CAMERA " + json.dumps(report))
