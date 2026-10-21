"""Reusable bounded Blender authoring operations, driven by a JSON request file.

blender --background source.blend --disable-autoexec --python-exit-code 7
  --python Tools/Content/blender_batch.py -- request.json
Request: {source, result, operations:[{op, id|name, ...}]}. inspect does not save.
"""
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import uuid
import bpy


def run(request):
    source = Path(request["source"]).resolve()
    if not bpy.data.filepath or Path(bpy.data.filepath).resolve() != source:
        raise ValueError("source_mismatch: Open the request source .blend before running this batch.")
    before = hashlib.sha256(source.read_bytes()).digest()
    operations = request.get("operations", [])
    if not isinstance(operations, list) or len(operations) > 128:
        raise ValueError("batch_limit: Supply at most 128 operations.")
    changed = False
    for operation in operations:
        matches = [obj for obj in bpy.context.scene.objects if
                   (obj.get("he_id") == operation["id"] if "id" in operation else obj.name == operation.get("name"))]
        if len(matches) != 1:
            raise ValueError("object_identity: Select exactly one object by persistent id or exact name.")
        obj = matches[0]
        op = operation["op"]
        if op in {"translate", "rotate", "scale"}:
            value = operation["value"]
            if len(value) != 3 or any(not math.isfinite(float(v)) for v in value):
                raise ValueError("transform: Expected three finite numbers in Blender local coordinates.")
            setattr(obj, {"translate": "location", "rotate": "rotation_euler", "scale": "scale"}[op], value)
        elif op == "rename":
            obj.name = operation["value"]
        elif op == "duplicate":
            duplicate = obj.copy()
            if operation.get("uniqueGeometry") and obj.data:
                duplicate.data = obj.data.copy()
            duplicate.name = operation["newName"]
            bpy.context.scene.collection.objects.link(duplicate)
        elif op == "parent":
            parent_id = operation.get("parentId")
            parents = [p for p in bpy.context.scene.objects if p.get("he_id") == parent_id] if parent_id else []
            if parent_id and len(parents) != 1:
                raise ValueError("parent_identity: Parent id must identify one object.")
            obj.parent = parents[0] if parents else None
        elif op == "delete":
            bpy.data.objects.remove(obj, do_unlink=True)
        elif op == "metadata":
            key = operation["key"]
            if key not in {"he_primary", "he_character", "he_proxy", "he_collision", "he_motion", "he_light_range", "he_exposure_ev100"}:
                raise ValueError("metadata: Not an editable authoring-semantic key.")
            obj[key] = operation["value"]
        else:
            raise ValueError("operation: Unsupported operation " + op)
        changed = True
    if changed:
        temporary = source.with_name(source.stem + ".he-batch-" + uuid.uuid4().hex + ".blend")
        try:
            bpy.ops.wm.save_as_mainfile(filepath=str(temporary), copy=True, check_existing=False)
            with temporary.open("r+b") as stream:
                os.fsync(stream.fileno())
            if hashlib.sha256(source.read_bytes()).digest() != before:
                raise ValueError("source_changed: Another process saved the source; batch refused to overwrite it.")
            os.replace(str(temporary), str(source))
        finally:
            if temporary.exists():
                temporary.unlink()
    limit = min(128, max(1, int(request.get("limit", 32))))
    objects = sorted(bpy.context.scene.objects, key=lambda obj: obj.name)
    return dict(ok=True, changed=changed, total=len(objects), objects=[
        dict(id=obj.get("he_id"), name=obj.name, type=obj.type, location=list(obj.location),
             meshId=obj.data.get("he_id") if obj.type == "MESH" else None) for obj in objects[:limit]])


if __name__ == "__main__":
    path = Path(sys.argv[sys.argv.index("--") + 1])
    request = json.loads(path.read_text(encoding="utf-8-sig"))
    try:
        result = run(request)
    except Exception as error:
        Path(request["result"]).write_text(json.dumps(dict(ok=False, error=str(error))), encoding="utf-8")
        raise
    Path(request["result"]).write_text(json.dumps(result, indent=2), encoding="utf-8")
