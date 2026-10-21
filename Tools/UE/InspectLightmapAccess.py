"""Read-only UE lightmap API probe; writes metadata outside the UE project."""

import json
import os
from pathlib import Path

import unreal


level = os.environ.get("HE_UE_LEVEL")
output_text = os.environ.get("HE_UE_LIGHTMAP_REPORT")
if not level or not output_text:
    raise RuntimeError("Set HE_UE_LEVEL and HE_UE_LIGHTMAP_REPORT")
output = Path(output_text).resolve()
content = Path(unreal.Paths.project_content_dir()).resolve()
if output == content or content in output.parents:
    raise RuntimeError("Report must be outside UE Content")
if not unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).load_level(level):
    raise RuntimeError("Could not load level " + level)
try:
    unreal.SystemLibrary.execute_console_command(
        unreal.EditorLevelLibrary.get_editor_world(),
        "obj list class=LightMapTexture2D")
except Exception as error:
    unreal.log_warning("HE_UE_LIGHTMAP_OBJECT_LIST " + repr(error))

report = {"level": level, "classes": {}, "objects": {}, "sampleMesh": {}}
report["apiCandidates"] = [name for name in dir(unreal)
                           if ("object" in name.lower() or "asset" in name.lower())
                           and (name.startswith("get_") or name.startswith("find_")
                                or name.startswith("load_"))][:80]
report["apiDocs"] = {
    name: str(getattr(unreal, name).__doc__)[:900]
    for name in ("find_object", "load_object")
    if getattr(unreal, name, None) is not None
}
package_name = os.environ.get("HE_UE_BUILTDATA_ASSET", level + "_BuiltData")
asset_path = package_name + "." + package_name.rsplit("/", 1)[-1]
try:
    built_data = unreal.load_object(None, asset_path)
    report["builtDataObject"] = {
        "loaded": built_data is not None,
        "class": built_data.get_class().get_name() if built_data else None,
        "properties": {},
    }
    if built_data:
        for property_name in ("mesh_build_data", "level_precomputed_light_volume_build_data",
                              "level_precomputed_volumetric_lightmap_build_data"):
            try:
                report["builtDataObject"]["properties"][property_name] = str(
                    built_data.get_editor_property(property_name))[:500]
            except Exception as error:
                report["builtDataObject"]["properties"][property_name] = {
                    "error": str(error)[:250]}
except Exception as error:
    report["builtDataObject"] = {"error": repr(error)}
report["lightmapObjectsByName"] = []
for name in filter(None, os.environ.get("HE_UE_LIGHTMAP_OBJECTS", "").split(",")):
    try:
        obj = unreal.find_object(built_data, name, unreal.LightMapTexture2D)
        if obj:
            texture = {"path": obj.get_path_name(), "class": obj.get_class().get_name(),
                       "size": None, "properties": {}}
            try:
                texture["size"] = [obj.blueprint_get_size_x(), obj.blueprint_get_size_y()]
            except Exception as error:
                texture["sizeError"] = repr(error)
            for property_name in ("srgb", "compression_settings", "mip_gen_settings",
                                  "virtual_texture_streaming"):
                try:
                    texture["properties"][property_name] = str(obj.get_editor_property(property_name))
                except Exception as error:
                    texture["properties"][property_name] = {"error": str(error)[:200]}
            report["lightmapObjectsByName"].append(texture)
    except Exception as error:
        report["lightmapObjectsByName"].append({"name": name, "error": repr(error)})
report["exporterClasses"] = [name for name in dir(unreal)
                             if "Exporter" in name and ("Texture" in name or "EXR" in name)]
report["lightmapExportLimitation"] = (
    "UE 5.4 TextureExporterEXR asserts SupportsTexture on LightMapTexture2D; "
    "the standard texture exporter is unsafe for these built-data subobjects.")
try:
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    assets = registry.get_assets_by_package_name(package_name)
    report["builtDataAssets"] = [str(asset)[:500] for asset in assets[:32]]
except Exception as error:
    report["builtDataAssets"] = {"error": repr(error)}
try:
    report["mapFolderAssets"] = [str(path) for path in
        unreal.EditorAssetLibrary.list_assets(
            package_name.rsplit("/", 1)[0], recursive=False, include_folder=False)[:30]]
except Exception as error:
    report["mapFolderAssets"] = {"error": repr(error)}
for name in ("MapBuildDataRegistry", "LightMapTexture2D", "LightMapVirtualTexture2D"):
    cls = getattr(unreal, name, None)
    report["classes"][name] = cls is not None
    if cls is None:
        continue
    try:
        objects = unreal.get_objects_of_class(cls)
        report["objects"][name] = {
            "count": len(objects),
            "samples": [obj.get_path_name() for obj in objects[:8]],
        }
    except Exception as error:
        report["objects"][name] = {"error": repr(error)}

sample_mesh_prefix = os.environ.get("HE_UE_SAMPLE_MESH_PREFIX", "")
for actor in unreal.EditorLevelLibrary.get_all_level_actors():
    if not actor.get_name().startswith(sample_mesh_prefix):
        continue
    components = actor.get_components_by_class(unreal.StaticMeshComponent)
    if not components:
        continue
    component = components[0]
    report["sampleMesh"] = {
        "actor": actor.get_path_name(),
        "component": component.get_path_name(),
        "properties": {},
    }
    for property_name in ("map_build_data_id", "lod_data", "light_map_resolution",
                          "override_light_map_res", "b_override_light_map_res",
                          "static_lighting_texture_guid"):
        try:
            value = component.get_editor_property(property_name)
            report["sampleMesh"]["properties"][property_name] = str(value)[:500]
        except Exception as error:
            report["sampleMesh"]["properties"][property_name] = {"error": str(error)[:300]}
    break

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
print("HE_UE_LIGHTMAP_ACCESS " + str(output))
