"""Read-only UE 5.4 six-face HDR captures for HE local reflection probes.

Set HE_UE_PROBE_CONFIG to a JSON file outside UE Content, then run using the
full-editor -ExecutePythonScript mode. See Docs/UEFabAssetIntake.md. The script
spawns only transient actors and never saves the UE level or render targets.
"""

import json
import os
from pathlib import Path

import unreal


FACE_ROTATIONS = {
    "px": (0, 0), "nx": (0, 180),
    "py": (0, 90), "ny": (0, -90),
    "pz": (90, 0), "nz": (-90, 0),
}


def capture_probe(world, probe):
    name = probe["name"]
    if not name or not all(character.isalnum() or character in "_-" for character in name):
        raise ValueError("Probe name must contain only letters, digits, underscore or dash")
    location = probe["locationCm"]
    if len(location) != 3:
        raise ValueError("locationCm must contain UE X/Y/Z coordinates")
    resolution = int(probe.get("resolution", 256))
    if resolution < 32 or resolution > 2048:
        raise ValueError("Probe resolution must be 32..2048")
    output = Path(probe["outputDirectory"])
    if not output.is_absolute():
        raise ValueError("outputDirectory must be absolute")
    output = output.resolve()
    content = Path(unreal.Paths.project_content_dir()).resolve()
    if output == content or content in output.parents:
        raise ValueError("Do not write derived probe files into UE Content")
    output.mkdir(parents=True, exist_ok=True)
    target = unreal.RenderingLibrary.create_render_target2d(
        world, resolution, resolution, unreal.TextureRenderTargetFormat.RTF_RGBA16F)
    files = []
    for face, (pitch, yaw) in FACE_ROTATIONS.items():
        actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
            unreal.SceneCapture2D,
            unreal.Vector(location[0], location[1], location[2]),
            unreal.Rotator(pitch=pitch, yaw=yaw, roll=0), transient=True)
        if actor is None:
            raise RuntimeError("Could not spawn SceneCapture2D for " + face)
        try:
            components = actor.get_components_by_class(unreal.SceneCaptureComponent2D)
            if len(components) != 1:
                raise RuntimeError("Expected one capture component for " + face)
            component = components[0]
            component.set_editor_property("fov_angle", 90.0)
            component.set_editor_property("capture_source", unreal.SceneCaptureSource.SCS_SCENE_COLOR_HDR)
            component.set_editor_property("capture_every_frame", False)
            component.set_editor_property("capture_on_movement", False)
            component.set_editor_property("texture_target", target)
            component.capture_scene()
            filename = name + "-" + face
            unreal.RenderingLibrary.export_render_target(world, target, str(output), filename)
            path = output / filename
            if not path.is_file() or path.stat().st_size == 0:
                raise RuntimeError("Capture failed for " + face)
            files.append({"face": face, "file": path.name, "bytes": path.stat().st_size,
                          "pitch": pitch, "yaw": yaw})
        finally:
            actor.destroy_actor()
    report = {"name": name, "locationCm": location, "resolution": resolution,
              "format": "linear RGBA16F OpenEXR", "files": files}
    (output / (name + "-report.json")).write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print("HE_UE_PROBE " + json.dumps(report))


config_path = os.environ.get("HE_UE_PROBE_CONFIG")
if not config_path:
    raise RuntimeError("Set HE_UE_PROBE_CONFIG to an absolute JSON configuration path")
config = json.loads(Path(config_path).read_text(encoding="utf-8"))
level = config["level"]
loaded = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level)
if not loaded:
    raise RuntimeError("Could not load " + level)
world = unreal.EditorAssetLibrary.load_asset(level)
if world is None:
    raise RuntimeError("Could not resolve loaded UWorld " + level)
for item in config["probes"]:
    capture_probe(world, item)
