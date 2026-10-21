"""Bounded adapter of Assimp's native resources into content-sync's transaction.

No animation decoding: Assimp owns geometry, skinning and animation conversion.
One skinned source model, named joints and named clips are required. External
controllers remain engine-owned and are not automatically migrated.
"""
import uuid

from content_sync import Failure, read_json


def source_id(item):
    try:
        extras = item["extras"]
        if extras["he_schema"] != 1:
            raise ValueError("schema")
        return str(uuid.UUID(extras["he_id"]))
    except (KeyError, ValueError, TypeError) as error:
        raise Failure("baked_identity", "Initialize persistent source IDs before baked import.") from error


def validate_source(document):
    ids = []
    for kind in ("scenes", "nodes", "meshes", "materials", "skins", "animations"):
        ids.extend(source_id(item) for item in document.get(kind, []))
    if len(ids) != len(set(ids)):
        raise Failure("identity_duplicate", "Duplicate source IDs are ambiguous.")
    skinned = [node for node in document.get("nodes", []) if "skin" in node]
    if len(document.get("skins", [])) != 1 or len(document.get("meshes", [])) != 1 or len(skinned) != 1:
        raise Failure("baked_scope", "Baked sync requires exactly one skin, mesh and skinned model node.")
    if skinned[0].get("mesh") != 0 or skinned[0]["skin"] != 0:
        raise Failure("baked_scope", "Skinned model must reference the sole mesh and skin.")
    if len(document["meshes"][0].get("primitives", [])) != 1:
        raise Failure("baked_scope", "The bounded adapter supports one material primitive; it never guesses native slot ordering.")
    animations = document.get("animations", [])
    names = [item.get("name", "") for item in animations]
    if not names or not all(names) or len(set(names)) != len(names):
        raise Failure("baked_clip_identity", "Unique nonempty animation names are required for native correspondence.")
    joints = document["skins"][0].get("joints", [])
    joint_names = [document["nodes"][index].get("name", "") for index in joints]
    if not joints or not all(joint_names) or len(set(joint_names)) != len(joint_names):
        raise Failure("baked_joint_identity", "Unique named joints are required.")
    return skinned[0]


def adapt(document, staging, project, default_clip_id):
    node = validate_source(document)
    scene_index = document.get("scene", 0)
    document_id = source_id(document["scenes"][scene_index])
    animation_ids = {source_id(item): item for item in document["animations"]}
    if default_clip_id not in animation_ids:
        raise Failure("default_clip_missing", "The requested stable default clip ID is missing; no automatic replacement is allowed.")
    meshes, skeletons = list(staging.glob("*.hmesh")), list(staging.glob("*.hskel"))
    if len(meshes) != 1 or len(skeletons) != 1:
        raise Failure("baked_output", "Expected exactly one native mesh and skeleton.")
    relative = lambda path: path.relative_to(project).as_posix()
    mesh_path, skeleton_path = relative(meshes[0]), relative(skeletons[0])
    mesh, skeleton = read_json(meshes[0]), read_json(skeletons[0])
    if mesh.get("skeletonPath") != skeleton_path:
        raise Failure("baked_dependency", "Native mesh does not reference the emitted skeleton.")
    native_bones = {bone["name"]: bone["index"] for bone in skeleton["bones"]}
    joint_map = {}
    for index in document["skins"][0]["joints"]:
        joint = document["nodes"][index]
        if joint["name"] not in native_bones:
            raise Failure("baked_joint_identity", "A source joint is absent from the native skeleton: " + joint["name"])
        joint_map[source_id(joint)] = dict(name=joint["name"], boneIndex=native_bones[joint["name"]])
    native_clips = {}
    for path in staging.glob("*.hanim"):
        animation = read_json(path)
        name = animation.get("name")
        if name in native_clips or len(animation.get("clips", [])) != 1:
            raise Failure("baked_clip_identity", "Native clips have ambiguous names or unexpected grouping.")
        if animation.get("meshResPath") != mesh_path or animation.get("targetSkeletonPath") != skeleton_path:
            raise Failure("baked_dependency", "Native animation mesh/skeleton reference mismatch.")
        native_clips[name] = relative(path)
    if set(native_clips) != {item["name"] for item in animation_ids.values()}:
        raise Failure("baked_clip_identity", "Source and native animation names differ.")
    clips = {key: dict(name=item["name"], path=native_clips[item["name"]]) for key, item in animation_ids.items()}
    # The baked importer owns model transforms inside its mesh/skeleton output.
    # Do not apply the source node transform a second time to the ECS entity.
    entity = {
        "TagComponent": {"Tag": node.get("name", "ImportedModel")},
        "TransformComponent": {"Translation": [0.0, 0.0, 0.0], "Rotation": [0.0, 0.0, 0.0], "Scale": [1.0, 1.0, 1.0], "ParentUuid": ""},
        "MeshComponent": {"Path": mesh_path, "meshResPath": mesh_path, "MaterialOverrides": []},
        "AnimatorComponent": {"animResPath": clips[default_clip_id]["path"], "controllerPath": "None", "currentClipIndex": 0, "bPlay": True, "bLoop": True, "playSpeed": 1.0, "bEnableRootMotion": False, "bLockRootMotionXZ": True},
    }
    primitives = document["meshes"][0].get("primitives", [])
    slots = [source_id(document["materials"][p["material"]]) if "material" in p else "default" for p in primitives]
    # Override vectors are parallel to native primitives. Refuse a conversion
    # that reordered/combined material slots instead of guessing identity.
    if len(mesh.get("primitives", [])) != len(slots):
        raise Failure("baked_material_slots", "Native/source primitive counts differ.")
    entity["MeshComponent"]["MaterialOverrides"] = [""] * len(slots)
    return dict(ok=True, tool="assimp-baked-adapter/1", documentId=document_id, defaultClipId=default_clip_id,
                diagnostics=[dict(severity="info", code="baked_scope", message="Single baked model; external controller and separate prefab instances are not automatically migrated.")],
                nodes={source_id(node): dict(entity=entity, name=node.get("name", "ImportedModel"), slots=slots,
                    meshId=source_id(document["meshes"][0]), worldMatrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])},
                skins={source_id(document["skins"][0]): dict(path=skeleton_path, joints=joint_map)}, clips=clips)


def authored_resource_references(document, kind):
    """Read active typed references, not tags, script strings or prefab history.

    v2 hprefab entities and nested overrides are component objects; legacy
    hprefab is itself a component object. BaselineJson is refresh history and
    must not prevent deletion of an otherwise unused source clip.
    """
    def entity_references(entity):
        if not isinstance(entity, dict):
            return
        animator = entity.get("AnimatorComponent")
        if isinstance(animator, dict) and isinstance(animator.get("animResPath"), str):
            yield animator["animResPath"].replace("\\", "/")
    if not isinstance(document, dict):
        return
    if kind == ".hanimctrl":
        if isinstance(document.get("targetSkeletonPath"), str):
            yield document["targetSkeletonPath"].replace("\\", "/")
        for node in document.get("nodes", []):
            if isinstance(node, dict) and isinstance(node.get("clipPath"), str):
                yield node["clipPath"].replace("\\", "/")
    elif kind == ".hprefab":
        if document.get("schemaVersion") == 2:
            for entity in document.get("entities", {}).values():
                yield from entity_references(entity)
            for nested in document.get("nested", []):
                for patch in nested.get("overrides", {}).values():
                    yield from entity_references(patch)
        else:
            yield from entity_references(document)
    elif kind == ".he":
        for entity in document.values():
            yield from entity_references(entity)


def check_removed_references(project, scene, old_record, conversion, current):
    removed = set()
    for kind in ("clips", "skins"):
        removed.update(value["path"] for key, value in old_record.get(kind, {}).items() if key not in conversion.get(kind, {}))
    if not removed:
        return
    if removed.intersection(authored_resource_references(current, ".he")):
        raise Failure("delete_resource_referenced", "Deleted source resource is still referenced by the current imported scene.")
    for suffix in ("*.he", "*.hanimctrl", "*.hprefab"):
        for path in (project / "Assets").rglob(suffix):
            if path.resolve() == scene.resolve():
                continue
            if removed.intersection(authored_resource_references(read_json(path), path.suffix.lower())):
                raise Failure("delete_resource_referenced", "Deleted source resource is referenced by " + str(path))
