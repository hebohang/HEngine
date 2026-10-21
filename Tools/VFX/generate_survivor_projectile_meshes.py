#!/usr/bin/env python3
"""Generate native HEngine projectile meshes from the reviewed OBJ/MTL sources.

The runtime loose-OBJ path does not reliably preserve MTL colors.  This tool
keeps the small, human-readable OBJ files as the authored source of truth and
deterministically emits native ``.hmesh/.bin/.hmat`` resources for gameplay.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Iterable, Tuple


SOURCE_ASSET_ROOT = Path("Project/Assets/Models/SurvivorWeapons")
OUTPUT_PROJECT_ROOT = Path("Project/Assets/Models/SurvivorWeapons/Generated")
OUTPUT_ASSET_ROOT = "Assets/Models/SurvivorWeapons/Generated"
MODEL_NAMES = (
    "StarWheel",
    "CometCore",
    "BubbleCore",
    "FireballCore",
    "PetalCore",
)
DOUBLE_SIDED_MATERIALS = {
    ("StarWheel", "MoonCandy"),
    ("FireballCore", "FlameOrange"),
    ("PetalCore", "PetalBlush"),
}
OPAQUE_SOURCE_OVERRIDES = {
    # HEngine's small, fast projectile path renders this layered shell more
    # reliably as saturated solid cyan than as a depth-sorted transparent mesh.
    ("BubbleCore", "BubbleGlass"),
    ("BubbleCore", "BubbleGlint"),
}


Vec3 = Tuple[float, float, float]


@dataclass(frozen=True)
class MtlMaterial:
    name: str
    diffuse: Vec3
    emissive: Vec3
    shininess: float
    opacity: float


@dataclass(frozen=True)
class ObjSource:
    positions: tuple[Vec3, ...]
    material_order: tuple[str, ...]
    triangles: tuple[tuple[str, tuple[int, int, int]], ...]
    material_library: str


def load_mesh_helpers(repo_root: Path) -> ModuleType:
    helper_path = repo_root / "Project/Tools/GenerateVerdantRift.py"
    specification = importlib.util.spec_from_file_location(
        "he_survivor_projectile_mesh_helpers",
        helper_path,
    )
    if specification is None or specification.loader is None:
        raise RuntimeError(f"Cannot load mesh helpers: {helper_path}")
    module = importlib.util.module_from_spec(specification)
    sys.modules[specification.name] = module
    specification.loader.exec_module(module)
    module.ASSET_MODEL_ROOT = OUTPUT_ASSET_ROOT
    return module


def parse_float_triplet(tokens: list[str], location: str) -> Vec3:
    if len(tokens) < 3:
        raise ValueError(f"Expected three numbers at {location}")
    values = tuple(float(token) for token in tokens[:3])
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"Non-finite value at {location}")
    return values  # type: ignore[return-value]


def parse_obj(path: Path) -> ObjSource:
    positions: list[Vec3] = []
    material_order: list[str] = []
    triangles: list[tuple[str, tuple[int, int, int]]] = []
    material_library = ""
    current_material = ""

    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(),
        start=1,
    ):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        tokens = line.split()
        command = tokens[0]
        values = tokens[1:]
        location = f"{path}:{line_number}"

        if command == "mtllib":
            if len(values) != 1 or material_library:
                raise ValueError(f"Expected exactly one mtllib at {location}")
            material_library = values[0]
        elif command == "v":
            positions.append(parse_float_triplet(values, location))
        elif command == "usemtl":
            if len(values) != 1:
                raise ValueError(f"Malformed usemtl at {location}")
            current_material = values[0]
            if current_material in material_order:
                raise ValueError(
                    f"Repeated usemtl would make primitive order ambiguous at {location}"
                )
            material_order.append(current_material)
        elif command == "f":
            if not current_material:
                raise ValueError(f"Face precedes usemtl at {location}")
            if len(values) < 3:
                raise ValueError(f"Face has fewer than three vertices at {location}")
            face: list[int] = []
            for token in values:
                vertex_text = token.split("/", 1)[0]
                if not vertex_text:
                    raise ValueError(f"Face has no position index at {location}")
                raw_index = int(vertex_text)
                resolved_index = (
                    raw_index - 1 if raw_index > 0 else len(positions) + raw_index
                )
                if resolved_index < 0 or resolved_index >= len(positions):
                    raise ValueError(f"Face position is out of range at {location}")
                face.append(resolved_index)
            for index in range(1, len(face) - 1):
                triangles.append(
                    (current_material, (face[0], face[index], face[index + 1]))
                )
        elif command in {"o", "g", "s", "vn", "vt"}:
            continue
        else:
            raise ValueError(f"Unsupported OBJ command {command!r} at {location}")

    if not material_library:
        raise ValueError(f"OBJ has no mtllib: {path}")
    if not positions or not triangles or not material_order:
        raise ValueError(f"OBJ has incomplete geometry: {path}")
    used_materials = {material for material, _ in triangles}
    if used_materials != set(material_order):
        raise ValueError(f"OBJ contains an empty usemtl group: {path}")
    return ObjSource(
        positions=tuple(positions),
        material_order=tuple(material_order),
        triangles=tuple(triangles),
        material_library=material_library,
    )


def parse_mtl(path: Path) -> dict[str, MtlMaterial]:
    raw_materials: dict[str, dict[str, object]] = {}
    current: dict[str, object] | None = None

    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8").splitlines(),
        start=1,
    ):
        line = raw_line.split("#", 1)[0].strip()
        if not line:
            continue
        tokens = line.split()
        command = tokens[0]
        values = tokens[1:]
        location = f"{path}:{line_number}"

        if command == "newmtl":
            if len(values) != 1 or values[0] in raw_materials:
                raise ValueError(f"Malformed or duplicate newmtl at {location}")
            current = {
                "name": values[0],
                "Kd": (1.0, 1.0, 1.0),
                "Ke": (0.0, 0.0, 0.0),
                "Ns": 32.0,
                "d": 1.0,
            }
            raw_materials[values[0]] = current
        elif command in {"Kd", "Ke"}:
            if current is None:
                raise ValueError(f"{command} precedes newmtl at {location}")
            current[command] = parse_float_triplet(values, location)
        elif command == "Ns":
            if current is None or len(values) != 1:
                raise ValueError(f"Malformed Ns at {location}")
            current[command] = float(values[0])
        elif command in {"d", "Tr"}:
            if current is None or len(values) != 1:
                raise ValueError(f"Malformed {command} at {location}")
            opacity = float(values[0])
            current["d"] = 1.0 - opacity if command == "Tr" else opacity
        elif command in {"Ka", "Ks", "Ni", "illum"}:
            continue
        else:
            raise ValueError(f"Unsupported MTL command {command!r} at {location}")

    materials: dict[str, MtlMaterial] = {}
    for name, raw in raw_materials.items():
        shininess = float(raw["Ns"])
        opacity = float(raw["d"])
        if not math.isfinite(shininess) or shininess < 0.0:
            raise ValueError(f"Invalid Ns for material {name} in {path}")
        if not math.isfinite(opacity) or not 0.0 <= opacity <= 1.0:
            raise ValueError(f"Invalid opacity for material {name} in {path}")
        materials[name] = MtlMaterial(
            name=name,
            diffuse=raw["Kd"],  # type: ignore[arg-type]
            emissive=raw["Ke"],  # type: ignore[arg-type]
            shininess=shininess,
            opacity=opacity,
        )
    if not materials:
        raise ValueError(f"MTL contains no materials: {path}")
    return materials


def pbr_roughness(shininess: float) -> float:
    # Standard Blinn-Phong exponent to perceptual roughness approximation.
    return max(0.12, min(0.72, math.sqrt(2.0 / (shininess + 2.0))))


def prefixed_material_name(model_name: str, material_name: str) -> str:
    return f"{model_name}_{material_name}"


def triangle_cross(positions: tuple[Vec3, ...], indices: tuple[int, int, int]) -> Vec3:
    a, b, c = (positions[index] for index in indices)
    ab = tuple(b[axis] - a[axis] for axis in range(3))
    ac = tuple(c[axis] - a[axis] for axis in range(3))
    return (
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    )


def validate_obj_topology(source: ObjSource, model_name: str) -> None:
    used_vertices: set[int] = set()
    adjacency: dict[int, set[int]] = {
        index: set() for index in range(len(source.positions))
    }
    for _, indices in source.triangles:
        used_vertices.update(indices)
        face_cross = triangle_cross(source.positions, indices)
        if not all(math.isfinite(value) for value in face_cross):
            raise ValueError(f"{model_name} contains a non-finite triangle")
        if sum(value * value for value in face_cross) <= 1.0e-12:
            raise ValueError(f"{model_name} contains a degenerate triangle")
        for index in indices:
            adjacency[index].update(indices)
    if used_vertices != set(range(len(source.positions))):
        raise ValueError(f"{model_name} contains unused OBJ vertices")

    remaining = set(used_vertices)
    while remaining:
        first = next(iter(remaining))
        component = {first}
        pending = [first]
        remaining.remove(first)
        while pending:
            current = pending.pop()
            for neighbor in adjacency[current]:
                if neighbor in remaining:
                    remaining.remove(neighbor)
                    component.add(neighbor)
                    pending.append(neighbor)

        component_triangles = [
            indices for _, indices in source.triangles if indices[0] in component
        ]
        edge_counts: dict[tuple[int, int], int] = {}
        for indices in component_triangles:
            for index in range(3):
                edge = tuple(sorted((indices[index], indices[(index + 1) % 3])))
                edge_counts[edge] = edge_counts.get(edge, 0) + 1
        if any(count > 2 for count in edge_counts.values()):
            raise ValueError(f"{model_name} contains a non-manifold mesh component")

        if edge_counts and all(count == 2 for count in edge_counts.values()):
            center = tuple(
                sum(source.positions[index][axis] for index in component)
                / len(component)
                for axis in range(3)
            )
            signed_volume = 0.0
            for indices in component_triangles:
                a, b, c = (source.positions[index] for index in indices)
                face_cross = triangle_cross(source.positions, indices)
                centroid = tuple((a[axis] + b[axis] + c[axis]) / 3.0 for axis in range(3))
                outward = sum(
                    face_cross[axis] * (centroid[axis] - center[axis])
                    for axis in range(3)
                )
                if outward <= 1.0e-9:
                    raise ValueError(
                        f"{model_name} contains an inward closed-shell triangle"
                    )
                signed_volume += (
                    a[0] * (b[1] * c[2] - b[2] * c[1])
                    - a[1] * (b[0] * c[2] - b[2] * c[0])
                    + a[2] * (b[0] * c[1] - b[1] * c[0])
                ) / 6.0
            if signed_volume <= 1.0e-9:
                raise ValueError(f"{model_name} contains a reversed closed shell")


def validate_mesh_normals(mesh: object) -> None:
    positions = mesh.positions
    normals = mesh.normals
    texcoords = mesh.texcoords
    if (
        len(positions) != len(normals)
        or len(positions) != len(texcoords)
        or len(positions) % 3 != 0
    ):
        raise ValueError(f"Mesh {mesh.name} has invalid expanded attributes")
    for index in range(0, len(positions), 3):
        a, b, c = positions[index:index + 3]
        ab = tuple(b[axis] - a[axis] for axis in range(3))
        ac = tuple(c[axis] - a[axis] for axis in range(3))
        face = (
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0],
        )
        length = math.sqrt(sum(component * component for component in face))
        if length <= 1.0e-8:
            raise ValueError(f"Mesh {mesh.name} contains a degenerate triangle")
        expected = tuple(component / length for component in face)
        for actual in normals[index:index + 3]:
            if not all(math.isfinite(component) for component in actual):
                raise ValueError(f"Mesh {mesh.name} contains a non-finite normal")
            normal_length = math.sqrt(sum(component * component for component in actual))
            if abs(normal_length - 1.0) > 1.0e-5:
                raise ValueError(f"Mesh {mesh.name} contains a non-unit normal")
            alignment = sum(expected[axis] * actual[axis] for axis in range(3))
            if alignment < 0.9999:
                raise ValueError(f"Mesh {mesh.name} contains an incorrect face normal")


def validate_mesh_resource(
    resource: dict[str, object],
    binary: bytes,
    model_name: str,
    material_order: tuple[str, ...],
) -> None:
    if resource.get("version") != 1 or resource.get("name") != model_name:
        raise ValueError(f"Native mesh header is invalid for {model_name}")
    buffers = resource.get("bufferVec")
    if not isinstance(buffers, list) or len(buffers) != 1:
        raise ValueError(f"Native mesh needs one buffer for {model_name}")
    buffer = buffers[0]
    expected_uri = f"{OUTPUT_ASSET_ROOT}/{model_name}.bin"
    if (
        buffer.get("uri") != expected_uri
        or buffer.get("data") != []
        or buffer.get("byteLength") != len(binary)
    ):
        raise ValueError(f"Native mesh buffer metadata is invalid for {model_name}")

    views = resource.get("bufferViewVec")
    accessors = resource.get("accessorVec")
    primitives = resource.get("primitives")
    if not isinstance(views, list) or not isinstance(accessors, list):
        raise ValueError(f"Native mesh tables are invalid for {model_name}")
    if not isinstance(primitives, list) or len(primitives) != len(material_order):
        raise ValueError(f"Native mesh primitive count is invalid for {model_name}")

    cursor = 0
    for view in sorted(views, key=lambda value: int(value["byteOffset"])):
        offset = int(view["byteOffset"])
        length = int(view["byteLength"])
        if (
            view.get("bufferIndex") != 0
            or view.get("byteStride") != 0
            or offset != cursor
            or offset % 4 != 0
            or length <= 0
            or offset + length > len(binary)
        ):
            raise ValueError(f"Native mesh buffer view is invalid for {model_name}")
        cursor += length
    if cursor != len(binary):
        raise ValueError(f"Native mesh views do not cover its buffer for {model_name}")

    for accessor in accessors:
        view_index = int(accessor.get("bufferViewIndex", -1))
        if view_index < 0 or view_index >= len(views) or int(accessor.get("count", 0)) <= 0:
            raise ValueError(f"Native mesh accessor is invalid for {model_name}")

    expected_material_paths = [
        f"{OUTPUT_ASSET_ROOT}/Materials/"
        f"{prefixed_material_name(model_name, material)}.hmat"
        for material in material_order
    ]
    actual_material_paths: list[str] = []
    for primitive in primitives:
        if primitive.get("mode") != 4:
            raise ValueError(f"Native mesh primitive mode is invalid for {model_name}")
        attributes = primitive.get("attributes")
        if not isinstance(attributes, dict):
            raise ValueError(f"Native mesh attributes are invalid for {model_name}")
        for semantic, value_type in (
            ("POSITION", "VEC3"),
            ("NORMAL", "VEC3"),
            ("TEXCOORD_0", "VEC2"),
        ):
            accessor_index = int(attributes.get(semantic, -1))
            if accessor_index < 0 or accessor_index >= len(accessors):
                raise ValueError(f"Missing {semantic} for {model_name}")
            accessor = accessors[accessor_index]
            if accessor.get("componentType") != 5126 or accessor.get("type") != value_type:
                raise ValueError(f"Invalid {semantic} accessor for {model_name}")
        position_accessor = accessors[int(attributes["POSITION"])]
        normal_accessor = accessors[int(attributes["NORMAL"])]
        texcoord_accessor = accessors[int(attributes["TEXCOORD_0"])]
        if not (
            position_accessor["count"]
            == normal_accessor["count"]
            == texcoord_accessor["count"]
        ):
            raise ValueError(f"Native mesh attribute counts differ for {model_name}")

        index_accessor_index = int(primitive.get("indices", -1))
        if index_accessor_index < 0 or index_accessor_index >= len(accessors):
            raise ValueError(f"Missing index accessor for {model_name}")
        index_accessor = accessors[index_accessor_index]
        index_count = int(index_accessor.get("count", 0))
        if (
            index_accessor.get("componentType") not in {5121, 5123, 5125}
            or index_accessor.get("type") != "SCALAR"
            or index_count <= 0
            or index_count % 3 != 0
            or int(index_accessor["min"][0]) < 0
            or int(index_accessor["max"][0]) >= int(position_accessor["count"])
        ):
            raise ValueError(f"Invalid index accessor for {model_name}")
        actual_material_paths.append(str(primitive.get("materialResPath", "")))
    if actual_material_paths != expected_material_paths:
        raise ValueError(f"Native mesh material order drifted for {model_name}")


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=4, ensure_ascii=False) + "\n").encode("utf-8")


def build_payloads(repo_root: Path) -> tuple[dict[Path, bytes], dict[str, object]]:
    helpers = load_mesh_helpers(repo_root)
    source_root = repo_root / SOURCE_ASSET_ROOT
    output_root = repo_root / OUTPUT_PROJECT_ROOT
    payloads: dict[Path, bytes] = {}
    model_reports: list[dict[str, object]] = []
    source_digest = hashlib.sha256()

    for model_name in MODEL_NAMES:
        obj_path = source_root / f"{model_name}.obj"
        if not obj_path.is_file():
            raise FileNotFoundError(f"Missing projectile OBJ source: {obj_path}")
        source = parse_obj(obj_path)
        mtl_path = obj_path.with_name(source.material_library)
        if not mtl_path.is_file() or mtl_path.parent != source_root:
            raise FileNotFoundError(f"Missing local projectile MTL source: {mtl_path}")
        mtl_materials = parse_mtl(mtl_path)
        if set(source.material_order) != set(mtl_materials):
            raise ValueError(
                f"OBJ/MTL material sets differ for {model_name}: "
                f"{sorted(set(source.material_order) ^ set(mtl_materials))}"
            )
        validate_obj_topology(source, model_name)

        for source_path in (obj_path, mtl_path):
            relative = source_path.relative_to(repo_root).as_posix().encode("utf-8")
            source_digest.update(relative)
            source_digest.update(b"\0")
            source_digest.update(source_path.read_bytes())

        mesh = helpers.MeshBuilder(model_name)
        for material_name, indices in source.triangles:
            a, b, c = (source.positions[index] for index in indices)
            mesh.triangle(
                prefixed_material_name(model_name, material_name),
                a,
                b,
                c,
            )
        if mesh.triangle_count != len(source.triangles):
            raise ValueError(f"Triangle conversion dropped geometry for {model_name}")
        if len(mesh.primitive_indices) != len(source.material_order):
            raise ValueError(f"Primitive count differs from usemtl count for {model_name}")
        validate_mesh_normals(mesh)

        mesh_resource, binary = helpers.mesh_payload(mesh)
        helpers.assert_forward_slashes(mesh_resource, f"mesh:{model_name}")
        validate_mesh_resource(
            mesh_resource,
            binary,
            model_name,
            source.material_order,
        )
        payloads[output_root / f"{model_name}.hmesh"] = json_bytes(mesh_resource)
        payloads[output_root / f"{model_name}.bin"] = binary

        for material_name in source.material_order:
            source_material = mtl_materials[material_name]
            material_key = (model_name, material_name)
            if source_material.opacity < 1.0:
                if material_key not in OPAQUE_SOURCE_OVERRIDES:
                    raise ValueError(
                        f"Unexpected transparent projectile material: {material_key}"
                    )
            elif material_key in OPAQUE_SOURCE_OVERRIDES:
                raise ValueError(f"Expected a source opacity override for {material_key}")
            generated_name = prefixed_material_name(model_name, material_name)
            emissive = (
                (*source_material.emissive, 1.0)
                if any(value > 0.0 for value in source_material.emissive)
                else None
            )
            specification = helpers.MaterialSpec(
                (*source_material.diffuse, 1.0),
                0.0,
                pbr_roughness(source_material.shininess),
                emissive,
                material_key in DOUBLE_SIDED_MATERIALS,
                "OPAQUE",
            )
            material_resource = helpers.material_json(generated_name, specification)
            helpers.assert_forward_slashes(
                material_resource,
                f"material:{generated_name}",
            )
            properties = material_resource["Properties"]
            if properties["pbrMetallicRoughness"]["baseColorFactor"][:3] != list(
                source_material.diffuse
            ):
                raise ValueError(f"Base color drifted for {generated_name}")
            if properties["emissiveFactor"] != list(source_material.emissive):
                raise ValueError(f"Emissive color drifted for {generated_name}")
            if properties["alphaMode"] != "OPAQUE":
                raise ValueError(f"Projectile material must remain OPAQUE: {generated_name}")
            payloads[
                output_root / "Materials" / f"{generated_name}.hmat"
            ] = json_bytes(material_resource)

        model_reports.append({
            "model": model_name,
            "source_vertices": len(source.positions),
            "triangles": mesh.triangle_count,
            "primitives": len(mesh.primitive_indices),
            "materials": list(source.material_order),
        })

    payload_digest = hashlib.sha256()
    for path, payload in sorted(
        payloads.items(),
        key=lambda item: item[0].as_posix(),
    ):
        payload_digest.update(path.relative_to(repo_root).as_posix().encode("utf-8"))
        payload_digest.update(b"\0")
        payload_digest.update(payload)

    return payloads, {
        "output": OUTPUT_PROJECT_ROOT.as_posix(),
        "source_sha256": source_digest.hexdigest().upper(),
        "payload_sha256": payload_digest.hexdigest().upper(),
        "generated_file_count": len(payloads),
        "model_count": len(model_reports),
        "primitive_count": sum(int(report["primitives"]) for report in model_reports),
        "triangle_count": sum(int(report["triangles"]) for report in model_reports),
        "models": model_reports,
    }


def existing_output_files(output_root: Path) -> set[Path]:
    if not output_root.exists():
        return set()
    files: set[Path] = set()
    for path in output_root.rglob("*"):
        if path.is_symlink():
            raise RuntimeError(f"Refusing generated output symlink: {path}")
        if path.is_file():
            files.add(path.resolve())
    return files


def validate_output_set(payloads: dict[Path, bytes], output_root: Path) -> None:
    expected = {path.resolve() for path in payloads}
    unexpected = existing_output_files(output_root) - expected
    if unexpected:
        relative = [path.relative_to(output_root).as_posix() for path in sorted(unexpected)]
        raise RuntimeError(f"Unexpected files in owned generated output: {relative}")


def write_payloads(payloads: dict[Path, bytes], output_root: Path) -> None:
    validate_output_set(payloads, output_root)
    output_root.mkdir(parents=True, exist_ok=True)
    for path, payload in sorted(payloads.items(), key=lambda item: item[0].as_posix()):
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".tmp")
        if temporary.exists():
            raise RuntimeError(f"Refusing stale generated temporary file: {temporary}")
        try:
            temporary.write_bytes(payload)
            temporary.replace(path)
        finally:
            if temporary.exists():
                temporary.unlink()


def check_payloads(payloads: dict[Path, bytes], output_root: Path) -> None:
    validate_output_set(payloads, output_root)
    missing = [path for path in payloads if not path.is_file()]
    changed = [
        path for path, payload in payloads.items()
        if path.is_file() and path.read_bytes() != payload
    ]
    if missing or changed:
        def names(paths: Iterable[Path]) -> list[str]:
            return [path.relative_to(output_root).as_posix() for path in sorted(paths)]
        raise RuntimeError(
            f"Generated projectile assets are stale; missing={names(missing)} "
            f"changed={names(changed)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="HEngineDev repository root",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="Verify checked-in generated assets without writing files",
    )
    arguments = parser.parse_args()

    repo_root = Path(arguments.repo_root).resolve()
    if not (repo_root / "Project/Project.heproject").is_file():
        raise RuntimeError(f"Not an HEngineDev repository root: {repo_root}")
    output_root = (repo_root / OUTPUT_PROJECT_ROOT).resolve()
    project_root = (repo_root / "Project").resolve()
    if project_root not in output_root.parents:
        raise RuntimeError(f"Generated output escapes Project: {output_root}")

    payloads, report = build_payloads(repo_root)
    if arguments.check:
        check_payloads(payloads, output_root)
        report["mode"] = "check"
    else:
        write_payloads(payloads, output_root)
        report["mode"] = "generate"
    report["result"] = "PASS"
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
