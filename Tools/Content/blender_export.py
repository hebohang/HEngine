"""Run with Blender --background --disable-autoexec --python-exit-code 7.

Adapter v1: meters, +Y up glTF, evaluated modifiers, visible objects in active
scene; explicit realization required for collection/Geometry Nodes instances.
"""
import json
import hashlib
import os
from pathlib import Path
import struct
import sys
import uuid
import bpy

ADAPTER = "he-blender/1"
DIAGNOSTIC_CONTEXT = dict(objectId="", name="", property="document")


def export(request):
    scene = bpy.context.scene
    if not ((4, 5, 0) <= bpy.app.version < (6, 0, 0)):
        raise ValueError("blender_version: Adapter v1 supports Blender 4.5 through 5.x; revalidate before using another major version.")
    if abs(scene.unit_settings.scale_length - 1.0) > 1e-6:
        raise ValueError("units: Use one Blender unit = one meter; apply unit conversion before export.")
    objects = [obj for obj in scene.objects if not obj.hide_render and obj.visible_get()]
    allowed = {"MESH", "EMPTY", "CAMERA", "LIGHT"}
    dependencies = set()
    diagnostics = [dict(severity="warning", code="world_lighting_not_exported", objectId=scene.get("he_id", ""),
        name=scene.name, property="world", message="Blender World/background lighting is not exported. Configure HEngine environment/exposure separately; point and sun lights are exported.")]
    def file_hash(path):
        digest = hashlib.sha256()
        with Path(path).open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
        return digest.hexdigest()
    if file_hash(request["source"]) != request["sourceSha256"]:
        raise ValueError("source_changed: Source changed before export; retry.")
    for obj in objects:
        DIAGNOSTIC_CONTEXT.update(objectId=obj.get("he_id", ""), name=obj.name, property="object")
        if obj.type not in allowed:
            raise ValueError("object_type: Convert/bake unsupported object " + obj.name + " (" + obj.type + ")")
        if obj.instance_type != "NONE" or any(m.type == "NODES" for m in obj.modifiers):
            raise ValueError("instances: Realize Collection/Geometry Nodes instances explicitly before export: " + obj.name)
        if obj.parent and obj.parent not in objects:
            raise ValueError("hidden_parent: A visible object's transform parent is excluded: " + obj.name)
        if obj.library or (obj.data and obj.data.library):
            raise ValueError("linked_identity: Make linked objects/data local before assigning persistent IDs: " + obj.name)
        if obj.animation_data and (obj.animation_data.action or obj.animation_data.nla_tracks):
            raise ValueError("animation: Static scene export rejects animated objects: " + obj.name)
        if obj.type == "LIGHT" and obj.data.type not in {"POINT", "SUN"}:
            raise ValueError("light_type: Only POINT and SUN are mapped: " + obj.name)
    materials = {slot.material for obj in objects if obj.type == "MESH" for slot in obj.material_slots if slot.material}
    for material in materials:
        DIAGNOSTIC_CONTEXT.update(objectId=material.get("he_id", ""), name=material.name, property="material")
        if material.library:
            raise ValueError("linked_identity: Make linked materials local: " + material.name)
        if not material.use_nodes:
            continue
        for node in material.node_tree.nodes:
            if node.type not in {"OUTPUT_MATERIAL", "BSDF_PRINCIPLED", "TEX_IMAGE", "NORMAL_MAP", "SEPARATE_COLOR", "SEPRGB", "REROUTE"}:
                raise ValueError("material_graph: Bake unsupported node " + node.type + " in " + material.name)
            if node.type == "BSDF_PRINCIPLED":
                supported_links = {"Base Color", "Metallic", "Roughness", "Normal", "Emission Color", "Alpha"}
                for socket in node.inputs:
                    if socket.is_linked and socket.name not in supported_links:
                        raise ValueError("material_graph: Bake linked " + socket.name + " in " + material.name)
                for name in ["Base Color", "Emission Color", "Alpha"]:
                    socket = node.inputs.get(name)
                    if socket and socket.is_linked and socket.links[0].from_node.type != "TEX_IMAGE":
                        raise ValueError("material_graph: Use a direct image link for " + name + " in " + material.name)
                for name in ["Coat Weight", "Transmission Weight", "Sheen Weight", "Anisotropic IOR Level", "Subsurface Weight"]:
                    socket = node.inputs.get(name)
                    if socket and (socket.is_linked or abs(socket.default_value) > 1e-6):
                        raise ValueError("material_extension: Bake unsupported " + name + " in " + material.name)
    for image in bpy.data.images:
        DIAGNOSTIC_CONTEXT.update(objectId="", name=image.name, property="image.filepath")
        if image.source == "FILE" and not image.packed_file and image.filepath:
            path = Path(bpy.path.abspath(image.filepath)).resolve()
            if not path.is_file():
                raise ValueError("dependency_missing: " + str(path))
            dependencies.add(str(path))
    for library in bpy.data.libraries:
        path = Path(bpy.path.abspath(library.filepath)).resolve()
        if not path.is_file():
            raise ValueError("dependency_missing: " + str(path))
        dependencies.add(str(path))
    dependency_hashes = {path: file_hash(path) for path in dependencies}

    # Persistent ID pointers in a non-exported Text datablock disambiguate copied
    # custom properties even when the original is renamed. Never use name ordering.
    registry = bpy.data.texts.get(".he_identity_owners")
    changed = False
    datablocks = [scene] + objects + list({obj.data for obj in objects if obj.type == "MESH"}) + list(materials)
    for block in datablocks:
        DIAGNOSTIC_CONTEXT.update(objectId=block.get("he_id", ""), name=block.name, property="extras.he_id")
        existing = block.get("he_id")
        owner = registry.get(existing) if registry and existing else None
        valid = False
        try:
            valid = existing is not None and str(uuid.UUID(existing)) == existing and block.get("he_schema") == 1
        except (ValueError, TypeError):
            pass
        if not valid or (owner is not None and owner != block):
            if not request.get("initializeIds") or request.get("plan"):
                raise ValueError("identity_initialization: Missing/duplicated ID on " + block.name + "; run sync -InitializeIds once. This saves IDs to the .blend.")
            existing = str(uuid.uuid4())
            block["he_id"], block["he_schema"] = existing, 1
            changed = True
        if registry is None or owner != block:
            if not request.get("initializeIds") or request.get("plan"):
                raise ValueError("identity_registry: Register ownership with sync -InitializeIds before reimport: " + block.name)
            registry = registry or bpy.data.texts.new(".he_identity_owners")
            registry[existing] = block
            changed = True
    # Never partially overwrite the original. Save a sibling, then atomic replace.
    # Blender writes all evaluated/export changes below only in this background copy.
    if changed:
        source = Path(request["source"])
        temporary = source.with_name(source.stem + ".he-save-" + uuid.uuid4().hex + ".blend")
        try:
            bpy.ops.wm.save_as_mainfile(filepath=str(temporary), copy=True, check_existing=False)
            with temporary.open("r+b") as stream:
                os.fsync(stream.fileno())
            if file_hash(source) != request["sourceSha256"]:
                raise ValueError("source_changed: Refusing to overwrite a concurrent Blender save.")
            os.replace(str(temporary), str(source))
        finally:
            if temporary.exists():
                temporary.unlink()
    saved_source_hash = file_hash(request["source"])
    # Only the background process changes selection/export state; never save this.
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    options = dict(filepath=request["output"], export_format="GLB", use_selection=True,
                   export_extras=True, export_yup=True, export_apply=True,
                   export_animations=False, export_skins=False, export_morph=False,
                   export_cameras=True, export_lights=True, export_normals=True,
                   export_tangents=True, export_materials="EXPORT", export_texcoords=True,
                   export_attributes=True)
    properties = bpy.ops.export_scene.gltf.get_rna_type().properties
    options = {k: v for k, v in options.items() if k in properties}
    result = bpy.ops.export_scene.gltf(**options)
    if "FINISHED" not in result:
        raise ValueError("export_failed: glTF exporter did not finish.")
    # Blender does not consistently export scene extras, finite light radius or
    # evaluated mesh datablock extras across versions. Normalize authoring metadata
    # only; geometry conversion remains exclusively cgltf in HAssetImporter.
    output = Path(request["output"])
    data = output.read_bytes()
    json_size, kind = struct.unpack_from("<II", data, 12)
    if kind != 0x4E4F534A:
        raise ValueError("glb_layout: Expected JSON first chunk.")
    document = json.loads(data[20:20 + json_size])
    document["scenes"][document.get("scene", 0)]["extras"] = {"he_schema": 1, "he_id": scene["he_id"]}
    by_id = {obj["he_id"]: obj for obj in objects}
    for node in document.get("nodes", []):
        obj = by_id.get(node.get("extras", {}).get("he_id"))
        if obj is None:
            raise ValueError("export_identity: Exporter produced an untracked object; realize/bake the source hierarchy.")
        if obj.type == "MESH" and "mesh" in node:
            document["meshes"][node["mesh"]]["extras"] = {"he_schema": 1, "he_id": obj.data["he_id"]}
        if obj.type == "LIGHT" and obj.data.type == "POINT":
            light_index = node["extensions"]["KHR_lights_punctual"]["light"]
            light = document["extensions"]["KHR_lights_punctual"]["lights"][light_index]
            light["range"] = float(obj.get("he_light_range", 10.0))
        if obj.type == "CAMERA":
            camera = document["cameras"][node["camera"]]
            if camera["type"] == "perspective":
                camera["perspective"]["aspectRatio"] = scene.render.resolution_x / scene.render.resolution_y
    chunk = json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    chunk += b" " * (-len(chunk) % 4)
    tail = data[20 + json_size:]
    output.write_bytes(struct.pack("<III", 0x46546C67, 2, 20 + len(chunk) + len(tail)) + struct.pack("<II", len(chunk), 0x4E4F534A) + chunk + tail)
    if any(file_hash(path) != value for path, value in dependency_hashes.items()) or file_hash(request["source"]) != saved_source_hash:
        raise ValueError("source_changed: A source dependency changed during export; retry.")
    diagnostics[0]["objectId"] = scene["he_id"]
    return dict(ok=True, adapter=ADAPTER, blender=bpy.app.version_string, options=options,
                dependencies=sorted(dependencies), dependencyHashes=dependency_hashes,
                sourceSha256=saved_source_hash, idsSaved=changed, diagnostics=diagnostics)


if __name__ == "__main__":
    request = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text(encoding="utf-8-sig"))
    try:
        result = export(request)
    except Exception as error:
        diagnostic = dict(DIAGNOSTIC_CONTEXT, severity="error", code=str(error).split(":")[0], message=str(error))
        Path(request["result"]).write_text(json.dumps(dict(ok=False, adapter=ADAPTER, error=str(error), diagnostics=[diagnostic])), encoding="utf-8")
        raise
    Path(request["result"]).write_text(json.dumps(result, indent=2), encoding="utf-8")
