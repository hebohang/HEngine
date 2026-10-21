"""Versioned HEngine VFX recipes, editable sources and deterministic packages."""
from __future__ import annotations
import argparse
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

import generate_survivor_toon_projectiles_v2 as toon
import generate_effekseer_stress_assets as stress

ROOT = Path(__file__).resolve().parents[2]
RECIPES = Path(__file__).parent / "recipes"
TEMPLATES = ("hit", "explosion", "trail", "telegraph", "aura", "environment")
VERSION = 1


class RecipeError(ValueError):
    pass


def encoded(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def fields(value, names, where):
    if not isinstance(value, dict) or set(value) != set(names):
        raise RecipeError("{} must contain exactly {}".format(where, sorted(names)))


def number(value, low, high, where, integer=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise RecipeError(where + " must be finite numeric")
    if not low <= value <= high or (integer and not isinstance(value, int)):
        raise RecipeError("{} must be {} in [{}, {}]".format(where, "integer" if integer else "numeric", low, high))


def validate(recipe):
    fields(recipe, ("schema_version", "id", "template", "semantic", "compile", "playback", "budget", "requirements", "dependencies"), "recipe")
    if type(recipe["schema_version"]) is not int or recipe["schema_version"] != VERSION:
        raise RecipeError("Unsupported recipe schema_version")
    if not isinstance(recipe["id"], str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", recipe["id"]):
        raise RecipeError("id must be a lowercase safe resource identifier")
    if recipe["template"] not in TEMPLATES:
        raise RecipeError("Unknown template")
    fields(recipe["semantic"], ("purpose", "style"), "semantic")
    if not all(isinstance(v, str) and 0 < len(v) <= 512 for v in recipe["semantic"].values()):
        raise RecipeError("semantic descriptions must be nonempty strings up to 512 characters")
    c = recipe["compile"]
    fields(c, ("color", "radius_m", "attack_s", "sustain_s", "decay_s", "particles", "direction", "seed", "simulation", "quality"), "compile")
    for name, size, low, high in (("color", 4, 0, 255), ("direction", 3, -1, 1)):
        if not isinstance(c[name], list) or len(c[name]) != size:
            raise RecipeError(name + " has incorrect vector length")
        for value in c[name]:
            number(value, low, high, name, name == "color")
    if sum(v*v for v in c["direction"]) < 0.0001:
        raise RecipeError("direction must be nonzero")
    if recipe["template"] in ("telegraph", "aura") and c["direction"] != [0, 1, 0]:
        raise RecipeError("Ground-ring templates currently require direction [0,1,0]")
    number(c["radius_m"], .01, 50, "radius_m")
    for name in ("attack_s", "sustain_s", "decay_s"):
        number(c[name], 0, 30, name)
    if c["attack_s"] + c["sustain_s"] + c["decay_s"] < 1/60:
        raise RecipeError("effect duration must be at least one simulation frame")
    number(c["particles"], 1, 100000, "particles", True)
    number(c["seed"], 0, 2147483647, "seed", True)
    if c["simulation"] not in ("cpu", "gpu") or c["quality"] not in ("Low", "Medium", "High"):
        raise RecipeError("Unknown simulation or quality")
    if recipe["template"] == "explosion" and c["simulation"] == "cpu" and c["direction"] != [0, 1, 0]:
        raise RecipeError("CPU explosion currently requires direction [0,1,0] as its symmetric radial baseline")
    if c["simulation"] == "gpu" and recipe["template"] not in ("explosion", "environment"):
        raise RecipeError("GPU authoring currently supports explosion and environment only")
    fields(recipe["playback"], ("scale", "speed", "intensity"), "playback")
    number(recipe["playback"]["scale"], 0, 1000, "playback.scale")
    if recipe["playback"]["scale"] == 0:
        raise RecipeError("playback.scale must be greater than zero")
    number(recipe["playback"]["speed"], 0, 16, "playback.speed")
    number(recipe["playback"]["intensity"], 0, 1, "playback.intensity")
    fields(recipe["budget"], ("max_particles",), "budget")
    number(recipe["budget"]["max_particles"], 1, 100000, "max_particles", True)
    if c["particles"] > recipe["budget"]["max_particles"]:
        raise RecipeError("Particle count exceeds declared budget")
    fields(recipe["requirements"], ("backends", "fallback_recipe"), "requirements")
    if recipe["requirements"]["backends"] != ["DX12", "Vulkan"]:
        raise RecipeError("Authoring packages target DX12 and Vulkan; runtime capability still requires validation")
    fallback = recipe["requirements"]["fallback_recipe"]
    if fallback is not None and (not isinstance(fallback, str) or not re.fullmatch(r"[a-z][a-z0-9_-]{0,63}", fallback)):
        raise RecipeError("Invalid explicit fallback recipe id")
    if c["simulation"] == "gpu" and not fallback:
        raise RecipeError("GPU recipes require an explicit CPU fallback recipe")
    if recipe["dependencies"] != []:
        raise RecipeError("External dependencies require the intake workflow; v1 templates use procedural textures only")
    return recipe


def set_xml(root, path, value):
    for name in path.split("/"):
        child = root.find(name)
        if child is None:
            child = ET.SubElement(root, name)
        root = child
    root.text = str(value)


def source(recipe):
    c = recipe["compile"]
    template = recipe["template"]
    frames = max(1, round(60 * (c["attack_s"] + c["sustain_s"] + c["decay_s"])))
    radius = c["radius_m"]
    count = c["particles"]
    sustained = template in ("trail", "aura", "environment")
    # The pinned tool clamps emission intervals to 1e-5 frames. Declare that
    # minimum explicitly so the compiled schedule remains auditable.
    interval = max(.001, c["sustain_s"] * 60 / count) if sustained else .00001
    life = max(1, math.ceil(frames - (count - 1) * interval)) if sustained else frames
    gpu_emit_per_frame = max(1, math.ceil(count / max(1, round(c["sustain_s"]*60)))) if sustained else count
    if c["simulation"] == "gpu":
        # GPU emission is integer batches, not the CPU's fractional interval.
        # Align the last batch's death with the authored end; using the CPU
        # interval here made sparse GPU emitters disappear before decay.
        life = max(1, frames - (count - 1) // gpu_emit_per_frame)
    if life < 1:
        raise RecipeError("Emitter duration cannot contain requested count")
    norm = math.sqrt(sum(v*v for v in c["direction"]))
    direction = [v/norm for v in c["direction"]]
    texture = "Texture/ring.png" if template in ("telegraph", "aura") else "Texture/spark.png"
    size = radius * 2 if template in ("telegraph", "aura") else radius * .14
    spread = radius / life
    kwargs = {}
    if template == "explosion" and c["simulation"] == "cpu":
        kwargs["velocity"] = (0, 0, spread)
    elif template in ("hit", "explosion"):
        kwargs.update(velocity_min=tuple((v*.25-1)*spread for v in direction),
                      velocity_max=tuple((v*.25+1)*spread for v in direction))
        kwargs["velocity"] = tuple(v * spread * .25 for v in direction)
    elif template == "environment":
        kwargs.update(location_min=(-radius, -radius, -radius), location_max=(radius, radius, radius))
        kwargs["velocity"] = tuple(v * spread * .1 for v in direction)
    if template == "trail":
        kwargs["velocity"] = tuple(-v * spread for v in direction)
    node = toon.make_sprite(recipe["id"], texture, tuple(c["color"]),
                            (size, size, size), (size * .1, size * .1, size * .1) if template not in ("telegraph", "aura") else (size, size, size),
                            life, count, interval, fade_in=round(c["attack_s"]*60), fade_out=round(c["decay_s"]*60),
                            billboard=2 if template in ("telegraph", "aura") else 0,
                            rotation=(90, 0, 0) if template in ("telegraph", "aura") else (0, 0, 0), **kwargs)
    if template == "explosion" and c["simulation"] == "cpu":
        # Zero-radius spherical birth rotations turn fixed local Z motion into
        # radial motion. Orthogonal rotations preserve speed=radius/life.
        set_xml(node, "GenerationLocationValues/EffectsRotation", "True")
        set_xml(node, "GenerationLocationValues/Type", 1)
        for name, minimum, maximum in (("Radius", 0, 0), ("RotationX", -90, 90), ("RotationY", -180, 180)):
            for field, value in (("Center", 0), ("Min", minimum), ("Max", maximum)):
                set_xml(node, "GenerationLocationValues/Sphere/" + name + "/" + field, value)
    # The pinned editor stores timing inside CommonValues.Generation. Legacy
    # flat fields are ignored for this project version, silently yielding the
    # default one-particle-per-frame schedule (burst tails grow; auras end early).
    common = node.find("CommonValues")
    if template == "trail" and c["simulation"] == "cpu":
        # Keep each emitted particle's birth transform. The shared projectile
        # helper uses Always (2), which drags old particles with the emitter.
        for field in ("LocationEffectType", "RotationEffectType", "ScaleEffectType"):
            set_xml(common, field, 1)  # pinned Effekseer WhenCreating
    generation = ET.SubElement(common, "Generation")
    for field in ("GenerationTime", "GenerationTimeOffset"):
        timing = common.find(field)
        common.remove(timing)
        generation.append(timing)
    # make_sprite predates Effekseer 1.80's shared renderer color. The declared
    # 1.80.6 source must write the current field, not the old Sprite-only field.
    drawing = node.find("DrawingValues")
    for channel, value in zip(("R", "G", "B", "A"), c["color"]):
        set_xml(drawing, "ColorAll/Fixed/" + channel, value)
    set_xml(drawing, "ColorAll/Type", 0)
    if c["simulation"] == "gpu":
        gpu = ET.parse(RECIPES / "templates/gpu-sprite.xml").getroot()
        for transform in ("LocationValues", "RotationValues", "ScalingValues"):
            node.remove(node.find(transform))
        # IsRendered also gates GPU NewEmitter in Effekseer.Instance.cpp.
        # Disable only the CPU renderer type, never the node's render flag.
        set_xml(node, "IsRendered", "True")
        set_xml(drawing, "Type", 0)
        set_xml(node, "CommonValues/MaxGeneration/Value", 1)
        for key in ("Center", "Min", "Max"):
            # StopEmit leaves existing GPU particles to finish their lifetime.
            # The owner follows the authored duration; adding extra life here
            # delayed handle retirement beyond the recipe's end barrier.
            set_xml(node, "CommonValues/Life/" + key, frames)
        set_xml(gpu, "Basic/EmitCount/Value", count)
        set_xml(gpu, "Basic/EmitPerFrame", gpu_emit_per_frame)
        if template == "environment":
            set_xml(gpu, "EmitShape/Shape", 3)
            set_xml(gpu, "EmitShape/SphereRadius", radius)
        for key in ("Center", "Min", "Max"):
            set_xml(gpu, "Basic/LifeTime/" + key, life)
            set_xml(gpu, "Velocity/InitialSpeed/" + key, spread)
        for key, value in zip(("X", "Y", "Z"), direction):
            set_xml(gpu, "Velocity/Direction/" + key, value)
        set_xml(gpu, "Velocity/Spread", 180 if template == "explosion" else 20)
        set_xml(gpu, "RenderShape/Shape", 0)
        set_xml(gpu, "RenderShape/ShapeSize", size)
        model = gpu.find("RenderShape/ModelPath")
        if model is not None:
            gpu.find("RenderShape").remove(model)
        color = gpu.find("RenderColor/ColorAll")
        color.clear()
        set_xml(color, "Type", 0)
        for channel, value in zip(("R", "G", "B", "A"), c["color"]):
            set_xml(color, "Fixed/" + channel, value)
        set_xml(gpu, "RenderColor/FadeIn", c["attack_s"]*60)
        # Root color remains dynamically applicable to already emitted GPU
        # particles (Effekseer 1.80.6 NotBind_Root), rather than creation-only.
        set_xml(gpu, "RenderColor/ColorInheritType", 3)
        set_xml(gpu, "RenderColor/FadeOut", c["decay_s"]*60)
        set_xml(gpu, "RenderMaterial/ColorTexture/Path", texture)
        node.append(gpu)
    project = ET.Element("EffekseerProject")
    root = ET.SubElement(project, "Root")
    set_xml(root, "Name", "Root")
    ET.SubElement(root, "Children").append(node)
    for path, value in (("ToolVersion", "1.80.6"), ("Version", 3), ("StartFrame", 0), ("EndFrame", frames+2), ("IsLoop", "False"), ("Global/RandomSeed", c["seed"])):
        set_xml(project, path, value)
    toon.indent_xml(project)
    return b'<?xml version="1.0" encoding="utf-8"?>\n' + ET.tostring(project, encoding="utf-8")


def textures():
    import struct
    import zlib
    size = 64
    rows = bytearray()
    for y in range(size):
        rows.append(0)
        for x in range(size):
            radius = math.hypot((x+.5)/size*2-1, (y+.5)/size*2-1)
            alpha = round(255*max(0, 1-abs(radius-.85)/.09))
            rows.extend((255, 255, 255, alpha))
    ring = b'\x89PNG\r\n\x1a\n' + stress.png_chunk(b'IHDR', struct.pack('>IIBBBBB',size,size,8,6,0,0,0)) + stress.png_chunk(b'IDAT',zlib.compress(bytes(rows))) + stress.png_chunk(b'IEND',b'')
    return {"Texture/spark.png": stress.build_spark_texture_png(), "Texture/ring.png": ring}


def inspect_compiled(binary_path, recipe):
    decoded = binary_path.with_suffix(".roundtrip.xml")
    process = subprocess.run(["pwsh", "-NoProfile", "-File", str(RECIPES / "InspectCompiled.ps1"),
                              "-InputFile", str(binary_path), "-OutputFile", str(decoded),
                              "-CoreAssembly", str(ROOT / "Engine/Tools/Effekseer/bin/EffekseerCore.dll")],
                             capture_output=True, text=True, timeout=30)
    if process.returncode != 0 or not decoded.is_file():
        raise RecipeError("Compiled EDIT inspection failed: " + process.stderr[-2000:])
    document = ET.parse(decoded).getroot()
    node = document.find("Root/Children/Node")
    gpu = recipe["compile"]["simulation"] == "gpu"
    if gpu and (node.findtext("GpuParticles/Enabled") != "True" or node.findtext("IsRendered", "True") != "True"):
        raise RecipeError("Compiled GPU node is not enabled and rendered")
    if gpu and node.findtext("DrawingValues/Type") != "0":
        raise RecipeError("GPU recipe unexpectedly enables CPU drawing")
    color_path = "GpuParticles/RenderColor/ColorAll/Fixed" if gpu else "DrawingValues/ColorAll/Fixed"
    color = [int(node.findtext(color_path+"/"+channel, "255")) for channel in ("R","G","B","A")]
    if color != recipe["compile"]["color"]:
        raise RecipeError("Compiler discarded authored color: " + str(color))
    expected_node = ET.fromstring(source(recipe)).find("Root/Children/Node")
    verified = {}
    timing_paths = ["CommonValues/MaxGeneration/Value", "CommonValues/Life/Center",
                    "CommonValues/Generation/GenerationTime/Center",
                    "CommonValues/Generation/GenerationTimeOffset/Center"]
    if gpu:
        paths = ["GpuParticles/Basic/EmitCount/Value", "GpuParticles/Basic/EmitPerFrame",
                 "GpuParticles/Basic/LifeTime/Center", "GpuParticles/Velocity/Spread",
                 "GpuParticles/Velocity/InitialSpeed/Center", "GpuParticles/RenderShape/ShapeSize",
                 "GpuParticles/RenderColor/FadeIn", "GpuParticles/RenderColor/FadeOut",
                 "GpuParticles/RenderColor/ColorInheritType"]
        if recipe["template"] == "environment":
            paths += ["GpuParticles/EmitShape/Shape", "GpuParticles/EmitShape/SphereRadius"]
        if any(node.find(path) is not None for path in ("LocationValues", "RotationValues", "ScalingValues")):
            raise RecipeError("GPU owner unexpectedly transforms particle simulation twice")
        verified["gpu_owner_transform"] = "identity"
    else:
        paths = ["ScalingValues/Easing/Start/X/Center",
                 "ScalingValues/Easing/End/X/Center"]
        for path, seconds in (("RendererCommonValues/FadeIn/Frame",recipe["compile"]["attack_s"]),
                              ("RendererCommonValues/FadeOut/Frame",recipe["compile"]["decay_s"])):
            if round(seconds*60) > 0:
                paths.append(path)
    # EDIT omits defaults. These are the pinned tool's defaults, not a fallback
    # to the requested value: an omitted non-default timing must fail validation.
    defaults = {"CommonValues/MaxGeneration/Value": "1", "CommonValues/Life/Center": "120",
                "CommonValues/Generation/GenerationTime/Center": "1",
                "CommonValues/Generation/GenerationTimeOffset/Center": "0"}
    if recipe["template"] == "trail" and not gpu:
        for field in ("LocationEffectType", "RotationEffectType", "ScaleEffectType"):
            path = "CommonValues/" + field
            paths.append(path)
            defaults[path] = "2"  # actual pinned Core default, never requested 1
        for axis in ("X", "Y", "Z"):
            for field in ("Center", "Min", "Max"):
                path = "LocationValues/PVA/Velocity/" + axis + "/" + field
                paths.append(path)
                defaults[path] = "0"
    if recipe["template"] == "explosion" and not gpu:
        if node.findtext("GenerationLocationValues/EffectsRotation", "False") != "True":
            raise RecipeError("CPU radial explosion lost birth rotation")
        paths.append("GenerationLocationValues/Type")
        defaults["GenerationLocationValues/Type"] = "0"
        for axis in ("X", "Y", "Z"):
            for field in ("Center", "Min", "Max"):
                path = "LocationValues/PVA/Velocity/" + axis + "/" + field
                paths.append(path)
                defaults[path] = "0"
        for name in ("Radius", "RotationX", "RotationY"):
            for field in ("Center", "Min", "Max"):
                path = "GenerationLocationValues/Sphere/" + name + "/" + field
                paths.append(path)
                defaults[path] = "0"  # pinned Core object's measured defaults
    for path in timing_paths + paths:
        expected = expected_node.findtext(path)
        actual = node.findtext(path, defaults.get(path, "0" if expected is not None and float(expected) == 0 else ""))
        if not actual or not math.isclose(float(expected),float(actual),rel_tol=1e-5,abs_tol=1e-6):
            raise RecipeError("Compiler changed {}: {} -> {}".format(path,expected,actual))
        verified[path] = float(actual)
    runtime = dict(stress.parse_chunks(binary_path.read_bytes()))[b"BIN_"]
    rgba_offset = runtime.find(bytes(color))
    if rgba_offset < 0:
        raise RecipeError("Authored RGBA is absent from compiled runtime BIN_")
    return {"gpu_enabled":gpu, "is_rendered":node.findtext("IsRendered", "True"),
            "color_rgba":color, "runtime_rgba_offset":rgba_offset,
            "life_frames":node.findtext("CommonValues/Life/Center", "120"),
            "size":node.findtext("GpuParticles/RenderShape/ShapeSize") if gpu else node.findtext("ScalingValues/Easing/Start/X/Center"),
            "verified_parameters":verified,
            "source_roundtrip_sha256":sha(decoded.read_bytes())}


def recover_publication(root, journal_path):
    if not journal_path.is_file():
        return
    journal = json.loads(journal_path.read_text())
    records = journal["records"]
    for record in records:
        path = (root / record["path"]).resolve()
        allowed = [(root / "Project/SourceAssets/VFXAuthoring").resolve(),
                   (root / "Project/Assets/Effects/Authoring").resolve()]
        if not any(parent in path.parents for parent in allowed):
            raise RecipeError("Unsafe publication recovery path")
        current = sha(path.read_bytes()) if path.is_file() else None
        if current not in (record["old_hash"], record["new_hash"]):
            raise RecipeError("External edit conflicts with interrupted publication: " + str(path))
        if record["old_hash"] is not None:
            if not re.fullmatch(r"[0-9]+\.backup", record["backup"]):
                raise RecipeError("Unsafe publication backup path")
            if sha((journal_path.parent / record["backup"]).read_bytes()) != record["old_hash"]:
                raise RecipeError("Publication backup integrity failure")
    # Inspect every file before restoring any, so a conflict leaves all evidence.
    for record in records:
        path = root / record["path"]
        if record["old_hash"] is None:
            if path.exists():
                path.unlink()
        else:
            backup = journal_path.parent / record["backup"]
            if sha(backup.read_bytes()) != record["old_hash"]:
                raise RecipeError("Publication backup integrity failure")
            toon.publish_atomic(path, backup.read_bytes())
            os.utime(path, ns=(record["atime_ns"], record["mtime_ns"]))
    for record in records:
        if record["backup"]:
            (journal_path.parent / record["backup"]).unlink()
    journal_path.unlink()


def publish_transaction(outputs, root, author, publisher=None):
    transaction = author.parent / ".transactions" / author.name
    transaction.mkdir(parents=True, exist_ok=True)
    journal_path = transaction / "journal.json"
    recover_publication(root, journal_path)
    records = []
    for path, data in outputs.items():
        if path.is_file() and path.read_bytes() == data:
            continue
        old = path.read_bytes() if path.is_file() else None
        record = {"path":path.relative_to(root).as_posix(), "old_hash":sha(old) if old is not None else None,
                  "new_hash":sha(data), "backup":None}
        if old is not None:
            record.update(backup=str(len(records))+".backup", atime_ns=path.stat().st_atime_ns, mtime_ns=path.stat().st_mtime_ns)
            (transaction / record["backup"]).write_bytes(old)
        records.append(record)
    if not records:
        return 0
    toon.publish_atomic(journal_path, encoded({"schema_version":1,"records":records}))
    try:
        for record in records:
            path = root / record["path"]
            current = sha(path.read_bytes()) if path.is_file() else None
            if current != record["old_hash"]:
                raise RecipeError("External edit during publication: " + str(path))
            (publisher or toon.publish_atomic)(path, outputs[path])
    except BaseException:
        recover_publication(root, journal_path)
        raise
    # Removing the journal commits the complete publication; leftover backups
    # after an interruption here are inert and never interpreted as a rollback.
    journal_path.unlink()
    for record in records:
        if record["backup"]:
            (transaction / record["backup"]).unlink()
    return len(records)


def resolve_fallback(fallback_id, root=ROOT):
    fallback_path = RECIPES / (fallback_id + ".recipe.json")
    published = not fallback_path.is_file()
    if published:
        fallback_path = root / "Project/SourceAssets/VFXAuthoring" / fallback_id / "recipe.json"
    if not fallback_path.is_file():
        raise RecipeError("Missing explicit CPU fallback recipe: " + fallback_id + "; build the independent CPU recipe first")
    fallback = validate(json.loads(fallback_path.read_text(encoding="utf-8")))
    if fallback["compile"]["simulation"] != "cpu" or fallback["id"] != fallback_id:
        raise RecipeError("Fallback must identify an explicit CPU recipe")
    if published:
        # Independent recipes are resolved from their existing author publication,
        # never by adding task-specific files to the canonical library.
        package = root / "Project/Assets/Effects/Authoring" / (fallback_id + ".efkpkg")
        report_path = fallback_path.with_name("build.json")
        if not report_path.is_file() or not package.is_file():
            raise RecipeError("Build the independent CPU fallback before its GPU recipe: " + fallback_id)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if (report.get("inputs", {}).get("recipe") != sha(encoded(fallback)) or
                report.get("outputs", {}).get(package.relative_to(root).as_posix()) != sha(package.read_bytes())):
            raise RecipeError("Independent CPU fallback publication is stale or modified: " + fallback_id)
    return fallback


def build(recipe, root=ROOT, force=False, compiler=None, publisher=None):
    validate(recipe)
    fallback_id = recipe["requirements"]["fallback_recipe"]
    if fallback_id:
        fallback = resolve_fallback(fallback_id, root)
    lock = json.loads((RECIPES / "toolchain.lock.json").read_text())
    inputs = {"recipe": sha(encoded(recipe)), "template_version": VERSION}
    if fallback_id:
        inputs["fallback_recipe"] = sha(encoded(fallback))
    for relative, expected in lock["files"].items():
        actual = sha((root / relative).read_bytes())
        if actual != expected:
            raise RecipeError("Pinned Effekseer tool mismatch: " + relative)
        inputs[relative] = actual
    for path in (Path(__file__), Path(toon.__file__), Path(stress.__file__), RECIPES / "templates/gpu-sprite.xml", RECIPES / "toolchain.lock.json", RECIPES / "InspectCompiled.ps1"):
        inputs[path.name] = sha(path.read_bytes())
    license_path = root / stress.SOURCE_LICENSE
    license_bytes = license_path.read_bytes().replace(b'\r\n', b'\n')
    if sha(license_bytes) != stress.EXPECTED_LICENSE_SHA256:
        raise RecipeError("Official sample license evidence changed")
    inputs["license"] = sha(license_bytes)
    author = root / "Project/SourceAssets/VFXAuthoring" / recipe["id"]
    runtime = root / "Project/Assets/Effects/Authoring" / (recipe["id"] + ".efkpkg")
    recover_publication(root, author.parent / ".transactions" / author.name / "journal.json")
    report_path = author / "build.json"
    if not force and report_path.is_file():
        old = json.loads(report_path.read_text())
        if old["inputs"] == inputs and all((root/p).is_file() and sha((root/p).read_bytes()) == h for p,h in old["outputs"].items()):
            return {"status": "unchanged", "id": recipe["id"], "changed": 0, "package": str(runtime)}
    project = source(recipe)
    dependencies = textures()
    # Compile entirely outside the published tree. A compiler failure cannot
    # replace either the previous package or its authoring source.
    with tempfile.TemporaryDirectory(prefix="he-vfx-authoring-") as temporary:
        stage = Path(temporary)
        project_path = stage / (recipe["id"] + ".efkproj")
        project_path.write_bytes(project)
        for name, payload in dependencies.items():
            (stage / name).parent.mkdir(parents=True, exist_ok=True)
            (stage / name).write_bytes(payload)
        binary_path = stage / (recipe["id"] + ".efkefc")
        (compiler or toon.compile_effect)(root / "Engine/Tools/Effekseer/bin/Effekseer.exe", project_path, binary_path)
        inspection = inspect_compiled(binary_path, recipe)
        binary = binary_path.read_bytes()
    provenance = {"recipe": recipe, "inputs": inputs, "tool": lock, "runtime_validation": {"DX12": "not_verified", "Vulkan": "not_verified"},
                  "main_effect": recipe["id"] + ".efkefc", "lifecycle": "finite; stop releases handle; active resource reload behavior belongs to runtime"}
    entries = {recipe["id"] + ".efkefc": binary, "PROVENANCE.json": encoded(provenance), "LICENSE-CC0.md": license_bytes, **dependencies}
    package = toon.deterministic_zip(entries)
    outputs = {author / project_path.name: project, author / binary_path.name: binary,
               author / "recipe.json": encoded(recipe), author / "PROVENANCE.json": encoded(provenance),
               author / "LICENSE-CC0.md": license_bytes, **{author / n: p for n,p in dependencies.items()}, runtime: package}
    report = {"schema_version": VERSION, "inputs": inputs, "outputs": {p.relative_to(root).as_posix():sha(data) for p,data in outputs.items()}, "compiler": "PASS", "compiled_inspection":inspection, "runtime_validation": provenance["runtime_validation"]}
    outputs[report_path] = encoded(report)
    changed = publish_transaction(outputs, root, author, publisher)
    return {"status": "compiled", "id": recipe["id"], "changed": changed, "package": str(runtime), "runtime_validation": provenance["runtime_validation"]}


def catalog(describe=None):
    # Offline authoring knows compile constraints, not the running backend's
    # capabilities. Embedded callers can pass their live VFX.Describe callable.
    runtime = {"status":"unavailable", "reason":"No live HEngine.VFX.Describe provider"}
    try:
        if describe is None:
            import HEngine
            describe = HEngine.VFX.Describe
        runtime = {"status":"observed", "source":"HEngine.VFX.Describe",
                   "description":describe()}
    except (ImportError, AttributeError, RuntimeError) as error:
        runtime["reason"] = str(error)
    return {"schema_version": VERSION, "templates": list(TEMPLATES), "tool_version": "1.80.6",
            "compile_parameters": {"radius_m": "meters, [.01,50]; CPU explosion: approximate maximum particle-center radius over finite life, not the visible sprite/fade envelope", "attack_s/sustain_s/decay_s": "seconds, [0,30]", "particles": "[1,100000] compiler limit only; validate-runtime checks actual runtime capacity", "color": "RGBA integer [0,255]", "seed": "fixed simulation seed", "direction": "nonzero vector, normalized; CPU explosion only accepts [0,1,0] as symmetric radial baseline; ground-ring templates also require [0,1,0]"},
            "playback_parameters": {"scale": "uniform multiplier (0,1000], default 1", "speed": "simulation multiplier [0,16], default 1",
                "intensity": "componentwise RGBA attenuation [0,1], default 1; preserves base color; not HDR amplification"},
            "runtime_radius_m": "SetParameter target [.01,50] / recipe v1 compile.radius_m => uniform scale; not measured particle bounds",
            "semantic_only": ["purpose", "style"], "gpu_author_templates": ["explosion", "environment"],
            "runtime_support": runtime,
            "playback_application": "Metadata defaults, not baked into EFKE. Caller applies scale/speed at play and intensity with SetParameter; GPU intensity/color requires all compiled GPU nodes to use NotBind_Root"}


def validate_runtime_budget(recipe, stats):
    capacity = stats.get("particle_buffer_capacity")
    if isinstance(capacity, dict):
        capacity = capacity.get("current")
    if not isinstance(capacity, (int, float)) or capacity <= 0:
        raise RecipeError("Current runtime particle buffer capacity is unavailable")
    if recipe["compile"]["particles"] > capacity:
        raise RecipeError("Recipe exceeds current runtime particle capacity {}".format(capacity))
    if recipe["compile"]["simulation"] == "gpu" and not (
            stats.get("gpu_particles_capable") is True and stats.get("gpu_particles_enabled") is True):
        raise RecipeError("Current backend has not enabled verified GPU particle capability")
    return {"status":"capacity_checked", "capacity":capacity,
            "note":"Single-effect capacity only; simultaneous load and visual/performance acceptance still required"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("describe", "validate", "validate-runtime", "build", "build-library"))
    parser.add_argument("recipe", nargs="?", type=Path)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--capabilities", type=Path, help="Current VFX.GetStatsSnapshot JSON for validate-runtime")
    args = parser.parse_args()
    try:
        if args.action == "describe":
            print(json.dumps(catalog(), indent=2))
        else:
            paths = sorted(RECIPES.glob("*.recipe.json")) if args.action == "build-library" else [args.recipe]
            if not all(paths):
                raise RecipeError("recipe path is required")
            for path in paths:
                recipe = validate(json.loads(path.read_text(encoding="utf-8")))
                if args.action == "validate-runtime":
                    if not args.capabilities:
                        raise RecipeError("validate-runtime requires --capabilities from the current runtime")
                    result = validate_runtime_budget(recipe, json.loads(args.capabilities.read_text()))
                elif args.action == "validate":
                    result = {"status":"schema_valid", "runtime_budget":"not_verified", "id":recipe["id"]}
                else:
                    result = build(recipe, force=args.force)
                print(json.dumps(result))
        return 0
    except (RecipeError, toon.GenerationError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(json.dumps({"status":"FAIL", "error":str(error)}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
