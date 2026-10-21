"""HEngine scene orchestration. Python standard library; no runtime dependency.

Native conversion belongs to the Importers plugin. This module owns provenance,
three-way merge, immutable resources, recovery and bounded tool processes.
"""
import argparse
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import struct
import sys
import time
import uuid

if __name__ == "__main__":
    # Adapters share this module's Failure type when invoked as a script.
    sys.modules.setdefault("content_sync", sys.modules[__name__])

VERSION = "content-sync/1"
OWNED = {
    "TagComponent": ["Tag"],
    "TransformComponent": ["Translation", "Rotation", "Scale", "ParentUuid"],
    "MeshComponent": ["Path", "meshResPath", "MaterialOverrides"],
    "CameraComponent": ["Camera", "Primary", "FixedAspectRatio"],
    "PointLightComponent": ["Intensity", "IntensityUnits", "LightColor", "Radius"],
    "SpotLightComponent": ["Intensity", "IntensityUnits", "LightColor", "Radius", "InnerConeAngle", "OuterConeAngle"],
    "DirectionalLightComponent": ["Intensity", "Color", "LocalDirection"],
    "Rigidbody3DComponent": ["Shape", "Type", "ColliderOffset", "ColliderRotation", "ColliderScale", "CollisionAssetPath"],
    "CharacterMovementComponent": [],
    "CapsuleCollider3DComponent": [],
    "AnimatorComponent": ["animResPath"],
}


class Failure(Exception):
    def __init__(self, code, message, object_id="", prop="", exit_code=3):
        super().__init__(message)
        self.diagnostic = dict(severity="error", code=code, message=message,
                               objectId=object_id, property=prop)
        self.exit_code = exit_code


def encode(value):
    return (json.dumps(value, sort_keys=True, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def read_json(path):
    with Path(path).open(encoding="utf-8-sig") as stream:
        return json.load(stream)


def atomic(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return False
    temp = path.with_name(path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temp.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(str(temp), str(path))
    finally:
        if temp.exists():
            temp.unlink()
    return True


def confined(root, relative):
    root = Path(root).resolve()
    path = (root / relative).resolve()
    if root not in path.parents:
        raise Failure("path_escape", "Path must remain inside its owned root: " + str(relative))
    return path


def replace_refs(value, mapping):
    if isinstance(value, str):
        return mapping.get(value, value)
    if isinstance(value, list):
        return [replace_refs(v, mapping) for v in value]
    if isinstance(value, dict):
        return {k: replace_refs(v, mapping) for k, v in value.items()}
    return value


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, list):
        for v in value:
            yield from strings(v)
    elif isinstance(value, dict):
        for v in value.values():
            yield from strings(v)


def merge_scene(previous, current, incoming, old_slots, new_slots):
    """Only generated fields are owned. Slot overrides follow material identity."""
    merged = json.loads(json.dumps(current))
    changes = dict(added=0, modified=0, deleted=0)
    for entity_id, old in previous.items():
        now = current.get(entity_id)
        if now is None:
            raise Failure("entity_deleted_in_engine", "Restore the imported entity or explicitly detach this import.", entity_id)
        if entity_id not in incoming:
            if now != old:
                raise Failure("delete_modified", "Source deletion would remove engine components or overrides; detach/reconcile explicitly.", entity_id)
            del merged[entity_id]
            changes["deleted"] += 1
    deleted = set(previous) - set(incoming)
    for entity_id, new in incoming.items():
        if entity_id not in previous:
            if entity_id in current:
                raise Failure("identity_collision", "New source entity collides with existing engine UUID.", entity_id)
            merged[entity_id] = new
            changes["added"] += 1
            continue
        old = previous[entity_id]
        now = merged[entity_id]
        before = encode(now)
        for component, fields in OWNED.items():
            old_comp, new_comp = old.get(component), new.get(component)
            cur_comp = now.get(component)
            if old_comp is None and new_comp is None:
                continue
            if old_comp is None:
                if cur_comp is not None:
                    raise Failure("component_collision", "Source now owns a component already added in the engine.", entity_id, component)
                now[component] = new_comp
                continue
            if cur_comp is None:
                raise Failure("component_removed", "An imported component was removed in the engine.", entity_id, component)
            if new_comp is None:
                if cur_comp != old_comp:
                    raise Failure("component_delete_modified", "Removing this source component would lose engine edits.", entity_id, component)
                del now[component]
                continue
            for field in fields:
                old_value, cur_value, new_value = old_comp.get(field), cur_comp.get(field), new_comp.get(field)
                if field == "MaterialOverrides":
                    old_keys, new_keys = old_slots.get(entity_id, []), new_slots.get(entity_id, [])
                    overrides = {}
                    for index, path in enumerate(cur_value or []):
                        baseline = (old_value or [])[index] if index < len(old_value or []) else ""
                        if path != baseline:
                            if index >= len(old_keys) or old_keys[index] not in new_keys:
                                raise Failure("slot_override_conflict", "Material slot identity changed or disappeared under an engine override.", entity_id, "MeshComponent.MaterialOverrides")
                            overrides[old_keys[index]] = path
                    cur_comp[field] = [overrides.get(key, new_value[i]) for i, key in enumerate(new_keys)]
                else:
                    if cur_value != old_value and cur_value != new_value:
                        raise Failure("owned_field_conflict", "Engine modified a Blender-owned field; restore it or detach before sync.", entity_id, component + "." + field)
                    cur_comp[field] = new_value
        if encode(now) != before:
            changes["modified"] += 1
    for entity_id, entity in merged.items():
        references = deleted.intersection(strings(entity))
        if references:
            raise Failure("delete_referenced", "Deleted source object is still referenced by " + entity_id, sorted(references)[0])
    return merged, changes


def recover(task, scene):
    pending = task / "pending.json"
    if not pending.exists():
        return
    journal = read_json(pending)
    actual = file_hash(scene) if scene.exists() else None
    if actual == journal["after"]:
        atomic(task / "state.json", encode(journal["record"]))
    elif actual != journal["before"]:
        raise Failure("recovery_conflict", "Interrupted publication and external scene edits overlap; retain journal and reconcile manually.")
    pending.unlink()


@contextlib.contextmanager
def lock(path):
    import msvcrt
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if stream.tell() == 0:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            raise Failure("import_busy", "Another writer owns this import. Retry after it completes.")
        try:
            yield
        finally:
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def run_process(args, log, timeout):
    # Argument arrays, never shell text. On timeout reap the owned process tree,
    # including build/Agent children of the verification script.
    with Path(log).open("wb") as output:
        environment = os.environ.copy()
        if Path(str(args[0])).stem.lower() == "powershell":
            environment = {k: v for k, v in environment.items() if k.lower() != "psmodulepath"}
        process = subprocess.Popen([str(a) for a in args], stdout=output, stderr=subprocess.STDOUT,
                                   env=environment, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            raise Failure("timeout", "Tool timed out; see " + str(log), exit_code=5)
        finally:
            if process.poll() is None:
                try:
                    if os.name == "nt":
                        subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"],
                                       stdout=output, stderr=subprocess.STDOUT, timeout=15,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait()
            process.wait()


def find_blender(explicit):
    candidates = [explicit, os.environ.get("HE_BLENDER"), shutil.which("blender")]
    for base in [Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Blender Foundation"]:
        candidates.extend(str(p) for p in sorted(base.glob("Blender */blender.exe"), reverse=True))
    if os.name == "nt":
        import winreg
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
                steam = Path(winreg.QueryValueEx(key, "SteamPath")[0])
                candidates.append(str(steam / "steamapps/common/Blender/blender.exe"))
        except OSError:
            pass
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return Path(candidate).resolve()
    raise Failure("blender_missing", "Blender not found. Install Blender 4.5 LTS and pass -BlenderExe or HE_BLENDER.", exit_code=2)


def prepare_resources(staging, asset_relative, native_prefix="__content__"):
    mapping, resources = {}, {}
    for suffixes in [(".bin", ".png", ".jpg", ".jpeg", ".hcollision"), (".hmat", ".hskel"), (".hmesh",), (".hanim",)]:
        for path in sorted(staging.iterdir()):
            if path.suffix not in suffixes:
                continue
            data = encode(replace_refs(read_json(path), mapping)) if path.suffix in (".hmat", ".hmesh", ".hskel", ".hanim") else path.read_bytes()
            name = asset_relative + "/Resources/" + digest(data) + path.suffix
            resources[name] = data
            mapping["__content__/" + path.name] = name
            mapping[native_prefix + "/" + path.name] = name
            mapping[(native_prefix + "/" + path.name).replace("/", "\\")] = name
    return mapping, resources


def gltf_document(source):
    data = source.read_bytes()
    if source.suffix.lower() == ".glb":
        if len(data) < 20 or data[:4] != b"glTF":
            raise Failure("glb_header", "Invalid GLB header.")
        length, kind = struct.unpack_from("<II", data, 12)
        if kind != 0x4E4F534A or length > len(data) - 20:
            raise Failure("glb_json", "Invalid GLB JSON chunk.")
        return json.loads(data[20:20+length]), data[20+length:]
    return json.loads(data.decode("utf-8-sig")), None


def initialize_gltf(source):
    document, tail = gltf_document(source)
    seen = set()
    for kind in ("scenes", "nodes", "meshes", "materials", "skins", "animations"):
        for item in document.get(kind, []):
            extras = item.setdefault("extras", {})
            source_id = extras.get("he_id")
            if source_id:
                source_id = str(uuid.UUID(source_id))
                if source_id in seen:
                    raise Failure("identity_duplicate", "Duplicate persistent IDs require author-side repair; initialization never guesses the original object.")
            else:
                source_id = str(uuid.uuid4())
            seen.add(source_id)
            extras.update(he_schema=1, he_id=source_id)
    if "scene" not in document and len(document.get("scenes", [])) == 1:
        document["scene"] = 0
    if tail is None:
        atomic(source, encode(document))
    else:
        chunk = encode(document)
        chunk += b" " * (-len(chunk) % 4)
        atomic(source, struct.pack("<III", 0x46546C67, 2, 20+len(chunk)+len(tail)) + struct.pack("<II", len(chunk), 0x4E4F534A) + chunk + tail)


def check_references(project, scene, resources):
    for value in strings(scene):
        if value.startswith("__content__/"):
            raise Failure("unresolved_reference", "Unresolved native resource " + value)
        if value.startswith("Assets/"):
            path = confined(project, value)
            if value not in resources and not path.is_file():
                raise Failure("missing_native_dependency", "Missing native resource " + value)


def synchronize(args, report_dir):
    project, source = Path(args.project).resolve(), Path(args.source).resolve()
    baked = getattr(args, "mode", "scene") == "baked-model"
    if baked and source.suffix.lower() not in (".gltf", ".glb"):
        raise Failure("baked_source", "Baked mode currently requires glTF/GLB; the static Blender exporter does not export skins.")
    if not source.is_file() or source.suffix.lower() not in (".blend", ".gltf", ".glb"):
        raise Failure("source_argument", "Source must be an existing .blend, .gltf or .glb file.", exit_code=2)
    if not (project / "Project.heproject").is_file():
        raise Failure("project_argument", "Project.heproject not found.", exit_code=2)
    if not args.importer or not Path(args.importer).is_file():
        raise Failure("importer_argument",
                      "HAssetImporter executable not found: " + str(args.importer) +
                      ". Build HAssetImporter for the selected project and configuration.", exit_code=2)
    source_hash = file_hash(source)
    source_dependencies = {}
    recipe_path = source.with_suffix(".verify.json")
    if recipe_path.exists():
        if recipe_path.stat().st_size > 262144:
            raise Failure("recipe_limit", "Verification recipe exceeds 256 KiB.")
        source_dependencies[str(recipe_path)] = file_hash(recipe_path)
    if source.suffix.lower() != ".blend":
        if args.initialize_ids and args.action != "plan":
            initialize_gltf(source)
            source_hash = file_hash(source)
        document, _ = gltf_document(source)
        from urllib.parse import unquote
        for item in document.get("buffers", []) + document.get("images", []):
            uri = item.get("uri", "")
            if uri and not uri.startswith("data:"):
                dependency = confined(source.parent, unquote(uri))
                if not dependency.is_file():
                    raise Failure("dependency_missing", "Missing source dependency " + uri)
                source_dependencies[str(dependency)] = file_hash(dependency)
    staging_root = project / "Assets" / "Imported" / ".baked-staging" if baked else project / ".he-content" / "staging"
    staging = staging_root / uuid.uuid4().hex
    staging.mkdir(parents=True)
    exported, export_info = source, {}
    try:
        if source.suffix.lower() == ".blend":
            blender = find_blender(args.blender)
            exported = report_dir / "source.glb"
            request = dict(source=str(source), output=str(exported), initializeIds=args.initialize_ids,
                           sourceSha256=source_hash, plan=args.action == "plan", result=str(report_dir / "blender.json"))
            request_path = report_dir / "blender-request.json"
            atomic(request_path, encode(request))
            status = run_process([blender, "--background", "--factory-startup", "--disable-autoexec", str(source),
                                  "--python-exit-code", "7", "--python", Path(__file__).with_name("blender_export.py"),
                                  "--", request_path], report_dir / "blender.log", args.timeout)
            export_info = read_json(report_dir / "blender.json") if (report_dir / "blender.json").exists() else {}
            if status or not export_info.get("ok") or not exported.exists():
                failure = Failure("blender_export", "Blender export failed; inspect blender.json and blender.log.")
                failure.details = export_info.get("diagnostics", [])
                raise failure
            export_info.pop("idsSaved", None)
            export_info.get("options", {}).pop("filepath", None)
            source_hash = export_info["sourceSha256"]
            source_dependencies.update(export_info.get("dependencyHashes", {}))
        if baked:
            from baked_model_adapter import adapt, validate_source
            validate_source(document)
        command = [args.importer, project, exported, staging] + ([] if baked else ["--scene"])
        atomic(report_dir / "conversion-request.json", encode(dict(command=[str(value) for value in command],
            importerSha256=file_hash(args.importer), sourceSha256=source_hash, mode=getattr(args, "mode", "scene"))))
        status = run_process(command, report_dir / "convert.log", args.timeout)
        atomic(report_dir / "conversion-result.json", encode(dict(exitCode=status,
            logSha256=file_hash(report_dir / "convert.log") if (report_dir / "convert.log").is_file() else None)))
        conversion_path = staging / "conversion.json"
        conversion = read_json(conversion_path) if conversion_path.exists() else {}
        if baked and not status:
            conversion = adapt(document, staging, project, getattr(args, "default_clip_id", ""))
        conversion.setdefault("diagnostics", []).extend(export_info.get("diagnostics", []))
        atomic(report_dir / "conversion.json", encode(conversion))
        if status or not conversion.get("ok"):
            failure = Failure("conversion_failed", "Native conversion failed; inspect conversion.json and convert.log.")
            failure.details = conversion.get("diagnostics", [])
            raise failure
        document_id = str(uuid.UUID(conversion["documentId"]))
        asset_relative = "Assets/Imported/" + document_id
        scene = confined(project, asset_relative + "/Scene.he")
        task = project / ".he-content" / document_id
        with lock(task / "lock"):
            if args.action == "plan" and (task / "pending.json").exists():
                raise Failure("recovery_required", "Run sync to recover the interrupted transaction before planning.")
            if args.action != "plan":
                recover(task, scene)
            old_record = read_json(task / "state.json") if (task / "state.json").exists() else {}
            if old_record and old_record.get("schema") != 1:
                raise Failure("record_schema", "Unsupported import record schema; migrate the record before sync.")
            if old_record and old_record.get("documentId") != document_id:
                raise Failure("record_identity", "Import record document mismatch.")
            if old_record and Path(old_record["source"]).resolve() != source and Path(old_record["source"]).exists():
                raise Failure("document_clone", "Another existing source owns this document ID. Move the original or author a new document identity for the copy.")
            if scene.exists() and not old_record:
                raise Failure("unowned_scene", "Destination scene already exists without this import's ownership record.")
            before_hash = file_hash(scene) if scene.exists() else None
            current = read_json(scene) if scene.exists() else {}
            if old_record and baked and old_record.get("defaultClipId") != conversion.get("defaultClipId"):
                raise Failure("default_clip_changed", "The stable default clip ID cannot change during reimport; restore it or detach this import.")
            mapping, resources = prepare_resources(staging, asset_relative, staging.relative_to(project).as_posix() if baked else "__content__")
            if baked:
                from baked_model_adapter import check_removed_references
                check_removed_references(project, scene, old_record, conversion, current)
            identity = {source_id: str(uuid.uuid5(uuid.UUID(document_id), source_id)) for source_id in conversion["nodes"]}
            incoming, slots = {}, {}
            for source_id, node in conversion["nodes"].items():
                entity_id = identity[source_id]
                incoming[entity_id] = replace_refs(replace_refs(node["entity"], identity), mapping)
                slots[entity_id] = node["slots"]
            merged, changes = merge_scene(old_record.get("baseline", {}), current, incoming,
                                           old_record.get("slots", {}), slots)
            deleted = set(old_record.get("baseline", {})) - set(incoming)
            if deleted:
                for other in (project / "Assets").rglob("*.he"):
                    if other.resolve() == scene:
                        continue
                    if deleted.intersection(strings(read_json(other))):
                        raise Failure("delete_external_reference", "A different scene references a deleted object: " + str(other))
            scene_bytes = encode(merged)
            check_references(project, merged, resources)
            for name, data in resources.items():
                if name.endswith((".hmat", ".hmesh", ".hskel", ".hanim")):
                    check_references(project, json.loads(data), resources)
                    if baked and any(value.startswith(staging.relative_to(project).as_posix() + "/") for value in strings(json.loads(data))):
                        raise Failure("unresolved_reference", "A baked resource still references temporary staging.")
            dependencies = {str(source): source_hash, **source_dependencies}
            for dependency in conversion.get("dependencies", []) + export_info.get("dependencies", []):
                path = Path(dependency)
                if staging not in path.parents:
                    if str(path) not in dependencies:
                        raise Failure("dependency_snapshot", "Converter discovered an unsnapshotted dependency: " + str(path))
            new_resources = [name for name, data in resources.items() if not (project / name).exists()]
            for name, data in resources.items():
                if (project / name).exists() and file_hash(project / name) != digest(data):
                    raise Failure("immutable_resource_corrupt", "Content-addressed resource was modified: " + name)
            record = dict(schema=1, tool=VERSION, converter=conversion["tool"], importerSha256=file_hash(args.importer),
                          driverSha256=file_hash(__file__), adapterSha256=file_hash(Path(__file__).with_name("baked_model_adapter.py" if baked else "blender_export.py")),
                          documentId=document_id, source=str(source), dependencies=dependencies, export=export_info,
                          baseline=incoming, slots=slots, identities=identity,
                          ownership=dict(schema=1, mode="source-to-engine", fields=OWNED), diagnostics=conversion["diagnostics"],
                          materials=replace_refs(conversion.get("materials", {}), mapping),
                          skins=replace_refs(conversion.get("skins", {}), mapping), clips=replace_refs(conversion.get("clips", {}), mapping),
                          defaultClipId=conversion.get("defaultClipId"), mode=getattr(args, "mode", "scene"),
                          sourceObjects={key: dict(uuid=identity[key], name=node["name"], meshId=node.get("meshId"),
                              meshPath=incoming[identity[key]].get("MeshComponent", {}).get("Path"), slots=slots[identity[key]])
                              for key, node in conversion["nodes"].items()},
                          worldMatrices={identity[key]: node["worldMatrix"] for key, node in conversion["nodes"].items()},
                          scene=scene.relative_to(project).as_posix(), sceneSha256=digest(scene_bytes),
                          artifacts={name: digest(data) for name, data in resources.items()})
            changed = before_hash != digest(scene_bytes) or bool(new_resources)
            summary = dict(ok=True, status="planned" if args.action == "plan" else "changed" if changed else "unchanged",
                           changed=changed, changes=changes, newResources=len(new_resources),
                           scene=record["scene"], sceneSha256=record["sceneSha256"], importVersion=digest(encode(record)),
                           diagnostics=conversion["diagnostics"],
                           metrics=dict(geometryResources=sum(n.endswith(".hmesh") for n in resources),
                                        resourceBytes=sum(len(v) for v in resources.values()), entities=len(incoming),
                                        loadMilliseconds=None, frameMilliseconds=None))
            if args.action == "plan":
                return summary
            # Resources are immutable and unreachable from the old scene. No cleanup
            # of old versions: external scenes/runtime handles may still reference them.
            for name, data in resources.items():
                atomic(confined(project, name), data)
            candidate = staging / "candidate.he"
            try:
                atomic(candidate, scene_bytes)
                status = run_process([args.importer, project, candidate, scene.parent, "--validate-scene"], report_dir / "validate.log", args.timeout)
                if status:
                    raise Failure("scene_preflight", "Engine SceneDocument/native mesh validation failed; inspect validate.log.")
                if (file_hash(scene) if scene.exists() else None) != before_hash:
                    raise Failure("concurrent_scene_edit", "Scene changed during conversion; rerun to merge against the new snapshot.")
                if any(file_hash(path) != hash_value for path, hash_value in dependencies.items()):
                    raise Failure("concurrent_source_edit", "Source dependency changed during import; rerun sync.")
                journal = dict(schema=1, before=before_hash, after=digest(scene_bytes), record=record)
                atomic(task / "pending.json", encode(journal))
                if args.fault == "before-scene":
                    raise Failure("injected_failure", "Test fault before scene publication.")
                atomic(scene, scene_bytes)  # sole reader-visible commit, after all dependencies
                if args.fault == "after-scene":
                    raise Failure("injected_failure", "Test fault after scene commit, before record update.")
                atomic(task / "state.json", encode(record))
                (task / "pending.json").unlink()
            finally:
                candidate.unlink(missing_ok=True) if sys.version_info >= (3, 8) else (candidate.unlink() if candidate.exists() else None)
            if args.verify != "none":
                try:
                    summary["verification"] = verify(args, summary, report_dir, task / "state.json", recipe_path)
                except Failure as error:
                    error.published = summary
                    raise
            return summary
    finally:
        # Exactly the fresh task-owned staging directory, checked against its root.
        if staging.parent == staging_root and staging.name.isalnum():
            shutil.rmtree(str(staging))


def verify(args, summary, report_dir, record, recipe_path):
    request = dict(project=str(Path(args.project).resolve()), scene=summary["scene"],
                   sceneSha256=summary["sceneSha256"], importVersion=summary["importVersion"],
                   mode=args.verify, report=str(report_dir), timeout=args.timeout, baselineRecord=str(record),
                   recipe=read_json(recipe_path) if recipe_path.exists() else None)
    path = report_dir / "verify-request.json"
    atomic(path, encode(request))
    code = run_process(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        Path(__file__).with_name("Verify-Content.ps1"), "-RequestFile", path],
                       report_dir / "verify.log", args.timeout + 120)
    result_path = report_dir / "verification.json"
    result = read_json(result_path) if result_path.exists() else {}
    if code or not result.get("ok"):
        raise Failure("verification_failed", "Scene published, but verification failed; inspect verification.json/verify.log.", exit_code=4)
    return result


def main():
    class Parser(argparse.ArgumentParser):
        def error(self, message):
            print(json.dumps(dict(ok=False, status="parameter_error", errors=[message])))
            self.exit(2)
    parser = Parser(description=__doc__)
    parser.add_argument("action", choices=["sync", "plan", "diagnose"])
    parser.add_argument("--source", default="")
    parser.add_argument("--project", default="Project")
    parser.add_argument("--importer", default="")
    parser.add_argument("--blender", default="")
    parser.add_argument("--mode", choices=["scene", "baked-model"], default="scene")
    parser.add_argument("--default-clip-id", default="", help="Persistent glTF animation he_id; required in baked-model mode.")
    parser.add_argument("--initialize-ids", action="store_true")
    parser.add_argument("--verify", choices=["none", "visual", "runtime"], default="none")
    parser.add_argument("--timeout", type=int, default=120)
    parser.add_argument("--report", default="")
    parser.add_argument("--object-id", default="")
    parser.add_argument("--error-code", default="")
    parser.add_argument("--fault", choices=["", "before-scene", "after-scene"], default="", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.action == "diagnose":
        try:
            report = read_json(args.report)
        except (OSError, ValueError) as error:
            print(json.dumps(dict(ok=False, status="report_argument", errors=[str(error)])))
            return 2
        diagnostics = report.get("diagnostics", [])
        print(json.dumps([d for d in diagnostics if (not args.object_id or d.get("objectId") == args.object_id)
                          and (not args.error_code or d.get("code") == args.error_code)], ensure_ascii=False))
        return 0
    report_dir = Path(args.report).resolve() if args.report else Path(__file__).resolve().parents[2] / "out/agent/content" / (time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8])
    report_dir.mkdir(parents=True, exist_ok=True)
    storage_scope = contextlib.nullcontext(dict(status="unknown"))
    try:
        import importlib.util
        repository = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location("he_storage", repository / "Tools/Agent/storage.py")
        storage_module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(storage_module)
        manager = storage_module.Storage(repository)
        owned_report = report_dir
        if args.report:
            owned_report = repository / "out/agent/logs/content-guards" / uuid.uuid4().hex
            owned_report.mkdir(parents=True)
            atomic(owned_report / "references.json", encode(dict(report=str(report_dir), policy="explicit report preserved")))
        storage_scope = manager.workflow_scope(str(Path(args.project).resolve()), "content-" + args.action, [owned_report])
    except Exception as error:
        print("storage: content report retention deferred: " + str(error), file=sys.stderr)
    with storage_scope as storage_run:
        started = time.monotonic()
        code = 0
        try:
            result = synchronize(args, report_dir)
        except Failure as error:
            code = error.exit_code
            result = dict(getattr(error, "published", {}))
            result.update(ok=False, status=error.diagnostic["code"], diagnostics=getattr(error, "details", []) + [error.diagnostic])
            if hasattr(error, "published"):
                result["published"] = True
        except Exception as error:
            import traceback
            (report_dir / "exception.log").write_text(traceback.format_exc(), encoding="utf-8")
            code = 3
            result = dict(ok=False, status="unexpected_failure", diagnostics=[dict(severity="error", code="unexpected_failure", message=str(error))])
        result["elapsedSeconds"] = round(time.monotonic() - started, 3)
        result["report"] = str(report_dir / "report.json")
        atomic(report_dir / "report.json", encode(result))
        brief = {k: v for k, v in result.items() if k not in ("diagnostics", "verification")}
        brief["errors"] = [d["code"] for d in result["diagnostics"] if d.get("severity") == "error"][:8]
        brief["warnings"] = [d["code"] for d in result["diagnostics"] if d.get("severity") == "warning"][:8]
        brief["screenshot"] = result.get("verification", {}).get("screenshot")
        print(json.dumps(brief, ensure_ascii=False))
        storage_run["status"] = "success" if code == 0 else "failed"
        return code


if __name__ == "__main__":
    sys.exit(main())
